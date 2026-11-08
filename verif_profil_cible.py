#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 BOBI SAS, France
#
# Garde sur `_profil_cible`, la fonction qui décide si l'on RECONFIGURE la carte au démarrage.
# Elle vaut une garde à elle seule : se tromper ici, c'est basculer une carte qui n'avait rien
# demandé — et une bascule de profil COUPE les prises. Deux cas comptent plus que les autres :
#
#   · une carte MONO-PROFIL (`profils: null`, ce que la sonde écrit faute de gestionnaire de
#     profil) ne doit JAMAIS produire de cible : il n'y a rien à basculer ;
#   · un profil FULL-duplex ne doit jamais être choisi, même s'il est le seul offert : il fait
#     entrer ET sortir sur la même prise, ce dont nous ne voulons nulle part.
#
# La fonction est extraite du GABARIT (`script.py` passe par str.format : accolades doublées),
# pas recopiée — une copie se périmerait en silence.
import pathlib, re, sys

src = (pathlib.Path(__file__).parent / "script.py").read_text(encoding="utf-8")
m = re.search(r"^def _profil_cible\(sub\):.*?(?=^def |\Z)", src, re.S | re.M)
if not m:
    print("ÉCHEC : _profil_cible introuvable dans script.py"); sys.exit(1)
ns = {"re": re}
exec(m.group(0).replace("{{", "{").replace("}}", "}"), ns)          # noqa: S102
_profil_cible = ns["_profil_cible"]

def p(*ids):
    return {"profils": [{"id": i} for i in ids]}

CAS = [
    ("carte mono-profil (profils: null) — rien à basculer", {"profils": None},   None),
    ("aucun profil offert",                                 {"profils": []},     None),
    ("relevé sans la clé du tout",                          {},                  None),
    # ★ LE SEUL CAS MESURÉ, relevé sur la Duo 2 de dell-1 le 20/10 : elle n'offre QUE ces
    #   deux profils — il n'y a pas de `4dhd` sur une Duo 2. Et son `2dhd` actif expose
    #   QUATRE prises half-duplex indépendantes : le chiffre du four-CC ne compte pas les
    #   connecteurs. Les cas qui suivent restent, eux, fabriqués.
    ("Duo 2 RÉELLE : 1dfd + 2dhd → 2dhd",                   p("1dfd","2dhd"),    "2dhd"),
    ("plusieurs half-duplex : le plus grand chiffre",       p("2dhd","2dfd","1dfd","4dhd"), "4dhd"),
    ("ordre inverse : le choix ne dépend pas de l'ordre",    p("4dhd","2dhd"),    "4dhd"),
    ("QUE du full-duplex → on refuse de choisir",            p("1dfd","2dfd"),    None),
    ("1dhd seul reste mieux que rien",                      p("1dhd"),           "1dhd"),
    ("identifiants inconnus ignorés",                        p("zzz","","4d","dhd"), None),
    # Le SDK s'arrête à `4dhd`. Si une carte en offrait davantage, la RÈGLE doit la suivre sans
    # qu'on touche au code — c'est tout l'intérêt de lire le chiffre plutôt que le modèle. (Ce
    # chiffre ordonne les profils ; il ne dit PAS combien de prises on obtiendra.)
    ("un hypothétique 8dhd serait choisi (règle calculée)", p("4dhd","8dhd"),    "8dhd"),
]

ko = 0
for titre, sub, attendu in CAS:
    got = _profil_cible(sub)
    bon = got == attendu
    ko |= 0 if bon else 1
    print("  %s %-52s → %r" % ("ok  " if bon else "ÉCHEC", titre, got))
print("→ la règle de profil ne reconfigure que ce qu'elle doit." if not ko
      else "→ ÉCHEC : la carte pourrait être basculée à tort.")
sys.exit(ko)
