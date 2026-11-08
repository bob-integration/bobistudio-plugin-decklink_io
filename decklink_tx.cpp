// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 BOBI SAS, France
// Auteur : Cyril Mazouer, pour le compte de BOBI SAS
// Distribué sous licence GNU GPL v3 (ou ultérieure) ; voir le fichier LICENSE.
//
// SORTIE bus MXL → SDI — chantier `decklink_io` (docs/chantiers/DECKLINK.md), jalon 7.
//
//   ./decklink_tx <persistent_id> <flux_source> <mode> [secondes] [--stats <f>] [--preroll <n>]
//
// Lit un flux vidéo MXL (planar 10 bits) et l'émet sur un connecteur Blackmagic, en v210.
//
// ★ Le TX est INDÉPENDANT du producteur. Il émet à la cadence de la CARTE, pas à celle du flux :
//   si la source décroche, on ré-émet la dernière trame connue plutôt que de laisser la sortie
//   se figer ou se trouer. Une sortie SDI qui s'arrête est un incident d'antenne ; une trame
//   répétée ne l'est pas. Les répétitions sont COMPTÉES et publiées — sans ce compteur, une
//   source morte donnerait une sortie parfaitement cadencée et parfaitement fausse.
//
// ★ Le sous-périphérique est désigné par PERSISTENT ID, jamais par index (un changement de
//   profil renumérote), et son INACTIVITÉ est testée avant sa capacité de lecture.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <dlfcn.h>
#include <string>
#include <vector>

#include "decklink_sdk/DeckLinkAPI.h"
#include <mxl/mxl.h>
#include <mxl/flow.h>
#include <mxl/flowinfo.h>
#include <mxl/time.h>
#include <mxl/rational.h>

#include <openssl/sha.h>

typedef void (*pack10_fn)(const uint16_t *, const uint16_t *, const uint16_t *, uint8_t *,
                          size_t, int, int);
static pack10_fn g_pack10 = NULL;

static int charger_v210(void) {
    static const char *cand[] = {"libbobi_v210_v3.so", "libbobi_v210.so",
                                 "/usr/local/lib/libbobi_v210_v3.so",
                                 "/usr/local/lib/libbobi_v210.so", NULL};
    for (int i = 0; cand[i]; i++) {
        void *h = dlopen(cand[i], RTLD_NOW);
        if (!h) continue;
        g_pack10 = (pack10_fn)dlsym(h, "bobi_v210_pack10");
        if (g_pack10) { fprintf(stderr, "v210 : %s\n", cand[i]); return 0; }
        dlclose(h);
    }
    fprintf(stderr, "ERREUR : aucun convertisseur v210 — on refuse plutôt que d'émettre du bruit\n");
    return -1;
}

static const uint8_t NS_BOBI[16] = {
    0xd4,0xe7,0x7c,0xba,0x0e,0x52,0x55,0xd9,0x82,0xed,0x72,0x26,0xb7,0xbd,0xa7,0x57};

static void flow_id_str(const char *name, char out[37]) {
    SHA_CTX c; unsigned char h[20];
    SHA1_Init(&c); SHA1_Update(&c, NS_BOBI, 16);
    SHA1_Update(&c, name, strlen(name)); SHA1_Final(h, &c);
    h[6] = (unsigned char)((h[6] & 0x0f) | 0x50);
    h[8] = (unsigned char)((h[8] & 0x3f) | 0x80);
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             h[0],h[1],h[2],h[3],h[4],h[5],h[6],h[7],h[8],h[9],h[10],h[11],h[12],h[13],h[14],h[15]);
}

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int) { g_stop = 1; }

static int64_t maintenant_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static double horloge_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/// NOIR LÉGAL en v210 (`bmdFormat10BitYUV`, le format de sortie de ce binaire) : Y=64,
/// Cb=Cr=512 sur 10 bits.
///
/// ⚠ SURTOUT PAS un `memset` à zéro. Zéro est SOUS le niveau du noir : c'est illégal sur SDI,
///   et en aval un moniteur l'affiche autrement qu'un autre, un écrêteur le remonte, un
///   encodeur le garde — on aurait donc « du noir » dont l'apparence dépend de l'équipement,
///   c'est-à-dire un mode de mise au point sur lequel on ne peut rien conclure.
///
/// Le motif v210 couvre 6 pixels sur 4 mots de 32 bits (ordre : u0 y0 v0 | y1 u1 y2 |
/// v1 y3 u2 | y4 v2 y5), donc une alternance de deux mots seulement quand tout est constant.
static void remplir_noir_v210(uint8_t *dst, int w, int h, size_t rowBytes) {
    if (!dst || w <= 0 || h <= 0 || !rowBytes) return;
    const uint32_t N = 64, C = 512;
    const uint32_t motif[4] = { C | (N << 10) | (C << 20), N | (C << 10) | (N << 20),
                                C | (N << 10) | (C << 20), N | (C << 10) | (N << 20) };
    const size_t mots = (((size_t)w + 5) / 6) * 4;
    // La ligne allouée par la carte est plus large que l'image (rowBytes = ((w+47)/48)*128) :
    // on n'écrit que les mots de l'image, jamais au-delà du compte calculé.
    const size_t max_mots = rowBytes / 4;
    const size_t n = mots < max_mots ? mots : max_mots;
    for (int y = 0; y < h; y++) {
        uint32_t *l = (uint32_t *)(dst + (size_t)y * rowBytes);
        for (size_t i = 0; i < n; i++) l[i] = motif[i & 3];
    }
}


// Fenêtre minimale avant de publier une dérive. ★ CALIBRÉE PAR LA MESURE, pas choisie (2026-09-09).
//
// Elle valait 300 s, hérité de la méthode par horodatage. Sur la méthode par cadence, NEUF fenêtres
// de 300 s consécutives ont donné : −1,30 · +1,46 · −0,62 · +1,23 · −0,60 · −0,04 · −0,84 · +0,89 ·
// −0,57 ppm. Moyenne −0,04 ppm, écart-type 0,94, étendue 2,8 ppm — autrement dit du BRUIT autour de
// zéro, sur une chaîne dont la fenêtre longue (7443 s) donne −0,06 ppm. Un seuil d'alerte à 1 ppm
// aurait crié plusieurs fois par heure sans qu'il ne se passe rien.
//
// La cause est la gigue d'ordonnancement du callback : ~300 µs entre l'émission réelle et
// `clock_gettime`. Elle borne la résolution à gigue/fenêtre, soit ±1,0 ppm à 300 s et ±0,17 ppm à
// 1800 s. On publie donc sur 1800 s : trente minutes avant la première valeur, mais une valeur qui
// veut dire quelque chose. `null` d'ici là — un blanc se lit comme un blanc, un chiffre faux non.
static const double FENETRE_DERIVE_MIN_S = 1800.0;

// Déclarée ici parce que la classe de rappel l'appelle et qu'elle est définie plus bas, près du
// reste du chemin audio — la garder avec ses pairs vaut mieux que la remonter seule ici.
struct Etat;
// Déclaré ici parce que l'ouverture d'un flux audio s'en sert, et qu'elle précède sa
// définition : le fichier est ordonné par chemin de données, pas par dépendances.
static int canaux_declares_du_flux(const char *domaine, const char *fid);
static void servir_audio(Etat *e, IDeckLinkOutput *out, BMDTimeValue t,
                         BMDTimeValue duree, BMDTimeScale echelle);

struct Etat {
    mxlInstance   mxl = NULL;
    mxlFlowReader rd  = NULL;
    long  emises = 0, repetees = 0, ratees = 0;
    // ★ `repetees` compte DEPUIS TOUJOURS les ré-émissions ; ce compteur-ci porte le contrat
    //   partagé avec le mélangeur (décision du 30/10) : « images émises qui ne sont pas une
    //   image FRAÎCHE de la source, quelle qu'en soit la raison ». Les deux montent ensemble
    //   aujourd'hui, et c'est voulu : le nom partagé est ce qui se lit à l'échelle du parc, et
    //   `repetees` reste le nom historique que les relevés existants citent.
    long  non_fraiches = 0;
    // 0 = repeat (défaut), 1 = black. Cf. `relire_mode` : le mode ne vient PAS de la ligne de
    // commande, pour qu'en changer ne coupe pas l'antenne.
    int   mode_manquant = 0;
    std::string mode_fichier;
    double t_mode = 0.0;
    long  emises_fenetre = 0;
    double t_fenetre = 0.0, fps = 0.0;
    uint64_t dernier_index = 0;
    bool  source_vivante = false;
    int   w = 0, h = 0;
    std::string flux;

    // ── DÉCROCHAGE DE GÉNÉRATION ─────────────────────────────────────────────────────────
    // ★★ UN PRODUCTEUR REDÉPLOYÉ FIGEAIT LA SORTIE POUR TOUJOURS, EN SILENCE (22/10).
    //    Un producteur qui DÉTRUIT puis RECRÉE son flux sous le même nom (redéploiement,
    //    changement de source) laisse nos lecteurs accrochés à la génération MORTE. Le piège
    //    est qu'elle reste LISIBLE : les grains se servent encore, l'index est simplement figé.
    //    Donc aucun SIGBUS, aucune erreur, aucun code de retour — et une reprise « sur erreur »
    //    ne se déclenche JAMAIS. La sortie rejoue la dernière image en boucle en annonçant
    //    50 i/s et genlock verrouillé.
    //    Mesuré ce jour-là : le player a été redéployé à 07 h 38, SIX de nos neuf sorties sont
    //    restées sur l'anneau d'avant. Le système le disait — 13 zones mémoire « (deleted) »
    //    sur les inodes 155-161 pendant que le flux vivant tournait sur 18145 — mais aucun de
    //    NOS compteurs ne le disait. J'ai d'abord accusé le producteur, à tort.
    // ⚠ LE DÉTECTEUR EST `lastWriteTime`, PAS L'INDEX. Sur une génération morte, l'index est
    //   figé mais il l'est AUSSI chez un producteur légitimement à l'arrêt : l'index seul
    //   confond les deux. L'écart entre l'horloge et la dernière écriture, lui, croît sans
    //   borne dans le premier cas. C'est le détecteur que `_StaleGuard` (bobimxl) utilise déjà
    //   pour les lecteurs Python depuis le 2026-08-22, éprouvé en production : on ne réinvente
    //   pas une parade contre une panne silencieuse, on reprend celle qui a déjà mordu.
    long   regen_video = 0;          // réouvertures du flux VIDÉO après décrochage
    long   regen_audio = 0;          // idem, flux audio (les deux groupes confondus)
    double t_garde = 0.0;            // dernier passage de la garde (1 contrôle/s, pas par trame)
    bool   attente_flux = false;     // on a décroché et le flux n'est pas encore réapparu

    // ⚠⚠ L'AUDIO NE SE JUGE PAS SUR LE MÊME TÉMOIN QUE LA VIDÉO, ET L'IGNORER A COUPÉ LE SON
    //    EN DIRECT (22/10). Les producteurs AUDIO ne mettent pas `lastWriteTime` à jour. Une
    //    garde qui les juge dessus les croit donc morts EN PERMANENCE : elle relâchait et
    //    reprenait le lecteur une fois par seconde, et chaque reprise ré-ancre la position de
    //    lecture au début. Mesuré sur la machine du testeur : 1 300 à 1 700 échantillons/s au
    //    lieu de 48 000, sur les quatre sorties porteuses de son.
    //    L'avertissement était écrit dans `_StaleGuard` (bobimxl), d'où ce mécanisme est repris.
    //    Il a été lu et pas appliqué : c'est une faute d'attention, pas une surprise technique.
    // ★ LE SIGNE CORRECT EST UN `headIndex` QUI NE BOUGE PLUS — et il est fiable parce qu'un
    //   flux réellement MUET a un index qui AVANCE : le producteur comble le silence. Un index
    //   figé n'est donc jamais « du silence », c'est un lecteur décroché ou un producteur mort,
    //   et rouvrir est bon dans les deux cas.
    uint64_t a_tete_vue = 0,  a2_tete_vue = 0;   // dernier index observé
    double   a_tete_t = 0.0,  a2_tete_t = 0.0;   // quand il a bougé pour la dernière fois
    bool     a_tete_amorce = false, a2_tete_amorce = false;

    // ── PROFONDEUR DE LA SOURCE ──────────────────────────────────────────────────────────
    // ★ LE BUS N'EST PAS FORCÉMENT EN 10 BITS, ET CE BINAIRE LE SUPPOSAIT (19/10). Le réglage
    //   d'orchestrateur `mxl_pipeline_bit_depth` vaut « force8 » par défaut : TOUT le produit
    //   écrit donc du planar 8 bits — mire, mur, player. Nous lisions ces octets comme du
    //   10 bits, soit deux fois moins d'échantillons par ligne : image doublée horizontalement
    //   et dominante verte, CONSTATÉE SUR MONITEUR. Et en silence : 50 i/s, genlock verrouillé,
    //   0 retard, 0 rejet — aucun compteur ne bronchait.
    //   Le bouclage carte→carte marchait parce que notre propre capture écrit du 10 bits ; nous
    //   n'avions donc jamais émis autre chose que nos propres images.
    // ⚠ LA PROFONDEUR SE LIT DANS LE FLUX, elle ne se suppose ni ne se déduit de la taille du
    //   grain : un champ ENTRELACÉ 10 bits pèse exactement autant qu'une trame PROGRESSIVE
    //   8 bits (2·w·h octets). Déduire de la taille confondrait les deux.
    int   bits_src = 10;
    std::vector<uint16_t> conv;      // tampon d'élargissement 8 → 10 bits

