// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 BOBI SAS, France
// Auteur : Cyril Mazouer, pour le compte de BOBI SAS
// Distribué sous licence GNU GPL v3 (ou ultérieure) ; voir le fichier LICENSE.
//
// CHEMIN DE DONNÉES DeckLink → bus MXL — chantier `decklink_io` (docs/chantiers/DECKLINK.md),
// jalon 4. C'est le cœur du futur plugin ; il vit encore dans `tools/bancs/` tant qu'il n'a pas
// tourné assez longtemps pour mériter un plugin autour de lui.
//
//   ./decklink_rx <persistent_id> <nom> [secondes] [--stats <f>] [--audio <n>] [--anc]
//
// `--stats` écrit périodiquement un JSON que le superviseur Python lit (même convention que
// `mtl_rx --stats_file`). `secondes` à 0 = sans limite (mode plugin).
//
// Conventions RESPECTÉES, et chacune pour une raison payée ailleurs :
//
//   ★ le sous-périphérique se désigne par son PERSISTENT ID, jamais par son index — un
//     changement de profil renumérote et redistribue les index (mesuré le 2026-09-08 : les
//     groupes sont {0,2} et {1,3}, pas des paires consécutives) ;
//   ★ `duplex != inactive` est testé AVANT `VideoIOSupport`, qui reste vrai sur un
//     sous-périphérique désactivé ;
//   ★ la reconfiguration sur changement de format n'a lieu QUE si le mode diffère réellement —
//     sinon `EnableVideoInput` re-déclenche la notification et le manège tourne (66 « changements »
//     en 3 s sur une source stable) ;
//   ★ une trame marquée `bmdFrameHasNoInputSource` n'est JAMAIS écrite sur le bus. Pas de trame
//     fabriquée en silence (§1) ;
//   ★ capture en **v210 10 bits**, le format natif de la carte, converti en planar 10 bits par
//     `v210convert.c` (déjà écrit et mesuré — on ne le réécrit pas). Capturer en 8 bits aurait
//     été plus simple et aurait jeté deux bits sans que rien ne le signale.
//
// ⚠ LIMITE ASSUMÉE de cette version : l'index de grain est ANCRÉ sur `mxlGetCurrentIndex`
//   (horloge TAI courante) et NON sur l'horodatage matériel de la trame. La raison est mesurée :
//   le compteur de la carte ne porte pas l'époque (§9 quinquies), il faudrait relever son origine
//   au démarrage pour l'ancrer. Tant que ce n'est pas fait, une trame capturée est indexée à
//   l'instant où NOUS la traitons, pas à l'instant où elle est entrée. À corriger avant de
//   prétendre à une latence chiffrée.
//   ★ Ce que cette version CORRIGE (28/10) : l'index n'est plus RECALCULÉ sur l'horloge à chaque
//     trame. Il est désormais CONTIGU et ne se ré-ancre que sur un écart franc (`index_trame`),
//     parce que relire l'horloge à chaque trame perdait un tiers des images de toute source dont
//     la phase passait sur la frontière d'un créneau — et sans genlock, toutes y passent.
//     L'ancrage sur l'horloge ne subsiste donc qu'à la PREMIÈRE trame et aux décrochages.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <dlfcn.h>
#include <time.h>
#include <math.h>
#include <string>
#include <vector>
#include <openssl/sha.h>

#include "decklink_sdk/DeckLinkAPI.h"
#include "anc_rfc8331.h"
#include <mxl/mxl.h>
#include <mxl/flow.h>
#include <mxl/time.h>
#include <mxl/rational.h>

// ── Convertisseur v210 → planar ────────────────────────────────────────────────────────────
// Chargé à l'EXÉCUTION, pas lié à la compilation : l'image d'exécution ne publie que la variante
// AVX2 (`libbobi_v210_v3.so`), et un nœud sans AVX2 en a une autre. Un lien direct produirait un
// binaire qui ne démarre pas sur la moitié de la flotte — piège déjà payé sur les R620.
typedef void (*unpack10_fn)(const uint8_t *, size_t, uint16_t *, uint16_t *, uint16_t *, int, int);
static unpack10_fn g_unpack10 = NULL;

static int charger_v210(void) {
    static const char *candidats[] = {
        "libbobi_v210_v3.so", "libbobi_v210.so",
        "/usr/local/lib/libbobi_v210_v3.so", "/usr/local/lib/libbobi_v210.so", NULL};
    for (int i = 0; candidats[i]; i++) {
        void *h = dlopen(candidats[i], RTLD_NOW);
        if (!h) continue;
        g_unpack10 = (unpack10_fn)dlsym(h, "bobi_v210_unpack10");
        if (g_unpack10) { fprintf(stderr, "v210 : %s\n", candidats[i]); return 0; }
        dlclose(h);
    }
    fprintf(stderr, "ERREUR : aucun convertisseur v210 trouvable — la capture 10 bits est "
                    "impossible. On refuse plutôt que d'écrire n'importe quoi.\n");
    return -1;
}

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int) { g_stop = 1; }

static std::string prendre(const char *s) {
    if (!s) return std::string();
    std::string o(s); free(const_cast<char *>(s)); return o;
}

// Namespace UUIDv5 Bobi.Studio — DOIT être identique à bobimxl._NS_BOBI et à mtl_rx.c, sinon les
// flux écrits ici sont invisibles aux consommateurs. (Vérifié contre bobimxl, cf. README.)
static const uint8_t NS_BOBI[16] = {
    0xd4,0xe7,0x7c,0xba,0x0e,0x52,0x55,0xd9,0x82,0xed,0x72,0x26,0xb7,0xbd,0xa7,0x57};

static void flow_id_str(const char *name, char out[37]) {
    SHA_CTX c; unsigned char h[20];
    SHA1_Init(&c);
    SHA1_Update(&c, NS_BOBI, 16);
    SHA1_Update(&c, name, strlen(name));
    SHA1_Final(h, &c);
    h[6] = (unsigned char)((h[6] & 0x0f) | 0x50);
    h[8] = (unsigned char)((h[8] & 0x3f) | 0x80);
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             h[0],h[1],h[2],h[3],h[4],h[5],h[6],h[7],h[8],h[9],h[10],h[11],h[12],h[13],h[14],h[15]);
}

// flowDef planar 10 bits — mêmes champs et mêmes noms que build_video_flowdef() de mtl_rx.c.
// ⚠ DUPLICATION ASSUMÉE et à résorber : deux producteurs natifs écrivent maintenant ces
//   conventions chacun de leur côté. Elles doivent être EXTRAITES dans un module partagé — une
//   divergence d'un seul champ rendrait les flux illisibles, sans erreur visible.
// flowDef AUDIO — mêmes conventions que build_audio_flowdef() de mtl_rx.c.
// ★ Le bus MXL porte du **float32 planar par canal**, pas du L24 entrelacé : la conversion est
//   à notre charge. Écrire les octets de la carte tels quels donnerait un flux qui se lit sans
//   erreur et ne contient que du bruit.
// flowDef DATA/ANC — mêmes conventions que build_data_flowdef() de mtl_rx.c. Le champ
// `bobi_anc_format` ANNONCE le codage aux consommateurs (flotte mixte pendant la migration).
static std::string build_anc_flowdef(const char *name, mxlRational rate) {
    char id[37]; flow_id_str(name, id);
    char buf[1024];
    snprintf(buf, sizeof buf,
        "{\"id\":\"%s\",\"bobi_anc_format\":\"rfc8331\","
        "\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"%s:Data\"]},"
        "\"format\":\"urn:x-nmos:format:data\",\"label\":\"%s\","
        "\"media_type\":\"video/smpte291\","
        "\"grain_rate\":{\"numerator\":%lld,\"denominator\":%lld}}",
        id, name, name, (long long)rate.numerator, (long long)rate.denominator);
    return std::string(buf);
}

static std::string build_audio_flowdef(const char *name, int canaux) {
    char id[37]; flow_id_str(name, id);
    char buf[1024];
    snprintf(buf, sizeof buf,
        "{\"id\":\"%s\","
        "\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"%s:Audio\"]},"
        "\"format\":\"urn:x-nmos:format:audio\","
        "\"label\":\"%s\","
        "\"media_type\":\"audio/float32\","
        "\"sample_rate\":{\"numerator\":48000,\"denominator\":1},"
        "\"channel_count\":%d,\"bit_depth\":32}",
        id, name, name, canaux);
    return std::string(buf);
}

// MODE TRANCHE — hauteur de bande de la SORTIE. Même algorithme que `_cc_slice_h` du
// color_corrector et `_proxy_slice_h` de la pyramide : le plus petit diviseur de `h` supérieur
// ou égal à h/30 (1080 → 36 lignes, soit 30 bandes). Une hauteur sans diviseur raisonnable rend
// 0 = inéligible, donc trame entière — un producteur qui mentirait sur son découpage serait pire
// qu'un producteur non tranché.
//
// ★ POURQUOI LA CAPTURE TRANCHE ALORS QU'ELLE REÇOIT DES TRAMES ENTIÈRES. La carte ne livre
//   qu'une trame complète (aucun rappel sous-trame dans le SDK, vérifié en 12.2.2 et dans les
//   symboles de la 16.4) : on ne gagne donc RIEN sur la capture elle-même. Mais publier d'un
//   bloc impose une trame d'attente à TOUT consommateur en mode tranche placé derrière — le
//   correcteur de couleur, le mur, le scope. Trancher ici ne récupère pas notre trame : ça
//   cesse de la faire payer à toute la chaîne en aval.
static int hauteur_tranche(int h) {
    const int lo = h / 30 > 0 ? h / 30 : 1;
    for (int sh = lo; sh <= h / 2; sh++)
        if (h % sh == 0) return sh;
    return 0;
}

// ★ PROFONDEUR PUBLIÉE — elle ne s'invente pas, elle vient de l'orchestrateur (19/10). La carte
//   livre du v210, donc 10 bits ; mais `mxl_pipeline_bit_depth` arbitre la profondeur du BUS pour
//   TOUT le produit, et vaut « force8 » par défaut. Nous écrivions 10 en dur : notre capture
//   ignorait cet arbitrage, doublait la mémoire partagée contre une décision explicite, et
//   publiait une profondeur que les autres consommateurs n'attendent pas — exactement la faute
//   que notre propre SORTIE commettait en sens inverse, et qui a donné une image doublée.
//   La conversion se fait ICI, à la frontière de la carte.
static int g_bits_pub = 10;
static inline size_t octets_pub(void) { return g_bits_pub >= 10 ? 2u : 1u; }
static std::vector<uint16_t> g_tampon;      // déballage v210 avant resserrement 10 → 8

