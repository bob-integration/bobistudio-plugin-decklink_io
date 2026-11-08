// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 BOBI SAS, France
// Auteur : Cyril Mazouer, pour le compte de BOBI SAS
// Distribué sous licence GNU GPL v3 (ou ultérieure) ; voir le fichier LICENSE.
//
// Banc de CAPTURE DeckLink — chantier `decklink_io` (docs/chantiers/DECKLINK.md), jalon 4.
//
// Il essaie CHAQUE sous-périphérique actif capable de capture, l'un après l'autre, et dit pour
// chacun : le format détecté, le nombre de trames reçues, et surtout s'il y a du SIGNAL.
//
// ★ Pourquoi ce banc et pas la sonde : la sonde lit `bmdDeckLinkStatusVideoInputSignalLocked`
//   SANS avoir activé l'entrée. Or le manuel ne fait cette lecture qu'APRÈS `EnableVideoInput`
//   (§2.4.13.1). Un « pas de verrou » lu à froid peut donc être un artefact de la méthode et non
//   une absence de signal — c'est précisément ce que ce banc départage, en lisant le même statut
//   avant ET après activation.
//
// ★ Exigence du §1 : « absence de signal → état publié, pas de trame fabriquée en silence ». La
//   carte livre des trames marquées `bmdFrameHasNoInputSource` quand rien n'entre ; elles sont
//   comptées à part et ne doivent JAMAIS être prises pour des trames valides.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <string>
#include <vector>

#include "decklink_sdk/DeckLinkAPI.h"

static std::string prendre(const char *s) {
    if (!s) return std::string();
    std::string o(s); free(const_cast<char *>(s)); return o;
}

class Ecoute : public IDeckLinkInputCallback {
public:
    Ecoute(IDeckLinkInput *in) : in_(in) {}
    HRESULT QueryInterface(REFIID, void **v) override { *v=NULL; return E_NOINTERFACE; }
    ULONG AddRef(void) override  { return ++refs_; }
    ULONG Release(void) override { ULONG r=--refs_; if(!r) delete this; return r; }

    // Détection automatique : c'est la carte qui nous dit ce qui entre, on ne le suppose pas.
    HRESULT VideoInputFormatChanged(BMDVideoInputFormatChangedEvents,
                                    IDeckLinkDisplayMode *mode,
                                    BMDDetectedVideoInputFormatFlags) override {
        if (!mode) return S_OK;
        const char *n = NULL;
        if (mode->GetName(&n) == S_OK) format_ = prendre(n);
        largeur_ = mode->GetWidth(); hauteur_ = mode->GetHeight();
        BMDTimeValue d=0; BMDTimeScale e=0;
        if (mode->GetFrameRate(&d,&e) == S_OK && d) fps_ = e/(double)d;
        // ★ Ne reconfigurer QUE si le mode a réellement changé. Reconfigurer à chaque
        //   notification fait boucler : `EnableVideoInput` re-déclenche `VideoInputFormatChanged`,
        //   qui reconfigure, qui re-déclenche… Mesuré le 2026-09-08 : 66 « changements » en 3 s
        //   sur une source pourtant stable, et des trames invalides à chaque tour de manège.
        //   Le plugin doit se garder du même piège.
        const BMDDisplayMode nouveau = mode->GetDisplayMode();
        if (nouveau == courant_) return S_OK;
        courant_ = nouveau;
        changements_++;
        in_->PauseStreams();
        in_->EnableVideoInput(nouveau, bmdFormat8BitYUV, bmdVideoInputEnableFormatDetection);
        in_->FlushStreams();
        in_->StartStreams();
        return S_OK;
    }

