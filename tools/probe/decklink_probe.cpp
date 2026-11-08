// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 BOBI SAS, France
// Auteur : Cyril Mazouer, pour le compte de BOBI SAS
// Distribué sous licence GNU GPL v3 (ou ultérieure) ; voir le fichier LICENSE.
//
// Sonde d'énumération DeckLink — jalon 2 du chantier `decklink_io` (docs/chantiers/DECKLINK.md).
//
// Ce qu'elle fait : lister les cartes, leurs sous-périphériques, leurs attributs et leurs
// PROFILS, en JSON sur la sortie standard. Rien d'autre : elle n'ouvre aucun flux, ne change
// aucun profil, n'écrit nulle part. Elle est sans effet de bord PAR CONSTRUCTION — le seul
// appel de l'API qui modifie l'état de la carte (`IDeckLinkProfile::SetActive`) est absent.
//
// ★ Le point qui compte : « aucune carte » et « sonde inopérante » se ressemblent trait pour
//   trait, et une sonde qui les confond ne vaut rien le jour du montage. Trois états DISTINCTS
//   sont donc rendus, et le code de retour les sépare :
//
//     status=ok            (rc 0) : le SDK répond, au moins une carte énumérée
//     status=no_devices    (rc 0) : SDK ET module noyau en place, zéro carte — réponse CORRECTE
//                                   sur une machine sans matériel
//     status=sdk_absent    (rc 2) : libDeckLinkAPI.so introuvable — pilote non installé
//     status=sdk_broken    (rc 3) : bibliothèque présente mais l'itérateur ne se crée pas
//     status=module_absent (rc 4) : la bibliothèque répond, mais AUCUN module noyau blackmagic
//                                   n'est chargé — voir plus bas
//     status=non_enumere   (rc 5) : le NOYAU expose des périphériques (/dev/blackmagic/*) que le
//                                   SDK n'énumère pas — voir plus bas
//     status=enumere_partiel (rc 6) : le noyau en expose PLUS que le SDK n'en énumère — une carte
//                                   au moins est invisible alors qu'une autre répond
//
//   Le dispatch fourni par Blackmagic rend NULL dans les deux derniers cas sans les distinguer
//   (son `IsDeckLinkAPIPresent` est compilé hors service par un `#if 0`), d'où le dlopen fait
//   ici en propre, uniquement pour récupérer le message de dlerror().
//
// ★ `non_enumere` a été ajouté le 2026-09-08, le jour du montage de la première carte. Une
//   DeckLink Duo 2 dont le MICROLOGICIEL est périmé se comporte ainsi : le pilote l'active et
//   peuple `/dev/blackmagic/`, mais le SDK refuse de l'énumérer. La sonde répondait alors
//   « aucune carte, réponse correcte sur une machine sans matériel » — un mensonge, avec une
//   carte dans le châssis. Le noyau et le SDK ne sont pas d'accord : c'est ce désaccord qui est
//   l'information, et il faut le dire au lieu de choisir le camp du SDK.
//
// ★ `module_absent` a été ajouté le 2026-09-07, après le redémarrage de dell-1 : les modules
//   `blackmagic` / `blackmagic_io` ne se rechargent PAS au démarrage tant qu'aucune carte n'est
//   là pour les appeler, et dans cet état la bibliothèque répond parfaitement — elle annonce
//   « zéro carte ». Sur une machine SANS carte, la réponse reste juste ; sur une machine AVEC
//   une carte et le module déchargé, « zéro carte » aurait été un DIAGNOSTIC FAUX, impossible à
//   distinguer de la panne réelle. On lit donc /proc/modules pour le dire.

#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <string>
#include <vector>

#include "decklink_sdk/DeckLinkAPI.h"

static const char *kLib = "libDeckLinkAPI.so";

// GetString rend, sous Linux, une chaîne allouée que l'appelant doit libérer.
static std::string take(const char *s) {
    if (!s) return std::string();
    std::string out(s);
    free(const_cast<char *>(s));
    return out;
}