static std::string build_flowdef(const char *name, int w, int h, mxlRational rate, bool interlace, bool tff) {
    char id[37]; flow_id_str(name, id);
    char buf[2048];
    // `slice_height` en TÊTE, comme le fait `build_flow_def` de bobimxl. Absent en entrelacé :
    // la règle du dépôt garde le champ-natif en trame entière, et le grain y est déjà un CHAMP.
    char tranche[48] = {0};
    const int sh = interlace ? 0 : hauteur_tranche(h);
    if (sh > 0) snprintf(tranche, sizeof tranche, "\"slice_height\":%d,", sh);
    snprintf(buf, sizeof buf,
        "{%s\"id\":\"%s\","
        "\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"%s:Video\"]},"
        "\"format\":\"urn:x-nmos:format:video\","
        "\"label\":\"%s\","
        "\"media_type\":\"video/x-mxl-planar\","
        "\"grain_rate\":{\"numerator\":%lld,\"denominator\":%lld},"
        "\"frame_width\":%d,\"frame_height\":%d,"
        "\"interlace_mode\":\"%s\","
        "\"colorspace\":\"BT709\","
        "\"components\":["
          "{\"name\":\"Y\",\"width\":%d,\"height\":%d,\"bit_depth\":%d},"
          "{\"name\":\"Cb\",\"width\":%d,\"height\":%d,\"bit_depth\":%d},"
          "{\"name\":\"Cr\",\"width\":%d,\"height\":%d,\"bit_depth\":%d}]}",
        tranche, id, name, name,
        (long long)rate.numerator, (long long)rate.denominator, w, h,
        interlace ? (tff ? "interlaced_tff" : "interlaced_bff") : "progressive",
        w, h, g_bits_pub, w / 2, h, g_bits_pub, w / 2, h, g_bits_pub);
    return std::string(buf);
}

// État publié pour le superviseur. Écrit en ATOMIQUE (fichier temporaire + rename) : le lecteur
// Python peut échantillonner à tout moment sans jamais tomber sur un JSON tronqué.
// Cadence → rationnel CANONIQUE. Miroir de `fps_to_rational` dans mtl_rx.c.
//
// ★ Ce n'était PAS cosmétique. Je publiais le rationnel brut de DeckLink (25000/1000 au lieu de
//   25/1) en notant la divergence comme une dette de propreté. Sur une source ENTRELACÉE, la
//   création du flux échouait alors avec « Unsupported grain version » : libmxl double la
//   cadence en interne pour obtenir la cadence CHAMP, et la forme non canonique du rationnel ne
//   survit pas à ce calcul. Un même rapport ne vaut pas un même rationnel.
static mxlRational fps_canonique(double f) {
    mxlRational r = {25, 1};
    if      (fabs(f - 23.98) < 0.05) { r.numerator = 24000; r.denominator = 1001; }
    else if (fabs(f - 24.0)  < 0.05) { r.numerator = 24;    r.denominator = 1; }
    else if (fabs(f - 25.0)  < 0.05) { r.numerator = 25;    r.denominator = 1; }
    else if (fabs(f - 29.97) < 0.05) { r.numerator = 30000; r.denominator = 1001; }
    else if (fabs(f - 30.0)  < 0.05) { r.numerator = 30;    r.denominator = 1; }
    else if (fabs(f - 50.0)  < 0.05) { r.numerator = 50;    r.denominator = 1; }
    else if (fabs(f - 59.94) < 0.1)  { r.numerator = 60000; r.denominator = 1001; }
    else if (fabs(f - 60.0)  < 0.05) { r.numerator = 60;    r.denominator = 1; }
    else if (fabs(f - 100.0) < 0.05) { r.numerator = 100;   r.denominator = 1; }
    else if (fabs(f - 120.0) < 0.05) { r.numerator = 120;   r.denominator = 1; }
    else {
        // Cadence hors liste : on la TRANSPORTE telle quelle plutôt que de la ramener à 25 en
        // silence. Un format exotique doit rester visible, quitte à échouer bruyamment ensuite.
        r.numerator = (int64_t)(f * 1000.0 + 0.5); r.denominator = 1000;
    }
    return r;
}

struct Etat;
static void ecrire_stats(const struct Etat *e, const char *chemin);

struct Etat {
    mxlInstance     mxl    = NULL;
    mxlFlowWriter   writer = NULL;
    mxlRational     rate   = {25, 1};
    std::string     nom;
    int w = 0, h = 0;
    long ecrites = 0, sans_source = 0, refusees = 0, changements = 0;
    // ── POURQUOI une trame a été refusée ──────────────────────────────────────────────────
    // ⚠ `refusees` SEUL NE DIT RIEN. Il agrégeait SIX causes sans rapport entre elles — flux
    //   jamais créé, tampon que la carte ne rend pas, anneau MXL qui refuse le grain, grain trop
    //   court — et une seule d'entre elles renseignait `diag`. Relevé le 2026-10-02 : 2235
    //   refus en onze jours, `diag` VIDE, donc aucun moyen de savoir laquelle avait mordu.
    //   Un compteur qui ne distingue pas ce qu'il compte ne vaut pas mieux que pas de compteur.
    //   `refusees` reste la SOMME (les lecteurs existants ne changent pas) ; le détail s'ajoute.
    long r_sans_writer = 0;   // le flux de sortie n'existe pas — tout est perdu, pas un incident isolé
    long r_sans_octets = 0;   // GetBytes a échoué : la carte n'a pas rendu son tampon
    long r_open_grain = 0;    // l'anneau MXL a refusé d'ouvrir le grain (contention, index)
    long r_grain_court = 0;   // grain plus petit que l'image : on REFUSE plutôt que de tronquer
    long r_avance_grille = 0; // trame JETÉE : la source est en avance franche sur la grille MXL
    // Cadence GLISSANTE : le total divisé par la durée écraserait un décrochage récent sous la
    // moyenne. On mesure sur une fenêtre courte, comme le reste de la flotte.
    long   ecrites_fenetre = 0;
    double t_fenetre = 0.0, fps = 0.0;
    bool   signal = false;
    // ★ LE BALAYAGE, et il manquait : `interlace_` vivait dans la classe Capture, donc
    //   `ecrire_stats` ne pouvait pas le publier. Un format annoncé « 1080p50 » alors que la
    //   source est entrelacée est pire qu'un format absent, et c'est ce que l'extérieur aurait
    //   déduit de la seule paire (largeur, hauteur).
    bool   entrelace = false;

    // ── Index VIDÉO (grille TRAME) : contigu, cf. `index_trame` ──
    uint64_t v_dernier = 0;       // dernier index TRAME attribué
    bool     v_amorce = false;
    // ★ ÉCART ENTIER À LA GRILLE, en CRÉNEAUX et pas en millisecondes. C'était
    //   `video_offset_ms`, et il était MAL NOMMÉ : `(suivant - horloge)` est un nombre entier
    //   de créneaux, donc le multiplier par la durée d'une trame ne pouvait rendre que des
    //   multiples de 20 ms. Mesuré sur la Z8 le 30/10, 60 échantillons par entrée : exactement
    //   ±20,00 ms sur les cinq entrées, min = médiane = max, pas une variation. Un témoin qui
    //   ne bouge jamais n'est pas un témoin stable, c'est un témoin qui ne mesure pas ce qu'on
    //   croit. En régime normal il vaut -1 (on écrit un créneau derrière l'horloge vive, ce
    //   qu'impose la latence de la carte) ; +1 dit que la source DEVANCE le nœud, et c'est
    //   cette entrée-là qui jettera une trame tôt ou tard.
    long long v_grille_trames = 0;
    // ★★ LA PHASE, elle, est CONTINUE — et c'est elle qu'il fallait publier le 28/10 : la
    //    caméra perdue arrivait à 19,99 ms d'un créneau de 20, les quatre saines entre 1,3 et
    //    5,6 ms. Aucun compteur ne disait ça, et l'écart entier ci-dessus ne pouvait pas le
    //    dire.
    //    ⚠ ORIGINE MESURÉE, PAS SUPPOSÉE : `mxlIndexToTimestamp` rend le CENTRE du créneau,
    //      pas son début (relevé le 30/10 dans le conteneur : `now - timestamp(index courant)`
    //      balaie [-9,38 ; +9,85] ms pour un créneau de 20). `v_phase_ms` est donc signé
    //      autour du centre : 0 = pile au milieu, ±10 = sur la frontière.
    double   v_phase_ms = 0.0;
    // ★ CE QUI SE LIT EN UNE COMPARAISON : la distance au bord le plus proche. PETIT = EN
    //   DANGER (une gigue d'une milliseconde suffit à changer de créneau), T/2 = idéal. La
    //   phase signée ci-dessus dit de quel côté on dérive ; celle-ci dit le risque, sans que
    //   le lecteur ait à savoir que le danger est aux DEUX extrémités d'un intervalle.
    double   v_bord_ms = 0.0;
    // ★ LE MINIMUM COURU DEPUIS LE DÉMARRAGE : un relevé ponctuel RATE le passage. La phase
    //   dérive de ~1,5 ms/h, donc la traversée dure quelques minutes — entre deux sondages
    //   d'une console, elle passe inaperçue. Ce minimum-là ne la rate pas.
    double   v_bord_min_ms = 1e9;
    long     v_resync_avant = 0;   // retard franc : on saute à l'horloge (le trou reste un trou)
    long     v_recal_arriere = 0;  // avance franche : la trame est JETÉE pour reprendre la phase