    HRESULT VideoInputFrameArrived(IDeckLinkVideoInputFrame *f, IDeckLinkAudioInputPacket *) override {
        if (!f) return S_OK;
        if (f->GetFlags() & bmdFrameHasNoInputSource) { sans_source_++; return S_OK; }
        valides_++;

        // ★ TIMECODE — le SDK l'expose par DEUX chemins : l'itérateur de paquets ANC, et cette
        //   API dédiée. Les deux peuvent diverger : si `GetTimecode` rend une valeur alors que
        //   l'itérateur ANC ne voit rien, le timecode EST là et c'est notre chemin ANC qui le
        //   rate. C'est ce que ce relevé départage, sans avoir à changer de source.
        static const struct { BMDTimecodeFormat f; const char *nom; } formats[] = {
            {bmdTimecodeRP188Any,   "RP188 (n'importe lequel)"},
            {bmdTimecodeRP188VITC1, "RP188 VITC1"},
            {bmdTimecodeRP188LTC,   "RP188 LTC"},
            {bmdTimecodeSerial,     "série"},
        };
        for (size_t i = 0; i < sizeof formats / sizeof *formats; i++) {
            IDeckLinkTimecode *tc = NULL;
            if (f->GetTimecode(formats[i].f, &tc) == S_OK && tc) {
                const char *str = NULL;
                if (tc->GetString(&str) == S_OK && str) {
                    if (!tc_vu_) { snprintf(tc_, sizeof tc_, "%s = %s", formats[i].nom, str); tc_vu_ = true; }
                    free((void *)str);
                }
                tc->Release();
            }
        }

        // Paquets ANC vus par l'ITÉRATEUR — le second terme de la comparaison.
        IDeckLinkVideoFrameAncillaryPackets *ap = NULL;
        if (f->QueryInterface(IID_IDeckLinkVideoFrameAncillaryPackets, (void **)&ap) == S_OK && ap) {
            IDeckLinkAncillaryPacketIterator *pit = NULL;
            if (ap->GetPacketIterator(&pit) == S_OK && pit) {
                IDeckLinkAncillaryPacket *p = NULL;
                while (pit->Next(&p) == S_OK) {
                    anc_paquets_++;
                    if (anc_dids_[0] == 0)
                        snprintf(anc_dids_, sizeof anc_dids_, "DID=0x%02X SDID=0x%02X ligne=%u",
                                 p->GetDID(), p->GetSDID(), p->GetLineNumber());
                    p->Release();
                }
                pit->Release();
            }
            // L'itérateur est vide : le paquet ATC (DID 0x60 / SDID 0x60) est-il tout de même
            // trouvable par recherche directe ? Les deux chemins ne sont pas garantis équivalents.
            if (!anc_paquets_) {
                IDeckLinkAncillaryPacket *p60 = NULL;
                if (ap->GetFirstPacketByID(0x60, 0x60, &p60) == S_OK && p60) {
                    anc_par_id_ = true;
                    p60->Release();
                }
            }
            ap->Release();
        }
        return S_OK;
    }

    int changements_ = 0, valides_ = 0, sans_source_ = 0;
    long anc_paquets_ = 0;
    bool tc_vu_ = false;
    char tc_[128] = {0};
    char anc_dids_[128] = {0};
    bool anc_par_id_ = false;
    BMDDisplayMode courant_ = (BMDDisplayMode)0;
    std::string format_;
    long largeur_ = 0, hauteur_ = 0;
    double fps_ = 0;
private:
    IDeckLinkInput *in_;
    ULONG refs_ = 1;
};

static bool lire_verrou(IDeckLink *dev) {
    IDeckLinkStatus *st = NULL;
    bool v = false;
    if (dev->QueryInterface(IID_IDeckLinkStatus, (void **)&st) == S_OK && st) {
        st->GetFlag(bmdDeckLinkStatusVideoInputSignalLocked, &v);
        st->Release();
    }
    return v;
}

