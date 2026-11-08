#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Garde : tout compteur publié par les DEUX binaires arrive jusqu'à l'état (24/10, étendu 28/10).

CE QUE CETTE GARDE PROTÈGE
    Le binaire `decklink_tx` publie ses compteurs dans un JSON ; le superviseur en recopie une
    LISTE BLANCHE dans l'état que lisent `/state` et la console. Un compteur absent de cette
    liste existe, est écrit, et n'est visible de personne — il faut ouvrir un terminal dans le
    conteneur pour le lire.

    C'EST ARRIVÉ DEUX FOIS. Le 22/10 pour `audio_group2_missing` et `audio_resyncs` : il a fallu
    un `docker exec` pour répondre au testeur, et le commentaire posé alors conclut « un
    compteur qu'on doit aller chercher à la main n'est pas publié ». Le 24/10, DEUX LIGNES PLUS
    BAS, pour dix autres (`sliced`, `head_ontime`, `late_previous`, `slice_timeouts`, `dropped`,
    `late`, `flushed`…) : une sortie répétait 15 trames par seconde et le seul chiffre visible,
    `repeated`, ne distinguait pas les trois pannes possibles — nous en retard, la carte qui
    refuse, ou la source qui ne commet pas. Il a encore fallu un `docker exec`.

    Le commentaire n'a rien protégé. C'est le rôle d'une garde.

    ⚠⚠ ET ELLE NE REGARDAIT QUE `decklink_tx` — LA MOITIÉ DU CODE. Le 28/10, l'état d'une
    ENTRÉE publiait 7 champs sur les 39 que `decklink_rx` écrit : la console audio #214 ne
    recevait rien des entrées SDI, le binaire SAVAIT que son index audio dérivait, et il a
    encore fallu un `docker exec` pour le lire. Une garde qui ne couvre qu'une moitié du code
    donne la même confiance qu'une garde complète, sans la rendre — c'est mot pour mot ce que
    dit le Makefile de ce plugin à propos de sa cible `syntaxe`, pour la même raison.

POURQUOI COMPARER LES DEUX SOURCES
    On ne vérifie pas une liste écrite à la main : elle se périmerait comme la précédente. On lit
    les clés que le C++ publie RÉELLEMENT dans sa chaîne de format, et on exige que l'état les
    reprenne toutes. Un compteur ajouté au binaire sans être publié fait rougir cette garde.
"""
import ast
import os
import re
import sys

ICI = os.path.dirname(os.path.abspath(__file__))

# Clés dont l'absence de l'état est VOULUE, avec la raison. Toute autre doit être publiée.
TOLEREES = {
    "t_frame_ns": "horodatage interne, sans intérêt pour l'exploitant",
    "frame_index": "déjà repris sous ce nom par la voie",
    "flow": "porté par la vue de sortie elle-même",
    "genlock_samples": "détail de la fenêtre de mesure du genlock",
    # Propres à l'entrée : déjà portées par la vue d'entrée sous ces noms-là.
    "signal": "déjà repris tel quel par la vue d'entrée",
    "fps": "déjà repris tel quel",
    "width": "déjà repris tel quel",
    "height": "déjà repris tel quel",
}

# ⚠ LES GUILLEMETS SONT ÉCHAPPÉS DANS LE SOURCE C (`\"fps\": %.2f`). Le motif doit donc viser
#   antislash + guillemet, soit `\\"` en expression régulière. Première version : `\"`, qui en
#   expression régulière vaut un simple guillemet — elle ne trouvait aucune clé réelle et
#   attrapait les `? \"true\" : \"false\"` des ternaires. Elle annonçait alors « la chaîne de
#   format a changé de forme » : un diagnostic faux, qui envoie chercher la panne ailleurs.
MOTIF_CLE = re.compile(r'\\"([a-z_0-9]+)\\"\s*:')


def cles_publiees_par_le_binaire(src):
    """Les clés JSON de la chaîne de format du `fprintf` de `stats()`."""
    i = src.find(r'fprintf(f, "{\"t_frame_ns')
    if i < 0:
        return []
    # Borner au PROCHAIN `fprintf(` : la chaîne est découpée en plusieurs littéraux séparés par
    # des commentaires, dont certains contiennent `);` — couper au premier `);` n'en ramenait
    # qu'une poignée.
    j = src.find("fprintf(", i + 10)
    return MOTIF_CLE.findall(src[i:j if j > 0 else i + 8000])


def cles_de_letat_sortie(py):
    """Les CLÉS que l'état de sortie publie, lues dans l'arbre du script rendu.

    ⚠ NE PAS CHERCHER LE NOM DANS LE TEXTE DU BLOC : `d.get("audio_pending")` contient la
      chaîne, donc un compteur publié sous une clé FAUSSE
      (`"x_pending": d.get("audio_pending")`) satisfaisait le motif. Mutation trouvée inerte
      le 24/10. On ne retient donc que les clés du dictionnaire — le côté GAUCHE.
    """
    rendu = py.format(config=repr({"outputs": []}), hostname="d", plugin_version="0")
    arbre = ast.parse(rendu)
    cles = set()
    for n in ast.walk(arbre):
        if not (isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute)
                and n.func.attr == "update"):
            continue
        cible = n.func.value
        if not (isinstance(cible, ast.Name) and cible.id == "etat"):
            continue
        for arg in n.args:
            if isinstance(arg, ast.Dict):
                for k in arg.keys:
                    if isinstance(k, ast.Constant) and isinstance(k.value, str):
                        cles.add(k.value)
    return cles


def main():
    py = open(os.path.join(ICI, "script.py"), encoding="utf-8").read()

    # ★ LES DEUX BINAIRES, PAS UN SEUL. Celui de capture publie 39 clés, celui de sortie 51 :
    #   n'en regarder qu'un laissait l'autre perdre ses compteurs en silence.
    publiees = []
    for nom in ("decklink_tx.cpp", "decklink_rx.cpp"):
        cpp = open(os.path.join(ICI, nom), encoding="utf-8").read()
        trouvees = cles_publiees_par_le_binaire(cpp)
        if len(trouvees) < 20:
            print("ÉCHEC  %d clé(s) seulement relue(s) dans la chaîne de format de %s — c'est "
                  "cette GARDE qui est à réparer, pas le code" % (len(trouvees), nom))
            return 1
        print("  %-20s %d clé(s) publiée(s)" % (nom, len(trouvees)))
        publiees += [k for k in trouvees if k not in publiees]

    cles_etat = cles_de_letat_sortie(py)
    if len(cles_etat) < 10:
        print("ÉCHEC  %d clé(s) seulement relue(s) dans les `etat.update(...)` du script — "
              "cette GARDE est à adapter" % len(cles_etat))
        return 1

    manquantes = [k for k in publiees if k not in TOLEREES and k not in cles_etat]
    print("clés publiées par le binaire : %d | tolérées : %d"
          % (len(publiees), len(TOLEREES)))
    if manquantes:
        print("\nÉCHEC  %d compteur(s) écrit(s) par le binaire et JETÉ(S) par l'état :"
              % len(manquantes))
        for k in manquantes:
            print("         - %s" % k)
        print("\n       Ils existent, ils sont écrits, et personne ne les voit : il faut un "
              "`docker exec` pour les lire.\n       Les ajouter à `etat.update(...)`, ou les "
              "inscrire dans TOLEREES avec la raison.")
        return 1
    print("ok     tout compteur publié par le binaire arrive jusqu'à l'état")
    return 0


if __name__ == "__main__":
    sys.exit(main())
