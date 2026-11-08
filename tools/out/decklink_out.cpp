// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 BOBI SAS, France
// Auteur : Cyril Mazouer, pour le compte de BOBI SAS
// Distribué sous licence GNU GPL v3 (ou ultérieure) ; voir le fichier LICENSE.
//
// Banc de SORTIE DeckLink — chantier `decklink_io` (docs/chantiers/DECKLINK.md).
//
// Ce qu'il mesure, et pourquoi il n'a besoin d'AUCUNE entrée SDI :
//
//   1. le PONT D'HORLOGE du §3. `GetFrameCompletionReferenceTimestamp` est annoncé par le manuel
//      comme « verrouillé sur l'horloge système ». Notre horloge système étant disciplinée par
//      PTP, c'est censé être la relation carte ↔ TAI. Le banc compare, trame par trame, cet
//      horodatage à CLOCK_REALTIME lu dans le même callback, et rapporte l'écart ET sa DÉRIVE ;
//   2. la CADENCE réelle : les trames arrivent-elles à intervalle constant ?
//   3. la SANTÉ de la lecture ordonnancée : combien de trames complétées, en retard, perdues
//      (`bmdOutputFrameCompleted` / `DisplayedLate` / `Dropped`).
//
// ★ Sans référence de genlock branchée, la carte tourne sur son propre quartz : la dérive
//   mesurée ici est donc la dérive LIBRE, et c'est une mesure utile en soi — c'est le zéro par
//   rapport auquel on jugera le gain du genlock une fois la référence câblée.
//
// Effet de bord : il ÉMET sur le connecteur de sortie du sous-périphérique choisi. Il ne change
// aucun profil et ne touche à aucune entrée.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#include <string>
#include <vector>

#include "decklink_sdk/DeckLinkAPI.h"

static const BMDTimeScale kEchelle = 1000000000LL;   // nanosecondes

static int64_t maintenant_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static std::string prendre(const char *s) {
    if (!s) return std::string();
    std::string o(s); free(const_cast<char *>(s)); return o;
}

// Une trame par appel : on ne mesure pas le contenu, on mesure le temps. Motif simple et
// reconnaissable à l'œil si quelqu'un branche un moniteur.
static void remplir(IDeckLinkMutableVideoFrame *f, int n) {
    void *p = NULL;
    if (f->GetBytes(&p) != S_OK || !p) return;
    const long w = f->GetWidth(), h = f->GetHeight(), stride = f->GetRowBytes();
    uint8_t *o = (uint8_t *)p;
    // 8BitYUV (UYVY) : deux pixels par 4 octets. Barre claire qui défile, fond gris.
    const long barre = (n * 16) % (w ? w : 1);
    for (long y = 0; y < h; y++) {
        uint8_t *l = o + y * stride;
        for (long x = 0; x < w; x += 2) {
            const bool dans = (x >= barre && x < barre + 64);
            const uint8_t luma = dans ? 200 : 64;
            l[x * 2 + 0] = 128; l[x * 2 + 1] = luma;   // U Y
            l[x * 2 + 2] = 128; l[x * 2 + 3] = luma;   // V Y
        }
    }
}

struct Echantillon { int64_t ecart_ns; int64_t horloge_ns; };

class Rappel : public IDeckLinkVideoOutputCallback {
public:
    Rappel(IDeckLinkOutput *o, BMDTimeValue dur, BMDTimeScale ech, int total)
        : sortie_(o), duree_(dur), echelle_(ech), total_(total) {}

    HRESULT QueryInterface(REFIID, void **v) override { *v = NULL; return E_NOINTERFACE; }
    ULONG AddRef(void) override  { return ++refs_; }
    ULONG Release(void) override { ULONG r = --refs_; if (!r) delete this; return r; }