    // ── Audio ──
    mxlFlowWriter a_writer = NULL;
    // ★★ 16 CANAUX = DEUX FLUX DE 8, PAS UN FLUX DE 16 (21/10, demandé après la panne de ce
    //    jour-là). Nous publiions un unique flux de 16 canaux : sur les 28 flux audio du
    //    domaine, 25 en portaient 8 et les 3 seuls à en porter 16 étaient NOS captures. Cette
    //    forme-là, nous étions seuls à la produire et seuls à savoir la lire — et elle ratait
    //    en SILENCE dans les deux sens : une sortie réglée sur 16 devant un flux de 8 laissait
    //    les canaux 9-16 muets (vécu sur Quad SDI 1), et un consommateur ordinaire branché sur
    //    notre flux de 16 n'en lisait que les 8 premiers sans le dire.
    //    Le second groupe prend le suffixe `_audio_2` — convention DÉJÀ en service dans le
    //    produit (`rl-micros_audio_1`…`_audio_7`). Le PREMIER garde son nom `_audio` : le
    //    renommer casserait tous les câbles déjà posés.
    mxlFlowWriter a_writer2 = NULL;
    mxlRational   srate    = {48000, 1};
    std::string   a_nom;
    std::string   a_nom2;
    int      canaux  = 0;      // canaux pris SUR LA CARTE (2, 8 ou 16)
    int      canaux_g1 = 0;    // canaux du 1er flux (= min(canaux, 8))
    int      canaux_g2 = 0;    // canaux du 2e flux (= canaux - 8, ou 0)
    uint64_t a_end   = 0;        // index UN-APRÈS-LA-FIN du dernier échantillon écrit
    // ★★ LE TÉMOIN QUI MANQUAIT, ET SANS LEQUEL ON NE POUVAIT PAS RÉPONDRE (28/10). Sur la Z8,
    //    l'audio capturé était estampillé ~200 ms EN RETARD et l'écart GRANDISSAIT ; la console
    //    #214 lisait l'index d'il y a 10 ms et ne trouvait jamais rien — zéro lecture réussie.
    //    Or la seule grandeur qui décide, l'écart entre où l'on ÉCRIT et où l'horloge en est,
    //    n'était publiée nulle part : il a fallu la déduire de l'extérieur, flux par flux.
    //    Signé : POSITIF = on écrit en AVANCE sur l'horloge, NÉGATIF = en retard (le symptôme).
    double   a_ecart_ms = 0.0;
    long     a_resync_avant = 0;  // recalages VERS L'AVANT (on avait pris du retard) + silence
    long     a_recal_arriere = 0; // décrochage franc : on écrivait très en avance
    bool     a_amorce = false;
    long     a_echant = 0, a_silence = 0;
    // ── Témoins du chemin AUDIO ───────────────────────────────────────────────────────────
    // ⚠ CE CHEMIN ABANDONNAIT EN SILENCE À TROIS ENDROITS : paquet vide, `OpenSamples` refusé,
    //   et trou trop long pour être comblé. Aucun ne comptait quoi que ce soit. Mesuré le
    //   2026-10-02 : on n'écrivait que 29 834 échantillons/s là où le temps réel en demande
    //   48 000 — donc 38 % du flux manquait, et RIEN dans les statistiques ne disait où il
    //   partait. Un abandon muet est une panne qu'on s'interdit de diagnostiquer.
    long     a_paquets = 0;      // paquets audio reçus de la carte
    long     a_vides = 0;        // paquet sans échantillon, ou tampon non rendu
    long     a_open_echec = 0;   // `OpenSamples` a refusé : ces échantillons sont PERDUS
    long     a_trou_long = 0;    // trou > 250 ms : délibérément NON comblé (ré-ancrage franc)
    long     a_recul = 0;        // index REÇU derrière l'index écrit : recouvrement
    long     a_reancrages = 0;   // recalages francs sur l'horloge (dérive > 250 ms)
    // Crête mesurée sur les échantillons BRUTS de la carte, AVANT toute conversion. C'est ce qui
    // départage « la source n'envoie pas de son » de « notre conversion l'écrase » — deux pannes
    // qui produisent exactement le même silence chez le consommateur.
    int32_t  a_crete_brute = 0;
    // ── ANC ──
    mxlFlowWriter anc_writer = NULL;
    std::string   anc_nom;
    bool     anc_on = false;
    long     anc_grains = 0, anc_paquets = 0, anc_trop_gros = 0;
    // ⚠ CE REFUS ABANDONNAIT EN SILENCE (`return` nu) : l'ANC perdait EXACTEMENT les mêmes
    //   trames que la vidéo à la frontière d'un créneau, et rien ne le comptait.
    long     anc_open_echec = 0;
    long     anc_tc_synth = 0;      // paquets ATC que NOUS avons construits (pilote muet)
    char     diag[400] = {0};       // motif d'un échec de création de flux, publié tel quel
    // Témoins du mode tranche : sans eux, une capture retombée en trame entière est
    // indiscernable d'une capture qui publie par bandes — et elle impose une trame d'attente
    // à tout le reste de la chaîne sans que rien ne le dise.
    long     tranchees = 0, entieres = 0;
    // ★ Instant de la DERNIÈRE trame comptée, pris DANS le callback. Écrire l'horloge depuis la
    //   boucle principale la décalerait de 200 ms au pire — soit 22 ppm sur une fenêtre de 900 s,
    //   alors qu'on cherche à en résoudre 1. La paire (compte, instant) doit être prise au même
    //   endroit du CODE, pas seulement au même moment du fichier.
    int64_t  t_derniere_trame = 0;
    char     tc[16] = {0};
    long     a_flottants = 0;      // combien de float ont RÉELLEMENT été écrits dans l'anneau
    int      a_tranches  = -1;     // slc.count observé (nombre de tampons de canal)
};

// Nomme la cause du PREMIER refus, et ne l'écrase plus : la première est la plus instructive
// (les suivantes en découlent souvent), et un diagnostic qui change à chaque trame est illisible.
// Sans ça, `refused` montait sans que rien ne dise POURQUOI — constaté le 2026-10-02.
static void noter_cause(Etat *e, const char *motif) {
    if (e && !e->diag[0] && motif)
        snprintf(e->diag, sizeof e->diag, "trame refusée : %s", motif);
}

static int64_t maintenant_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static double horloge_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void ecrire_stats(const Etat *e, const char *chemin) {
    if (!chemin || !*chemin) return;
    // ★★ DEUX ÉCARTS DIFFÉRENTS, ET J'AI D'ABORD PUBLIÉ LE MAUVAIS (28/10, après 0.29.31).
    //    `audio_offset_ms` compare le curseur d'écriture à l'horloge AU MOMENT OÙ UN PAQUET
    //    ARRIVE : c'est un détecteur de DÉRIVE, et il est à ~0 parce que la dérive est corrigée.
    //    Mais il ne peut RIEN dire du retard que subit un consommateur, parce qu'il n'est évalué
    //    qu'aux instants où ce retard est nul par construction — juste après une écriture.
    //    La session Z8 l'a vu tout de suite : offset à 0, et pourtant des têtes à 21-39 ms
    //    derrière TAI, mesurées depuis le conteneur de la console.
    //
    //    Celui-ci est l'autre écart : la tête du flux audio par rapport à MAINTENANT, échantillonné
    //    à l'instant du relevé de statistiques, donc INCORRÉLÉ de l'arrivée des paquets.
    // ⚠ C'EST UN ÉCHANTILLON DE DENT DE SCIE, PAS UNE CONSTANTE. L'audio arrive par paquets :
    //   juste après une écriture la tête est à l'heure, juste avant la suivante elle a UNE
    //   PÉRIODE DE PAQUET de retard. Une valeur isolée ne veut donc rien dire ; c'est son MAXIMUM
    //   qui est la latence vraie, et il vaut la période du paquet (`audio_packets` par seconde le
    //   donne : 25/s → 40 ms, 50/s → 20 ms). Lire un seul relevé comme « la » latence est
    //   exactement l'erreur que ce commentaire existe pour empêcher.
    double tete_ms = 0.0;
    if (e->a_amorce && e->srate.numerator) {
        const double ech_s = e->srate.numerator / (double)e->srate.denominator;
        tete_ms = ((double)(int64_t)(e->a_end - mxlGetCurrentIndex(&e->srate))) * 1000.0 / ech_s;
    }
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", chemin);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    // ★ `t_node` : l'horloge du NŒUD, écrite dans le MÊME instant que `frame_index`.
    //   Sans elle, mesurer la cadence réelle depuis l'orchestrateur revient à dater le compte
    //   avec une horloge distante et une latence variable : deux relevés espacés de 120 s ont
    //   donné +674 ppm, et 300 s +107 ppm — l'artefact décroît avec la fenêtre, ce qui le trahit.
    //   La paire (compte, instant) doit être ATOMIQUE, sinon aucune fenêtre n'est assez longue.
    fprintf(f, "{\"t_frame_ns\": %lld, \"fps\": %.2f, \"frame_index\": %ld, \"signal\": %s, "
               "\"width\": %d, \"height\": %d, \"interlaced\": %s, \"flow\": \"%s\", "
               "\"no_source\": %ld, \"refused\": %ld, \"format_changes\": %ld, "
               // ★ LES TÉMOINS D'HORLOGE VIDÉO, et c'est de leur ABSENCE que le défaut du 28/10
               //   a vécu : seul `refused_open_grain` bougeait, sans dire de quel côté de la
               //   frontière du créneau la source se trouvait ni si l'on corrigeait.
               //   `video_phase_ms` est la PHASE dans le créneau (signée, origine au CENTRE) et
               //   `video_edge_ms` la distance au bord le plus proche : PETIT = EN DANGER.
               //   `video_edge_min_ms` est la pire approche depuis le démarrage — un relevé
               //   ponctuel rate la traversée, qui dure quelques minutes.
               //   `video_grid_frames` est l'écart ENTIER en créneaux (-1 en régime normal,
               //   +1 = la source devance le nœud). Il remplace `video_offset_ms`, qui
               //   prétendait être des millisecondes alors qu'il ne pouvait valoir que des
               //   multiples d'une trame (relevé Z8 du 30/10 : ±20,00 ms, immobile).
               //   `video_resync_fwd` = sauts en avant (trous réels, non comblés),
               //   `video_resync_back` = trames JETÉES parce que la source devance la grille.
               "\"video_phase_ms\": %.2f, \"video_edge_ms\": %.2f, "
               "\"video_edge_min_ms\": %.2f, \"video_grid_frames\": %lld, "
               "\"video_resync_fwd\": %ld, "
               "\"video_resync_back\": %ld, "
               // ★ `audio_flows` et `audio_channels_2` : sans eux, rien ne distingue de
               //   l'extérieur « 16 canaux sur deux flux » de « 8 canaux, second flux refusé ».
               "\"audio_flows\": %d, \"audio_channels_2\": %d, "
               "\"audio_channels\": %d, \"audio_samples\": %ld, \"audio_silence\": %ld, "
               "\"audio_peak_raw\": %d, \"audio_floats\": %ld, \"audio_slices\": %d, "
               "\"audio_packets\": %ld, \"audio_empty\": %ld, \"audio_open_fail\": %ld, "
               "\"audio_long_gap\": %ld, \"audio_backward\": %ld, \"audio_reanchor\": %ld, "
               // ★ L'ÉCART SIGNÉ EST LE TÉMOIN, les deux recalages en sont les symptômes.
               // ★ `audio_offset_ms` = dérive du curseur d'écriture (à l'arrivée d'un paquet).
               //   `audio_head_lag_ms` = retard de la TÊTE sur maintenant, échantillonné ici.
               //   Les deux ne mesurent PAS la même chose et aucun ne remplace l'autre.
               "\"audio_offset_ms\": %.2f, \"audio_head_lag_ms\": %.2f, \"audio_resync_fwd\": %ld, "
               "\"audio_resync_back\": %ld, "
               "\"anc_grains\": %ld, \"anc_packets\": %ld, \"anc_oversize\": %ld, "
               "\"anc_tc_synth\": %ld, \"timecode\": \"%s\", "
               "\"sliced\": %ld, \"whole\": %ld, "
               "\"refused_no_writer\": %ld, \"refused_no_bytes\": %ld, "
               "\"refused_open_grain\": %ld, \"refused_grain_short\": %ld, "
               "\"refused_ahead_of_grid\": %ld, \"anc_open_fail\": %ld, "
               "\"diag\": \"%s\"}\n",
            (long long)e->t_derniere_trame,
            e->fps, e->ecrites, e->signal ? "true" : "false",
            e->w, e->h, e->entrelace ? "true" : "false", e->nom.c_str(),
            e->sans_source, e->refusees, e->changements,
            e->v_phase_ms, e->v_bord_ms,
            (e->v_bord_min_ms > 1e8 ? 0.0 : e->v_bord_min_ms), e->v_grille_trames,
            e->v_resync_avant, e->v_recal_arriere,
            e->a_writer2 ? 2 : (e->a_writer ? 1 : 0), e->canaux_g2,
            e->canaux_g1, e->a_echant, e->a_silence, e->a_crete_brute, e->a_flottants, e->a_tranches,
            e->a_paquets, e->a_vides, e->a_open_echec, e->a_trou_long, e->a_recul, e->a_reancrages,
            e->a_ecart_ms, tete_ms, e->a_resync_avant, e->a_recal_arriere,
            e->anc_grains, e->anc_paquets, e->anc_trop_gros, e->anc_tc_synth, e->tc,
            e->tranchees, e->entieres,
            e->r_sans_writer, e->r_sans_octets, e->r_open_grain, e->r_grain_court,
            e->r_avance_grille, e->anc_open_echec,
            e->diag);
    fclose(f);
    rename(tmp, chemin);                    // atomique : jamais de JSON tronqué chez le lecteur
}

