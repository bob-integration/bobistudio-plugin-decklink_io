// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 BOBI SAS, France
// Auteur : Cyril Mazouer, pour le compte de BOBI SAS
// Distribué sous licence GNU GPL v3 (ou ultérieure) ; voir le fichier LICENSE.
//
// Bascule de PROFIL DeckLink — chantier `decklink_io` (docs/chantiers/DECKLINK.md), jalon 3.
//
//   ./decklink_profile              → liste les profils et dit ce qu'un changement perturberait
//   ./decklink_profile <fourcc>     → active ce profil (ex. 2dhd)
//
// ★ C'est l'opération DESTRUCTIVE du chantier, et l'outil la traite comme telle :
//   - il annonce d'abord le RAYON D'ACTION (`GetPeers`) — combien de sous-périphériques du même
//     groupe seront emportés par le changement ;
//   - il refuse d'agir si un sous-périphérique du groupe a un signal d'entrée verrouillé, sauf
//     `--force` : on ne coupe pas une source vivante par inadvertance ;
//   - il ATTEND `ProfileActivated`. Le manuel est explicite : l'activation n'est pas terminée au
//     retour de `SetActive`. Croire le retour de la fonction, c'est ré-énumérer trop tôt.
//
// ★ Après activation, les IDeckLink d'avant ne valent plus rien : le nombre de sous-périphériques
//   CHANGE. L'outil ré-énumère depuis un itérateur neuf, et n'essaie pas de réutiliser ses
//   pointeurs — c'est la même raison qui interdit d'identifier un flux par son index.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <string>

#include "decklink_sdk/DeckLinkAPI.h"

static std::string prendre(const char *s) {
    if (!s) return std::string();
    std::string o(s); free(const_cast<char *>(s)); return o;
}

static std::string fourcc(int64_t v) {
    char b[5] = {0};
    b[0]=(char)((v>>24)&0xFF); b[1]=(char)((v>>16)&0xFF);
    b[2]=(char)((v>>8)&0xFF);  b[3]=(char)(v&0xFF);
    for (int i=0;i<4;i++) if (b[i]<0x20||b[i]>0x7E) return std::string("?");
    return std::string(b);
}

static int64_t depuis_fourcc(const char *s) {
    if (!s || strlen(s) != 4) return 0;
    return ((int64_t)(uint8_t)s[0]<<24)|((int64_t)(uint8_t)s[1]<<16)|
           ((int64_t)(uint8_t)s[2]<<8)|((int64_t)(uint8_t)s[3]);
}

static const char *libelle(int64_t v) {
    switch (v) {
        case bmdProfileOneSubDeviceFullDuplex:   return "1 sous-périph. full-duplex";
        case bmdProfileOneSubDeviceHalfDuplex:   return "1 sous-périph. half-duplex";
        case bmdProfileTwoSubDevicesFullDuplex:  return "2 sous-périph. full-duplex";
        case bmdProfileTwoSubDevicesHalfDuplex:  return "2 sous-périph. half-duplex";
        case bmdProfileFourSubDevicesHalfDuplex: return "4 sous-périph. half-duplex";
        default: return "inconnu";      // transporté tel quel : un appareil plus récent que nous
    }
}

// L'activation n'est pas finie au retour de SetActive — le manuel le dit, il faut attendre.
class Attente : public IDeckLinkProfileCallback {
public:
    HRESULT QueryInterface(REFIID, void **v) override { *v=NULL; return E_NOINTERFACE; }
    ULONG AddRef(void) override  { return ++refs_; }
    ULONG Release(void) override { ULONG r=--refs_; if(!r) delete this; return r; }
    HRESULT ProfileChanging(IDeckLinkProfile *, bool flux_arretes) override {
        printf("   … changement en cours (flux arrêtés de force : %s)\n", flux_arretes ? "OUI" : "non");
        return S_OK;
    }
    HRESULT ProfileActivated(IDeckLinkProfile *) override { active_ = true; return S_OK; }
    volatile bool active_ = false;
private:
    ULONG refs_ = 1;
};