    HRESULT ScheduledFrameCompleted(IDeckLinkVideoFrame *frame,
                                    BMDOutputFrameCompletionResult res) override {
        switch (res) {
            case bmdOutputFrameCompleted:     completees_++;  break;
            case bmdOutputFrameDisplayedLate: en_retard_++;   break;
            case bmdOutputFrameDropped:       perdues_++;     break;
            case bmdOutputFrameFlushed:       vidangees_++;   break;
            default: break;
        }

        // ★ LE point du banc : l'horodatage de la carte, contre l'horloge système, MAINTENANT.
        BMDTimeValue t_carte = 0;
        const int64_t t_sys = maintenant_ns();
        if (sortie_->GetFrameCompletionReferenceTimestamp(frame, kEchelle, &t_carte) == S_OK) {
            Echantillon e; e.ecart_ns = t_sys - (int64_t)t_carte; e.horloge_ns = t_sys;
            ech_.push_back(e);
        } else {
            sans_horodatage_++;
        }

        if (programmees_ < total_) {
            remplir((IDeckLinkMutableVideoFrame *)frame, programmees_);
            sortie_->ScheduleVideoFrame(frame, programmees_ * duree_, duree_, echelle_);
            programmees_++;
        }
        return S_OK;
    }
    HRESULT ScheduledPlaybackHasStopped(void) override { return S_OK; }

    int programmees_ = 0, completees_ = 0, en_retard_ = 0, perdues_ = 0, vidangees_ = 0;
    int sans_horodatage_ = 0;
    std::vector<Echantillon> ech_;
private:
    IDeckLinkOutput *sortie_;
    BMDTimeValue duree_;
    BMDTimeScale echelle_;
    int total_;
    ULONG refs_ = 1;
};