// Comble un trou par du SILENCE, comme mtl_rx : sans ça, le consommateur lit des échantillons
// PÉRIMÉS de l'anneau à la place — du son ancien qui repasse, ce qui s'entend et ne se voit sur
// aucun compteur. Au-delà de ~250 ms on ne comble pas : on se ré-ancre franchement.
static bool combler_un(Etat *e, mxlFlowWriter w, uint64_t trou) {
    mxlMutableWrappedMultiBufferSlice g;
    memset(&g, 0, sizeof g);
    if (mxlFlowWriterOpenSamples(w, e->a_end, trou, &g) != MXL_STATUS_OK) return false;
    for (size_t c = 0; c < g.count; c++)
        for (int f = 0; f < 2; f++) {            // deux fragments : l'anneau peut s'enrouler
            if (!g.base.fragments[f].size) continue;
            memset((uint8_t *)g.base.fragments[f].pointer + c * g.stride, 0,
                   g.base.fragments[f].size);   // 0x00000000 == 0.0f en IEEE754
        }
    mxlFlowWriterCommitSamples(w);
    return true;
}

static void combler_silence(Etat *e, uint64_t idx) {
    if (!e->a_amorce) return;
    if (idx <= e->a_end) { if (idx < e->a_end) e->a_recul++; return; }
    const uint64_t trou = idx - e->a_end;
    if (trou > (uint64_t)(0.25 * e->srate.numerator / (double)e->srate.denominator)) {
        e->a_trou_long++;
        return;
    }
    // ⚠ LE COMBLEMENT VAUT POUR LES DEUX FLUX. Ne combler que le premier ferait avancer son
    //   index seul : les deux groupes ne décriraient plus le même instant, et le décalage
    //   entre les canaux 1-8 et 9-16 grandirait à chaque trou.
    if (!combler_un(e, e->a_writer, trou)) return;
    if (e->a_writer2) combler_un(e, e->a_writer2, trou);
    e->a_silence += (long)trou;
    e->a_end = idx;
}

class Capture : public IDeckLinkInputCallback {
public:
    Capture(IDeckLinkInput *in, Etat *e) : in_(in), e_(e) {}
    HRESULT QueryInterface(REFIID, void **v) override { *v = NULL; return E_NOINTERFACE; }
    ULONG AddRef(void) override  { return ++refs_; }
    ULONG Release(void) override { ULONG r = --refs_; if (!r) delete this; return r; }

    HRESULT VideoInputFormatChanged(BMDVideoInputFormatChangedEvents,
                                    IDeckLinkDisplayMode *m,
                                    BMDDetectedVideoInputFormatFlags) override {
        if (!m) return S_OK;
        const BMDDisplayMode nouveau = m->GetDisplayMode();
        if (nouveau == courant_) return S_OK;        // ★ sinon : manège
        courant_ = nouveau;
        e_->changements++;
        e_->w = (int)m->GetWidth(); e_->h = (int)m->GetHeight();
        BMDTimeValue d = 0; BMDTimeScale s = 0;
        if (m->GetFrameRate(&d, &s) == S_OK && d) e_->rate = fps_canonique(s / (double)d);
        const BMDFieldDominance fd = m->GetFieldDominance();
        interlace_ = (fd == bmdUpperFieldFirst || fd == bmdLowerFieldFirst);
        tff_ = (fd == bmdUpperFieldFirst);
        e_->entrelace = interlace_;
        in_->PauseStreams();
        in_->EnableVideoInput(nouveau, bmdFormat10BitYUV, bmdVideoInputEnableFormatDetection);
        in_->FlushStreams();
        in_->StartStreams();
        ouvrir_writer();
        return S_OK;
    }

    // Entrelacé : la carte livre une TRAME, libmxl attend UN GRAIN PAR CHAMP, à la cadence
    // CHAMP (il double le grain_rate en interne). On écrit donc deux grains en sautant une
    // ligne sur deux. Isolé dans sa propre méthode depuis le 2026-10-02 : tant qu'il
    // partageait le préambule du chemin progressif, il n'était jamais atteint.
    //
    // ★ Parité : index PAIR = premier champ. En tff le premier champ est le TOP, donc la
    //   ligne 0 ; en bff c'est la ligne 1. Se tromper de parité produit une image qui a
    //   l'air correcte sur un arrêt sur image et saccade en MOUVEMENT — le pire des
    //   défauts, parce qu'il passe la relecture d'un opérateur pressé.
    // Déverse `nl` lignes v210 (lues depuis `src`, pas `stride`) dans le grain, À LA PROFONDEUR
    // PUBLIÉE, en écrivant à partir de la ligne `l0` de plans hauts de `hp`.
    //
    // ★ UN SEUL ENDROIT CONVERTIT. Les trois chemins d'écriture (trame entière, bande, champ)
    //   passent par ici : en dupliquer la conversion garantissait qu'un des trois dériverait.
    // ⚠ En 8 bits on déballe dans un tampon puis on RESSERRE (>> 2) : `bobi_v210_unpack10` écrit
    //   des échantillons 16 bits, et il n'existe pas de variante 8 bits. Le tampon ne couvre que
    //   la bande demandée — en mode tranche cette méthode est appelée une trentaine de fois par
    //   image, et déballer la trame entière à chaque appel ferait du mode tranche le chemin lent.
    static void verser(const uint8_t *src, size_t stride, uint8_t *grain,
                       int w, int hp, int l0, int nl) {
        const size_t wc = (size_t)(w / 2);
        if (g_bits_pub >= 10) {
            uint16_t *y  = (uint16_t *)grain;
            uint16_t *cb = y  + (size_t)w * hp;
            uint16_t *cr = cb + wc * hp;
            g_unpack10(src, stride, y + (size_t)l0 * w,
                       cb + (size_t)l0 * wc, cr + (size_t)l0 * wc, w, nl);
            return;
        }
        const size_t ny = (size_t)w * (size_t)nl, nc = wc * (size_t)nl;
        if (g_tampon.size() < ny + 2 * nc) g_tampon.resize(ny + 2 * nc);
        uint16_t *ty = g_tampon.data(), *tcb = ty + ny, *tcr = tcb + nc;
        g_unpack10(src, stride, ty, tcb, tcr, w, nl);
        uint8_t *y  = grain;
        uint8_t *cb = grain + (size_t)w * hp;
        uint8_t *cr = cb + wc * hp;
        for (size_t i = 0; i < ny; i++) y [(size_t)l0 * w  + i] = (uint8_t)(ty [i] >> 2);
        for (size_t i = 0; i < nc; i++) cb[(size_t)l0 * wc + i] = (uint8_t)(tcb[i] >> 2);
        for (size_t i = 0; i < nc; i++) cr[(size_t)l0 * wc + i] = (uint8_t)(tcr[i] >> 2);
    }

    void ecrire_champs(IDeckLinkVideoInputFrame *f, int w, int h, uint64_t idx_trame) {
        void *src = NULL;
        if (f->GetBytes(&src) != S_OK || !src) {
            e_->refusees++; e_->r_sans_octets++;
            noter_cause(e_, "la carte n'a pas rendu le tampon de la trame (GetBytes)");
            return;
        }
        const size_t besoin_champ =
            ((size_t)w * (h / 2) + (size_t)(w / 2) * (h / 2) * 2u) * octets_pub();
        // ★ La grille CHAMP se dérive de l'index de TRAME, elle ne se relit pas sur l'horloge :
        //   2·idx est par construction aligné sur un premier champ, et c'est la même valeur que
        //   l'ancien `mxlGetCurrentIndex(cadence champ) & ~1` — mais désormais CONTIGUË, donc
        //   sans la répétition d'index qui perdait les trames à la frontière d'un créneau.
        const uint64_t base = idx_trame * 2ULL;
        for (int ch = 0; ch < 2; ch++) {
            // Décalage de ligne du champ : en tff, le champ 0 commence ligne 0.
            const int ligne0 = tff_ ? ch : (1 - ch);
            mxlGrainInfo gi2; uint8_t *p2 = NULL;
            if (mxlFlowWriterOpenGrain(e_->writer, base + ch, &gi2, &p2) != MXL_STATUS_OK) {
                e_->refusees++; e_->r_open_grain++;
                noter_cause(e_, "l'anneau MXL a refusé d'ouvrir le grain d'un CHAMP");
                continue;
            }
            if (gi2.grainSize && (size_t)gi2.grainSize < besoin_champ) {
                e_->refusees++; e_->r_grain_court++;
                if (!e_->diag[0])
                    snprintf(e_->diag, sizeof e_->diag,
                             "grain de champ %u octets pour %zu nécessaires — dimensionnement "
                             "inattendu, rien n'est écrit", (unsigned)gi2.grainSize, besoin_champ);
                continue;
            }
            const size_t stride = (size_t)f->GetRowBytes();
            // Une ligne sur deux : on avance d'une ligne pour le second champ, et on double
            // le pas. Le convertisseur v210 honore le pas source, donc rien à recopier.
            verser((const uint8_t *)src + (size_t)ligne0 * stride, stride * 2,
                   p2, w, h / 2, 0, h / 2);
            gi2.validSlices = gi2.totalSlices;
            mxlFlowWriterCommitGrain(e_->writer, &gi2);
            e_->ecrites++;
            e_->t_derniere_trame = maintenant_ns();
        }
        if (e_->anc_writer) ecrire_anc(f, idx_trame);
        const double now2 = horloge_s();
        if (e_->t_fenetre == 0.0) e_->t_fenetre = now2;
        e_->ecrites_fenetre += 2;
        if (now2 - e_->t_fenetre >= 1.0) {
            e_->fps = e_->ecrites_fenetre / (now2 - e_->t_fenetre);
            e_->ecrites_fenetre = 0; e_->t_fenetre = now2;
        }
    }