// Le module noyau est-il chargé ? Sans lui, la bibliothèque répond quand même — et à vide.
// Le chemin est surchargeable pour que la vérification sans carte puisse exercer les deux cas.
static bool module_charge(std::string *quels) {
    const char *path = getenv("BOBI_SDI_MODULES");
    FILE *f = fopen(path ? path : "/proc/modules", "r");
    if (!f) return false;
    char line[512];
    bool trouve = false;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "blackmagic", 10) != 0) continue;
        char *sp = strchr(line, ' ');
        if (sp) *sp = 0;
        if (!quels->empty()) *quels += ",";
        *quels += line;
        trouve = true;
    }
    fclose(f);
    return trouve;
}

// Combien de périphériques le NOYAU expose-t-il ? À confronter à ce que le SDK énumère : les
// deux peuvent être en désaccord, et ce désaccord est un diagnostic à lui seul.
static int noeuds_noyau(void) {
    const char *dir = getenv("BOBI_SDI_DEVDIR");
    DIR *d = opendir(dir ? dir : "/dev/blackmagic");
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (strncmp(e->d_name, "io", 2) == 0 || strncmp(e->d_name, "dv", 2) == 0) n++;
    closedir(d);
    return n;
}

static std::string esc(const std::string &s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += c;
        }
    }
    return o;
}

// Un BMDProfileID est un four-cc : on le rend lisible plutôt qu'en entier.
static std::string fourcc(int64_t v) {
    char b[5] = {0};
    b[0] = (char)((v >> 24) & 0xFF); b[1] = (char)((v >> 16) & 0xFF);
    b[2] = (char)((v >> 8) & 0xFF);  b[3] = (char)(v & 0xFF);
    for (int i = 0; i < 4; i++) if (b[i] < 0x20 || b[i] > 0x7E) return std::string();
    return std::string(b);
}

// Les connecteurs sont un champ de bits : une carte n'est pas « SDI » ou « HDMI », elle
// DÉCLARE ce qu'elle porte. Ne rien supposer — le SDK sert aussi des UltraStudio et des
// Intensity, qui n'ont pas de SDI du tout.
static std::string connexions(int64_t m) {
    static const struct { int64_t bit; const char *nom; } t[] = {
        {bmdVideoConnectionSDI,         "sdi"},
        {bmdVideoConnectionHDMI,        "hdmi"},
        {bmdVideoConnectionOpticalSDI,  "sdi-optique"},
        {bmdVideoConnectionComponent,   "composantes"},
        {bmdVideoConnectionComposite,   "composite"},
        {bmdVideoConnectionSVideo,      "s-video"},
    };
    std::string o;
    for (size_t i = 0; i < sizeof t / sizeof *t; i++)
        if (m & t[i].bit) { if (!o.empty()) o += ","; o += t[i].nom; }
    return o;
}

static const char *duplex_name(int64_t v) {
    switch (v) {
        case bmdDuplexFull:     return "full";
        case bmdDuplexHalf:     return "half";
        case bmdDuplexSimplex:  return "simplex";
        case bmdDuplexInactive: return "inactive";
        default:                return "inconnu";
    }
}

static const char *profile_name(int64_t v) {
    switch (v) {
        case bmdProfileOneSubDeviceFullDuplex:   return "1 sous-périph. full-duplex";
        case bmdProfileOneSubDeviceHalfDuplex:   return "1 sous-périph. half-duplex";
        case bmdProfileTwoSubDevicesFullDuplex:  return "2 sous-périph. full-duplex";
        case bmdProfileTwoSubDevicesHalfDuplex:  return "2 sous-périph. half-duplex";
        case bmdProfileFourSubDevicesHalfDuplex: return "4 sous-périph. half-duplex";
        default:                                 return "inconnu";
    }
}

static void emit_int(IDeckLinkProfileAttributes *a, BMDDeckLinkAttributeID id, const char *key) {
    int64_t v = 0;
    if (a && a->GetInt(id, &v) == S_OK) printf("      \"%s\": %lld,\n", key, (long long)v);
    else                               printf("      \"%s\": null,\n", key);
}

static void emit_flag(IDeckLinkProfileAttributes *a, BMDDeckLinkAttributeID id, const char *key) {
    bool v = false;
    if (a && a->GetFlag(id, &v) == S_OK) printf("      \"%s\": %s,\n", key, v ? "true" : "false");
    else                                 printf("      \"%s\": null,\n", key);
}

