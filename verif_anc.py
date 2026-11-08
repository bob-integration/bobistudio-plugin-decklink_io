#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 BOBI SAS, France
#
# Compare l'empaqueteur ANC RFC 8331 natif (anc_rfc8331.h) à la référence
# `bobimxl.anc_pack_rfc8331`, OCTET POUR OCTET.
#
# Pourquoi c'est le contrôle qui compte : un grain ANC mal sérialisé se lit SANS ERREUR. Le
# consommateur y voit « ANC count: 0 » ou des paquets fantaisistes, et rien ne signale qu'un
# octet était faux. Deux implémentations d'un même format ne restent d'accord que si on le
# vérifie.

import os, random, subprocess, sys, pathlib

ICI = pathlib.Path(__file__).parent
sys.path.insert(0, "/opt/bobistudio/script_templates")
try:
    import bobimxl
except ImportError:
    print("bobimxl introuvable — lancer depuis une machine qui l'a"); sys.exit(1)

EXE = ICI / "verif_anc"
if not EXE.exists():
    print("construire d'abord : make verif_anc"); sys.exit(1)


def cas():
    """Les cas limites d'abord — c'est là que deux implémentations divergent."""
    yield 0, []                                              # aucun paquet
    yield 0, [dict(did=0x60, sdid=0x60, line=9,  hori=0xFFF, c=0, s=0, stream_num=0,
                   udw=bytes(range(16)))]                    # timecode RP188 typique
    yield 2, [dict(did=0x61, sdid=0x01, line=21, hori=0xFFF, c=0, s=0, stream_num=0,
                   udw=b"\x00")]                             # 1 seul UDW, champ 1
    yield 3, [dict(did=0x41, sdid=0x05, line=10, hori=0x123, c=1, s=1, stream_num=127,
                   udw=bytes([0xFF] * 255))]                 # Data_Count MAXIMAL, tous les champs à fond
    yield 0, [dict(did=0x60, sdid=0x60, line=9, hori=0xFFF, c=0, s=0, stream_num=0, udw=b"")]  # UDW vide
    # Plusieurs paquets : c'est l'alignement 32 bits entre paquets qui se teste ici.
    yield 0, [dict(did=0x60, sdid=0x60, line=9,  hori=0xFFF, c=0, s=0, stream_num=0, udw=b"\x01\x02\x03"),
              dict(did=0x61, sdid=0x02, line=11, hori=0xFFF, c=0, s=0, stream_num=0, udw=b"\x04"),
              dict(did=0x43, sdid=0x02, line=12, hori=0xFFF, c=0, s=0, stream_num=0, udw=bytes(range(7)))]
    rnd = random.Random(20260908)                            # graine FIXE : un échec se rejoue
    for _ in range(60):
        n = rnd.randint(1, 6)
        yield rnd.choice((0, 2, 3)), [
            dict(did=rnd.randrange(256), sdid=rnd.randrange(256), line=rnd.randrange(2048),
                 hori=rnd.randrange(4096), c=rnd.randrange(2), s=rnd.randrange(2),
                 stream_num=rnd.randrange(128),
                 udw=bytes(rnd.randrange(256) for _ in range(rnd.randrange(0, 40))))
            for _ in range(n)]


lignes, attendus = [], []
for field, pk in cas():
    lignes.append("%d;%s" % (field, "|".join(
        "%d,%d,%d,%d,%d,%d,%d:%s" % (p["did"], p["sdid"], p["line"], p["hori"], p["c"], p["s"],
                                     p["stream_num"], p["udw"].hex())
        for p in pk)))
    attendus.append(bytes(bobimxl.anc_pack_rfc8331(pk, field=field)).hex())

res = subprocess.run([str(EXE)], input="\n".join(lignes) + "\n", capture_output=True, text=True)
obtenus = res.stdout.split()

ko = 0
for i, (att, got) in enumerate(zip(attendus, obtenus)):
    if att != got:
        ko += 1
        if ko <= 3:
            print("  ÉCHEC cas %d\n    attendu %s\n    obtenu  %s" % (i, att[:120], got[:120]))
if len(obtenus) != len(attendus):
    print("  ÉCHEC : %d sorties pour %d cas" % (len(obtenus), len(attendus))); ko += 1
print("  %d cas comparés octet pour octet, %d divergence(s)" % (len(attendus), ko))
# ── ATC : le timecode → 16 UDW ────────────────────────────────────────────────────────────────
# Séparé de l'empaquetage : une charge utile ATC fausse produit un grain PARFAITEMENT sérialisé
# qui affiche le mauvais timecode. Deux erreurs successives ont déjà été payées là-dessus le
# 2026-08-07, dont une invisible tant que le compteur restait sous dix images — d'où les cas qui
# FRANCHISSENT la dizaine ci-dessous.
cas_tc = [(0,0,0,0,0), (0,0,0,9,0), (0,0,0,10,0), (0,0,0,29,0),     # dizaines d'images
          (0,1,5,21,0),                                              # le cas exact du bug de 2026-08-07
          (0,0,59,24,0), (0,59,59,24,0), (23,59,59,24,0),            # bornes
          (12,34,56,12,1), (0,0,0,0,1), (19,0,0,0,0), (9,9,9,9,0),
          # HORS BORNES délibéré : un timecode corrompu doit être masqué de la MÊME façon des
          # deux côtés. Sans ce cas, élargir un masque passe inaperçu — les valeurs valides ne
          # le font jamais mordre (23 h → 2, identique en 0x3 et 0x7).
          (40,0,0,0,0), (0,80,0,0,0), (0,0,80,0,0), (0,0,0,40,0)]
res2 = subprocess.run([str(EXE), "atc"],
                      input="\n".join("%d %d %d %d %d" % c for c in cas_tc) + "\n",
                      capture_output=True, text=True)
got2 = res2.stdout.split()
ko2 = 0
for (h, m, s, f, df), g in zip(cas_tc, got2):
    att = bobimxl.anc_atc_encode(h, m, s, f, drop_frame=bool(df)).hex()
    if att != g:
        ko2 += 1
        print("  ÉCHEC ATC %02d:%02d:%02d:%02d df=%d\n    attendu %s\n    obtenu  %s"
              % (h, m, s, f, df, att, g))
if len(got2) != len(cas_tc):
    print("  ÉCHEC ATC : %d sorties pour %d cas" % (len(got2), len(cas_tc))); ko2 += 1
print("  %d timecodes comparés, %d divergence(s)" % (len(cas_tc), ko2))

print("→ empaqueteur ET charge utile ATC concordent." if not (ko or ko2)
      else "→ ÉCHEC : le grain ANC natif ne serait PAS lu correctement.")
sys.exit(1 if (ko or ko2) else 0)