    HRESULT VideoInputFrameArrived(IDeckLinkVideoInputFrame *f,
                                   IDeckLinkAudioInputPacket *a) override {
        if (a && e_->a_writer) ecrire_audio(a);
        if (!f) return S_OK;
        if (f->GetFlags() & bmdFrameHasNoInputSource) {
            e_->sans_source++;
            e_->signal = false;             // ★ publié : « pas de signal » est un ÉTAT, pas un silence
            e_->fps = 0.0;
            return S_OK;
        }
        e_->signal = true;
        if (!e_->writer) { e_->refusees++; e_->r_sans_writer++; noter_cause(e_, "flux de sortie absent"); return S_OK; }

        void *src = NULL;
        if (f->GetBytes(&src) != S_OK || !src) {
            e_->refusees++; e_->r_sans_octets++;
            noter_cause(e_, "la carte n'a pas rendu le tampon de la trame (GetBytes)");
            return S_OK;
        }

        const int w = (int)f->GetWidth(), h = (int)f->GetHeight();

        // ⚠⚠ L'ORDRE DE CE QUI SUIT EST LE BUG QUI A TENU L'ENTRELACÉ EN ÉCHEC (2026-10-02).
        //    On ouvrait ICI un grain à l'index CADENCE-TRAME, avant même de regarder si la source
        //    est entrelacée. Or pour un flux entrelacé libmxl double la cadence en interne : son
        //    espace d'index vit à la cadence CHAMP. Un index calculé à 25 Hz vaut donc la MOITIÉ
        //    de ce qu'il attend, tombe hors fenêtre, et `OpenGrain` refuse — systématiquement.
        //    La fonction repartait alors par `return`, si bien que le chemin par CHAMP, juste en
        //    dessous, n'était JAMAIS atteint : écrit en septembre, compilé depuis, jamais exécuté.
        //    Mesuré à la première vraie source 1080i50 : 7250 trames refusées, 0 écrite.
        //
        //    Au passage, ce grain ouvert et jamais commité fuyait à chaque trame entrelacée.
        //
        //    L'entrelacé passe donc AVANT, et ne partage plus rien avec le chemin progressif.
        // ★ UN SEUL calcul d'index par trame reçue, AVANT le partage progressif/entrelacé, et
        //   partagé par la vidéo et l'ANC. Il est placé ici et pas plus haut pour que les refus
        //   francs (pas de flux, pas de tampon) n'avancent pas la numérotation.
        uint64_t idx = 0;
        if (!index_trame(&idx)) return S_OK;     // trame jetée : en avance sur la grille

        if (interlace_) {
            ecrire_champs(f, w, h, idx);
            return S_OK;
        }
        mxlGrainInfo gi; uint8_t *payload = NULL;
        if (mxlFlowWriterOpenGrain(e_->writer, idx, &gi, &payload) != MXL_STATUS_OK) {
            e_->refusees++; e_->r_open_grain++;
            noter_cause(e_, "l'anneau MXL a refusé d'ouvrir le grain");
            return S_OK;
        }

        // ── ENTRELACÉ : un grain = UN CHAMP ────────────────────────────────────────────────
        // La carte livre une TRAME entière, lignes des deux champs entrelacées. libmxl attend un
        // grain par CHAMP, à la cadence CHAMP (il double le grain_rate en interne). On écrit donc
        // DEUX grains par trame, en sautant une ligne sur deux à la lecture.
        //
        // ★ Parité : index PAIR = premier champ. En tff le premier champ est le TOP, donc la
        //   ligne 0 ; en bff c'est la ligne 1. Se tromper de parité produit une image qui a
        //   l'air correcte sur un arrêt sur image et saccade en mouvement — le pire des défauts,
        //   parce qu'il passe la relecture d'un opérateur pressé.
        // ★★ GARDE-FOU DE DÉBORDEMENT — l'omettre a coûté cher. En ENTRELACÉ, libmxl dimensionne
        //    chaque grain à UN CHAMP (demi-hauteur) ; écrire une trame entière déborde du grain
        //    et écrase la mémoire voisine de l'anneau, en-têtes de grains compris. Le symptôme
        //    n'apparaît qu'à l'exécution SUIVANTE : le flux ne peut plus être rouvert
        //    (« Unsupported grain version »), et rien ne désigne le vrai coupable.
        //    On REFUSE d'écrire plutôt que de tronquer : une demi-image publiée comme valide
        //    serait pire que pas d'image. (mtl_rx.c a ce garde-fou depuis toujours ; je ne
        //    l'avais pas repris.)
        const size_t besoin = ((size_t)w * h + (size_t)(w / 2) * h * 2u) * octets_pub();
        if (gi.grainSize && (size_t)gi.grainSize < besoin) {
            e_->refusees++; e_->r_grain_court++;
            if (!e_->diag[0])
                snprintf(e_->diag, sizeof e_->diag,
                         "grain de %u octets pour %zu nécessaires (%dx%d planar 10 bits) — "
                         "flux ENTRELACÉ ? le grain est alors un CHAMP, et l'écriture par champ "
                         "n'est pas encore implémentée. Rien n'est écrit.",
                         (unsigned)gi.grainSize, besoin, w, h);
            return S_OK;
        }

        // Planar : Y pleine largeur, Cb/Cr demi-largeur. Le nombre d'octets par échantillon
        // dépend de la profondeur PUBLIÉE — `verser` en tient compte, seul.
        const size_t rb = (size_t)f->GetRowBytes();
        const int total = (int)gi.totalSlices;
        const int haut = total > 0 ? h / total : 0;

        // ── PUBLICATION PROGRESSIVE ────────────────────────────────────────────────────────
        // Contrat du dépôt : k tranches valides ⇔ lignes [0, k·slice_height) écrites SUR LES
        // TROIS PLANS. On convertit bande par bande et on committe à chaque fois. La conversion
        // coûte le même temps : elle est simplement PARTAGÉE au fil de l'eau au lieu d'être
        // gardée pour soi jusqu'au dernier pixel.
        if (total > 1 && haut > 0 && haut * total == h) {
            for (int k = 1; k <= total; k++) {
                const int l0 = (k - 1) * haut;
                verser((const uint8_t *)src + (size_t)l0 * rb, rb,
                       (uint8_t *)payload, w, h, l0, haut);
                gi.validSlices = (uint16_t)k;
                mxlFlowWriterCommitGrain(e_->writer, &gi);
            }
            e_->tranchees++;
            e_->ecrites++;
            e_->t_derniere_trame = maintenant_ns();
            if (e_->anc_writer) ecrire_anc(f, idx);
            const double now3 = horloge_s();
            if (e_->t_fenetre == 0.0) e_->t_fenetre = now3;
            e_->ecrites_fenetre++;
            if (now3 - e_->t_fenetre >= 1.0) {
                e_->fps = e_->ecrites_fenetre / (now3 - e_->t_fenetre);
                e_->ecrites_fenetre = 0; e_->t_fenetre = now3;
            }
            return S_OK;
        }

        verser((const uint8_t *)src, rb, (uint8_t *)payload, w, h, 0, h);
        // ★ SANS CETTE LIGNE le grain est écrit et reste INCOMPLET à jamais : un grain n'est
        //   lisible que lorsque validSlices == totalSlices. Rien ne le signale — le producteur
        //   compte ses grains, le consommateur n'en voit aucun, et aucune erreur n'est levée
        //   d'un bout à l'autre. Constaté le 2026-09-08 : 380 grains « écrits », 0 lu.
        gi.validSlices = gi.totalSlices;
        mxlFlowWriterCommitGrain(e_->writer, &gi);
        e_->ecrites++;
        e_->t_derniere_trame = maintenant_ns();
        e_->entieres++;

        if (e_->anc_writer) ecrire_anc(f, idx);

        // Fenêtre glissante d'une seconde.
        const double now = horloge_s();
        if (e_->t_fenetre == 0.0) e_->t_fenetre = now;
        e_->ecrites_fenetre++;
        if (now - e_->t_fenetre >= 1.0) {
            e_->fps = e_->ecrites_fenetre / (now - e_->t_fenetre);
            e_->ecrites_fenetre = 0;
            e_->t_fenetre = now;
        }
        return S_OK;
    }

private:
    // ── INDEX VIDÉO : CONTIGU, et ré-ancré seulement s'il DÉRIVE ──────────────────────────
    // ⚠⚠ IL ÉTAIT DÉRIVÉ DE L'HORLOGE À CHAQUE TRAME, et une source dont la phase tombe sur la
    //    frontière d'un créneau y perdait un tiers de ses images. Mesuré sur la Z8 le 2026-10-28
    //    (#231) : une caméra arrivant en médiane à 19,99 ms dans un créneau de 20 tournait à
    //    34,2 fps avec +334 refus d'`OpenGrain` en 30 s, quand les quatre autres entrées de la
    //    MÊME carte (arrivées de 1,3 à 5,6 ms) tenaient 50 fps sans un seul refus.
    //    Le mécanisme : deux callbacks consécutifs, avec ±1 ms de gigue autour de la frontière,
    //    calculent le MÊME créneau ; le second `OpenGrain` est refusé, la trame est perdue, et
    //    le créneau suivant reste vide. Sans genlock, chaque source dérive (~1,5 ms/h mesuré sur
    //    la Z8) : TOUTES y passent, chacune son tour, et le compteur de refus en était la seule
    //    trace — le fps affiché, lui, restait « juste » pour les autres entrées.
    //
    // ★ MÊME CORRECTIF QUE L'AUDIO, pour la même raison de fond : la cadence d'une source se
    //   COMPTE, elle ne se devine pas à l'horloge à chaque trame. On numérote à la suite, et on
    //   ne se ré-ancre que sur un écart FRANC. L'hystérésis d'un créneau entier est exactement
    //   ce qui empêche d'osciller à la frontière, et un index strictement croissant ne peut
    //   plus se répéter — donc ce refus-là ne peut plus revenir.
    // ★ LES DEUX SENS NE SONT PAS SYMÉTRIQUES, et les confondre masquerait le défaut suivant :
    //    · EN RETARD de 2 créneaux ou plus (callbacks manqués, source repartie) : on saute à
    //      l'horloge. Le trou reste un trou — on ne fabrique aucune trame — et il est compté ;
    //    · EN AVANCE de 2 créneaux ou plus : la carte produit plus de trames que la grille n'a
    //      de créneaux (source libre, un peu plus rapide que le nœud). RECULER l'index serait
    //      écrire derrière ce qui est déjà publié, ce qui a coûté 60 % de l'audio le 02/10 : on
    //      JETTE cette trame-là, délibérément et comptée, et l'excédent est consommé. À 1,5 ms/h
    //      de dérive, cela fait une trame toutes quelques heures.
    // ★ L'index vit sur la grille TRAME. L'entrelacé en dérive ses deux champs (2·idx, 2·idx+1)
    //   — ce qui est identique au `mxlGetCurrentIndex(cadence champ) & ~1` d'avant, puisque
    //   floor(2t) & ~1 == 2·floor(t) — et l'ANC l'utilise tel quel. Les trois ne peuvent plus
    //   diverger, parce qu'il n'y a plus qu'un seul calcul pour les trois.
    //
    // Rend false quand la trame doit être JETÉE (et l'a déjà comptée).
    bool index_trame(uint64_t *out) {
        const uint64_t horloge = mxlGetCurrentIndex(&e_->rate);
        const double trame_ms = e_->rate.numerator
            ? 1000.0 * e_->rate.denominator / (double)e_->rate.numerator : 0.0;
        // ★★ LA PHASE DE CETTE TRAME DANS SON CRÉNEAU, la grandeur qui décidait le 28/10, et
        //    elle se mesure AVANT L'AMORCE : la première trame a une phase comme les autres, et
        //    laisser le témoin à 0 jusqu'à la deuxième le ferait lire comme « pile sur la
        //    frontière » pendant une trame. Détail, mais c'est exactement ce genre de valeur
        //    d'initialisation qui fait conclure à un danger inexistant.
        //    L'origine est le CENTRE (cf. `v_phase_ms` : convention de `mxlIndexToTimestamp`
        //    MESURÉE, pas devinée), donc `v_bord_ms` ramène le risque à une seule distance.
        const uint64_t maintenant = mxlGetTime();
        const uint64_t centre = mxlIndexToTimestamp(&e_->rate, horloge);
        e_->v_phase_ms = ((double)(int64_t)(maintenant - centre)) / 1e6;
        const double absphase = e_->v_phase_ms < 0.0 ? -e_->v_phase_ms : e_->v_phase_ms;
        e_->v_bord_ms = trame_ms * 0.5 - absphase;
        if (e_->v_bord_ms < 0.0) e_->v_bord_ms = 0.0;
        if (e_->v_bord_ms < e_->v_bord_min_ms) e_->v_bord_min_ms = e_->v_bord_ms;
        if (!e_->v_amorce) {                     // première trame : on se cale sur l'horloge
            e_->v_amorce = true;
            e_->v_dernier = horloge;
            *out = horloge;
            return true;
        }
        const uint64_t suivant = e_->v_dernier + 1;
        // ★ PUBLIÉS AVANT TOUTE DÉCISION : c'est l'écart SUBI qu'on veut lire, pas ce qu'il en
        //   reste après correction. En SIGNÉ — deux `uint64_t` soustraits à l'envers donnent
        //   1,8e19 et un graphique illisible.
        e_->v_grille_trames = (long long)(int64_t)(suivant - horloge);
        if (horloge > suivant + 1) {
            e_->v_resync_avant++;
            e_->v_dernier = horloge;
        } else if (suivant > horloge + 1) {
            e_->v_recal_arriere++;
            e_->refusees++; e_->r_avance_grille++;
            noter_cause(e_, "trame en avance de plus d'un creneau sur la grille MXL : JETEE pour "
                            "reprendre la phase (source non verrouillee, plus rapide que le noeud)");
            return false;
        } else {
            e_->v_dernier = suivant;             // régime normal : à la suite, sans gigue
        }
        *out = e_->v_dernier;
        return true;
    }