int main(int argc, char **argv) {
    const int secondes  = (argc > 1) ? atoi(argv[1]) : 10;
    const int precharge = (argc > 2) ? atoi(argv[2]) : 3;
    // ★ Mode et sous-périphérique EXPLICITES. Sans eux, le banc prenait le premier mode déclaré
    //   (du NTSC SD) sur le premier sous-périphérique actif — qui peut être celui qui ne voit
    //   PAS la référence. Trois essais ont ainsi mesuré le mauvais mode au mauvais endroit le
    //   2026-09-08, et rendu « pas asservi » sans que ça veuille rien dire. Un banc qui choisit
    //   sa cible tout seul mesure ce qu'il veut, pas ce qu'on lui demande.
    const char   *mode_voulu = (argc > 3 && *argv[3]) ? argv[3] : NULL;
    const int64_t pid_voulu  = (argc > 4) ? strtoll(argv[4], NULL, 0) : 0;

    IDeckLinkIterator *it = CreateDeckLinkIteratorInstance();
    if (!it) { fprintf(stderr, "SDK indisponible\n"); return 2; }

    // Choisir un sous-périphérique : ACTIF d'abord (le piège du 2026-09-08 — VideoIOSupport
    // reste vrai sur un sous-périphérique désactivé), capable de lecture ensuite.
    IDeckLink *dev = NULL, *choisi = NULL;
    std::string nom;
    int64_t persistent = 0;
    while (it->Next(&dev) == S_OK) {
        IDeckLinkProfileAttributes *a = NULL;
        int64_t duplex = 0, io = 0, pid = 0;
        if (dev->QueryInterface(IID_IDeckLinkProfileAttributes, (void **)&a) == S_OK && a) {
            a->GetInt(BMDDeckLinkDuplex, &duplex);
            a->GetInt(BMDDeckLinkVideoIOSupport, &io);
            a->GetInt(BMDDeckLinkPersistentID, &pid);
            a->Release();
        }
        if (!choisi && duplex != bmdDuplexInactive && (io & bmdDeviceSupportsPlayback)
                && (!pid_voulu || pid == pid_voulu)) {
            choisi = dev; persistent = pid;
            const char *s = NULL;
            if (dev->GetDisplayName(&s) == S_OK) nom = prendre(s);
            continue;                       // gardé : on ne le relâche pas
        }
        dev->Release();
    }
    it->Release();
    if (!choisi) {
        fprintf(stderr, "aucun sous-périphérique actif capable de lecture%s\n",
                pid_voulu ? " pour ce persistent id" : "");
        return 3;
    }

    IDeckLinkOutput *sortie = NULL;
    if (choisi->QueryInterface(IID_IDeckLinkOutput, (void **)&sortie) != S_OK) {
        fprintf(stderr, "pas d'interface de sortie\n"); return 3;
    }

    // Le format ne se code pas en dur : on prend le premier mode que la carte DÉCLARE supporter.
    IDeckLinkDisplayModeIterator *mit = NULL;
    IDeckLinkDisplayMode *mode = NULL, *retenu = NULL;
    if (sortie->GetDisplayModeIterator(&mit) != S_OK) { fprintf(stderr, "pas de modes\n"); return 3; }
    std::vector<std::string> dispo;
    while (mit->Next(&mode) == S_OK) {
        const char *nm = NULL; std::string nm_s;
        if (mode->GetName(&nm) == S_OK && nm) { nm_s = nm; free((void *)nm); }
        dispo.push_back(nm_s);
        bool ok = false; BMDDisplayMode reel;
        const bool supporte =
            (sortie->DoesSupportVideoMode(bmdVideoConnectionUnspecified, mode->GetDisplayMode(),
                                          bmdFormat8BitYUV, bmdNoVideoOutputConversion,
                                          bmdSupportedVideoModeDefault, &reel, &ok) == S_OK && ok);
        if (supporte && !retenu && (!mode_voulu || nm_s == mode_voulu)) { retenu = mode; continue; }
        mode->Release();
    }
    if (!retenu && mode_voulu) {
        fprintf(stderr, "mode « %s » introuvable. Modes disponibles :\n", mode_voulu);
        for (size_t i = 0; i < dispo.size(); i++) fprintf(stderr, "  %s\n", dispo[i].c_str());
        return 3;
    }
    mit->Release();
    if (!retenu) { fprintf(stderr, "aucun mode supporté\n"); return 3; }

    BMDTimeValue duree = 0; BMDTimeScale echelle = 0;
    retenu->GetFrameRate(&duree, &echelle);
    const char *nm = NULL; std::string nom_mode;
    if (retenu->GetName(&nm) == S_OK) nom_mode = prendre(nm);
    const double fps = echelle / (double)duree;
    const int total = (int)(secondes * fps);

    printf("Appareil    : %s (persistent %lld)\n", nom.c_str(), (long long)persistent);
    printf("Mode        : %s — %ldx%ld, %.3f i/s\n", nom_mode.c_str(),
           retenu->GetWidth(), retenu->GetHeight(), fps);
    printf("Durée       : %d s (%d trames), pré-charge %d\n\n", secondes, total, precharge);

    if (sortie->EnableVideoOutput(retenu->GetDisplayMode(), bmdVideoOutputFlagDefault) != S_OK) {
        fprintf(stderr, "EnableVideoOutput a échoué\n"); return 4;
    }

    // ★ DEUX lectures, et elles ne disent pas la même chose — constaté le 2026-09-08.
    //   `IDeckLinkStatus` rapporte la présence PHYSIQUE d'une référence à l'entrée Ref In.
    //   `IDeckLinkOutput::GetReferenceStatus` rapporte si CETTE SORTIE y est asservie, ce qui
    //   suppose une référence COMPATIBLE avec son mode vidéo. Une référence 59,94 Hz avec une
    //   sortie 50 Hz donne « présente » d'un côté et « absente » de l'autre.
    //   On publie les deux : choisir l'une des deux masquerait précisément ce cas.
    BMDReferenceStatus ref = (BMDReferenceStatus)0;
    sortie->GetReferenceStatus(&ref);
    bool ref_physique = false;
    IDeckLinkStatus *st_dev = NULL;
    if (choisi->QueryInterface(IID_IDeckLinkStatus, (void **)&st_dev) == S_OK && st_dev) {
        st_dev->GetFlag(bmdDeckLinkStatusReferenceSignalLocked, &ref_physique);
        st_dev->Release();
    }
    printf("Référence AVANT démarrage : entrée %s, sortie asservie %s\n\n",
           ref_physique ? "PRÉSENTE" : "absente",
           (ref & bmdReferenceLocked) ? "OUI" : "non");
    // ★ Cette lecture-là ne conclut RIEN : l'asservissement met quelques secondes à s'établir
    //   APRÈS le démarrage de la lecture. Trois essais ont conclu « pas asservi » le 2026-09-08
    //   sur cette seule lecture précoce. Le verdict est plus bas, échantillonné en marche.

    Rappel *cb = new Rappel(sortie, duree, echelle, total);
    sortie->SetScheduledFrameCompletionCallback(cb);

    // Pré-charge : le manuel dit qu'elle n'est PAS obligatoire, mais qu'elle protège de la
    // famine. C'est le paramètre du §4, et il se mesure — d'où l'argument de ligne de commande.
    std::vector<IDeckLinkMutableVideoFrame *> trames;
    for (int i = 0; i < precharge + 2; i++) {
        IDeckLinkMutableVideoFrame *f = NULL;
        if (sortie->CreateVideoFrame(retenu->GetWidth(), retenu->GetHeight(),
                                     retenu->GetWidth() * 2, bmdFormat8BitYUV,
                                     bmdFrameFlagDefault, &f) != S_OK) break;
        remplir(f, i);
        sortie->ScheduleVideoFrame(f, cb->programmees_ * duree, duree, echelle);
        cb->programmees_++;
        trames.push_back(f);
    }
    if (trames.empty()) { fprintf(stderr, "impossible de créer les trames\n"); return 4; }

    sortie->StartScheduledPlayback(0, echelle, 1.0);
    int n_verrou = 0, n_ref = 0, n_ech = 0;
    while (cb->programmees_ < total || cb->completees_ + cb->perdues_ + cb->en_retard_ < total) {
        usleep(20000);
        if ((++n_ech % 25) == 0) {                    // ~2 relevés par seconde
            BMDReferenceStatus r = (BMDReferenceStatus)0;
            sortie->GetReferenceStatus(&r);
            if (r & bmdReferenceLocked) n_verrou++;
            bool p = false;
            IDeckLinkStatus *sd = NULL;
            if (choisi->QueryInterface(IID_IDeckLinkStatus, (void **)&sd) == S_OK && sd) {
                sd->GetFlag(bmdDeckLinkStatusReferenceSignalLocked, &p);
                sd->Release();
            }
            if (p) n_ref++;
        }
    }
    const int n_releves = n_ech / 25;
    sortie->StopScheduledPlayback(0, NULL, 0);
    sortie->DisableVideoOutput();

    // ---- Verdict ----
    printf("\nRÉFÉRENCE EN MARCHE (%d relevés) : présente à l'entrée %d fois, "
           "sortie ASSERVIE %d fois%s\n",
           n_releves, n_ref, n_verrou,
           (n_ref && !n_verrou)
             ? "  ← présente mais jamais utilisée : standard incompatible avec ce mode ?" : "");
    printf("Trames      : %d complétées, %d en retard, %d perdues, %d vidangées\n",
           cb->completees_, cb->en_retard_, cb->perdues_, cb->vidangees_);
    if (cb->sans_horodatage_)
        printf("⚠ %d trames SANS horodatage de fin\n", cb->sans_horodatage_);

    if (cb->ech_.size() >= 3) {
        const std::vector<Echantillon> &e = cb->ech_;
        int64_t mn = e[0].ecart_ns, mx = e[0].ecart_ns;
        for (size_t i = 0; i < e.size(); i++) {
            if (e[i].ecart_ns < mn) mn = e[i].ecart_ns;
            if (e[i].ecart_ns > mx) mx = e[i].ecart_ns;
        }
        // Dérive : pente de l'écart entre la première et la dernière seconde de mesure.
        const size_t n = e.size();
        const double dt_s = (e[n - 1].horloge_ns - e[0].horloge_ns) / 1e9;
        const double derive_ppm = dt_s > 0
            ? ((double)(e[n - 1].ecart_ns - e[0].ecart_ns) / 1e9) / dt_s * 1e6 : 0.0;
        printf("\nHorodatage carte vs horloge système (%zu échantillons) :\n", n);
        printf("  écart      : %.3f ms → %.3f ms (amplitude %.3f ms)\n",
               mn / 1e6, mx / 1e6, (mx - mn) / 1e6);
        printf("  dérive     : %+.2f ppm sur %.1f s\n", derive_ppm, dt_s);
        // ★ Le verdict s'appuie sur les relevés EN MARCHE, jamais sur la lecture précoce : celle
        //   d'avant démarrage disait « pas asservi » alors que le verrou s'établissait ensuite.
        printf("  → %s\n", n_verrou > n_releves / 2
               ? "SORTIE ASSERVIE à la référence sur la majorité des relevés : cette dérive est "
                 "celle du genlock, elle doit tendre vers zéro"
               : "sortie NON asservie : dérive du quartz de la carte");
    }

    for (size_t i = 0; i < trames.size(); i++) trames[i]->Release();
    sortie->SetScheduledFrameCompletionCallback(NULL);
    cb->Release();
    retenu->Release();
    sortie->Release();
    choisi->Release();
    return 0;
}