    // ── AUDIO EMBARQUÉ ───────────────────────────────────────────────────────────────────
    // ★ UNE SORTIE SDI N'ACCEPTAIT QUE DE LA VIDÉO (20/10, signalé en recette). Le port audio
    //   n'existait qu'en ENTRÉE : on pouvait capturer 16 canaux d'une source, pas en émettre un
    //   seul. Ce n'était pas un réglage introuvable, c'était une capacité absente — ce fichier
    //   ne contenait pas une ligne d'audio.
    // ⚠ DEUX CONVERSIONS À LA FRONTIÈRE, et aucune n'est optionnelle : le bus MXL porte du
    //   float32 PAR CANAL (planaire), la carte veut des entiers ENTRELACÉS. Se tromper sur
    //   l'une des deux donne du bruit blanc à pleine échelle dans une régie.
    mxlFlowReader rd_audio = NULL;
    // ★★ UNE SORTIE DÉMARRÉE AVANT L'EXISTENCE DE SON FLUX AUDIO RESTAIT MUETTE POUR TOUJOURS
    //    (21/10). L'ouverture du lecteur audio n'avait lieu QU'UNE FOIS, au démarrage : un
    //    conteneur relancé avant son producteur de son — l'ordre normal après un redémarrage de
    //    nœud — émettait l'image sans jamais reprendre l'audio, en annonçant 50 i/s et genlock
    //    verrouillé. Rien dans les compteurs ne le disait : `audio_samples` restait à 0, ce qui
    //    ressemble à une source muette. C'est la MÊME faute que la profondeur devinée de la
    //    veille (0.67.0) : un état lu une seule fois, au pire moment, et tenu pour définitif.
    //    On garde donc le NOM demandé et on réessaie en cours de route.
    // ★★ 16 CANAUX = DEUX FLUX DE 8 (21/10). Le bus porte des flux de 8 canaux : sur les 28
    //    flux audio du domaine, 25 en portaient 8 et les 3 seuls à en porter 16 étaient nos
    //    propres captures. Une sortie réglée sur 16 devant un flux de 8 laissait donc les
    //    canaux 9-16 muets SANS RIEN DIRE — c'est la panne de Quad SDI 1 du 21/10, que le
    //    testeur a d'abord cherchée dans son câblage.
    mxlFlowReader rd_audio2 = NULL;
    std::string flux_audio2_voulu;   // 2e groupe : canaux 9-16 de la carte
    int      canaux_g1 = 0, canaux_g2 = 0;
    // ★ CE QUE LE BUS DÉCLARE, à comparer à ce qu'on remplit. C'est ce contrôle manquant — et
    //   non le réglage — qui a transformé une erreur de configuration en chasse au câble.
    int      canaux_declares = 0, canaux_declares2 = 0;
    std::string flux_audio_voulu;    // non vide = audio demandé ; à (re)ouvrir tant que NULL
    long     audio_attentes = 0;     // trames passées sans lecteur audio (compte les essais)
    long     audio_attentes2 = 0;    // idem pour le 2e groupe
    long     audio_sautes2 = 0;      // trames où le 2e groupe n'avait pas ses échantillons
    long     audio_ouvertures = 0;   // fois où le flux a été pris EN COURS DE ROUTE
    int      canaux_audio = 0;
    long     ech_audio = 0;          // échantillons servis (compteur d'exploitation)
    long     audio_sautes = 0;       // trames où le bus n'avait pas les échantillons
    long     audio_ecretes = 0;      // échantillons ramenés au plein échelle — cf. plus bas
    long     audio_reancrages = 0;   // fois où la position de lecture a dû être recalée
    uint64_t idx_audio = 0;          // position de lecture CONTINUE (0 = pas encore ancrée)
    uint64_t reste_ech = 0;          // report pour les cadences non entières (29,97)
    std::vector<int32_t> tampon_audio;

    // ── Horloge ──────────────────────────────────────────────────────────────────────────
    // `derive_ppm` = écart entre la CADENCE RÉELLEMENT ÉMISE et la cadence nominale, l'une comptée
    // par la carte, l'autre par l'horloge du nœud. Le rythme d'émission est imposé par l'horloge
    // de sortie de la carte, donc asservie à la référence quand il y en a une : cette grandeur-ci
    // EST genlock-sensible.
    //
    // ⚠ Elle a d'abord été dérivée de l'horodatage de fin de trame, et c'était FAUX : contrôle du
    //   2026-09-08, deux fenêtres de 900 s, sortie asservie (1791/1791) → +1,90 et +1,60 ppm ;
    //   sortie NON asservie (0/1791) → +1,54 ppm. Le même chiffre des deux côtés — cet horodatage
    //   ne suit pas la référence, et j'en avais tiré la conclusion INVERSE de la vérité. La
    //   cadence, elle, donne +0,035 ppm sur 1577 s et +0,094 ppm sur 1830 s.
    //
    // La paire (compte, instant) est prise DANS le callback, au même endroit du code : datée
    // ailleurs, elle dérape de 20 ms, soit 67 ppm sur une fenêtre de 300 s.
    long    ancre_emises = 0;                 // premier échantillon de la fenêtre courante
    int64_t ancre_t_ns = 0;
    double  derive_ppm = 0.0;
    double  derive_fenetre_s = 0.0;           // 0 = pas encore de mesure publiable
    long    n_genlock = 0, n_releves = 0;     // asservissement observé, sur relevés

    // ── Glissement NET ────────────────────────────────────────────────────────────────────
    // Le glissement, c'est l'écart entre ce que la SOURCE a produit (avance de son index de
    // grain) et ce que la sortie a ÉMIS, sur la même fenêtre. Rien d'autre.
    //
    // ⚠ Il a d'abord été déduit du seul compteur de RÉPÉTITIONS, et c'était faux d'un facteur 240 :
    //   mesuré le 2026-09-09 sur 2101 s, 5 répétitions pour 105 052 trames émises. Lues comme du
    //   glissement, elles annoncent 47,6 ppm ; la cadence mesurée sur la même fenêtre vaut
    //   +0,20 ppm. Une répétition est presque toujours de la GIGUE DE PHASE — la source n'avait
    //   rien de neuf à cet instant précis, et elle livrera deux grains au suivant. Répétitions et
    //   sauts se compensent ; seul leur SOLDE est du glissement. Un seuil posé sur les répétitions
    //   aurait crié « un glissement toutes les 7 minutes » sur une chaîne qui en connaît un toutes
    //   les 140 heures.
    long     rep_ancre = 0;
    uint64_t ancre_index = 0;      // index de grain SOURCE au début de la fenêtre
    double   rep_ancre_t = 0.0;
    double slip_h = 0.0;                      // heures entre deux glissements ; 0 = pas mesurable
    double slip_fenetre_s = 0.0;
    int64_t t_derniere_trame = 0;             // instant de la dernière trame émise (horloge nœud)

    // ── Mode tranche ──────────────────────────────────────────────────────────────────────
    // ★ CES TROIS COMPTEURS SONT LE TÉMOIN. Sans eux, on ne saurait pas si le mode tranche
    //   mord réellement : une sortie qui retombe systématiquement sur le grain complet affiche
    //   exactement la même cadence, le même genlock et le même glissement qu'une sortie qui
    //   suit les bandes — pendant qu'elle coûte une trame de latence à toute la chaîne.
    // ⚠ CES COMPTEURS DOIVENT DISCRIMINER CE QUI COÛTE UNE TRAME. Une première version comptait
    //   « par tranches » et « entier », ce qui mettait dans le MÊME panier « j'ai émis head »
    //   (à l'heure) et « je suis retombé sur head-1 » (une trame de retard) : le chiffre montait
    //   dans les deux cas et ne disait donc rien de la latence, qui est tout l'enjeu.
    long   suivi_tranches = 0;        // `head` suivi bande par bande (producteur tranché)
    long   suivi_entier = 0;          // `head` pris d'un bloc (producteur non tranché) — à l'heure
    long   repli_precedent = 0;       // `head-1` émis : UNE TRAME DE RETARD
    long   tranches_incompletes = 0;  // `head` pas fini avant l'échéance → repli
    // ⚠⚠ CES QUATRE COMPTEURS SONT NÉS D'UNE QUESTION QU'ON N'A PAS PU TRANCHER SANS EUX (28/10).
    //    La Z8 voyait `late_previous` monter à 47/s avec `slice_timeouts` À ZÉRO : la sortie
    //    prenait `head-1` sans que l'attente expire une seule fois. Il y avait TROIS sorties
    //    muettes vers le repli — grain de tête illisible, `totalSlices` à 0, hauteur non
    //    divisible — et aucune ne comptait. Un chemin de repli qui ne dit pas POURQUOI il a
    //    replié oblige à relire le code à chaque fois ; c'est arrivé, et ça a coûté un
    //    aller-retour entre deux sessions.
    long   tete_illisible = 0;        // l'accesseur refuse `head` jusqu'à l'échéance
    long   tete_sans_tranches = 0;    // `head` lisible mais `totalSlices == 0`
    long   geometrie_refusee = 0;     // hauteur non divisible par le nombre de tranches
    // ★ LA PREUVE QUE LE MODE TRANCHE MORD VRAIMENT : nombre de trames où on a attendu AU MOINS
    //   une bande de plus après en avoir déjà empaqueté une. Sans lui, `sliced` ne distingue pas
    //   « j'ai suivi le producteur bande par bande » de « le grain était déjà fini quand j'ai
    //   regardé » — et c'est exactement la confusion qui a fait croire le mode tranche actif.
    long   tranches_attendues = 0;
    // ── Attente du HEAD (répétitions évitées) — cf. le pavé dans le callback ───────────
    long   tete_attendue = 0;         // `head` a avancé dans le budget : répétition ÉVITÉE
    long   tete_immobile = 0;         // échéance expirée, `head` immobile : le producteur est en retard
    // ★★ LA MARGE, et c'est la session Z8 qui l'a réclamée en lisant son propre relevé (30/10,
    //    #231) : sur la sortie PGM, `head_awaited` montait de 2994 en 60 s — environ 50 par
    //    seconde, donc UNE ATTENTE À CHAQUE TRAME, et elle réussissait toujours (`repeated` et
    //    `head_frozen` à +0). Deux compteurs d'issues ne disent pas si l'on frôle l'échéance ou
    //    si l'on attend 200 µs : savoir que le correctif travaille ne dit pas de combien il
    //    tient. Ce maximum COURU se compare au budget `slice_wait_ms`, déjà publié à côté — s'il
    //    s'en approche, la prochaine trame sera répétée, et aucun compteur d'issue ne l'aurait
    //    annoncé. Pris sur LES DEUX issues : ne mesurer que les attentes réussies masquerait
    //    précisément celles qui ont expiré.
    double t_attente_max_ms = 0.0;
    // ── Entrelacé ─────────────────────────────────────────────────────────────────────────
    // ⚠ CE CHEMIN N'EXISTAIT PAS, et le TX se tuait dessus. Sur un flux entrelacé chaque grain
    //   est un CHAMP (demi-hauteur) ; le TX lisait `w × h` dans un grain qui en contient la
    //   moitié, débordait, et tombait en SIGSEGV — 22 relances en une minute, mesuré le
    //   2026-10-02. La capture avait son garde-fou de débordement depuis septembre ; la sortie
    //   n'en avait aucun. Le même oubli, de l'autre côté du bus.
    bool   entrelace = false;         // déduit de la TAILLE du grain, pas d'un réglage
    bool   entrelace_connu = false;   // la détection a eu lieu (un grain a été observé)
    bool   tff = true;                // dominance de champ du mode de sortie
    long   grains_courts = 0;         // grain plus petit qu'attendu : REFUSÉ, jamais lu
    long   champs_tisses = 0;         // trames assemblées à partir de deux champs
    long   jetees = 0;                // trame JETÉE : un trou à l'antenne
    long   tardives = 0;              // trame jouée EN RETARD : décalage, pas trou
    long   vidangees = 0;             // trame vidangée (arrêt de lecture)
    int    profondeur = 0;            // trames mises en file au démarrage (= la pré-charge)
    unsigned en_file = 0;             // ce que la CARTE dit garder d'avance, relevé en marche
    // ⚠ PUBLIÉ, pas seulement imprimé : la sortie standard est bufferisée vers le journal du
    //   conteneur, donc la ligne d'amorçage peut ne jamais arriver. Un réglage de latence qu'on
    //   CROIT posé et qui ne l'est pas fausse toutes les mesures qui suivent.
    bool   basse_latence = false;
    // ── ATTENTE DES TRANCHES ──────────────────────────────────────────────────────────────
    // Délai maximal passé dans le callback à attendre que le producteur FINISSE le grain `head`.
    //
    // ⚠ 10 MS ÉTAIT TROP COURT, ET ÇA S'EST VU À L'ANTENNE (28/10, PGM de la Z8). Le mélangeur
    //   finit son image vers 9-10 ms dans la trame — Cam1 arrive à 4,7 ms, le mélangeur prend
    //   ~9 ms. L'échéance expirait donc UNE TRAME SUR DEUX, et le repli ne retombe pas sur une
    //   image neuve : `head-1` est le grain DÉJÀ ÉMIS, donc une image répétée, puis une sautée
    //   au tour suivant. Les compteurs alternaient seconde par seconde entre `sliced` et
    //   `late_previous` pendant que la source, elle, était saine (50 i/s, held 0).
    //   La valeur est RÉGLABLE PAR SORTIE depuis cette version, et son défaut passe à 20 ms.
    //
    // ★ POURQUOI ATTENDRE RESTE SÛR : la carte a `preroll` trames en file devant elle. Attendre
    //   ne lui vole rien tant que l'attente ne dépasse pas la durée d'une trame — au-delà, on
    //   rendrait la main plus tard que la carte ne consomme, et la file se viderait. D'où le
    //   plafond posé plus bas, à la durée d'une trame du mode de sortie.
    // ⚠ CE PLAFOND EST UNE BORNE, PAS UN BUDGET CONFORTABLE : au défaut de 20 ms sur du 1080p50,
    //   l'attente VAUT la durée d'une trame. Un producteur qui ouvrirait un grain à chaque trame
    //   sans jamais le committer ferait rendre la main pile à l'échéance, tour après tour ;
    //   `queued_now` est le témoin à regarder si ça arrive.
    double attente_tranches_s = 0.020;
    // ★★ CE QUI REND L'ATTENTE SÛRE. `attente_tranches_s` n'est qu'un PLAFOND de confort : le
    //    vrai budget est le temps restant avant l'instant de diffusion visé. Le dépasser ne
    //    produit pas une image en retard, la carte JETTE la trame — une image de contenu PERDUE
    //    à l'antenne, comptée en `missed`.
    //
    //    ⚠ CE QUI A ÉTÉ MESURÉ, EXACTEMENT (Z8, PGM, 2026-10-30, binaire ≤ 0.29.35, après une
    //    recréation à 11:04) : 337 trames jetées la PREMIÈRE minute, puis 178, 125, 85, et
    //    enfin 0 à 3 par minute de 11:09 à 11:23. C'était donc un TRANSITOIRE de démarrage —
    //    la programmation rattrapait son retard — et NON un défaut de régime. Ne pas lire ces
    //    337 comme une panne permanente : elle n'a pas existé.
    //
    //    La borne reste néanmoins juste, et c'est un INVARIANT, pas un remède : l'attente de
    //    0.29.34 échangeait des images RÉPÉTÉES contre des images PERDUES dès que le producteur
    //    livrait tard (deux pics de commit du mélangeur à 19 et 15 ms pour 20 de budget, le
    //    même jour). Rien ne garantissait que ça reste un transitoire ; désormais la structure
    //    l'empêche au lieu de compter sur le producteur.
    double slack_vu_min_s = 1e9;     // pire marge observée avant l'instant visé
    double t_travail_max_s = 0.0;    // coût MESURÉ du travail d'après l'attente (réservé)
    double attendu_ce_tour_s = 0.0;  // attente du callback courant (0 = aucune)
    long   attente_ecourtee = 0;     // fois où la marge réelle a réduit le plafond
    long   reancrages = 0;           // fois où l'échéance a été remise sur l'horloge de la carte
    // Avance VOULUE, en trames (cf. `prochaine_echeance`). ⚠ POSÉE PAR `main` depuis la ligne
    // de commande : le défaut vit là-bas et NULLE PART AILLEURS. L'écrire ici aussi, c'est le
    // piège déjà payé sur `attente_tranches_s` — deux défauts qui divergent en silence.
    int    preroll = 0;
};