    void ecrire_anc(IDeckLinkVideoInputFrame *f, uint64_t idx) {
        // ★ L'index ANC vit sur la grille TRAME — et non sur celle du flux vidéo, qui est à la
        //   cadence CHAMP quand la source est entrelacée. Recevoir l'index du VIDÉO décalait
        //   l'ANC d'un facteur deux sans que rien ne le signale ; il reçoit donc l'index de
        //   TRAME, celui de `index_trame`, que l'entrelacé double de son côté. Le recalculer ici
        //   sur l'horloge, ce qu'il faisait jusqu'au 28/10, lui faisait perdre exactement les
        //   mêmes trames que la vidéo à la frontière d'un créneau — et en silence, lui.
        IDeckLinkVideoFrameAncillaryPackets *ap = NULL;
        if (f->QueryInterface(IID_IDeckLinkVideoFrameAncillaryPackets, (void **)&ap) != S_OK || !ap)
            return;
        IDeckLinkAncillaryPacketIterator *it = NULL;
        std::vector<bobi_anc_packet> pk;
        if (ap->GetPacketIterator(&it) == S_OK && it) {
            IDeckLinkAncillaryPacket *p = NULL;
            while (it->Next(&p) == S_OK && pk.size() < 32) {
                const void *data = NULL; uint32_t taille = 0;
                // ★ Format UInt8 : les UDW sont sur 8 bits de DONNÉES. Les bits 8 et 9 du mot
                //   SMPTE 291 sont la parité et son inverse, que l'empaqueteur RECALCULE — ce
                //   n'est pas une troncature, c'est la définition du mot.
                if (p->GetBytes(bmdAncillaryPacketFormatUInt8, &data, &taille) == S_OK && data) {
                    bobi_anc_packet e;
                    memset(&e, 0, sizeof e);
                    e.did  = p->GetDID();
                    e.sdid = p->GetSDID();
                    e.line = (uint16_t)(p->GetLineNumber() & 0x7FF);
                    e.hori = 0xFFF;                       // indéfini : la carte ne le donne pas
                    e.s    = p->GetDataStreamIndex() ? 1 : 0;
                    e.stream_num = p->GetDataStreamIndex();
                    if (taille > 255) { e_->anc_trop_gros++; }   // compté, PAS tronqué en silence
                    else {
                        memcpy(e.udw, data, taille);
                        e.udw_len = (uint16_t)taille;
                        pk.push_back(e);
                    }
                }
                p->Release();
                p = NULL;
            }
            it->Release();
        }
        ap->Release();

        // ★ TIMECODE : sur cette carte, le pilote CONSOMME l'ATC et ne l'expose pas en paquet —
        //   mesuré le 2026-09-08, les trois portes essayées sur la même trame (itérateur,
        //   recherche par id, API dédiée) ne disent pas la même chose. On le lit donc par l'API
        //   dédiée et on SYNTHÉTISE le paquet, pour que le flux ANC porte la même chose quelle
        //   que soit la carte.
        //   On ne l'ajoute que si l'itérateur n'en a pas déjà livré un : sur une carte qui les
        //   expose, ajouter le nôtre le DOUBLERAIT, et un consommateur lirait deux timecodes.
        bool a_deja_atc = false;
        for (size_t i = 0; i < pk.size(); i++)
            if (pk[i].did == 0x60 && pk[i].sdid == 0x60) { a_deja_atc = true; break; }
        if (!a_deja_atc && pk.size() < 32) {
            IDeckLinkTimecode *tc = NULL;
            if (f->GetTimecode(bmdTimecodeRP188Any, &tc) == S_OK && tc) {
                uint8_t hh = 0, mm = 0, ss = 0, ff = 0;
                if (tc->GetComponents(&hh, &mm, &ss, &ff) == S_OK) {
                    bobi_anc_packet a;
                    memset(&a, 0, sizeof a);
                    a.did = 0x60; a.sdid = 0x60;
                    a.line = 9;                       // ligne conventionnelle de l'ATC VITC1
                    a.hori = 0xFFF;
                    a.udw_len = 16;
                    const bool df = (tc->GetFlags() & bmdTimecodeIsDropFrame) != 0;
                    bobi_atc_encode(hh, mm, ss, ff, df ? 1 : 0, a.udw);
                    pk.push_back(a);
                    e_->anc_tc_synth++;
                    snprintf(e_->tc, sizeof e_->tc, "%02u:%02u:%02u%c%02u",
                             hh, mm, ss, df ? ';' : ':', ff);
                }
                tc->Release();
            }
        }

        mxlGrainInfo gi; uint8_t *payload = NULL;
        if (mxlFlowWriterOpenGrain(e_->anc_writer, idx, &gi, &payload) != MXL_STATUS_OK) {
            e_->anc_open_echec++;                // plus d'abandon muet
            return;
        }
        const size_t n = bobi_anc_pack(pk.empty() ? NULL : &pk[0], pk.size(), 0,
                                       payload, gi.grainSize);
        if (!n) { e_->anc_trop_gros++; return; }          // grain trop petit : on n'écrit PAS un demi-paquet
        if (n < gi.grainSize) memset(payload + n, 0, gi.grainSize - n);
        gi.validSlices = gi.totalSlices;
        mxlFlowWriterCommitGrain(e_->anc_writer, &gi);
        e_->anc_grains++;
        e_->anc_paquets += (long)pk.size();
    }