static int fail(const char *status, const std::string &detail, int rc) {
    printf("{\n  \"status\": \"%s\",\n  \"detail\": \"%s\",\n  \"devices\": []\n}\n",
           status, esc(detail).c_str());
    return rc;
}

int main(void) {
    std::string mods;
    const bool mod = module_charge(&mods);
    const int  noyau = noeuds_noyau();

    // 1. La bibliothèque est-elle là ? On la charge nous-mêmes pour pouvoir DIRE pourquoi non.
    void *h = dlopen(kLib, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        const char *e = dlerror();
        return fail("sdk_absent", e ? e : "dlopen a échoué sans message", 2);
    }

    IDeckLinkIterator *it = CreateDeckLinkIteratorInstance();
    if (!it)
        return fail("sdk_broken",
                    std::string(kLib) + " est chargeable mais CreateDeckLinkIteratorInstance() "
                    "rend NULL (symbole absent, ou version d'API incompatible)", 3);

    std::vector<std::string> blocks;
    IDeckLink *dev = NULL;
    int n = 0;

    printf("{\n");
    // On écrit les périphériques au fil de l'eau ; le statut dépend du compte final, donc il
    // est émis en dernier. JSON reste valide : l'ordre des clés n'a pas de sens en JSON.
    printf("  \"devices\": [");

    while (it->Next(&dev) == S_OK) {
        if (n) printf(",");
        printf("\n    {\n");

        std::string model, display;
        const char *s = NULL;
        if (dev->GetModelName(&s) == S_OK)   model = take(s);
        s = NULL;
        if (dev->GetDisplayName(&s) == S_OK) display = take(s);

        printf("      \"index\": %d,\n", n);
        printf("      \"model\": \"%s\",\n", esc(model).c_str());
        printf("      \"display_name\": \"%s\",\n", esc(display).c_str());

        IDeckLinkProfileAttributes *attr = NULL;
        if (dev->QueryInterface(IID_IDeckLinkProfileAttributes, (void **)&attr) != S_OK)
            attr = NULL;

        emit_int(attr, BMDDeckLinkPersistentID,        "persistent_id");
        emit_int(attr, BMDDeckLinkTopologicalID,       "topological_id");
        emit_int(attr, BMDDeckLinkSubDeviceIndex,      "sub_device_index");
        emit_int(attr, BMDDeckLinkNumberOfSubDevices,  "sub_device_count");
        // ANC : le seul attribut que les en-têtes 12.2.2 déclarent. Les attributs HANC
        // (SupportsHANCInput, filtres DID/SDID) sont apparus APRÈS — leur absence ici est une
        // limite connue, pas un oubli. Cf. docs/chantiers/DECKLINK.md.
        emit_flag(attr, BMDDeckLinkVANCRequires10BitYUVVideoFrames, "vanc_exige_10bit");
        emit_flag(attr, BMDDeckLinkSupportsInternalKeying, "keying_interne");
        emit_flag(attr, BMDDeckLinkSupportsExternalKeying, "keying_externe");
        emit_flag(attr, BMDDeckLinkSupportsDualLinkSDI,    "dual_link_sdi");
        emit_flag(attr, BMDDeckLinkSupportsQuadLinkSDI,    "quad_link_sdi");

        int64_t duplex = 0;
        if (attr && attr->GetInt(BMDDeckLinkDuplex, &duplex) == S_OK)
            printf("      \"duplex\": \"%s\",\n", duplex_name(duplex));
        else
            printf("      \"duplex\": null,\n");

        // Sens du sous-périphérique : capture, lecture, ou les deux.
        int64_t io = 0;
        if (attr && attr->GetInt(BMDDeckLinkVideoIOSupport, &io) == S_OK)
            printf("      \"io\": {\"capture\": %s, \"playback\": %s},\n",
                   (io & bmdDeviceSupportsCapture)  ? "true" : "false",
                   (io & bmdDeviceSupportsPlayback) ? "true" : "false");
        else
            printf("      \"io\": null,\n");

        // Ce que ce sous-périphérique porte RÉELLEMENT comme connecteurs, dans chaque sens.
        int64_t vin = 0, vout = 0;
        if (!attr || attr->GetInt(BMDDeckLinkVideoInputConnections, &vin) != S_OK)  vin = 0;
        if (!attr || attr->GetInt(BMDDeckLinkVideoOutputConnections, &vout) != S_OK) vout = 0;
        printf("      \"connecteurs\": {\"entree\": \"%s\", \"sortie\": \"%s\"},\n",
               esc(connexions(vin)).c_str(), esc(connexions(vout)).c_str());

        int64_t pid = 0;
        if (attr && attr->GetInt(BMDDeckLinkProfileID, &pid) == S_OK)
            printf("      \"profil_actif\": {\"id\": \"%s\", \"label\": \"%s\"},\n",
                   fourcc(pid).c_str(), profile_name(pid));
        else
            printf("      \"profil_actif\": null,\n");

        // ÉTAT DU SIGNAL — lecture seule (IDeckLinkStatus), aucun flux ouvert. C'est ce qui
        // permet de savoir ce qui est branché sans rien perturber sur un nœud en production.
        IDeckLinkStatus *stat = NULL;
        if (dev->QueryInterface(IID_IDeckLinkStatus, (void **)&stat) != S_OK) stat = NULL;
        if (stat) {
            bool verrou_in = false, verrou_ref = false;
            int64_t mode = 0, mode_ref = 0;
            bool a_in  = (stat->GetFlag(bmdDeckLinkStatusVideoInputSignalLocked, &verrou_in) == S_OK);
            bool a_ref = (stat->GetFlag(bmdDeckLinkStatusReferenceSignalLocked, &verrou_ref) == S_OK);
            bool a_mode = (stat->GetInt(bmdDeckLinkStatusDetectedVideoInputMode, &mode) == S_OK);
            // ★ LE FORMAT DE LA RÉFÉRENCE, PAS SEULEMENT « verrouillée ». Demandé en recette le
            //   2026-10-19 par deux testeurs le même jour (« qu'est-ce que tu reçois comme signal
            //   de ref ? ») : nous ne lisions que le DRAPEAU, donc nous ne pouvions pas répondre.
            //   Le verrou seul ne distingue pas un noir PAL SD d'une tri-niveaux HD à 50 Hz — il
            //   exclut juste une autre famille de cadence. Le nom que la carte en donne tranche.
            bool a_ref_mode = (stat->GetInt(bmdDeckLinkStatusReferenceSignalMode, &mode_ref) == S_OK);
            // ★ DIAGNOSTIC D'UNE ENTRÉE MUETTE (19/10). Un connecteur peut ne rien verrouiller
            //   alors qu'un signal y arrive : 3G niveau B, lien dual/quad attendu, format hors
            //   plage. Le drapeau « verrouillé » seul ne distingue pas « rien branché » de
            //   « branché mais indécodable » — or ce sont deux pannes opposées, l'une côté
            //   câblage, l'autre côté format. Ces trois valeurs les séparent.
            int64_t liens = 0, in_flags = 0, det_flags = 0;
            bool a_liens = (stat->GetInt(bmdDeckLinkStatusDetectedSDILinkConfiguration, &liens) == S_OK);
            bool a_inf = (stat->GetInt(bmdDeckLinkStatusCurrentVideoInputFlags, &in_flags) == S_OK);
            bool a_detf = (stat->GetInt(bmdDeckLinkStatusDetectedVideoInputFormatFlags, &det_flags) == S_OK);
            printf("      \"signal\": {\"entree_verrouillee\": %s, \"reference_verrouillee\": %s, "
                   "\"mode_detecte\": %s, \"mode_reference\": %s, \"lien_sdi\": %s, "
                   "\"drapeaux_entree\": %s, \"drapeaux_detectes\": %s},\n",
                   a_in  ? (verrou_in  ? "true" : "false") : "null",
                   a_ref ? (verrou_ref ? "true" : "false") : "null",
                   (a_mode && mode) ? ("\"" + fourcc(mode) + "\"").c_str() : "null",
                   (a_ref_mode && mode_ref) ? ("\"" + fourcc(mode_ref) + "\"").c_str() : "null",
                   a_liens ? ("\"" + fourcc(liens) + "\"").c_str() : "null",
                   a_inf ? std::to_string((long long)in_flags).c_str() : "null",
                   a_detf ? std::to_string((long long)det_flags).c_str() : "null");
            stat->Release();
        } else {
            printf("      \"signal\": null,\n");
        }

        // ── MODES DE SORTIE SUPPORTÉS ─────────────────────────────────────────────────────
        // ★ POURQUOI LES ÉNUMÉRER ICI. Le format d'une sortie doit être CHOISI, jamais deviné —
        //   « le premier supporté » tombe sur du NTSC SD, mesuré au banc. Mais tant que la sonde
        //   ne disait pas quels modes la carte accepte, l'interface n'avait qu'un champ libre à
        //   offrir : un format obligatoire qu'on saisit à la main, c'est une faute de frappe qui
        //   ne se voit qu'au déploiement, avec « mode introuvable ». La carte sait répondre ; on
        //   lui demande, et l'exploitant choisit dans une liste qui ne peut pas être fausse.
        //
        //   Noms RENDUS PAR LA CARTE, jamais traduits ni reconstruits : c'est exactement cette
        //   chaîne que le binaire de sortie compare (`n == mode_voulu`).
        {
            IDeckLinkOutput *outp = NULL;
            printf("      \"modes_sortie\": ");
            if (dev->QueryInterface(IID_IDeckLinkOutput, (void **)&outp) == S_OK && outp) {
                IDeckLinkDisplayModeIterator *mit = NULL;
                if (outp->GetDisplayModeIterator(&mit) == S_OK && mit) {
                    IDeckLinkDisplayMode *m = NULL;
                    int premier = 1;
                    printf("[");
                    while (mit->Next(&m) == S_OK) {
                        const char *nm = NULL;
                        if (m->GetName(&nm) == S_OK && nm) {
                            printf("%s\"%s\"", premier ? "" : ", ", esc(nm).c_str());
                            premier = 0;
                            free((void *)nm);
                        }
                        m->Release();
                    }
                    printf("],\n");
                    mit->Release();
                } else {
                    printf("null,\n");
                }
                outp->Release();
            } else {
                // `null` dit « on ne sait pas », pas « aucun mode ».
                //
                // ⚠ ET CE CAS NE SE PRODUIT PAS SUR LA DUO 2. J'avais écrit que les
                //   sous-périphériques INACTIFS rendraient `null` ; mesuré le 2026-10-16, ils
                //   annoncent les MÊMES 16 modes que les autres. L'interface de sortie existe
                //   même quand le profil a éteint la prise. Donc `modes_sortie` ne dit RIEN de
                //   la disponibilité d'un connecteur : c'est `duplex` qui en décide, et lui
                //   seul. Les confondre ferait proposer une sortie sur une prise morte.
                printf("null,\n");
            }
        }

        // Profils disponibles. Le gestionnaire n'existe QUE sur les cartes multi-profils —
        // son absence est une réponse, pas une erreur.
        IDeckLinkProfileManager *pm = NULL;
        if (dev->QueryInterface(IID_IDeckLinkProfileManager, (void **)&pm) == S_OK && pm) {
            IDeckLinkProfileIterator *pit = NULL;
            printf("      \"profils\": [");
            if (pm->GetProfiles(&pit) == S_OK && pit) {
                IDeckLinkProfile *pr = NULL;
                int m = 0;
                while (pit->Next(&pr) == S_OK) {
                    IDeckLinkProfileAttributes *pa = NULL;
                    int64_t id = 0;
                    if (pr->QueryInterface(IID_IDeckLinkProfileAttributes, (void **)&pa) == S_OK && pa) {
                        pa->GetInt(BMDDeckLinkProfileID, &id);
                        pa->Release();
                    }
                    bool active = false;
                    pr->IsActive(&active);

                    // Combien de sous-périphériques ce profil entraîne-t-il avec lui ?
                    // C'est CE nombre qui dit ce qu'un changement de profil va perturber.
                    int peers = 0;
                    IDeckLinkProfileIterator *peer_it = NULL;
                    if (pr->GetPeers(&peer_it) == S_OK && peer_it) {
                        IDeckLinkProfile *p2 = NULL;
                        while (peer_it->Next(&p2) == S_OK) { peers++; p2->Release(); }
                        peer_it->Release();
                    }

                    printf("%s\n        {\"id\": \"%s\", \"label\": \"%s\", \"actif\": %s, \"pairs\": %d}",
                           m ? "," : "", fourcc(id).c_str(), profile_name(id),
                           active ? "true" : "false", peers);
                    m++;
                    pr->Release();
                }
                pit->Release();
                if (m) printf("\n      ");
            }
            printf("]\n");
            pm->Release();
        } else {
            printf("      \"profils\": null\n");   // carte mono-profil
        }

        if (attr) attr->Release();
        printf("    }");
        dev->Release();
        dev = NULL;
        n++;
    }
    it->Release();

    printf(n ? "\n  ],\n" : "],\n");
    printf("  \"module_noyau\": %s,\n", mods.empty() ? "null" : ("\"" + esc(mods) + "\"").c_str());
    printf("  \"noeuds_noyau\": %d,\n", noyau);

    const char *st; const char *det; int rc;
    if (n && noyau > n) {
        // ★ ÉNUMÉRATION PARTIELLE (2026-10-20). Le même désaccord noyau/SDK que `non_enumere`,
        //   mais sur UNE carte parmi plusieurs — et la règle ne regardait que le cas total.
        //   Vécu le jour même : une Quad 2 au micrologiciel périmé posée à côté d'une Duo 2 à
        //   jour. Douze nœuds dans /dev/blackmagic, quatre cartes énumérées, et la sonde a
        //   répondu « ok, le SDK énumère au moins une carte ». Elle avait les deux chiffres
        //   sous les yeux sans les comparer, et l'exploitant a cherché la panne dans le
        //   conteneur. « Au moins une » n'est pas une réponse quand on en attend huit.
        //   La correspondance un nœud = un sous-périphérique est OBSERVÉE sur les deux modèles
        //   que nous avons vus, pas garantie par le SDK : d'où une comparaison STRICTE, qui ne
        //   se déclenche que sur un manque franc.
        // ⚠ LA FAUSSE ALERTE À CRAINDRE serait un profil qui ÉTEINT des sous-périphériques :
        //   si le SDK cessait de les énumérer, une carte saine passerait pour partielle. Ce
        //   n'est pas ce qu'il fait — il les énumère avec `duplex = inactive`, et c'est ainsi
        //   que la console sait afficher « éteinte par le profil ». Le compte reste donc
        //   complet quel que soit le profil. Si un modèle futur s'écartait de ça, c'est ICI
        //   qu'il faudrait regarder avant d'accuser le micrologiciel.
        st = "enumere_partiel"; rc = 6;
        det = "le noyau expose plus de périphériques que le SDK n'en énumère : une carte au "
              "moins est invisible — micrologiciel probablement périmé sur celle-là "
              "(DesktopVideoUpdateTool --list)";
    } else if (n) {
        st = "ok"; rc = 0;
        det = "le SDK répond et énumère au moins une carte";
    } else if (noyau > 0) {
        // Le noyau expose des périphériques que le SDK ignore. Ne PAS rapporter « aucune carte » :
        // il y a du matériel, et quelque chose empêche de s'en servir. Cause la plus fréquente,
        // constatée au premier montage : micrologiciel périmé.
        st = "non_enumere"; rc = 5;
        det = "le noyau expose des périphériques dans /dev/blackmagic mais le SDK n'en énumère "
              "aucun — micrologiciel probablement périmé (DesktopVideoUpdateTool --list)";
    } else if (!mod) {
        // Zéro carte AVEC le module déchargé n'est pas une information : c'est une absence
        // d'information. On refuse de la faire passer pour « aucune carte ».
        st = "module_absent"; rc = 4;
        det = "la bibliothèque répond mais aucun module noyau blackmagic n'est chargé : "
              "« zéro carte » ne veut rien dire dans cet état (modprobe blackmagic blackmagic-io)";
    } else {
        st = "no_devices"; rc = 0;
        det = "le SDK répond, module noyau chargé, aucune carte énumérée — "
              "réponse correcte sur une machine sans matériel";
    }
    printf("  \"status\": \"%s\",\n  \"detail\": \"%s\",\n  \"device_count\": %d\n}\n",
           st, det, n);
    return rc;
}