static int etat(bool detail) {
    IDeckLinkIterator *it = CreateDeckLinkIteratorInstance();
    if (!it) { fprintf(stderr, "SDK indisponible\n"); return 2; }
    IDeckLink *dev = NULL; int n = 0;
    while (it->Next(&dev) == S_OK) {
        IDeckLinkProfileAttributes *a = NULL;
        int64_t duplex = 0, pid = 0, prof = 0;
        if (dev->QueryInterface(IID_IDeckLinkProfileAttributes, (void **)&a) == S_OK && a) {
            a->GetInt(BMDDeckLinkDuplex, &duplex);
            a->GetInt(BMDDeckLinkPersistentID, &pid);
            a->GetInt(BMDDeckLinkProfileID, &prof);
            a->Release();
        }
        std::string nom; const char *s = NULL;
        if (dev->GetDisplayName(&s) == S_OK) nom = prendre(s);
        printf("  idx %d  %-20s persistent=%lld  duplex=%-8s  profil=%s\n", n, nom.c_str(),
               (long long)pid,
               duplex==bmdDuplexInactive?"inactive":duplex==bmdDuplexFull?"full":
               duplex==bmdDuplexHalf?"half":"simplex", fourcc(prof).c_str());
        if (detail && n == 0) {
            IDeckLinkProfileManager *pm = NULL;
            if (dev->QueryInterface(IID_IDeckLinkProfileManager, (void **)&pm) == S_OK && pm) {
                IDeckLinkProfileIterator *pit = NULL;
                if (pm->GetProfiles(&pit) == S_OK && pit) {
                    IDeckLinkProfile *pr = NULL;
                    printf("\n  Profils disponibles :\n");
                    while (pit->Next(&pr) == S_OK) {
                        IDeckLinkProfileAttributes *pa = NULL; int64_t id = 0;
                        if (pr->QueryInterface(IID_IDeckLinkProfileAttributes,(void**)&pa)==S_OK && pa) {
                            pa->GetInt(BMDDeckLinkProfileID, &id); pa->Release();
                        }
                        bool actif = false; pr->IsActive(&actif);
                        int pairs = 0; IDeckLinkProfileIterator *pi = NULL;
                        if (pr->GetPeers(&pi) == S_OK && pi) {
                            IDeckLinkProfile *p2 = NULL;
                            while (pi->Next(&p2) == S_OK) { pairs++; p2->Release(); }
                            pi->Release();
                        }
                        printf("    %s  %-28s %s  (rayon d'action : %d sous-périph. du groupe)\n",
                               fourcc(id).c_str(), libelle(id), actif ? "← ACTIF" : "       ", pairs);
                        pr->Release();
                    }
                    pit->Release();
                }
                pm->Release();
            }
        }
        dev->Release(); n++;
    }
    it->Release();
    return n;
}

static int64_t _pid(IDeckLink *d) {
    IDeckLinkProfileAttributes *a = NULL; int64_t pid = 0;
    if (d && d->QueryInterface(IID_IDeckLinkProfileAttributes, (void **)&a) == S_OK && a) {
        a->GetInt(BMDDeckLinkPersistentID, &pid); a->Release();
    }
    return pid;
}