int main(int argc, char **argv) {
    const int secondes = (argc > 1) ? atoi(argv[1]) : 4;

    IDeckLinkIterator *it = CreateDeckLinkIteratorInstance();
    if (!it) { fprintf(stderr, "SDK indisponible\n"); return 2; }

    IDeckLink *dev = NULL;
    int idx = -1, essayes = 0;
    while (it->Next(&dev) == S_OK) {
        idx++;
        IDeckLinkProfileAttributes *a = NULL;
        int64_t duplex = 0, io = 0, pid = 0;
        bool detection = false;
        if (dev->QueryInterface(IID_IDeckLinkProfileAttributes, (void **)&a) == S_OK && a) {
            a->GetInt(BMDDeckLinkDuplex, &duplex);
            a->GetInt(BMDDeckLinkVideoIOSupport, &io);
            a->GetInt(BMDDeckLinkPersistentID, &pid);
            a->GetFlag(BMDDeckLinkSupportsInputFormatDetection, &detection);
            a->Release();
        }
        // Actif d'abord (VideoIOSupport ment sur un sous-périphérique désactivé), capture ensuite.
        if (duplex == bmdDuplexInactive || !(io & bmdDeviceSupportsCapture)) { dev->Release(); continue; }

        std::string nom;
        const char *s = NULL;
        if (dev->GetDisplayName(&s) == S_OK) nom = prendre(s);
        printf("── idx %d — %s (persistent %lld)\n", idx, nom.c_str(), (long long)pid);
        printf("   détection auto de format : %s\n", detection ? "oui" : "NON");
        printf("   verrou AVANT activation  : %s\n", lire_verrou(dev) ? "oui" : "non");

        IDeckLinkInput *in = NULL;
        if (dev->QueryInterface(IID_IDeckLinkInput, (void **)&in) != S_OK) {
            printf("   (pas d'interface d'entrée)\n\n"); dev->Release(); continue;
        }

        // Mode de départ : le premier que la carte déclare. La détection corrigera.
        IDeckLinkDisplayModeIterator *mit = NULL; IDeckLinkDisplayMode *m = NULL, *depart = NULL;
        if (in->GetDisplayModeIterator(&mit) == S_OK) {
            while (mit->Next(&m) == S_OK) { if (!depart) { depart = m; continue; } m->Release(); }
            mit->Release();
        }
        if (!depart) { printf("   (aucun mode)\n\n"); in->Release(); dev->Release(); continue; }

        Ecoute *cb = new Ecoute(in);
        in->SetCallback(cb);
        BMDVideoInputFlags drapeaux = detection ? bmdVideoInputEnableFormatDetection
                                                : bmdVideoInputFlagDefault;
        if (in->EnableVideoInput(depart->GetDisplayMode(), bmdFormat8BitYUV, drapeaux) != S_OK) {
            printf("   EnableVideoInput a ÉCHOUÉ\n\n");
        } else if (in->StartStreams() != S_OK) {
            printf("   StartStreams a ÉCHOUÉ\n\n");
        } else {
            sleep(secondes);
            const bool verrou = lire_verrou(dev);
            in->StopStreams();
            printf("   verrou APRÈS activation  : %s\n", verrou ? "OUI" : "non");
            printf("   trames                   : %d valides, %d sans source\n",
                   cb->valides_, cb->sans_source_);
            if (cb->changements_)
                printf("   format détecté           : %s (%ldx%ld, %.3f i/s), %d changement(s)\n",
                       cb->format_.c_str(), cb->largeur_, cb->hauteur_, cb->fps_, cb->changements_);
            else
                printf("   format détecté           : AUCUN changement signalé\n");
            printf("   timecode (API dédiée)    : %s\n", cb->tc_vu_ ? cb->tc_ : "AUCUN");
            printf("   ATC par recherche directe: %s\n", cb->anc_par_id_ ? "TROUVÉ (0x60/0x60)" : "absent");
            printf("   paquets ANC (itérateur)  : %ld%s%s\n", cb->anc_paquets_,
                   cb->anc_dids_[0] ? " — 1er : " : "", cb->anc_dids_);
            printf("   → %s\n\n", cb->valides_ > 0 ? "SIGNAL PRÉSENT"
                                : (cb->sans_source_ > 0 ? "pas de signal (trames marquées sans source)"
                                                        : "aucune trame reçue"));
            essayes++;
        }
        in->DisableVideoInput();
        in->SetCallback(NULL);
        cb->Release();
        depart->Release();
        in->Release();
        dev->Release();
    }
    it->Release();
    if (!essayes) { printf("aucun sous-périphérique actif capable de capture\n"); return 3; }
    return 0;
}