/// Prochaine échéance de programmation, RÉ-ANCRÉE sur l'horloge de la carte si l'avance a été
/// mangée. Rend l'échéance à utiliser ; incrémente `*reancrages` quand elle a été corrigée.
///
/// ★★ LE DÉFAUT QUE CETTE FONCTION SUPPRIME, MESURÉ (Z8, PGM d'antenne, 2026-10-30).
///    `prochaine_` était un pur accumulateur : `+= duree_` à chaque callback, sans jamais être
///    confrontée à l'horloge de la carte. Or l'AVANCE sur cette horloge (≈ `preroll` trames)
///    est une quantité CONSERVÉE qui ne peut que diminuer : un seul décalage du processus plus
///    long que l'avance la mange, et rien ne la reconstitue — le nombre de callbacks égale le
///    nombre de trames écoulées, donc l'échéance avance exactement au rythme du temps.
///    Conséquence relevée : de 11:04 à 11:37, 89 655 trames sur 116 919 jouées EN RETARD (77 %)
///    sur la sortie d'antenne. Invisible à l'œil — le décalage est constant, la cadence juste —
///    et tous les autres compteurs verts. Puis l'avance devenue négative, la carte s'est mise à
///    JETER : 750 trames en trois minutes, ce qui a par accident remis l'échéance à l'heure.
///    La rafale n'était pas la panne, c'était la guérison ; la panne, c'étaient les 33 minutes.
///
/// ⚠ ON RÉ-ANCRE EN AVANT, donc on saute des instants : la carte n'a rien pour ceux-là et
///   produit UNE discontinuité. C'est le but — une glitch voulue et COMPTÉE, une fois, plutôt
///   qu'une demi-heure de retard suivie de trois minutes de trous. Même arbitrage que
///   l'hystérésis de la capture : un retard franc se rattrape par un saut, pas par une retenue.
///
/// ⚠ ET ON NE RÉ-ANCRE PAS AU MOINDRE ÉCART : il faut que l'avance soit tombée sous UNE trame.
///   Ré-ancrer à chaque callback détruirait la régularité que la carte attend, et rendrait la
///   sortie bien pire que le défaut corrigé.
///
/// ★★★ `t_flux` EST UN *STREAM TIME*, PAS L'HORLOGE MATÉRIELLE — et c'est là qu'était le défaut
///    qui a figé des sorties relancées à chaud (Z8, #228 et #237, 2026-11-02). Cette fonction a
///    été écrite avec `GetHardwareReferenceClock` en argument, alors que `prochaine_` est
///    l'échéance passée à `ScheduleVideoFrame` : elle vit dans le temps de FLUX, qui repart de
///    l'instant donné à `StartScheduledPlayback` (0 ici). L'horloge matérielle, elle, ne repart
///    pas : elle court depuis l'activation du sous-périphérique. Les deux domaines ne coïncident
///    donc QU'AU PREMIER démarrage — ce qui explique pourquoi tous les bancs passaient.
///
///    Relancée sous le même `persistent_id` alors que l'horloge matérielle est déjà à T, la
///    sortie lit son avance comme −T, ré-ancre UNE fois à T + preroll trames, et attend T
///    secondes sur sa dernière image. Relevé : `frame_index` figé à 4, `schedule_reanchors` 1,
///    `slack_min_ms` −438 999,98 sur une relance 439 s après la précédente, `source_alive` vrai
///    et 0 i/s — l'écart valait chaque fois le temps écoulé depuis le démarrage précédent. Le
///    gel ne frappait pas toutes les relances, parce qu'une carte entièrement relâchée repart
///    près de zéro et les deux domaines se recollent par accident.
///
///    La comparaison se fait donc avec `GetScheduledStreamTime`, le seul domaine où `prochaine_`
///    a un sens. Garde : `verif_domaine_temps_sortie.py`, qui refuse toute confrontation de
///    `prochaine_` à l'horloge matérielle — le défaut n'était pas dans cette formule, il était
///    dans l'unité de son argument, et aucune garde sur la formule seule ne l'aurait vu.
static BMDTimeValue prochaine_echeance(BMDTimeValue prochaine, BMDTimeValue t_flux,
                                       BMDTimeValue duree, int preroll, long *reancrages) {
    if (duree <= 0 || t_flux <= 0) return prochaine;      // horloge indisponible : on ne touche à rien
    const BMDTimeValue avance = prochaine - t_flux;
    if (avance >= duree) return prochaine;               // au moins une trame d'avance : rien à faire
    const int p = preroll > 0 ? preroll : 1;
    if (reancrages) (*reancrages)++;
    return t_flux + (BMDTimeValue)p * duree;
}

/// Budget d'attente RÉELLEMENT disponible, en secondes.
///
/// ★★ EXTRAITE EXPRÈS DU CORPS DE `remplir`. Enfouie dans une méthode qui manipule des
///    interfaces DeckLink, cette formule n'était exécutable par aucune garde : on ne pouvait
///    que relire le code et se convaincre. C'est la leçon de `coeurs_dedies_voulus` (28/10) —
///    une formule qui décide d'une sortie d'antenne doit être appelable seule.
///
/// `plafond_s` : le confort réglé (`--slice-wait`), borné ailleurs à une durée de trame.
/// `reste_s`   : temps restant avant l'instant de diffusion visé, mesuré sur l'horloge de la
///               CARTE (et non la nôtre : c'est elle qui décide de jeter la trame).
/// `travail_s` : coût mesuré du travail qui SUIT l'attente ; 0 = pas encore mesuré.
///
/// INVARIANT TENU : le résultat est toujours < `reste_s` dès que `reste_s` est connu. C'est
/// tout l'objet de la fonction — attendre au-delà fait JETER la trame par la carte, soit une
/// image perdue à l'antenne, alors qu'attendre moins ne coûte qu'une image répétée. Les deux
/// ne se valent pas, et c'est la seule raison pour laquelle cette asymétrie est câblée ici.
static double budget_attente_s(double plafond_s, double reste_s, double travail_s) {
    if (plafond_s < 0.0) plafond_s = 0.0;
    if (reste_s < 0.0) return 0.0;                  // l'instant visé est déjà passé
    const double reserve_s = travail_s > 0.0 ? travail_s * 1.5 : 0.002;
    const double dispo_s = reste_s - reserve_s;
    if (dispo_s <= 0.0) return 0.0;
    return dispo_s < plafond_s ? dispo_s : plafond_s;
}

/// Relit le mode « image manquante » depuis son fichier, AU PLUS une fois par seconde.
///
/// ★★ POURQUOI UN FICHIER ET PAS UN ARGUMENT. Le mode doit pouvoir changer À CHAUD : s'il
///    vivait dans la ligne de commande, le superviseur devrait relancer le connecteur pour en
///    changer (`_reconfigurer` compare les `args`), et sur la sortie PGM d'antenne basculer un
///    réglage de MISE AU POINT couperait le direct. Le chemin est stable, donc `args` ne change
///    jamais et rien ne redémarre.
///
/// ⚠ FICHIER ABSENT OU ILLISIBLE = ON GARDE CE QU'ON A, jamais un repli au noir. Un `ENOENT`
///   pendant une réécriture ne doit pas faire clignoter l'antenne en noir.
static int relire_mode(Etat *e) {
    if (e->mode_fichier.empty()) return e->mode_manquant;
    const double t = horloge_s();
    if (e->t_mode > 0.0 && t - e->t_mode < 1.0) return e->mode_manquant;
    e->t_mode = t;
    FILE *f = fopen(e->mode_fichier.c_str(), "r");
    if (!f) return e->mode_manquant;
    char buf[32];
    const size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *q = buf;
    while (*q == ' ' || *q == '\n' || *q == '\t' || *q == '\r') q++;
    if (!strncmp(q, "black", 5))       e->mode_manquant = 1;
    else if (!strncmp(q, "repeat", 6)) e->mode_manquant = 0;
    return e->mode_manquant;
}

static void ecrire_stats(const Etat *e, const char *chemin) {
    if (!chemin || !*chemin) return;
    char tmp[512]; snprintf(tmp, sizeof tmp, "%s.tmp", chemin);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    // `drift_ppm` vaut null tant que la fenêtre n'est pas assez longue : une valeur publiée
    // trop tôt serait du bruit qu'un seuil d'alerte prendrait pour un incident.
    char derive[64], gliss[64];
    if (e->derive_fenetre_s > 0.0) snprintf(derive, sizeof derive, "%.2f", e->derive_ppm);
    else                           snprintf(derive, sizeof derive, "null");
    // `slip_hours` : null tant qu'aucune fenêtre n'est close ; 0 signifie « aucun glissement
    // observé sur la dernière fenêtre », ce qui n'est PAS la même chose que « pas mesuré ».
    if (e->slip_fenetre_s <= 0.0)  snprintf(gliss, sizeof gliss, "null");
    else if (e->slip_h <= 0.0)     snprintf(gliss, sizeof gliss, "0");
    else                           snprintf(gliss, sizeof gliss, "%.2f", e->slip_h);
    fprintf(f, "{\"t_frame_ns\": %lld, \"fps\": %.2f, \"frame_index\": %ld, \"source_alive\": %s, "
               "\"repeated\": %ld, \"missed\": %ld, \"width\": %d, \"height\": %d, "
               "\"flow\": \"%s\", \"card_vs_node_ppm\": %s, \"drift_window_s\": %.0f, "
               "\"genlock\": %s, \"genlock_samples\": %ld, "
               "\"slip_hours\": %s, \"slip_window_s\": %.0f, "
               "\"sliced\": %ld, \"head_ontime\": %ld, \"late_previous\": %ld, "
               // ★ LA VALEUR EFFECTIVE, APRÈS PLAFONNEMENT. Publier celle qu'on a DEMANDÉE
               //   ferait croire à un réglage actif que la carte ne tient pas — c'est ce qui
               //   s'est passé pour la basse latence, juste au-dessus.
               "\"slice_timeouts\": %ld, \"slice_wait_ms\": %.1f, "
               // ★ LES QUATRE MOTIFS DE REPLI, SÉPARÉMENT. « late_previous monte et
               //   slice_timeouts reste à 0 » était indéchiffrable sans eux.
               "\"head_unreadable\": %ld, \"head_no_slices\": %ld, "
               "\"slice_geometry_refused\": %ld, \"slices_awaited\": %ld, "
               // ★ `head_awaited` est le chiffre qui MESURE le correctif du 28/10 (répétitions
               //   évitées en attendant, dans le budget, que le producteur ouvre son grain) ;
               //   `head_frozen` désigne au contraire un producteur réellement en retard.
               //   `head_wait_max_ms` est la PIRE attente courue, à comparer au budget
               //   `slice_wait_ms` publié plus haut : deux compteurs d'issues ne disent pas si
               //   l'on frôle l'échéance (réclamé par la Z8 le 30/10, cf. `t_attente_max_ms`).
               "\"head_awaited\": %ld, \"head_frozen\": %ld, \"head_wait_max_ms\": %.2f, "
               "\"low_latency\": %s, "
               "\"interlaced\": %s, \"woven\": %ld, \"short_grains\": %ld, "
               "\"queue_depth\": %d, \"queued_now\": %u, "
               "\"dropped\": %ld, \"late\": %ld, \"flushed\": %ld, "
               // ★ LES COMPTEURS AUDIO SONT PUBLIÉS, SINON ILS NE SERVENT À RIEN. Compter un
               //   écrêtage sans le montrer revient à l'écrêter en silence — et c'est
               //   précisément ce qui a fait chercher la panne dans la carte alors qu'elle
               //   était dans la source. Même raisonnement pour les ré-ancrages : ils disent
               //   que l'horloge de la carte et celle du nœud divergent, et ça s'entend.
               // ★ `audio_pending` SÉPARE « pas de son demandé » de « son demandé, flux pas
               //   encore là ». Sans lui, les deux se lisent audio_samples = 0, et une sortie
               //   qui attend son producteur ressemble à une source muette — c'est exactement
               //   ce qui a fait passer la panne du 21/10 pour un silence normal.
               "\"audio_samples\": %ld, \"audio_clipped\": %ld, "
               "\"audio_skipped\": %ld, \"audio_resyncs\": %ld, "
               // ★ Ce que la carte OUVRE, ce que le bus DÉCLARE, et ce qui est CÂBLÉ : les
               //   trois doivent être lisibles séparément. Un seul chiffre « 16 canaux » ne
               //   distingue pas « 16 portés » de « 16 ouverts, 8 alimentés » — la panne du
               //   21/10 tenait entière dans cette confusion.
               "\"audio_pending\": %s, \"audio_opens\": %ld, "
               "\"audio_flows\": %d, \"audio_channels_card\": %d, "
               "\"audio_channels_declared\": %d, \"audio_channels_declared_2\": %d, "
               // ★ COMBIEN DE FOIS LA SOURCE A ÉTÉ RECRÉÉE SOUS NOS PIEDS. Sans ces deux
               //   chiffres, la garde de génération est invérifiable : on ne saurait pas
               //   distinguer « elle n'a jamais eu à mordre » de « elle ne mord pas ». Un
               //   nombre qui grimpe sans arrêt dit l'inverse — un producteur qui se recrée en
               //   boucle, et c'est LUI qu'il faut alors aller voir.
               "\"audio_skipped_2\": %ld, \"audio_group2_missing\": %s, "
               // Contrat PARTAGÉ avec le mélangeur (30/10) : « images émises qui ne sont pas
               // une image fraîche de la source ». Le mode EFFECTIF est publié à côté, sans
               // quoi un relevé de compteur ne dirait pas ce que le spectateur a vu.
               // Les trois témoins de la borne d'attente. `slack_min_ms` est le PIRE temps
               // restant observé avant l'instant de diffusion : s'il approche de zéro, la
               // sortie est au bord de la trame jetée, et c'est `preroll` qu'il faut monter.
               // `schedule_reanchors` : chaque unité est UNE discontinuité volontaire à
               // l'antenne. Zéro est le régime normal ; un nombre qui grimpe dit que le
               // processus se fait décaler, et c'est LÀ qu'il faut chercher.
               "\"schedule_reanchors\": %ld, "
               "\"slack_min_ms\": %.2f, \"work_max_ms\": %.2f, \"wait_shortened\": %ld, "
               "\"missing_frames_repeated\": %ld, \"missing_frame_mode\": \"%s\", "
               "\"flow_regens\": %ld, \"audio_regens\": %ld}\n",
            (long long)e->t_derniere_trame,
            e->fps, e->emises, e->source_vivante ? "true" : "false",
            e->repetees, e->ratees, e->w, e->h, e->flux.c_str(),
            derive, e->derive_fenetre_s,
            (e->n_releves && e->n_genlock * 2 > e->n_releves) ? "true" : "false", e->n_releves,
            gliss, e->slip_fenetre_s, e->suivi_tranches, e->suivi_entier,
            e->repli_precedent, e->tranches_incompletes,
            e->attente_tranches_s * 1000.0,
            e->tete_illisible, e->tete_sans_tranches,
            e->geometrie_refusee, e->tranches_attendues,
            e->tete_attendue, e->tete_immobile, e->t_attente_max_ms,
            e->basse_latence ? "true" : "false",
            e->entrelace ? "true" : "false", e->champs_tisses, e->grains_courts,
            e->profondeur, e->en_file, e->jetees, e->tardives, e->vidangees,
            e->ech_audio, e->audio_ecretes, e->audio_sautes, e->audio_reancrages,
            (!e->rd_audio && !e->flux_audio_voulu.empty()) ? "true" : "false",
            e->audio_ouvertures,
            (e->rd_audio ? 1 : 0) + (e->rd_audio2 ? 1 : 0), e->canaux_audio,
            e->canaux_declares, e->canaux_declares2, e->audio_sautes2,
            (e->canaux_g2 > 0 && !e->rd_audio2) ? "true" : "false",
            e->reancrages,
            (e->slack_vu_min_s > 1e8 ? 0.0 : e->slack_vu_min_s * 1000.0),
            e->t_travail_max_s * 1000.0, e->attente_ecourtee,
            e->non_fraiches, e->mode_manquant == 1 ? "black" : "repeat",
            e->regen_video, e->regen_audio);
    fclose(f);
    rename(tmp, chemin);
}