    /// Écrit `canaux` canaux du paquet entrelacé dans UN flux, en partant du canal `premier`
    /// de la carte. Rend false si l'anneau a refusé la plage (l'appelant décide quoi en faire).
    bool ecrire_groupe(uint64_t idx, long n, const int32_t *ech,
                       int premier, int canaux, mxlFlowWriter w) {
        mxlMutableWrappedMultiBufferSlice slc;
        memset(&slc, 0, sizeof slc);
        if (mxlFlowWriterOpenSamples(w, idx, (size_t)n, &slc) != MXL_STATUS_OK)
            return false;
        if (e_->a_tranches < 0) e_->a_tranches = (int)slc.count;
        const size_t nc = slc.count < (size_t)canaux ? slc.count : (size_t)canaux;
        for (size_t c = 0; c < nc; c++) {
            size_t pos = 0;
            for (int fr = 0; fr < 2; fr++) {
                const size_t octets = slc.base.fragments[fr].size;
                if (!octets) continue;
                float *dst = (float *)((uint8_t *)slc.base.fragments[fr].pointer + c * slc.stride);
                const size_t cnt = octets / 4;
                for (size_t k = 0; k < cnt; k++)
                    dst[k] = (float)ech[(pos + k) * (size_t)e_->canaux
                                        + (size_t)premier + c] / 2147483648.0f;
                e_->a_flottants += (long)cnt;
                pos += cnt;
            }
        }
        mxlFlowWriterCommitSamples(w);
        return true;
    }

    void ecrire_audio(IDeckLinkAudioInputPacket *a) {
        void *src = NULL;
        e_->a_paquets++;
        const long n = (long)a->GetSampleFrameCount();
        if (n <= 0 || a->GetBytes(&src) != S_OK || !src) { e_->a_vides++; return; }

        // ── INDEX AUDIO : CONTIGU, et ré-ancré seulement s'il DÉRIVE ──────────────────────
        // ⚠ IL ÉTAIT DÉRIVÉ DE L'HORLOGE À CHAQUE PAQUET, et ça perdait 60 % de l'audio.
        //   Mesuré le 2026-10-02 sur une vraie source : 25 paquets/s de 1920 échantillons — soit
        //   exactement les 48 000/s attendus — dont seulement 202 sur 501 écrits. `OpenSamples`
        //   refusait 15 fois par seconde et l'index repassait 6 fois par seconde DERRIÈRE ce
        //   qu'on venait d'écrire. La raison : les paquets arrivent par rafales calées sur la
        //   vidéo, alors que `mxlGetCurrentIndex` suit une horloge continue — la gigue entre les
        //   deux faisait sortir l'index de la fenêtre inscriptible de l'anneau.
        //
        //   L'audio est un flux CONTINU : son index se compte, il ne se devine pas à l'horloge.
        //   On écrit donc à la suite (`a_end`) — mais PAS indéfiniment.
        //
        // ⚠⚠ ET C'EST LA TOLÉRANCE D'UN QUART DE SECONDE QUI ÉTAIT LE DÉFAUT (28/10, Z8). Écrire
        //    « à la suite » fait avancer l'index à l'horloge de la CARTE, pendant que la grille
        //    MXL avance à celle du NŒUD — et ce ne sont pas les mêmes. L'écart s'ACCUMULE donc,
        //    en silence, jusqu'à 250 ms ; or un consommateur qui lit l'index d'il y a 10 ms ne
        //    trouve plus rien bien avant. C'est exactement ce qu'a vu la console #214 : zéro
        //    lecture réussie sur SDI 1 et SDI 4, avec un retard de ~200 ms qui grandissait.
        //    Aucune perte de paquet n'était en cause — `open_fail`, `empty` et `silence` sont
        //    restés à zéro sur la fenêtre mesurée le 28/10 sur dell-1 : c'est l'accumulation,
        //    pas un trou.
        //
        // ★ LA TOLÉRANCE DEVIENT ASYMÉTRIQUE, et c'est tout le correctif :
        //    · EN RETARD (l'horloge a pris de l'avance sur ce qu'on a écrit) : on se recale VERS
        //      L'AVANT dès un quart de paquet, le trou étant comblé par du silence. Un saut avant
        //      est MONOTONE, donc sûr : il n'écrit jamais derrière ce qui est déjà écrit et ne
        //      sort pas de la fenêtre inscriptible. Le retard est borné à ~5 ms au lieu de 250,
        //      et il ne s'accumule plus JAMAIS ;
        //    · EN AVANCE : on ne recule PAS. C'est le recul qui avait coûté 60 % de l'audio le
        //      2026-10-02 — l'index repassait 6 fois par seconde derrière ce qu'on venait
        //      d'écrire. On garde la contiguïté, l'horloge rattrape d'elle-même au paquet
        //      suivant, et seul un écart d'un quart de seconde reste traité comme un vrai
        //      décrochage. Les deux cas sont comptés SÉPARÉMENT : ils ne disent pas la même
        //      chose, et les confondre sous un seul « reancrages » a masqué ce défaut.
        const uint64_t horloge = mxlGetCurrentIndex(&e_->srate) - (uint64_t)n;
        const double ech_s = e_->srate.numerator / (double)e_->srate.denominator;
        const uint64_t marge_arriere = (uint64_t)(0.25 * ech_s);
        const uint64_t marge_avance = (n / 4) ? (uint64_t)(n / 4) : 1u;
        uint64_t idx;
        if (!e_->a_amorce) {
            idx = horloge;                       // premier paquet : on se cale sur l'horloge
        } else {
            // ★ PUBLIÉ AVANT TOUTE DÉCISION : c'est l'écart SUBI qu'on veut lire, pas ce qu'il
            //   en reste après correction. Calculé en SIGNÉ — deux `uint64_t` soustraits à
            //   l'envers donnent 1,8e19 et un graphique illisible.
            e_->a_ecart_ms = ((double)(int64_t)(e_->a_end - horloge)) * 1000.0 / ech_s;
            if (horloge > e_->a_end + marge_avance) {
                idx = horloge;                   // on était en RETARD : on avance, silence comblé
                e_->a_resync_avant++;
            } else if (e_->a_end > horloge + marge_arriere) {
                idx = horloge;                   // décrochage franc, beaucoup trop en avance
                e_->a_recal_arriere++;
            } else {
                idx = e_->a_end;                 // régime normal : à la suite, sans gigue
            }
        }
        combler_silence(e_, idx);

        mxlMutableWrappedMultiBufferSlice slc;
        memset(&slc, 0, sizeof slc);
        const int32_t *ech = (const int32_t *)src;      // entrelacé, 32 bits signés
        for (long k = 0; k < n * e_->canaux; k++) {
            const int32_t v = ech[k] < 0 ? -ech[k] : ech[k];
            if (v > e_->a_crete_brute) e_->a_crete_brute = v;
        }
        // ⚠ LE PREMIER GROUPE DÉCIDE. Si son ouverture est refusée, on n'écrit pas le second :
        //   deux flux dont un seul a reçu ce paquet, c'est pire qu'un trou dans les deux — les
        //   canaux 9-16 avanceraient d'un paquet sur les canaux 1-8, pour toujours.
        if (!ecrire_groupe(idx, n, ech, 0, e_->canaux_g1, e_->a_writer)) {
            e_->a_open_echec++;      // ces échantillons sont perdus, et ça se SAIT maintenant
            return;
        }
        if (e_->a_writer2)
            ecrire_groupe(idx, n, ech, 8, e_->canaux_g2, e_->a_writer2);
        e_->a_end = idx + (uint64_t)n;
        e_->a_amorce = true;
        e_->a_echant += n;
    }

