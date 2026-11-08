// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 BOBI SAS, France
//
// FAUSSE libDeckLinkAPI.so, pour vérifier la sonde SANS carte ni pilote.
//
// Pourquoi elle existe : sans matériel, `decklink_probe` ne peut répondre que « SDK absent », et une
// sonde qui répondrait ÇA quoi qu'il arrive passerait pour bonne jusqu'au jour du montage. Ce
// stub fournit l'autre branche — une bibliothèque qui se charge et annonce zéro carte — pour que
// les deux réponses soient exercées et constatées DIFFÉRENTES avant l'arrivée de la carte.
//
// Il n'imite rien d'autre : aucun périphérique, aucun profil. Il ne sert qu'à distinguer.

#include "decklink_sdk/DeckLinkAPI.h"

namespace {

class VideIterateur : public IDeckLinkIterator {
public:
    HRESULT QueryInterface(REFIID, void **v) override { *v = nullptr; return E_NOINTERFACE; }
    ULONG   AddRef(void) override  { return ++refs_; }
    ULONG   Release(void) override { ULONG r = --refs_; if (!r) delete this; return r; }
    HRESULT Next(IDeckLink **d) override { *d = nullptr; return S_FALSE; }  // zéro carte
private:
    ULONG refs_ = 1;
};

}  // namespace

extern "C" IDeckLinkIterator *CreateDeckLinkIteratorInstance_0004(void) {
    return new VideIterateur();
}