// ── GARDE DE GÉNÉRATION ──────────────────────────────────────────────────────────────────
// Cf. le pavé « DÉCROCHAGE DE GÉNÉRATION » dans `Etat`. Au-delà de ce délai sans une seule
// écriture, aucune source vivante n'est plausible : à 50 i/s c'est 250 trames, à 25 i/s 125.
// Même valeur que `_StaleGuard.STALE_MS_DEFAULT` côté Python — les deux doivent décrocher au
// même moment, sans quoi un même flux mort serait jugé vivant par l'un et mort par l'autre.
static const uint64_t PERIME_NS = 5000ull * 1000ull * 1000ull;

/// Vrai si ce lecteur n'a rien vu écrire depuis `PERIME_NS`. Prudent par construction : un
/// `lastWriteTime` nul (flux jamais écrit) ou une horloge en arrière ne déclenchent RIEN —
/// une garde qui se trompe en rouvrant à tort coûte une coupure d'antenne.
static bool lecteur_perime(mxlFlowReader rd) {
    if (!rd) return false;
    mxlFlowInfo info;
    memset(&info, 0, sizeof info);
    info.version = 1; info.size = sizeof info;
    if (mxlFlowReaderGetInfo(rd, &info) != MXL_STATUS_OK) return false;
    const uint64_t derniere = info.runtime.lastWriteTime;
    if (!derniere) return false;
    const uint64_t maintenant = mxlGetTime();
    if (maintenant <= derniere) return false;
    return (maintenant - derniere) > PERIME_NS;
}

/// Pendant AUDIO de `lecteur_perime` : cf. le pavé de `Etat`. On juge sur l'index, pas sur
/// `lastWriteTime` — que les producteurs audio ne mettent pas à jour. Un index qui bouge
/// ré-arme le compteur ; un index figé plus de `PERIME_NS` est un décrochage.
static bool lecteur_audio_perime(mxlFlowReader rd, uint64_t *tete_vue, double *t_vue,
                                 bool *amorce) {
    if (!rd) return false;
    mxlFlowInfo info;
    memset(&info, 0, sizeof info);
    info.version = 1; info.size = sizeof info;
    if (mxlFlowReaderGetInfo(rd, &info) != MXL_STATUS_OK) return false;
    const uint64_t tete = info.runtime.headIndex;
    const double maintenant = horloge_s();
    // ★ PREMIÈRE OBSERVATION : on AMORCE, on ne juge pas. Sans ce cas, `*t_vue` valant 0 ferait
    //   paraître le lecteur périmé dès le premier passage — exactement la boucle de réouverture
    //   que ce correctif supprime, reconstituée autrement.
    if (!*amorce || tete != *tete_vue) {
        *amorce = true;
        *tete_vue = tete;
        *t_vue = maintenant;
        return false;
    }
    return (maintenant - *t_vue) > (double)(PERIME_NS / 1000000000ull);
}

/// Rouvre les lecteurs décrochés. Appelée AU PLUS une fois par seconde depuis le chemin par
/// trame : le contrôle coûte un `GetInfo` par lecteur, la réouverture est rare.
///
/// ⚠ L'ORDRE DES TROIS GESTES N'EST PAS LIBRE, il est repris de `_StaleGuard` qui l'a établi au
///   banc : (1) lâcher NOTRE poignée — tant qu'elle est tenue le flux reste RÉFÉRENCÉ et le
///   ramasse-miettes ne le réclame pas ; (2) ramasser — sinon le nom résout encore vers
///   l'orphelin ; (3) rouvrir. Inverser (1) et (2) rouvre sur le mort, et la garde boucle.
static void garde_generation(Etat *e) {
    if (!e->mxl) return;
    const double t = horloge_s();
    if (e->t_garde > 0.0 && t - e->t_garde < 1.0) return;
    e->t_garde = t;

    // ⚠⚠ `rd` NUL EST UN ÉTAT À RETENTER, PAS UN ÉTAT SAIN — et l'oublier rouvrait la panne
    //    même que cette garde corrige. `lecteur_perime(NULL)` rend FAUX : sans le `!e->rd`
    //    ci-dessous, une première réouverture ratée laissait `rd` nul, la garde ne voyait plus
    //    rien de « périmé », et la sortie restait figée POUR TOUJOURS en répétant son image.
    //    Et ce premier échec est le cas NORMAL, pas le cas rare : pendant un redéploiement, le
    //    producteur se tait 5 à 15 s (chargement du clip), donc le seuil de 5 s tombe presque
    //    toujours AVANT que le flux renaisse. Défaut signalé par la session Médias avant la
    //    mise en service — ma première version abandonnait au premier essai.
    const bool video_morte  = !e->rd || lecteur_perime(e->rd);
    // ⚠ PAS `lecteur_perime` ICI. Cf. le pavé de `Etat` : ce détecteur-là couperait le son.
    const bool audio_mort   = lecteur_audio_perime(e->rd_audio, &e->a_tete_vue,
                                                   &e->a_tete_t, &e->a_tete_amorce);
    const bool audio2_mort  = lecteur_audio_perime(e->rd_audio2, &e->a2_tete_vue,
                                                   &e->a2_tete_t, &e->a2_tete_amorce);
    if (!video_morte && !audio_mort && !audio2_mort) { e->attente_flux = false; return; }

    // (1) On lâche TOUT ce qui est mort avant de ramasser : une seule poignée encore tenue
    //     suffit à retenir l'orphelin, y compris celle d'un AUTRE flux de ce processus.
    if (video_morte && e->rd) { mxlReleaseFlowReader(e->mxl, e->rd);        e->rd = NULL; }
    // Le suivi d'index se REMET À ZÉRO avec la poignée : conservé, il comparerait l'index du
    // flux mort à celui du neuf et jugerait le lecteur neuf périmé d'entrée.
    if (audio_mort)  { mxlReleaseFlowReader(e->mxl, e->rd_audio);  e->rd_audio = NULL;
                       e->a_tete_amorce = false; }
    if (audio2_mort) { mxlReleaseFlowReader(e->mxl, e->rd_audio2); e->rd_audio2 = NULL;
                       e->a2_tete_amorce = false; }

    // (2) Ramasser.
    mxlGarbageCollectFlows(e->mxl);

    // (3) Rouvrir. L'AUDIO NE SE ROUVRE PAS ICI, VOLONTAIREMENT : le chemin paresseux de
    //     `servir_audio` le reprend tout seul à la trame suivante, et il sait déjà faire les
    //     deux choses qu'il faut en plus — ré-ancrer la position de lecture (celle d'avant
    //     désigne un endroit que ce nouveau flux n'a jamais écrit) et comparer les canaux
    //     déclarés à ceux ouverts. Le dupliquer ici, c'est se condamner à corriger deux fois.
    if (audio_mort || audio2_mort) e->regen_audio++;

    if (video_morte) {
        char fid[37]; flow_id_str(e->flux.c_str(), fid);
        if (mxlCreateFlowReader(e->mxl, fid, NULL, &e->rd) != MXL_STATUS_OK) {
            e->rd = NULL;    // le producteur n'a pas encore recréé le sien : on retentera dans 1 s
            // Une ligne à l'entrée dans l'attente, pas une par seconde : un journal qui se
            // remplit d'une ligne identique noie ce qui se passe ailleurs.
            if (!e->attente_flux) {
                e->attente_flux = true;
                fprintf(stderr, "flux « %s » décroché et pas encore réapparu — nouvel essai "
                                "toutes les secondes\n", e->flux.c_str());
            }
            return;
        }
        e->attente_flux = false;
        e->regen_video++;
        // L'entrelacement se REDÉDUIT : rien ne garantit que le flux recréé a le même balayage
        // que celui qu'il remplace, et le garder figé rouvrirait le débordement de lecture du
        // 2026-10-02 (grain de 540 lignes lu comme 1080).
        e->entrelace_connu = false;
        e->dernier_index = 0;
        fprintf(stderr, "flux « %s » recréé par son producteur — sortie raccrochée "
                        "(%ld réouverture%s)\n",
                e->flux.c_str(), e->regen_video, e->regen_video > 1 ? "s" : "");
    }
}

// Rappel de sortie : c'est LUI qui bat la mesure. À chaque trame complétée, on va chercher ce
// que la source a de plus frais ; si elle n'a rien de neuf, on ré-émet ce qu'on tient.
class Sortie : public IDeckLinkVideoOutputCallback {
public:
    Sortie(IDeckLinkOutput *o, Etat *e, BMDTimeValue d, BMDTimeScale s)
        : out_(o), e_(e), duree_(d), echelle_(s) {}
    HRESULT QueryInterface(REFIID, void **v) override { *v = NULL; return E_NOINTERFACE; }
    ULONG AddRef(void) override  { return ++refs_; }
    ULONG Release(void) override { ULONG r = --refs_; if (!r) delete this; return r; }

    HRESULT ScheduledFrameCompleted(IDeckLinkVideoFrame *frame,
                                    BMDOutputFrameCompletionResult res) override {
        // ★★ TROIS ISSUES SANS COMMUNE MESURE, UN SEUL COMPTEUR (corrigé le 2026-10-10).
        //    `ratees` additionnait « trame JETÉE » et « trame jouée EN RETARD ». Mesuré après huit
        //    jours de marche : 3,39 millions de « ratées » sur 16,15 M, et 100 % sur la fenêtre
        //    courante — alors que la cadence est exacte (25,0 i/s) et la latence inchangée
        //    (120 ms). Le compteur criait à la catastrophe sur une sortie saine.
        //
        //    Une trame JETÉE est un trou à l'antenne. Une trame EN RETARD, c'est notre horloge de
        //    programmation qui a pris du retard sur celle de la carte : l'image sort quand même,
        //    à cadence juste, avec un décalage CONSTANT. Les confondre rend le compteur muet dans
        //    les deux sens — on ne voit plus le vrai trou, et on s'alarme du décalage.
        switch (res) {
            case bmdOutputFrameDropped:       e_->jetees++;  e_->ratees++; break;
            case bmdOutputFrameDisplayedLate: e_->tardives++; break;
            case bmdOutputFrameFlushed:       e_->vidangees++; break;
            default: break;
        }

        // ★ CE QUE LA CARTE GARDE VRAIMENT. Notre profondeur de file est ce qu'on DEMANDE ;
        //   `GetBufferedVideoFrameCount` dit ce qui est réellement en attente. Les deux peuvent
        //   diverger, et c'est précisément là que se cacherait une latence qu'on n'explique pas.
        {
            uint32_t n_file = 0;
            if (out_->GetBufferedVideoFrameCount(&n_file) == S_OK) e_->en_file = n_file;
        }
        if ((e_->emises % 50) == 0) {           // ~1 relevé par seconde
            BMDReferenceStatus r = (BMDReferenceStatus)0;
            out_->GetReferenceStatus(&r);
            e_->n_releves++;
            if (r & bmdReferenceLocked) e_->n_genlock++;
        }
        const double t_av_travail = horloge_s();
        remplir((IDeckLinkMutableVideoFrame *)frame);
        out_->ScheduleVideoFrame(frame, prochaine_, duree_, echelle_);
        // ★ MESURÉ SUR LES TOURS OÙ L'ON N'A PAS ATTENDU, sinon la réserve inclurait l'attente
        //   qu'elle sert justement à borner — elle grandirait à chaque tour et finirait par
        //   interdire toute attente. Un compteur qui se nourrit de lui-même.
        if (e_->attendu_ce_tour_s <= 0.0) {
            const double cout_s = horloge_s() - t_av_travail;
            if (cout_s > e_->t_travail_max_s) e_->t_travail_max_s = cout_s;
        }
        servir_audio(e_, out_, prochaine_, duree_, echelle_);
        prochaine_ += duree_;
        // ★ CONFRONTER L'ÉCHÉANCE À L'HORLOGE DE LA CARTE, à chaque trame. Sans ça, l'avance
        //   ne peut que se perdre (cf. le pavé de `prochaine_echeance`).
        {
            BMDTimeValue t_flux = 0; double vitesse = 0.0;
            if (echelle_ > 0
                && out_->GetScheduledStreamTime(echelle_, &t_flux, &vitesse) == S_OK) {
                prochaine_ = prochaine_echeance(prochaine_, t_flux, duree_, e_->preroll,
                                                &e_->reancrages);
            }
        }
        e_->emises++;
        // ★ Instant de la trame ÉMISE, pris ici même. Couplé à `emises`, il donne la cadence
        //   RÉELLE de la sortie contre l'horloge du nœud — et la sortie, elle, EST asservie à la
        //   référence (1791 relevés sur 1791). C'est donc la mesure qui tranche la relation
        //   synchro-maison ↔ nœud, là où la capture ne dit que « source ↔ nœud ».
        e_->t_derniere_trame = maintenant_ns();

        // Dérive de la sortie, sur fenêtre glissante. Ré-ancrage à chaque publication pour que la
        // valeur reste RÉCENTE plutôt que de moyenner depuis le démarrage.
        // ★ NE PAS ANCRER PENDANT L'AMORÇAGE (2026-09-09). Ancrée sur la toute première trame,
        //   la fenêtre englobait la pré-charge : sortie déjà cadencée, source pas encore là,
        //   horodatages irréguliers. Mesuré : PREMIÈRE fenêtre à −1,12 ppm, les suivantes à 0,00
        //   et +0,11 sur la même chaîne — un transitoire de 2 ms sur l'ancre suffit à cela sur
        //   1800 s. J'ai d'abord lu ce −1,12 comme la RÉFUTATION du modèle de gigue ; c'était un
        //   artefact d'amorçage, et le modèle tient (contrôles indépendants : −0,027 ppm sur
        //   1200 s, +0,072 sur 2401 s, +0,039 sur 3602 s). Cinq secondes de rejet, et la première
        //   valeur publiée vaut les suivantes.
        // ⚠ LE `else` D'UN `&&` N'EST PAS CELUI QU'ON CROIT (2026-09-09). Écrite
        //   `if (!ancre && emises > 250) … else …`, cette garde envoyait les 250 PREMIÈRES trames
        //   dans la branche de CALCUL, avec `ancre_t_ns` encore à zéro : dt valait alors l'époque
        //   UNIX entière, donc ≥ 1800, donc on publiait — `−1 000 000 ppm`, pendant trente minutes,
        //   avant que la fenêtre suivante ne se close. Le correctif d'amorçage avait ainsi produit
        //   pire que le défaut qu'il corrigeait. Les deux conditions ne sont PAS de même nature :
        //   « pas encore d'ancre » choisit la branche, « assez de trames » n'est qu'un délai.
        if (!e_->ancre_t_ns) {
          if (e_->emises > 250) {
            e_->ancre_emises = e_->emises; e_->ancre_t_ns = e_->t_derniere_trame;
            // ★ ANCRER AUSSI LES RÉPÉTITIONS (2026-09-09). Sans cette ligne, la PREMIÈRE fenêtre
            //   comptait les répétitions d'AMORÇAGE — celles où la sortie tourne déjà et où la
            //   source n'est pas encore là. Mesuré : 23 répétitions → `slip_hours` = 0,02, soit
            //   « un glissement toutes les 72 secondes » annoncé sur une chaîne parfaitement saine.
            //   Le commentaire du calcul affirmait que l'amorçage était « derrière nous puisque la
            //   fenêtre fait 300 s » : c'est vrai de toutes les fenêtres SAUF de la première, et
            //   c'est la première que l'exploitant regarde en déployant.
            e_->rep_ancre = e_->repetees;
            e_->ancre_index = e_->dernier_index;
          }
        } else {
            const double dt = (e_->t_derniere_trame - e_->ancre_t_ns) / 1e9;
            if (dt >= FENETRE_DERIVE_MIN_S && duree_ > 0 && echelle_ > 0) {
                const double nominal = (double)echelle_ / (double)duree_;
                const double reelle  = (double)(e_->emises - e_->ancre_emises) / dt;
                const double ppm = (reelle / nominal - 1.0) * 1e6;
                // Une valeur absurde ne se publie pas : elle s'OMET. Un `null` se lit comme un
                // blanc ; `−1 000 000 ppm` se lit comme une mesure, et c'est ce qui a été affiché
                // une demi-heure durant. Le seuil est large exprès — il n'attrape que l'absurde,
                // jamais une vraie dérive, si grosse soit-elle.
                if (ppm > -1000.0 && ppm < 1000.0) {
                    e_->derive_ppm = ppm;
                    e_->derive_fenetre_s = dt;
                }
                // Glissement NET sur la même fenêtre : ce que la source a produit moins ce que
                // nous avons émis. Une source morte ne glisse pas, elle est absente — et
                // `source_alive` le dit déjà ; la compter ici donnerait un glissement énorme et
                // sans objet.
                const long long prod = (long long)(e_->dernier_index - e_->ancre_index);
                const long long emis = (long long)(e_->emises - e_->ancre_emises);
                const long long net  = prod > emis ? prod - emis : emis - prod;
                e_->slip_fenetre_s = dt;
                e_->slip_h = (e_->source_vivante && prod > 0 && net > 0)
                             ? (dt / (double)net) / 3600.0 : 0.0;
                e_->rep_ancre = e_->repetees;
                e_->ancre_index = e_->dernier_index;
                e_->ancre_emises = e_->emises; e_->ancre_t_ns = e_->t_derniere_trame;
            }
        }
        const double now = horloge_s();
        if (e_->t_fenetre == 0.0) e_->t_fenetre = now;
        e_->emises_fenetre++;
        if (now - e_->t_fenetre >= 1.0) {
            e_->fps = e_->emises_fenetre / (now - e_->t_fenetre);
            e_->emises_fenetre = 0; e_->t_fenetre = now;
        }
        return S_OK;
    }
    HRESULT ScheduledPlaybackHasStopped(void) override { return S_OK; }

