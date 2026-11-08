// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 BOBI SAS, France
//
// Contrôle de COUVERTURE des en-têtes vendorisés (12.2.2, cf. README) : tout ce dont le chantier
// `decklink_io` dépend est-il réellement DÉCLARÉ, et avec des types utilisables ?
//
// Pourquoi un fichier et pas un grep : un grep trouve un symbole cité dans un commentaire, ou
// une énumération dont la valeur a changé de type. Ici le compilateur exige que chaque méthode
// existe avec la bonne signature et que chaque constante soit de son type d'énumération. Ce
// fichier n'est jamais exécuté — il ne sert qu'à être compilé.

#include "decklink_sdk/DeckLinkAPI.h"

// Les constantes, chacune dans le type d'énumération qui doit la porter.
static BMDDeckLinkAttributeID  a1 = BMDDeckLinkSupportsSynchronizeToCaptureGroup;
static BMDDeckLinkAttributeID  a2 = BMDDeckLinkSupportsSynchronizeToPlaybackGroup;
static BMDDeckLinkAttributeID  a3 = BMDDeckLinkPersistentID;      // ★ clé d'identité d'un flux
static BMDDeckLinkAttributeID  a4 = BMDDeckLinkTopologicalID;
static BMDDeckLinkConfigurationID c1 = bmdDeckLinkConfigCaptureGroup;
static BMDDeckLinkConfigurationID c2 = bmdDeckLinkConfigPlaybackGroup;
static BMDDeckLinkStatusID     s1 = bmdDeckLinkStatusReferenceSignalLocked;
static BMDDeckLinkStatusID     s2 = bmdDeckLinkStatusVideoInputSignalLocked;
static BMDVideoInputFlags      f1 = bmdVideoInputSynchronizeToCaptureGroup;
static BMDVideoOutputFlags     f2 = bmdVideoOutputSynchronizeToPlaybackGroup;
static BMDPixelFormat          p1 = bmdFormat10BitYUV;            // v210, format natif de la carte
static BMDOutputFrameCompletionResult o1 = bmdOutputFrameDisplayedLate;
static BMDOutputFrameCompletionResult o2 = bmdOutputFrameDropped;
static BMDProfileID            r1 = bmdProfileTwoSubDevicesHalfDuplex;
static BMDProfileID            r2 = bmdProfileOneSubDeviceFullDuplex;

// Les méthodes, prises comme pointeurs de membre : le compilateur vérifie la signature entière.
static HRESULT (IDeckLinkOutput::*m1)(IDeckLinkVideoFrame *, BMDTimeScale, BMDTimeValue *)
    = &IDeckLinkOutput::GetFrameCompletionReferenceTimestamp;     // ★ le pont vers l'horloge TAI
static HRESULT (IDeckLinkOutput::*m2)(BMDReferenceStatus *)
    = &IDeckLinkOutput::GetReferenceStatus;                       // ★ témoin de genlock
static HRESULT (IDeckLinkVideoInputFrame::*m3)(BMDTimeScale, BMDTimeValue *, BMDTimeValue *)
    = &IDeckLinkVideoInputFrame::GetHardwareReferenceTimestamp;
static HRESULT (IDeckLinkOutput::*m4)(IDeckLinkVideoFrame *)
    = &IDeckLinkOutput::DisplayVideoFrameSync;                    // piste basse latence (§4)
static HRESULT (IDeckLinkOutput::*m5)(void)  = &IDeckLinkOutput::BeginAudioPreroll;
static HRESULT (IDeckLinkOutput::*m6)(uint32_t *) = &IDeckLinkOutput::GetBufferedVideoFrameCount;
static HRESULT (IDeckLinkProfile::*m7)(IDeckLinkProfileIterator **) = &IDeckLinkProfile::GetPeers;
static HRESULT (IDeckLinkProfile::*m8)(void) = &IDeckLinkProfile::SetActive;

// Les interfaces dont le chantier dépend doivent exister comme types complets.
static IDeckLinkKeyer                      *i1;  // incrustation (profil full-duplex)
static IDeckLinkVideoFrameAncillaryPackets *i2;  // ANC / RFC 8331
static IDeckLinkProfileManager             *i3;
static IDeckLinkVideoConversion            *i4;  // conversions SIMD (§6)
static IDeckLinkStatus                     *i5;

// Consomme tout pour qu'aucun avertissement « inutilisé » ne masque une absence.
void bobi_verif_entetes(void) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)c1; (void)c2; (void)s1; (void)s2;
    (void)f1; (void)f2; (void)p1; (void)o1; (void)o2; (void)r1; (void)r2;
    (void)m1; (void)m2; (void)m3; (void)m4; (void)m5; (void)m6; (void)m7; (void)m8;
    (void)i1; (void)i2; (void)i3; (void)i4; (void)i5;
}