int main(int argc, char **argv) {
    const char *cible = NULL; bool force = false; int64_t seul = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--force")) force = true;
        // ★ --device <persistent_id> : n'agir que sur CE sous-peripherique. Sans lui, l'outil
        //   balaie TOUS les groupes, et sa garde d'entree vivante est GLOBALE — donc une seule
        //   source en cours de capture bloquait une bascule qui, sur cette carte, ne porte que
        //   sur UN sous-peripherique (rayon d'action mesure le 2026-09-21). La seule echappatoire
        //   etait --force, qui bascule tout et coupe la source : une garde trop large pousse a
        //   employer un marteau. On designe par identifiant STABLE, jamais par index.
        else if (!strcmp(argv[i], "--device") && i + 1 < argc) seul = strtoll(argv[++i], NULL, 0);
        else cible = argv[i];
    }

    printf("État actuel :\n");
    etat(true);
    if (!cible) { printf("\n(aucun profil demandé — rien n'a été touché)\n"); return 0; }

    const int64_t vise = depuis_fourcc(cible);
    if (!vise) { fprintf(stderr, "\nprofil invalide : attendu un four-cc de 4 caractères\n"); return 1; }

    // Garde-fou : ne pas couper une entrée vivante par inadvertance.
    if (!force) {
        IDeckLinkIterator *it = CreateDeckLinkIteratorInstance();
        IDeckLink *d = NULL; bool vivant = false;
        while (it && it->Next(&d) == S_OK) {
            IDeckLinkStatus *st = NULL; bool v = false;
            if (d->QueryInterface(IID_IDeckLinkStatus, (void **)&st) == S_OK && st) {
                st->GetFlag(bmdDeckLinkStatusVideoInputSignalLocked, &v); st->Release();
            }
            if (v && (!seul || _pid(d) == seul)) vivant = true;
            d->Release();
        }
        if (it) it->Release();
        if (vivant) {
            fprintf(stderr, "\nREFUS : un sous-périphérique a un signal d'entrée VERROUILLÉ.\n"
                            "Changer de profil couperait une source vivante. --force pour passer outre.\n");
            return 4;
        }
    }

    printf("\nActivation du profil %s (%s)…\n", cible, libelle(vise));
    // ★ Un appareil peut porter PLUSIEURS groupes de profils (constaté sur Duo 2 le 2026-09-08 :
    //   deux groupes de deux sous-périphériques). Activer le profil sur un groupe ne touche PAS
    //   les autres — il faut donc balayer, et re-partir d'un itérateur NEUF à chaque fois, car
    //   l'énumération change sous nos pieds à chaque activation.
    int rc = 0, groupes = 0;
    for (int passe = 0; passe < 8; passe++) {
        IDeckLinkIterator *it = CreateDeckLinkIteratorInstance();
        IDeckLink *dev = NULL, *a_changer = NULL;
        while (it && it->Next(&dev) == S_OK) {
            IDeckLinkProfileAttributes *a = NULL; int64_t prof = 0;
            if (dev->QueryInterface(IID_IDeckLinkProfileAttributes, (void **)&a) == S_OK && a) {
                a->GetInt(BMDDeckLinkProfileID, &prof); a->Release();
            }
            if (!a_changer && prof != vise && (!seul || _pid(dev) == seul)) { a_changer = dev; continue; }
            dev->Release();
        }
        if (it) it->Release();
        if (!a_changer) break;                       // tous les groupes sont sur le profil visé
        dev = a_changer;
        groupes++;
        IDeckLinkProfileManager *pm = NULL;
        if (dev->QueryInterface(IID_IDeckLinkProfileManager, (void **)&pm) == S_OK && pm) {
            IDeckLinkProfile *pr = NULL;
            if (pm->GetProfile((BMDProfileID)vise, &pr) == S_OK && pr) {
                Attente *att = new Attente();
                pm->SetCallback(att);
                if (pr->SetActive() == S_OK) {
                    for (int i = 0; i < 100 && !att->active_; i++) usleep(100000);   // 10 s max
                    printf("   %s\n", att->active_ ? "ProfileActivated reçu."
                                                   : "⚠ ProfileActivated JAMAIS reçu (délai dépassé)");
                    rc = att->active_ ? 0 : 6;
                } else {
                    fprintf(stderr, "   SetActive a échoué\n"); rc = 5;
                }
                pm->SetCallback(NULL);
                att->Release();
                pr->Release();
            } else {
                fprintf(stderr, "   ce profil n'existe pas sur cet appareil\n"); rc = 1;
            }
            pm->Release();
        } else {
            fprintf(stderr, "   sous-périphérique sans gestionnaire de profils — ignoré\n");
            dev->Release();
            break;
        }
        dev->Release();
        if (rc) break;
    }
    printf("   %d groupe(s) basculé(s).\n", groupes);

    // Ré-énumération DEPUIS ZÉRO : le nombre de sous-périphériques a pu changer.
    printf("\nÉtat après activation :\n");
    etat(false);
    return rc;
}