    // Octets par échantillon dans le GRAIN tel qu'il est écrit : 1 en 8 bits, 2 au-delà. Toutes
    // les tailles attendues en dépendent — les calculer à 2 en dur classait une trame 8 bits
    // comme un champ entrelacé, puisqu'elle en pèse exactement le même nombre d'octets.
    size_t octets(void) const { return e_->bits_src >= 10 ? 2u : 1u; }

    // Élargit un grain 8 bits en échantillons 10 bits (échelle studio, << 2) et rend un pointeur
    // utilisable tel quel par le chemin 10 bits. En 10 bits natif, rend le grain sans rien
    // copier — le cas courant ne paie pas la conversion. Réservé au chemin ENTRELACÉ, qui lit
    // un champ entier d'un coup ; le chemin progressif convertit bande par bande.
    const uint8_t *en10(const uint8_t *p, size_t echantillons) {
        if (e_->bits_src >= 10) return p;
        if (e_->conv.size() < echantillons) e_->conv.resize(echantillons);
        uint16_t *d = e_->conv.data();
        for (size_t i = 0; i < echantillons; i++) d[i] = (uint16_t)p[i] << 2;
        return (const uint8_t *)d;
    }

    // Empaquette les lignes [l0, l1) d'un grain planar dans le tampon v210 de la carte. Les
    // trois plans sont indexés par LIGNE : Y pleine largeur, Cb/Cr demi-largeur, même hauteur.
    void packer_bande(const uint8_t *payload, uint8_t *dst, size_t rowBytes, int l0, int l1) {
        if (l1 <= l0) return;
        const int nl = l1 - l0;
        const size_t wy = (size_t)e_->w, wc = (size_t)(e_->w / 2), h = (size_t)e_->h;
        if (e_->bits_src >= 10) {
            const uint16_t *y  = (const uint16_t *)payload;
            const uint16_t *cb = y  + wy * h;
            const uint16_t *cr = cb + wc * h;
            g_pack10(y  + (size_t)l0 * wy,
                     cb + (size_t)l0 * wc,
                     cr + (size_t)l0 * wc,
                     dst + (size_t)l0 * rowBytes, rowBytes, e_->w, nl);
            return;
        }
        // ⚠ EN 8 BITS, ON N'ÉLARGIT QUE LA BANDE DEMANDÉE. En mode tranche, cette méthode est
        //   rappelée à CHAQUE bande qui devient valide (une trentaine par image) : convertir la
        //   trame entière à chaque appel coûterait trente conversions pleine trame par image, et
        //   le mode tranche — dont tout l'intérêt est la latence — deviendrait le chemin lent.
        const uint8_t *y8  = payload;
        const uint8_t *cb8 = y8  + wy * h;
        const uint8_t *cr8 = cb8 + wc * h;
        const size_t ny = wy * (size_t)nl, nc = wc * (size_t)nl;
        if (e_->conv.size() < ny + 2 * nc) e_->conv.resize(ny + 2 * nc);
        uint16_t *dy  = e_->conv.data();
        uint16_t *dcb = dy  + ny;
        uint16_t *dcr = dcb + nc;
        for (size_t i = 0; i < ny; i++) dy[i]  = (uint16_t)y8[(size_t)l0 * wy + i] << 2;
        for (size_t i = 0; i < nc; i++) dcb[i] = (uint16_t)cb8[(size_t)l0 * wc + i] << 2;
        for (size_t i = 0; i < nc; i++) dcr[i] = (uint16_t)cr8[(size_t)l0 * wc + i] << 2;
        g_pack10(dy, dcb, dcr, dst + (size_t)l0 * rowBytes, rowBytes, e_->w, nl);
    }

    // Entrelacé : deux grains-CHAMPS consécutifs → une trame v210. L'index PAIR porte le premier
    // champ (convention libmxl, miroir exact de ce qu'écrit la capture) ; en tff ce premier champ
    // occupe les lignes paires de la trame. Se tromper ici donnerait une image fixe correcte et
    // une saccade en mouvement — on reprend donc la MÊME règle que l'écriture, au lieu d'en
    // réinventer une seconde qui pourrait en diverger.
    bool tisser_champs(uint64_t head, uint8_t *dst, size_t rowBytes) {
        const uint64_t base = head & ~1ULL;          // aligné sur un premier champ
        const int hc = e_->h / 2;
        const size_t besoin_champ = ((size_t)e_->w * hc
                                   + (size_t)(e_->w / 2) * hc * 2u) * octets();
        for (int ch = 0; ch < 2; ch++) {
            mxlGrainInfo gi; uint8_t *p = NULL;
            if (mxlFlowReaderGetGrainNonBlocking(e_->rd, base + ch, &gi, &p) != MXL_STATUS_OK)
                return false;
            if (gi.validSlices != gi.totalSlices) return false;
            if (gi.grainSize && (size_t)gi.grainSize < besoin_champ) {
                e_->grains_courts++;
                return false;
            }
            p = (uint8_t *)en10(p, (size_t)e_->w * hc * 2u);
            const uint16_t *y  = (const uint16_t *)p;
            const uint16_t *cb = y  + (size_t)e_->w * hc;
            const uint16_t *cr = cb + (size_t)(e_->w / 2) * hc;
            const int ligne0 = e_->tff ? ch : (1 - ch);
            // Une ligne sur deux dans la trame de sortie : le pas destination est DOUBLE.
            g_pack10(y, cb, cr, dst + (size_t)ligne0 * rowBytes, rowBytes * 2, e_->w, hc);
        }
        return true;
    }

    /// UN SEUL ENDROIT POUR « PAS D'IMAGE FRAÎCHE », quelle qu'en soit la raison.
    ///
    /// ★ POURQUOI CENTRALISÉ. Trois sites ré-émettaient et incrémentaient `repetees` chacun de
    ///   son côté. Laisser le noir se greffer sur chacun garantissait qu'un QUATRIÈME site,
    ///   ajouté plus tard, l'oublierait — et l'oubli serait INVISIBLE : l'image sortante aurait
    ///   l'air normale, le mode de mise au point marcherait « à moitié », et on conclurait que
    ///   la chaîne ne saute pas sur le cas précisément non couvert.
    void sans_image_fraiche(void *dst, size_t rowBytes) {
        e_->repetees++;
        e_->non_fraiches++;
        if (relire_mode(e_) == 1) remplir_noir_v210((uint8_t *)dst, e_->w, e_->h, rowBytes);
    }