    void ouvrir_writer(void) {
        if (e_->writer) { mxlReleaseFlowWriter(e_->mxl, e_->writer); e_->writer = NULL; }
        // Le flux ANC suit le même cycle : sa cadence est celle du format qu'on vient de
        // détecter, donc il se (re)crée ici et pas avant.
        if (!e_->anc_nom.empty()) {
            if (e_->anc_writer) { mxlReleaseFlowWriter(e_->mxl, e_->anc_writer); e_->anc_writer = NULL; }
            const std::string adef = build_anc_flowdef(e_->anc_nom.c_str(), e_->rate);
            bool cree_anc = false;
            if (mxlCreateFlowWriter(e_->mxl, adef.c_str(), NULL, &e_->anc_writer, NULL, &cree_anc)
                    != MXL_STATUS_OK) {
                fprintf(stderr, "flux ANC %s : création impossible — capture SANS ANC\n",
                        e_->anc_nom.c_str());
                e_->anc_writer = NULL;
            } else {
                fprintf(stderr, "flux ANC %s : RFC 8331, cadence %lld/%lld\n", e_->anc_nom.c_str(),
                        (long long)e_->rate.numerator, (long long)e_->rate.denominator);
            }
        }
        const std::string def = build_flowdef(e_->nom.c_str(), e_->w, e_->h, e_->rate, interlace_, tff_);
        bool cree = false;
        // ★ REPRISE, sans ramasse-miettes. Un flux PÉRIMÉ de définition différente peut squatter
        //   l'identifiant — typiquement le nôtre, laissé par une instance tuée sans être libérée
        //   (SIGKILL), ou par un format d'entrée qui vient de changer. libmxl rend alors
        //   « Unsupported grain version », qui ne dit rien de la vraie cause.
        //
        // ⚠ `mtl_rx` appelle `mxlGarbageCollectFlows` dans sa boucle équivalente. On s'en abstient
        //   DÉLIBÉRÉMENT : ce ramasse-miettes est GLOBAL au domaine, et il a déjà détruit quatre
        //   flux VIVANTS appartenant à un autre process (moteur 2110, 2026-08-19), laissant tous
        //   leurs lecteurs à 0 i/s sur une génération morte. On préfère échouer en le DISANT
        //   plutôt que réparer notre démarrage en cassant le voisin.
        mxlStatus st = mxlCreateFlowWriter(e_->mxl, def.c_str(), NULL, &e_->writer, NULL, &cree);
        for (int essai = 0; st != MXL_STATUS_OK && essai < 5; essai++) {
            usleep(300000);        // laisse le propriétaire précédent se libérer, s'il le peut
            st = mxlCreateFlowWriter(e_->mxl, def.c_str(), NULL, &e_->writer, NULL, &cree);
        }
        if (st != MXL_STATUS_OK) {
            char id[37]; flow_id_str(e_->nom.c_str(), id);
            snprintf(e_->diag, sizeof e_->diag,
                     "flux « %s » (%s) : création refusée après 5 essais — un flux périmé de "
                     "définition différente squatte cet identifiant (instance tuée sans "
                     "libération, ou format d'entrée changé). Le supprimer dans le domaine MXL.",
                     e_->nom.c_str(), id);
            fprintf(stderr, "%s\n", e_->diag);
            e_->writer = NULL;
        } else {
            e_->diag[0] = 0;
            fprintf(stderr, "flux MXL %s : %dx%d @ %lld/%lld%s\n", e_->nom.c_str(), e_->w, e_->h,
                    (long long)e_->rate.numerator, (long long)e_->rate.denominator,
                    interlace_ ? " entrelacé" : "");
        }
    }
    IDeckLinkInput *in_;
    Etat *e_;
    BMDDisplayMode courant_ = (BMDDisplayMode)0;
    bool interlace_ = false, tff_ = true;
    ULONG refs_ = 1;
};

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <persistent_id> <nom_de_flux> [secondes]\n", argv[0]);
        return 1;
    }
    const int64_t vise = strtoll(argv[1], NULL, 0);
    const char *nom = argv[2];
    int secondes = 10;
    const char *stats = NULL;
    int canaux = 0;
    bool anc = false;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--stats") && i + 1 < argc) { stats = argv[++i]; continue; }
        if (!strcmp(argv[i], "--audio") && i + 1 < argc) { canaux = atoi(argv[++i]); continue; }
        if (!strcmp(argv[i], "--anc")) { anc = true; continue; }
        // Profondeur PUBLIÉE sur le bus. Elle vient de l'orchestrateur (`mxl_pipeline_bit_depth`),
        // pas de la carte : celle-ci livre toujours du v210. Valeur absente ou aberrante → 10,
        // le comportement historique, plutôt qu'un flux écrit dans une profondeur inventée.
        if (!strcmp(argv[i], "--bits") && i + 1 < argc) {
            const int b = atoi(argv[++i]);
            g_bits_pub = (b == 8 || b == 10) ? b : 10;
            continue;
        }
        secondes = atoi(argv[i]);
    }
    // Le SDK n'accepte que 2, 8 ou 16 canaux. On REFUSE une valeur hors liste plutôt que de la
    // corriger en douce : un exploitant qui demande 6 canaux doit savoir qu'il n'en aura pas 6.
    if (canaux && canaux != 2 && canaux != 8 && canaux != 16) {
        fprintf(stderr, "canaux audio invalides (%d) — le SDK n'accepte que 2, 8 ou 16\n", canaux);
        return 1;
    }
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    if (charger_v210() != 0) return 6;

    IDeckLinkIterator *it = CreateDeckLinkIteratorInstance();
    if (!it) { fprintf(stderr, "SDK indisponible\n"); return 2; }

    // ★ Désignation par PERSISTENT ID. On refuse plutôt que de prendre « le premier qui marche » :
    //   se tromper de connecteur en silence est pire que ne rien faire.
    IDeckLink *dev = NULL, *choisi = NULL;
    while (it->Next(&dev) == S_OK) {
        IDeckLinkProfileAttributes *a = NULL;
        int64_t duplex = 0, io = 0, pid = 0;
        if (dev->QueryInterface(IID_IDeckLinkProfileAttributes, (void **)&a) == S_OK && a) {
            a->GetInt(BMDDeckLinkDuplex, &duplex);
            a->GetInt(BMDDeckLinkVideoIOSupport, &io);
            a->GetInt(BMDDeckLinkPersistentID, &pid);
            a->Release();
        }
        if (!choisi && pid == vise) {
            if (duplex == bmdDuplexInactive) {
                fprintf(stderr, "le sous-périphérique %lld est INACTIF dans le profil courant — "
                                "son connecteur est éteint\n", (long long)vise);
                dev->Release(); it->Release(); return 3;
            }
            if (!(io & bmdDeviceSupportsCapture)) {
                fprintf(stderr, "le sous-périphérique %lld ne sait pas capturer\n", (long long)vise);
                dev->Release(); it->Release(); return 3;
            }
            choisi = dev; continue;
        }
        dev->Release();
    }
    it->Release();
    if (!choisi) { fprintf(stderr, "aucun sous-périphérique de persistent id %lld\n", (long long)vise); return 3; }

    Etat e; e.nom = nom;
    e.mxl = mxlCreateInstance(getenv("MXL_DOMAIN") ? getenv("MXL_DOMAIN") : "/dev/shm/mxl", NULL);
    if (!e.mxl) { fprintf(stderr, "mxlCreateInstance a échoué\n"); return 4; }

    IDeckLinkInput *in = NULL;
    if (choisi->QueryInterface(IID_IDeckLinkInput, (void **)&in) != S_OK) {
        fprintf(stderr, "pas d'interface d'entrée\n"); return 3;
    }
    IDeckLinkDisplayModeIterator *mit = NULL; IDeckLinkDisplayMode *m = NULL, *depart = NULL;
    if (in->GetDisplayModeIterator(&mit) == S_OK) {
        while (mit->Next(&m) == S_OK) { if (!depart) { depart = m; continue; } m->Release(); }
        mit->Release();
    }
    if (!depart) { fprintf(stderr, "aucun mode\n"); return 3; }

    // ── Audio embarqué (optionnel) ──
    if (canaux > 0) {
        e.canaux    = canaux;
        e.canaux_g1 = canaux > 8 ? 8 : canaux;
        e.canaux_g2 = canaux > 8 ? canaux - 8 : 0;
        e.a_nom  = std::string(nom) + "_audio";
        if (e.canaux_g2) e.a_nom2 = std::string(nom) + "_audio_2";
        const std::string adef = build_audio_flowdef(e.a_nom.c_str(), e.canaux_g1);
        bool cree = false;
        if (mxlCreateFlowWriter(e.mxl, adef.c_str(), NULL, &e.a_writer, NULL, &cree) != MXL_STATUS_OK) {
            fprintf(stderr, "flux audio %s : création impossible — capture vidéo SEULE\n",
                    e.a_nom.c_str());
            e.a_writer = NULL;
            e.canaux = e.canaux_g1 = e.canaux_g2 = 0;   // ★ sinon les stats annoncent des canaux
        } else if (in->EnableAudioInput(bmdAudioSampleRate48kHz,
                                        bmdAudioSampleType32bitInteger,
                                        (uint32_t)canaux) != S_OK) {
            fprintf(stderr, "EnableAudioInput a échoué — capture vidéo SEULE\n");
            mxlReleaseFlowWriter(e.mxl, e.a_writer);
            e.a_writer = NULL; e.canaux = e.canaux_g1 = e.canaux_g2 = 0;
        } else {
            fprintf(stderr, "flux audio %s : %d canaux, 48 kHz, float32\n",
                    e.a_nom.c_str(), e.canaux_g1);
            // ⚠ LE SECOND GROUPE NE DOIT PAS POUVOIR TUER LE PREMIER. S'il ne se crée pas, on
            //   capture les canaux 1-8 et on le DIT : perdre la moitié des canaux est moins
            //   grave que de perdre tout l'audio, et un silence non nommé est le pire des trois.
            if (e.canaux_g2) {
                const std::string adef2 = build_audio_flowdef(e.a_nom2.c_str(), e.canaux_g2);
                bool cree2 = false;
                if (mxlCreateFlowWriter(e.mxl, adef2.c_str(), NULL, &e.a_writer2, NULL, &cree2)
                        != MXL_STATUS_OK) {
                    fprintf(stderr, "flux audio %s : création impossible — seuls les canaux "
                                    "1-%d sont publiés\n", e.a_nom2.c_str(), e.canaux_g1);
                    e.a_writer2 = NULL;
                    e.canaux_g2 = 0;
                } else {
                    fprintf(stderr, "flux audio %s : %d canaux (9-%d de la carte), 48 kHz, "
                                    "float32\n", e.a_nom2.c_str(), e.canaux_g2, canaux);
                }
            }
        }
    }

    // ── ANC (optionnel) ── le flux est créé PLUS TARD, avec le writer vidéo : sa cadence dépend
    // du format, qui n'est connu qu'après détection. Le créer ici lui donnait la cadence par
    // défaut (25/1), fausse sur toute source qui n'est pas en 25 images.
    if (anc) e.anc_nom = std::string(nom) + "_anc";

    Capture *cb = new Capture(in, &e);
    in->SetCallback(cb);
    if (in->EnableVideoInput(depart->GetDisplayMode(), bmdFormat10BitYUV,
                             bmdVideoInputEnableFormatDetection) != S_OK ||
        in->StartStreams() != S_OK) {
        fprintf(stderr, "activation de l'entrée impossible\n"); return 4;
    }

    // secondes == 0 → sans limite (mode plugin : c'est l'agent qui arrête le script).
    const double t0 = horloge_s();
    while (!g_stop && (secondes <= 0 || horloge_s() - t0 < secondes)) {
        usleep(200000);
        // Pas de trame depuis plus d'une seconde ⇒ la cadence est NULLE, pas « la dernière
        // connue ». Une valeur figée ferait passer une entrée morte pour vivante.
        if (e.t_fenetre > 0.0 && horloge_s() - e.t_fenetre > 1.5) { e.fps = 0.0; e.signal = false; }
        ecrire_stats(&e, stats);
    }

    in->StopStreams(); in->DisableVideoInput();
    if (e.a_writer) in->DisableAudioInput();
    in->SetCallback(NULL);
    if (e.writer) mxlReleaseFlowWriter(e.mxl, e.writer);
    if (e.a_writer2) mxlReleaseFlowWriter(e.mxl, e.a_writer2);
    if (e.a_writer) mxlReleaseFlowWriter(e.mxl, e.a_writer);
    if (e.anc_writer) mxlReleaseFlowWriter(e.mxl, e.anc_writer);
    mxlDestroyInstance(e.mxl);
    cb->Release(); depart->Release(); in->Release(); choisi->Release();

    printf("\nflux « %s » : %ld grains écrits, %ld trames sans source, %ld refusées, "
           "%ld changement(s) de format\n", nom, e.ecrites, e.sans_source, e.refusees, e.changements);
    if (e.canaux)
        printf("flux « %s » : %ld échantillons sur %d canaux, dont %ld comblés en silence\n"
               "  crête BRUTE (avant conversion) : %d / 2147483647%s\n",
               e.a_nom.c_str(), e.a_echant, e.canaux, e.a_silence, e.a_crete_brute,
               e.a_crete_brute ? "" : "  ← la CARTE ne livre que du silence : la source n'a pas d'audio");
    if (e.anc_writer || e.anc_grains)
        printf("flux « %s » : %ld grains ANC, %ld paquets (dont %ld ATC synthétisés), "
               "%ld écartés — dernier timecode : %s\n",
               e.anc_nom.c_str(), e.anc_grains, e.anc_paquets, e.anc_tc_synth,
               e.anc_trop_gros, e.tc[0] ? e.tc : "aucun");
    if (e.canaux)
        printf("  écrits dans l'anneau : %ld flottants, slc.count=%d (attendu %d)\n",
               e.a_flottants, e.a_tranches, e.canaux);
    return e.ecrites > 0 ? 0 : 5;
}
