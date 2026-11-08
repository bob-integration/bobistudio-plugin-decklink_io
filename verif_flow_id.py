#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 BOBI SAS, France
#
# Compare l'UUID de flux du chemin natif DeckLink à celui de bobimxl. Les deux DOIVENT coïncider :
# le flowDef porte cet id, et c'est par lui que les consommateurs ouvrent le flux. Une divergence
# ne produit aucune erreur — juste un flux que personne ne trouve.
import subprocess, sys, uuid, pathlib

NS = uuid.uuid5(uuid.NAMESPACE_DNS, "mxl.bobi.studio")   # = bobimxl._NS_BOBI
NOMS = ["decklink-1", "sdi_1", "dl-4040731139", "", "éà-accents", "x" * 200,
        "decklink_io_1", "2110-io-dl360-1_2"]

ici = pathlib.Path(__file__).parent
exe = ici / "verif_flow_id"
if not exe.exists():
    print("construire d'abord : make verif_flow_id"); sys.exit(1)

sortie = subprocess.run([str(exe), *NOMS], capture_output=True, text=True)
obtenus = sortie.stdout.split()
ko = 0
for nom, got in zip(NOMS, obtenus):
    attendu = str(uuid.uuid5(NS, nom))
    etat = "ok  " if got == attendu else "ÉCHEC"
    if got != attendu: ko = 1
    print(f"  {etat} {nom!r:24} → {got}")
if len(obtenus) != len(NOMS):
    print(f"  ÉCHEC : {len(obtenus)} sorties pour {len(NOMS)} noms"); ko = 1
print("→ les deux implémentations concordent." if not ko
      else "→ ÉCHEC : divergence — les flux natifs seraient introuvables côté Python.")
sys.exit(ko)