    void remplir(IDeckLinkMutableVideoFrame *f) {
        void *dst = NULL;
        if (!f || f->GetBytes(&dst) != S_OK || !dst) return;
        const size_t rowBytes = (size_t)f->GetRowBytes();

        // ★ LA GARDE PASSE AVANT LA LECTURE, pas après : placée en aval, elle ne corrigerait
        //   qu'à la trame SUIVANTE et laisserait celle-ci partir depuis l'anneau mort. C'est la
        //   même faute de placement que la garde de taille du 2026-10-02, qui protégeait un
        //   site déjà franchi.
        garde_generation(e_);

        // ⚠ `rd` PEUT ÊTRE NUL depuis la garde : un flux décroché dont le producteur n'a pas
        //   encore recréé le sien laisse la sortie SANS lecteur. On ré-émet alors la dernière
        //   image, ce que fait déjà le chemin « rien de neuf » — mais il faut sortir AVANT
        //   d'appeler la bibliothèque, qui déréférencerait la poignée.
        if (!e_->rd) { sans_image_fraiche(dst, rowBytes); e_->source_vivante = false; return; }

        mxlFlowInfo info;
        memset(&info, 0, sizeof info);
        info.version = 1; info.size = sizeof info;
        uint64_t head = 0;
        if (mxlFlowReaderGetInfo(e_->rd, &info) == MXL_STATUS_OK)
            head = info.runtime.headIndex;

        // ★ UN SEUL BUDGET D'ATTENTE PAR CALLBACK, posé ici et partagé par les deux attentes
        //   qui suivent (celle du head, celle des tranches). Deux échéances indépendantes
        //   auraient laissé passer DEUX fois `attente_tranches_s` dans le même callback, soit
        //   jusqu'à deux durées de trame — on aurait vidé la file de sortie pour réparer une
        //   répétition.
        // Cf. le pavé de `Etat` : on n'attend JAMAIS au-delà de ce dont on dispose.
        double budget_s = e_->attente_tranches_s;
        {
            BMDTimeValue t_flux = 0; double vitesse = 0.0;
            if (echelle_ > 0
                && out_->GetScheduledStreamTime(echelle_, &t_flux, &vitesse) == S_OK) {
                const double reste_s = (double)(prochaine_ - t_flux) / (double)echelle_;
                if (reste_s < e_->slack_vu_min_s) e_->slack_vu_min_s = reste_s;
                // La réserve est MESURÉE, pas supposée — cf. `budget_attente_s` et le relevé
                // de `t_travail_max_s`, pris sur les tours où l'on n'a pas attendu.
                const double borne_s = budget_attente_s(e_->attente_tranches_s, reste_s,
                                                        e_->t_travail_max_s);
                if (borne_s < budget_s) { budget_s = borne_s; e_->attente_ecourtee++; }
            }
        }
        e_->attendu_ce_tour_s = 0.0;
        const double t_entree_remplir = horloge_s();
        const double echeance = t_entree_remplir + budget_s;

        // ── ATTENDRE QUE LE PRODUCTEUR OUVRE LE GRAIN SUIVANT ──────────────────────────────
        // ⚠⚠ ON RÉPÉTAIT AUSSITÔT, SANS TOUCHER AU BUDGET DÉJÀ ACCORDÉ. Mesuré sur la Z8 le
        //    28/10 (#231) : la sortie du multiview affichait ~1,9 `repeated`/s alors que le
        //    multiview publiait ses 50 grains/s sans en rater un seul, avec `slice_timeouts` et
        //    `late_previous` à ZÉRO. Le chemin est celui-ci : quand le callback arrive un cheveu
        //    AVANT que le producteur ait ouvert le grain suivant, `head == dernier_index` ; le
        //    chemin par tranches est alors sauté (sa condition l'exclut), le repli sort
        //    immédiatement sur `idx == dernier_index`, et on tombe sur `repetees++` — sans avoir
        //    attendu une microseconde. Le callback suivant trouve DEUX grains neufs et n'en émet
        //    qu'un : la répétition coûte donc AUSSI un grain source, jamais émis.
        //    Et aucun compteur d'attente ne bougeait, puisqu'aucune attente n'avait lieu : c'est
        //    pour ça que la cause s'est laissé chercher.
        // ★ On attend donc, DANS le budget, que `head` avance — et seulement si la source est
        //   vivante : devant un producteur arrêté, `head` est immobile pour de bon et dépenser
        //   le budget à chaque trame ne rendrait rien. `source_vivante` tombe après ~1 s de
        //   répétition, ce qui sort tout seul de l'attente inutile.
        // ★ LES DEUX ISSUES SE COMPTENT SÉPARÉMENT, parce qu'elles ne disent pas la même chose :
        //   `head_awaited` = on a attendu et le grain est arrivé (répétition ÉVITÉE, et c'est le
        //   chiffre qui mesure ce correctif) ; `head_frozen` = l'échéance a expiré sans que
        //   `head` bouge (le producteur est vraiment en retard — là, répéter est la bonne
        //   réponse, et le chiffre désigne le producteur, plus la sortie).
        if (head && head == e_->dernier_index && e_->source_vivante) {
            const double t_debut_attente = horloge_s();
            while (horloge_s() < echeance) {
                usleep(150);
                mxlFlowInfo i2;
                memset(&i2, 0, sizeof i2);
                i2.version = 1; i2.size = sizeof i2;
                if (mxlFlowReaderGetInfo(e_->rd, &i2) != MXL_STATUS_OK) break;
                if (i2.runtime.headIndex != head) {
                    head = i2.runtime.headIndex;
                    e_->tete_attendue++;
                    break;
                }
            }
            if (head == e_->dernier_index) e_->tete_immobile++;
            const double attendu_s = horloge_s() - t_debut_attente;
            e_->attendu_ce_tour_s += attendu_s;
            const double attendu_ms = attendu_s * 1000.0;
            if (attendu_ms > e_->t_attente_max_ms) e_->t_attente_max_ms = attendu_ms;
        }

        // ── ENTRELACÉ : À DÉTECTER AVANT TOUT LE RESTE ─────────────────────────────────────
        // ⚠ LA GARDE DE TAILLE ÉTAIT PLUS BAS, ET C'ÉTAIT TROP TARD. Le chemin « mode tranche »
        //   ci-dessous traite un flux à `totalSlices == 1` comme une bande unique de pleine
        //   hauteur : il lisait donc 1080 lignes dans un grain qui en contient 540, bien avant
        //   d'atteindre le contrôle. D'où le SIGSEGV (22 relances en une minute, mesuré le
        //   2026-10-02). Une garde placée après le site qu'elle protège ne protège rien.
        //
        //   La détection se fait sur la TAILLE du grain, pas sur un réglage : c'est le flux qui
        //   sait ce qu'il contient, et un réglage mal saisi nous ferait déborder à nouveau.
        if (head && !e_->entrelace_connu) {
            mxlGrainInfo g0; uint8_t *p0 = NULL;
            if (mxlFlowReaderGetGrainNonBlocking(e_->rd, head, &g0, &p0) == MXL_STATUS_OK
                && g0.grainSize) {
                const size_t plein = ((size_t)e_->w * e_->h
                                    + (size_t)(e_->w / 2) * e_->h * 2u) * octets();
                if ((size_t)g0.grainSize < plein) {
                    if ((size_t)g0.grainSize * 2u >= plein) {
                        e_->entrelace = true;      // grain = demi-trame → flux entrelacé
                    } else {
                        e_->grains_courts++;       // ni trame ni champ : on ne lit RIEN
                        return;
                    }
                }
                e_->entrelace_connu = true;
            }
        }
        if (e_->entrelace) {
            if (tisser_champs(head, (uint8_t *)dst, rowBytes)) {
                e_->dernier_index = head;
                e_->source_vivante = true;
                e_->champs_tisses++;
            } else {
                sans_image_fraiche(dst, rowBytes);  // champs indisponibles : on ré-émet
            }
            return;
        }

        // ── MODE TRANCHE ───────────────────────────────────────────────────────────────────
        // ⚠ CE TX COÛTAIT UNE TRAME, ET AUCUN COMPTEUR NE LE DISAIT. Il refusait tout grain
        //   incomplet : face à un producteur qui committe progressivement, `head` est le grain
        //   EN COURS d'écriture, donc rejeté — on émettait `head-1`, c'est-à-dire la trame
        //   précédente. La cadence restait parfaite et `slip_hours` à zéro : la dette ne se
        //   voyait nulle part, exactement ce que la règle du dépôt décrit.
        //
        //   On suit désormais `head` et on empaquette chaque bande DÈS qu'elle est valide, en
        //   attendant les suivantes. Attendre ici est sûr : la carte a déjà `preroll` trames en
        //   file. L'attente est BORNÉE — `attente_tranches_s`, réglable par sortie, plafonnée à
        //   la durée d'une trame (cf. le pavé dans `Etat`) — et son échec retombe sur l'ancien
        //   comportement plutôt que d'émettre une demi-trame.
        if (head && head != e_->dernier_index) {
            mxlGrainInfo gi; uint8_t *payload = NULL;
            int faites = 0, total = 0, haut_tranche = 0;
            long *motif = NULL;                   // pourquoi on a abandonné, s'il a fallu
            // `echeance` vient du haut du callback : UN budget pour les deux attentes.
            for (;;) {
                // ⚠⚠ L'ACCESSEUR DE TRANCHE, ET C'EST TOUT LE SUJET. On appelait ici
                //    `mxlFlowReaderGetGrainNonBlocking`, qui est l'accesseur de grain ENTIER :
                //    son propre en-tête dit « expected to wait until the FULL grain is available
                //    […] for partial grain access use mxlFlowReaderGetGrainSlice() ». Il ne rend
                //    donc JAMAIS un grain en cours d'écriture — la boucle ci-dessous ne pouvait
                //    pas voir une bande arriver, elle ne réussissait que si le grain était DÉJÀ
                //    complet au premier regard. Le « mode tranche » de cette sortie n'a donc
                //    jamais suivi personne : il tirait au sort contre la phase du producteur.
                //    Mesuré le 28/10 : sur la Z8, 3 réussites par seconde contre 47 replis sur
                //    `head-1`, avec `slice_timeouts` à 0 puisque l'attente n'était jamais
                //    entamée ; sur dell-1, 60/40 — deux résultats qui ne disent rien d'autre que
                //    deux phases différentes. Tous les lecteurs tranchés du produit passent par
                //    l'accesseur de tranche (`bobimxl.get_slice_nonblocking`) ; ce binaire était
                //    le seul à ne pas le faire.
                const mxlStatus st = mxlFlowReaderGetGrainSliceNonBlocking(
                    e_->rd, head, (uint16_t)(faites + 1), &gi, &payload);
                if (st != MXL_STATUS_OK) {
                    // Pas encore la bande suivante. On ATTEND — c'est le budget `--slice-wait`,
                    // et c'est maintenant qu'il sert : même la PREMIÈRE bande se fait attendre.
                    if (horloge_s() >= echeance) {
                        motif = faites ? &e_->tranches_incompletes : &e_->tete_illisible;
                        break;
                    }
                    usleep(150);
                    continue;
                }
                if (gi.totalSlices == 0) { motif = &e_->tete_sans_tranches; break; }
                total = (int)gi.totalSlices;
                haut_tranche = e_->h / total;                     // hauteur d'une bande, en lignes
                if (haut_tranche <= 0 || haut_tranche * total != e_->h) {
                    motif = &e_->geometrie_refusee; break;
                }
                const int valides = (int)gi.validSlices < total ? (int)gi.validSlices : total;
                if (valides > faites) {
                    if (faites) e_->tranches_attendues++;         // on a VRAIMENT suivi une bande
                    packer_bande(payload, (uint8_t *)dst, rowBytes,
                                 faites * haut_tranche, valides * haut_tranche);
                    faites = valides;
                }
                if (faites >= total) break;
                if (horloge_s() >= echeance) { motif = &e_->tranches_incompletes; break; }
                usleep(150);
            }
            if (total && faites >= total) {
                e_->dernier_index = head;
                e_->source_vivante = true;
                if (total > 1) e_->suivi_tranches++; else e_->suivi_entier++;
                return;
            }
            if (motif) (*motif)++;            // on retombe sur le grain précédent, ci-dessous
        }

        // ── REPLI : le grain COMPLET le plus récent ────────────────────────────────────────
        // Chemin d'origine, conservé tel quel : producteur non tranché qui n'a pas fini à temps,
        // hauteur non divisible par le nombre de tranches, ou grain volatilisé.
        for (int essai = 0; essai < 2 && head; essai++) {
            const uint64_t idx = head - essai;
            if (idx == e_->dernier_index) break;          // rien de neuf : on répétera
            mxlGrainInfo gi; uint8_t *payload = NULL;
            if (mxlFlowReaderGetGrainNonBlocking(e_->rd, idx, &gi, &payload) != MXL_STATUS_OK)
                continue;
            if (gi.validSlices != gi.totalSlices) continue;   // grain incomplet : on n'émet pas ça
            // ★ GARDE DE DÉBORDEMENT — on REFUSE un grain trop court, on ne le lit jamais.
            //   C'est ce qui manquait : un grain de CHAMP lu comme une trame sort de la zone
            //   allouée et tue le processus, sans que rien n'explique pourquoi.
            // Filet : l'entrelacé est tranché plus haut, mais un grain court qui arriverait
            // ici ne doit jamais être LU — on refuse, on ne tronque pas.
            const size_t besoin = ((size_t)e_->w * e_->h
                                 + (size_t)(e_->w / 2) * e_->h * 2u) * octets();
            if (gi.grainSize && (size_t)gi.grainSize < besoin) { e_->grains_courts++; continue; }
            packer_bande(payload, (uint8_t *)dst, rowBytes, 0, e_->h);
            e_->dernier_index = idx;
            e_->source_vivante = true;
            if (idx == head) e_->suivi_entier++; else e_->repli_precedent++;
            return;
        }
        // Rien de neuf : la trame conserve son contenu précédent et repart telle quelle.
        sans_image_fraiche(dst, rowBytes);
        if (e_->repetees % 50 == 0) e_->source_vivante = false;   // ~1 s de répétition = source morte
    }

    BMDTimeValue prochaine_ = 0;
private:
    IDeckLinkOutput *out_;
    Etat *e_;
    BMDTimeValue duree_;
    BMDTimeScale echelle_;
    ULONG refs_ = 1;
};

// Profondeur DÉCLARÉE par le flux, lue dans son `flow_def.json`. On ne la déduit pas de la
// taille du grain : un champ entrelacé 10 bits et une trame progressive 8 bits pèsent le même
// nombre d'octets, et les confondre produirait un peigne sur une source progressive.
// Lecture volontairement textuelle : la première `"bit_depth"` qui suit `"components"` est celle
// du plan Y. Embarquer un analyseur JSON pour un entier ne se justifie pas, et un format
// inattendu doit RETOMBER sur 10 (le cas historique) plutôt que de faire échouer la sortie.
// ── AUDIO : du bus MXL vers la carte ───────────────────────────────────────────────────────
// Le bus donne des échantillons float32, UN TAMPON PAR CANAL ; la carte veut un bloc ENTRELACÉ
// d'entiers 32 bits. On convertit ici, une fois par trame, dans un tampon réutilisé.
//
// ★ LE NOMBRE D'ÉCHANTILLONS PAR TRAME N'EST PAS TOUJOURS ENTIER. À 50 Hz il vaut 960 pile ;
//   à 29,97 il vaut 1601,6. Arrondir à chaque trame ferait dériver l'audio de la vidéo de
//   plusieurs millisecondes par minute — inaudible une minute, flagrant au bout d'une heure.
//   On garde donc le RESTE d'une trame à l'autre, et le compte moyen est exact.
static size_t echantillons_de_trame(Etat *e, BMDTimeValue duree, BMDTimeScale echelle) {
    const uint64_t num = 48000ull * (uint64_t)duree + e->reste_ech;
    e->reste_ech = num % (uint64_t)echelle;
    return (size_t)(num / (uint64_t)echelle);
}

/// Dépose `canaux` canaux lus dans `rd` à partir du canal `premier` de la carte. Rend false si
/// le bus n'avait pas ces échantillons — l'appelant décide (le 1er groupe commande, cf. plus bas).
static bool remplir_groupe(Etat *e, mxlFlowReader rd, size_t n, int premier, int canaux,
                           int32_t *dst) {
    mxlWrappedMultiBufferSlice tranches;
    memset(&tranches, 0, sizeof tranches);
    if (mxlFlowReaderGetSamplesNonBlocking(rd, e->idx_audio, n, &tranches) != MXL_STATUS_OK
            || tranches.count == 0)
        return false;
    const size_t nc = (size_t)canaux < tranches.count ? (size_t)canaux : tranches.count;
    for (size_t c = 0; c < nc; c++) {
        size_t ecrit = 0;
        for (int f = 0; f < 2 && ecrit < n; f++) {
            // Chaque canal est un anneau : la plage demandée peut enjamber le point de bouclage,
            // d'où DEUX fragments. En ignorer un, c'est perdre la fin de chaque trame par
            // intermittence — un craquement périodique dont la cause est introuvable à l'oreille.
            const unsigned char *base =
                (const unsigned char *)tranches.base.fragments[f].pointer;
            if (!base) continue;
            const float *src = (const float *)(base + c * tranches.stride);
            size_t dispo = tranches.base.fragments[f].size / sizeof(float);
            for (size_t i = 0; i < dispo && ecrit < n; i++, ecrit++) {
                float v = src[i];
                // ★ L'ÉCRÊTAGE EST COMPTÉ, PAS SILENCIEUX (20/10). Le bus porte du flottant, qui
                //   accepte des valeurs AU-DELÀ du plein échelle ; une sortie SDI ne le peut
                //   pas. Mesuré sur le banc le jour même : le flux du player crêtait à 1,502,
                //   soit +3,5 dBFS, et nous écrêtions sans un mot — l'exploitant a cherché la
                //   panne dans la carte alors qu'elle était dans la source. Un écrêtage qu'on
                //   ne compte pas désigne le mauvais équipement.
                if (v > 1.0f)  { v = 1.0f;  e->audio_ecretes++; }
                if (v < -1.0f) { v = -1.0f; e->audio_ecretes++; }
                dst[ecrit * (size_t)e->canaux_audio + (size_t)premier + c] =
                    (int32_t)(v * 2147483392.0f);
            }
        }
    }
    return true;
}

/// Ouvre un flux audio du bus et DIT ce qu'il déclare. `canaux_voulus` = ce qu'on compte y lire.
static bool ouvrir_flux_audio(Etat *e, const std::string &nom, int canaux_voulus,
                              mxlFlowReader *rd, int *declares, bool bavard) {
    char fid[37]; flow_id_str(nom.c_str(), fid);
    if (mxlCreateFlowReader(e->mxl, fid, NULL, rd) != MXL_STATUS_OK) {
        *rd = NULL;
        return false;
    }
    const char *dom = getenv("MXL_DOMAIN") ? getenv("MXL_DOMAIN") : "/dev/shm/mxl";
    *declares = canaux_declares_du_flux(dom, fid);
    // ★★ ON COMPARE CE QU'ON VEUT LIRE À CE QUE LE FLUX DÉCLARE, ET ON LE DIT. Sans cette
    //    ligne, une sortie réglée sur 16 canaux devant un flux de 8 remplit 8 canaux et laisse
    //    les 8 autres muets, tous compteurs au vert : le 21/10, le testeur a cherché la panne
    //    dans son câble pendant une heure.
    if (*declares && *declares < canaux_voulus)
        fprintf(stderr, "flux audio « %s » : %d canaux déclarés alors que %d sont attendus — "
                        "les canaux au-delà du %d sortiront MUETS\n",
                nom.c_str(), *declares, canaux_voulus, *declares);
    else if (bavard)
        fprintf(stderr, "flux audio « %s » : %d canaux déclarés\n", nom.c_str(), *declares);
    return true;
}

static void servir_audio(Etat *e, IDeckLinkOutput *out, BMDTimeValue t,
                         BMDTimeValue duree, BMDTimeScale echelle) {
    if (e->canaux_audio <= 0) return;
    // ★★ ON RÉESSAIE D'OUVRIR LE FLUX AUDIO TANT QU'IL EST DEMANDÉ (21/10, cf. l'état).
    //    La carte a été configurée pour l'audio au démarrage — c'est le NOMBRE DE CANAUX qui le
    //    décide, et il est DÉCLARÉ, pas déduit du bus. Il ne reste donc qu'à prendre le flux
    //    quand il apparaît : rien à rouvrir côté matériel, et le flux étant horodaté, les
    //    échantillons se raccrochent à la trame courante sans re-préchargement.
    if (!e->rd_audio) {
        if (e->flux_audio_voulu.empty()) return;
        // Une tentative par seconde environ : ouvrir à chaque trame martèlerait le bus pour
        // rien, et ce code tourne DANS le rappel de sortie — il ne doit pas coûter une trame.
        if (e->audio_attentes++ % 50) return;
        if (!ouvrir_flux_audio(e, e->flux_audio_voulu, e->canaux_g1,
                               &e->rd_audio, &e->canaux_declares, false))
            return;
        // La position se ré-ancre au premier service : celle d'avant, s'il y en avait une,
        // désigne un endroit du bus que ce nouveau flux n'a jamais écrit.
        e->idx_audio = 0;
        e->reste_ech = 0;
        e->audio_ouvertures++;
        fprintf(stderr, "flux audio « %s » apparu (après %ld trames) — la sortie embarque "
                        "l'audio\n", e->flux_audio_voulu.c_str(), e->audio_attentes - 1);
    }
    // Le second groupe se rattrape SÉPARÉMENT : il peut naître après le premier, et son
    // absence ne doit jamais faire taire les canaux 1-8.
    if (!e->rd_audio2 && !e->flux_audio2_voulu.empty() && !(e->audio_attentes2++ % 50))
        ouvrir_flux_audio(e, e->flux_audio2_voulu, e->canaux_g2,
                          &e->rd_audio2, &e->canaux_declares2, false);
    const size_t n = echantillons_de_trame(e, duree, echelle);
    if (!n) return;

    // ★★ LA POSITION DE LECTURE EST CONTINUE, ELLE NE SE REDEMANDE PAS À CHAQUE TRAME (20/10).
    //    Première version : « où en est-on maintenant ? » à chaque trame, puis on lit les n
    //    derniers échantillons. Entre deux trames il manquait ou il se répétait alors quelques
    //    échantillons — UNE DISCONTINUITÉ PAR TRAME, soit 50 clics par seconde à 50 i/s.
    //    Entendu en recette dès la première écoute.
    //    On ancre donc UNE FOIS, puis on avance d'exactement `n` échantillons par trame : le
    //    compte est juste par construction, et la forme d'onde est continue d'une trame à
    //    l'autre.
    const mxlRational cadence_audio = { 48000, 1 };
    const uint64_t tete = mxlGetCurrentIndex(&cadence_audio);
    // ★★ ON LIT EN RETRAIT D'UNE TRAME, PAS « JUSQU'À MAINTENANT » (20/10, signalé en recette :
    //    du son avec le player, aucun avec une entrée de carte — même code).
    //    `mxlGetCurrentIndex` rend l'instant PRÉSENT sur la grille audio. Un PLAYER écrit ses
    //    échantillons d'avance : demander « maintenant » tombe sur du déjà-écrit, et ça marche.
    //    Une CAPTURE, elle, écrit APRÈS avoir capté — « maintenant » n'existe pas encore sur le
    //    bus. La lecture ne rendait donc rien, aucun son n'était programmé, et c'était un
    //    SILENCE SANS ERREUR : le genre de panne qu'on cherche à l'autre bout de la chaîne.
    //    Une trame de retrait suffit à couvrir l'écart d'écriture, et coûte exactement une trame
    //    de latence audio — le même ordre de grandeur que la pré-charge vidéo.
    const uint64_t retrait = (uint64_t)n;
    if (!e->idx_audio) e->idx_audio = (tete > retrait) ? (tete - retrait) : tete;
    // ⚠ MAIS UNE POSITION LIBRE DÉRIVE. Nos trames sont cadencées par la RÉFÉRENCE de la carte,
    //   la grille audio par l'horloge TAI du nœud : les deux ne sont pas le même oscillateur, et
    //   l'écart grandit sans borne. Au-delà d'un dixième de seconde on se recale — et on le
    //   COMPTE, pour qu'un recalage fréquent se voie au lieu de passer pour un craquement
    //   inexpliqué.
    // L'écart NORMAL vaut désormais le retrait : on mesure la dérive autour de lui, pas autour
    // de zéro, sinon on se recalerait en permanence sur un décalage voulu.
    const int64_t ecart = (int64_t)tete - (int64_t)e->idx_audio - (int64_t)retrait;
    if (ecart > 4800 || ecart < -4800) {
        e->idx_audio = (tete > retrait) ? (tete - retrait) : tete;
        e->audio_reancrages++;
    }
    if (e->tampon_audio.size() < n * (size_t)e->canaux_audio)
        e->tampon_audio.resize(n * (size_t)e->canaux_audio);
    int32_t *dst = e->tampon_audio.data();
    memset(dst, 0, n * (size_t)e->canaux_audio * sizeof(int32_t));

    // ⚠ LE PREMIER GROUPE COMMANDE. S'il n'a pas ses échantillons, on ne programme RIEN plutôt
    //   que du silence inventé : un trou d'audio s'entend et se cherche, du silence poussé à la
    //   place d'un trou se confond avec une source muette, et on va chercher la panne à l'autre
    //   bout de la chaîne.
    if (!remplir_groupe(e, e->rd_audio, n, 0, e->canaux_g1, dst)) {
        e->audio_sautes++;
        return;
    }
    // Le second, lui, ne peut que laisser SES canaux à zéro — et on le compte, pour que
    // « 9-16 muets » ne se lise pas comme « la source est muette ».
    if (e->rd_audio2 && !remplir_groupe(e, e->rd_audio2, n, 8, e->canaux_g2, dst))
        e->audio_sautes2++;

    uint32_t servis = 0;
    if (out->ScheduleAudioSamples(dst, (uint32_t)n, t, echelle, &servis) == S_OK) {
        e->ech_audio += (long)servis;
        e->idx_audio += n;           // on avance d'un compte EXACT, jamais d'une re-lecture
    } else {
        e->audio_sautes++;
    }
}

/// `channel_count` déclaré par un flux AUDIO du bus, ou 0 si illisible. Même lecture que
/// `profondeur_declaree` : le fichier de définition, pas une déduction sur la taille.
static int canaux_declares_du_flux(const char *domaine, const char *fid) {
    char chemin[512];
    snprintf(chemin, sizeof chemin, "%s/%s.mxl-flow/flow_def.json", domaine, fid);
    FILE *f = fopen(chemin, "rb");
    if (!f) return 0;
    std::string txt;
    char tampon[4096];
    size_t n;
    while ((n = fread(tampon, 1, sizeof tampon, f)) > 0) txt.append(tampon, n);
    fclose(f);
    size_t c = txt.find("\"channel_count\"");
    if (c == std::string::npos) return 0;
    c = txt.find(':', c);
    if (c == std::string::npos) return 0;
    const int v = atoi(txt.c_str() + c + 1);
    return (v > 0 && v <= 64) ? v : 0;
}

static int profondeur_declaree(const char *domaine, const char *fid) {
    char chemin[512];
    snprintf(chemin, sizeof chemin, "%s/%s.mxl-flow/flow_def.json", domaine, fid);
    FILE *f = fopen(chemin, "rb");
    if (!f) return 0;                       // 0 = inconnue, l'appelant décide
    std::string txt;
    char tampon[4096];
    size_t n;
    while ((n = fread(tampon, 1, sizeof tampon, f)) > 0) txt.append(tampon, n);
    fclose(f);
    size_t c = txt.find("\"components\"");
    if (c == std::string::npos) return 0;
    size_t b = txt.find("\"bit_depth\"", c);
    if (b == std::string::npos) return 0;
    b = txt.find(':', b);
    if (b == std::string::npos) return 0;
    int v = atoi(txt.c_str() + b + 1);
    return (v == 8 || v == 10 || v == 12) ? v : 0;
}


int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <persistent_id> <flux_source> <mode> [secondes] "
                        "[--stats <f>] [--preroll <n>] [--slice-wait=<ms>]\n"
                        "  <mode> : nom de mode DeckLink, ex. 1080p50 / Hp50 — voir tools/out\n", argv[0]);
        return 1;
    }
    const int64_t vise = strtoll(argv[1], NULL, 0);
    const char *flux = argv[2];
    const char *mode_voulu = argv[3];
    const char *flux_audio = NULL;
    const char *flux_audio2 = NULL;
    int canaux_audio = 0;
    int secondes = 10, preroll = 3;
    const char *stats = NULL;
    const char *mode_fichier = NULL;
    // −1 = non demandé. On NE recopie PAS le défaut ici : il vit dans `Etat` et nulle part
    // ailleurs. La pré-charge a déjà payé le prix d'un défaut écrit à deux endroits.
    double attente_ms = -1.0;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--stats") && i + 1 < argc) { stats = argv[++i]; continue; }
        if (!strcmp(argv[i], "--preroll") && i + 1 < argc) { preroll = atoi(argv[++i]); continue; }
        // --slice-wait=<ms> : attente maximale des tranches du grain `head`. Cf. `Etat`.
        //
        // ⚠⚠ LA FORME EN UN SEUL JETON N'EST PAS UNE COQUETTERIE, ELLE ÉVITE UNE COUPURE
        //    D'ANTENNE, et c'est MESURÉ (28/10). Le binaire est compilé dans l'image de calcul :
        //    un redeploiement de plugin pousse `script.py` SEUL, donc un script neuf peut
        //    parfaitement parler à un binaire d'avant. Or le dernier `else` de cette boucle lit
        //    tout jeton inconnu comme la DURÉE EN SECONDES :
        //      « --slice-wait 25 » → l'ancien binaire comprend `secondes = 25` et S'ARRÊTE au bout
        //      de 25 s. Le superviseur le relance, il s'arrête encore : une sortie SDI qui cycle
        //      toutes les 25 secondes, en direct, sans qu'un seul compteur ne l'annonce.
        //      « --slice-wait=25 » → `atoi` rend 0, soit « sans limite » : exactement ce que le
        //      script passe déjà en positionnel. L'ancien binaire IGNORE l'option, et rien ne
        //      change pour lui.
        //    `script.py` n'émet donc QUE la forme collée. Les deux sont acceptées ici, parce
        //    qu'on ne va pas rendre la ligne de commande désagréable à taper à la main.
        // --missing-frame-mode-file=<chemin> : canal latéral du mode « image manquante ».
        // ⚠ FORME COLLÉE SEULEMENT, pour la raison écrite juste au-dessus : sur un binaire
        //   d'avant, `atoi("--missing-frame-mode-file=/…")` rend 0, soit « sans limite », et
        //   l'option est simplement ignorée. La forme séparée ferait s'arrêter l'ancien binaire.
        if (!strncmp(argv[i], "--missing-frame-mode-file=", 26)) {
            mode_fichier = argv[i] + 26; continue;
        }
        if (!strncmp(argv[i], "--slice-wait=", 13)) { attente_ms = atof(argv[i] + 13); continue; }
        if (!strcmp(argv[i], "--slice-wait") && i + 1 < argc) { attente_ms = atof(argv[++i]); continue; }
        // --audio <flux> <canaux> : le flux audio du bus à embarquer dans le signal SDI.
        if (!strcmp(argv[i], "--audio") && i + 2 < argc) {
            flux_audio = argv[++i];
            canaux_audio = atoi(argv[++i]);
            continue;
        }
        // --audio-2 <flux> : canaux 9-16, SECOND flux de 8. Séparé de `--audio` plutôt que
        // deviné d'un nom dérivé : un groupe peut venir d'une tout autre source que l'autre
        // (une sortie peut embarquer le programme sur 1-8 et des retours sur 9-16).
        if (!strcmp(argv[i], "--audio-2") && i + 1 < argc) {
            flux_audio2 = argv[++i];
            continue;
        }
        secondes = atoi(argv[i]);
    }
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    if (charger_v210() != 0) return 6;

    IDeckLinkIterator *it = CreateDeckLinkIteratorInstance();
    if (!it) { fprintf(stderr, "SDK indisponible\n"); return 2; }
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
                fprintf(stderr, "sous-périphérique %lld INACTIF dans le profil courant — "
                                "son connecteur est éteint\n", (long long)vise);
                dev->Release(); it->Release(); return 3;
            }
            if (!(io & bmdDeviceSupportsPlayback)) {
                fprintf(stderr, "sous-périphérique %lld : pas de lecture\n", (long long)vise);
                dev->Release(); it->Release(); return 3;
            }
            choisi = dev; continue;
        }
        dev->Release();
    }
    it->Release();
    if (!choisi) { fprintf(stderr, "persistent id %lld introuvable\n", (long long)vise); return 3; }

    Etat e; e.flux = flux;
    // ★ LU UNE PREMIÈRE FOIS TOUT DE SUITE, pas à la première image manquante : sans ça, la
    //   toute première répétition partirait en `repeat` même si l'exploitant a demandé `black`,
    //   et c'est exactement l'image qu'il regarde quand il vient d'armer le mode.
    if (mode_fichier) { e.mode_fichier = mode_fichier; relire_mode(&e); e.t_mode = 0.0; }
    e.preroll = preroll;        // l'avance voulue, pour le ré-ancrage de l'échéance
    e.mxl = mxlCreateInstance(getenv("MXL_DOMAIN") ? getenv("MXL_DOMAIN") : "/dev/shm/mxl", NULL);
    if (!e.mxl) { fprintf(stderr, "mxlCreateInstance a échoué\n"); return 4; }
    char fid[37]; flow_id_str(flux, fid);
    if (mxlCreateFlowReader(e.mxl, fid, NULL, &e.rd) != MXL_STATUS_OK) {
        fprintf(stderr, "flux « %s » (%s) introuvable — il doit exister AVANT le TX\n", flux, fid);
        // ★ Code DISTINCT : c'est la faute d'exploitation la plus probable (un nom de flux mal
        //   saisi dans une macro de routage). Le superviseur doit pouvoir la NOMMER, au lieu de
        //   n'afficher qu'un code d'arrêt sur une boucle de relance.
        return 7;
    }

    // ── Lecteur AUDIO (facultatif) ────────────────────────────────────────────────────────
    // ⚠ L'ABSENCE DU FLUX AUDIO N'ARRÊTE PAS LA VIDÉO. Une sortie à laquelle on vient
    //   d'affecter un flux audio pas encore créé doit continuer à émettre l'image : couper
    //   l'antenne parce qu'il manque du son serait une régression bien pire que le silence.
    //   On le DIT, et on repart sans audio.
    if (flux_audio && *flux_audio && canaux_audio > 0) {
        e.canaux_audio = canaux_audio;
        e.flux_audio_voulu = flux_audio;
        // Les canaux de la CARTE se répartissent en groupes de 8 : le 1er flux porte 1-8, le
        // 2e porte 9-16. Sans second flux câblé, les canaux 9-16 sortent muets — et le disent.
        e.canaux_g1 = canaux_audio > 8 ? 8 : canaux_audio;
        e.canaux_g2 = canaux_audio > 8 ? canaux_audio - 8 : 0;
        if (flux_audio2 && *flux_audio2 && e.canaux_g2 > 0)
            e.flux_audio2_voulu = flux_audio2;
        if (!ouvrir_flux_audio(&e, e.flux_audio_voulu, e.canaux_g1,
                               &e.rd_audio, &e.canaux_declares, true)) {
            // ⚠ PAS UN VERDICT DÉFINITIF : le flux peut n'être pas encore créé. On le DIT, on
            //   configure l'audio de la carte quand même, et on réessaie en cours de route.
            fprintf(stderr, "flux audio « %s » pas encore là — l'image part, l'audio sera pris "
                            "dès qu'il apparaît\n", flux_audio);
        } else {
            fprintf(stderr, "audio embarqué : flux « %s », canaux 1-%d de %d, 48 kHz\n",
                    flux_audio, e.canaux_g1, canaux_audio);
        }
        if (!e.flux_audio2_voulu.empty()) {
            if (ouvrir_flux_audio(&e, e.flux_audio2_voulu, e.canaux_g2,
                                  &e.rd_audio2, &e.canaux_declares2, true))
                fprintf(stderr, "audio embarqué : flux « %s », canaux 9-%d\n",
                        flux_audio2, canaux_audio);
            else
                fprintf(stderr, "flux audio « %s » (canaux 9-%d) pas encore là — il sera pris "
                                "dès qu'il apparaît\n", flux_audio2, canaux_audio);
        } else if (e.canaux_g2 > 0) {
            // ★ NOMMER CE CAS : c'est EXACTEMENT la panne du 21/10 sur Quad SDI 1. 16 canaux
            //   ouverts sur la carte, un seul flux de 8 câblé, et rien ne le disait.
            fprintf(stderr, "ATTENTION : %d canaux ouverts sur la carte mais AUCUN second flux "
                            "câblé — les canaux 9-%d sortiront MUETS\n",
                    canaux_audio, canaux_audio);
        }
    }

    // ★★ LA PROFONDEUR SE LIT APRÈS L'OUVERTURE DU LECTEUR, ET ON N'EN DEVINE PAS (2026-10-19).
    //    Elle était lue AVANT : si le flux était en train d'être créé ou RE-créé, le fichier de
    //    définition manquait l'espace d'un instant, on retombait sur « 10 par défaut » et on
    //    émettait toute la session en lisant du 8 comme du 10 — image doublée horizontalement,
    //    dominante verte, et tous les compteurs au vert. Constaté dans le journal :
    //        flux dl_b3 : planar 8 bits
    //        flux dl_b3 : planar 10 bits (non déclaré — 10 par défaut)   ← la course
    //        flux dl_b3 : planar 8 bits
    //    Le lecteur ouvert PROUVE que le flux existe ; sa définition doit donc être lisible. On
    //    réessaie brièvement, et si elle reste illisible on REFUSE DE DÉMARRER : émettre une
    //    image fausse en silence est pire que ne rien émettre — personne ne cherche une panne
    //    sur une sortie qui annonce 50 i/s, genlock verrouillé, zéro retard.
    {
        const char *dom = getenv("MXL_DOMAIN") ? getenv("MXL_DOMAIN") : "/dev/shm/mxl";
        int d = 0;
        for (int essai = 0; essai < 20 && !d; essai++) {
            d = profondeur_declaree(dom, fid);
            if (!d) usleep(100000);
        }
        if (!d) {
            fprintf(stderr, "flux « %s » : profondeur non déclarée après 2 s — on ne démarre "
                            "PAS plutôt que d'émettre une image fausse en silence\n", flux);
            return 8;
        }
        e.bits_src = d;
        fprintf(stderr, "flux %s : planar %d bits\n", flux, e.bits_src);
    }

    IDeckLinkOutput *out = NULL;
    if (choisi->QueryInterface(IID_IDeckLinkOutput, (void **)&out) != S_OK) {
        fprintf(stderr, "pas d'interface de sortie\n"); return 3;
    }
    // Le mode est DEMANDÉ, jamais deviné : « le premier supporté » tombe sur du NTSC SD.
    IDeckLinkDisplayModeIterator *mit = NULL; IDeckLinkDisplayMode *m = NULL, *retenu = NULL;
    std::vector<std::string> dispo;
    if (out->GetDisplayModeIterator(&mit) == S_OK) {
        while (mit->Next(&m) == S_OK) {
            const char *nm = NULL;
            std::string n;
            if (m->GetName(&nm) == S_OK && nm) { n = nm; free((void *)nm); }
            dispo.push_back(n);
            if (!retenu && n == mode_voulu) { retenu = m; continue; }
            m->Release();
        }
        mit->Release();
    }
    if (!retenu) {
        // Lister plutôt que de renvoyer « introuvable » sec : sans la liste, l'exploitant doit
        // deviner l'orthographe exacte du mode, et il n'a aucun moyen de la connaître.
        fprintf(stderr, "mode « %s » introuvable. Modes de ce sous-périphérique :\n", mode_voulu);
        for (size_t i = 0; i < dispo.size(); i++) fprintf(stderr, "  %s\n", dispo[i].c_str());
        return 3;
    }
    e.w = (int)retenu->GetWidth(); e.h = (int)retenu->GetHeight();
    {   // Dominance de champ du mode de SORTIE : c'est elle qui dit où va le premier champ.
        const BMDFieldDominance fd = retenu->GetFieldDominance();
        e.tff = (fd != bmdLowerFieldFirst);
    }
    BMDTimeValue duree = 0; BMDTimeScale echelle = 0;
    retenu->GetFrameRate(&duree, &echelle);
    // ★ LE PLAFOND NE PEUT PAS ÊTRE POSÉ PLUS TÔT : il vaut la durée d'une trame, et le mode
    //   n'est retenu qu'ici. Un réglage silencieusement ramené serait un réglage qui mente, donc
    //   on DIT qu'on l'a ramené, et c'est la valeur EFFECTIVE qui part dans les statistiques.
    const double trame_s = duree / (double)echelle;
    if (attente_ms >= 0.0) e.attente_tranches_s = attente_ms / 1000.0;
    if (e.attente_tranches_s > trame_s) {
        printf("attente des tranches : %.1f ms demandée, ramenée à %.1f ms "
               "(durée d'une trame)\n", e.attente_tranches_s * 1000.0, trame_s * 1000.0);
        e.attente_tranches_s = trame_s;
    }
    if (e.attente_tranches_s < 0.0) e.attente_tranches_s = 0.0;
    printf("sortie %dx%d @ %.3f i/s, pré-charge %d, attente des tranches %.1f ms, source « %s »\n",
           e.w, e.h, echelle / (double)duree, preroll, e.attente_tranches_s * 1000.0, flux);

    // ★ BASSE LATENCE DE SORTIE — le seul réglage de latence que la carte expose, et il n'était
    //   pas posé. `bmdDeckLinkConfigLowLatencyVideoOutput` réduit le tampon interne de la carte
    //   entre la trame programmée et le fil. C'est gratuit et réversible ; on DIT s'il n'a pas
    //   été accepté plutôt que de le supposer actif — un réglage de latence qu'on croit posé et
    //   qui ne l'est pas, c'est une mesure faussée pour longtemps.
    {
        IDeckLinkConfiguration *cfg = NULL;
        if (choisi->QueryInterface(IID_IDeckLinkConfiguration, (void **)&cfg) == S_OK && cfg) {
            const HRESULT hr = cfg->SetFlag(bmdDeckLinkConfigLowLatencyVideoOutput, true);
            e.basse_latence = (hr == S_OK);
            printf("basse latence de sortie : %s\n",
                   hr == S_OK ? "activée" : "REFUSÉE par la carte (réglage ignoré)");
            fflush(stdout);
            cfg->Release();
        } else {
            printf("basse latence de sortie : interface de configuration indisponible\n");
        }
    }

    if (out->EnableVideoOutput(retenu->GetDisplayMode(), bmdVideoOutputFlagDefault) != S_OK) {
        fprintf(stderr, "EnableVideoOutput a échoué\n"); return 4;
    }
    // ★ LE SDK N'ACCEPTE QUE 2, 8 OU 16 CANAUX. Une autre valeur n'est pas « arrondie au plus
    //   proche » : on refuse l'audio et on le dit. Annoncer 6 canaux et en livrer 8 décalerait
    //   silencieusement toutes les pistes d'une régie.
    // ★ L'AUDIO DE LA CARTE SE CONFIGURE SUR LES CANAUX DEMANDÉS, PAS SUR LA PRÉSENCE D'UN
    //   LECTEUR : sans ça, une sortie démarrée avant son flux audio n'aurait jamais de piste
    //   audio sur le fil, même quand le flux finit par apparaître (cf. l'état).
    if (e.canaux_audio > 0) {
        if (e.canaux_audio != 2 && e.canaux_audio != 8 && e.canaux_audio != 16) {
            fprintf(stderr, "audio : %d canaux refusés par le SDK (2, 8 ou 16) — sortie SANS "
                            "audio\n", e.canaux_audio);
            e.rd_audio = NULL; e.rd_audio2 = NULL; e.canaux_audio = 0;
            e.canaux_g1 = e.canaux_g2 = 0;
            e.flux_audio_voulu.clear(); e.flux_audio2_voulu.clear();
        } else if (out->EnableAudioOutput(bmdAudioSampleRate48kHz, bmdAudioSampleType32bitInteger,
                                          (uint32_t)e.canaux_audio,
                                          bmdAudioOutputStreamTimestamped) != S_OK) {
            fprintf(stderr, "EnableAudioOutput a échoué — la sortie émet SANS audio\n");
            e.rd_audio = NULL; e.rd_audio2 = NULL; e.canaux_audio = 0;
            e.canaux_g1 = e.canaux_g2 = 0;
            e.flux_audio_voulu.clear(); e.flux_audio2_voulu.clear();
        }
    }
    BMDReferenceStatus ref = (BMDReferenceStatus)0;
    out->GetReferenceStatus(&ref);
    printf("genlock : %s\n", (ref & bmdReferenceLocked) ? "verrouillé" : "ABSENT (quartz carte)");

    Sortie *cb = new Sortie(out, &e, duree, echelle);
    out->SetScheduledFrameCompletionCallback(cb);
    std::vector<IDeckLinkMutableVideoFrame *> trames;
    // ★★ LA PRÉ-CHARGE EST LA PROFONDEUR DE FILE, ET RIEN D'AUTRE (2026-10-02).
    //    On mettait `preroll + 2` trames en file. Ce « + 2 » n'était écrit nulle part ailleurs,
    //    n'avait jamais été mesuré, et coûtait 80 ms — à 25 i/s, deux trames. Mesuré sur trois
    //    profondeurs : la latence de bout en bout vaut 40 ms x (file) + 40 ms, le second terme
    //    étant la capture. Autrement dit, sur les 240 ms par défaut, 200 étaient NOTRE file.
    //
    //    Un réglage nommé « pré-charge » qui en met deux de plus que demandé est un réglage qui
    //    ment : l'exploitant croit acheter 3 trames de marge, il en paie 5. On met donc
    //    exactement ce qui est demandé, et le chiffre affiché dans l'interface devient vrai.
    //
    //    ⚠ CE QUE LE « + 2 » PROTÉGEAIT reste à établir : une file trop courte peut affamer
    //    l'ordonnanceur et faire sauter des trames. C'est `missed` qui tranche, pas une opinion.
    for (int i = 0; i < preroll; i++) {
        IDeckLinkMutableVideoFrame *f = NULL;
        if (out->CreateVideoFrame(e.w, e.h, ((e.w + 47) / 48) * 128, bmdFormat10BitYUV,
                                  bmdFrameFlagDefault, &f) != S_OK) break;
        cb->remplir(f);
        out->ScheduleVideoFrame(f, cb->prochaine_, duree, echelle);
        // ★ L'AUDIO SE PRÉ-CHARGE AVEC LA VIDÉO. Programmé seulement à partir de la première
        //   trame servie, il démarrerait avec le retard de toute la pré-charge — soit 40 ms par
        //   trame de file, un décalage lèvres/son parfaitement audible dès 2 trames.
        servir_audio(&e, out, cb->prochaine_, duree, echelle);
        cb->prochaine_ += duree;
        trames.push_back(f);
    }
    if (trames.empty()) { fprintf(stderr, "création de trames impossible\n"); return 4; }
    e.profondeur = (int)trames.size();
    out->StartScheduledPlayback(0, echelle, 1.0);

    const double t0 = horloge_s();
    while (!g_stop && (secondes <= 0 || horloge_s() - t0 < secondes)) {
        usleep(200000);
        if (e.t_fenetre > 0.0 && horloge_s() - e.t_fenetre > 1.5) e.fps = 0.0;
        ecrire_stats(&e, stats);
    }
    out->StopScheduledPlayback(0, NULL, 0);
    out->DisableVideoOutput();
    out->SetScheduledFrameCompletionCallback(NULL);
    printf("\n%ld trames émises, %ld RÉPÉTÉES (source sans neuf), %ld en retard/perdues\n",
           e.emises, e.repetees, e.ratees);
    for (size_t i = 0; i < trames.size(); i++) trames[i]->Release();
    cb->Release(); retenu->Release(); out->Release(); choisi->Release();
    if (e.rd_audio2) mxlReleaseFlowReader(e.mxl, e.rd_audio2);
    if (e.rd_audio)  mxlReleaseFlowReader(e.mxl, e.rd_audio);
    mxlReleaseFlowReader(e.mxl, e.rd); mxlDestroyInstance(e.mxl);
    return e.emises > 0 ? 0 : 5;
}
