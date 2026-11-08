#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""L'audio d'une sortie SDI se reprend-il en cours de route ?

★★ POURQUOI UNE GARDE DE TEXTE ICI. Le vrai essai demanderait une carte, un flux et une image
   reconstruite ; cette garde-là ne pourrait pas tourner en intégration continue, et c'est
   justement en intégration continue que la faute du 21/10 serait passée : une sortie démarrée
   AVANT l'existence de son flux audio restait muette POUR TOUJOURS, en annonçant 50 i/s et
   genlock verrouillé. Rien ne la signalait — `audio_samples` à 0 se lit comme une source muette.

   On vérifie donc les deux invariants de STRUCTURE qui rendent la reprise possible, et qui
   étaient tous deux faux avant le correctif :
     1. l'ouverture du lecteur audio existe DANS LE CHEMIN PAR TRAME (`servir_audio`), pas
        seulement au démarrage — sinon « pas encore là » est un verdict définitif ;
     2. l'audio de la CARTE est configuré sur le nombre de canaux DEMANDÉ, pas sur la présence
        d'un lecteur — sinon le flux peut bien apparaître, il n'y a pas de piste sur le fil.
   Et un troisième, qui est la seule façon de VOIR l'attente de l'extérieur :
     3. la ligne de statistiques publie `audio_pending`.
"""
import os
import re
import sys

ICI = os.path.dirname(os.path.abspath(__file__))


def corps_de_fonction(src, nom):
    """Rend le corps de `static ... nom(...) { ... }` par comptage d'accolades."""
    m = re.search(r"\n(?:static\s+)?[\w:<>*&\s]+\b" + re.escape(nom) + r"\s*\([^;{]*\)\s*\{",
                  src)
    if not m:
        return None
    i = src.index("{", m.start())
    prof = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            prof += 1
        elif src[j] == "}":
            prof -= 1
            if prof == 0:
                return src[i:j + 1]
    return None


def verifier(src):
    """Rend la liste des manquements (vide = conforme)."""
    maux = []

    corps = corps_de_fonction(src, "servir_audio")
    if corps is None:
        maux.append("servir_audio() introuvable — la garde ne sait plus quoi lire")
    elif not ("mxlCreateFlowReader" in corps or "ouvrir_flux_audio(" in corps):
        # ⚠ L'OUVERTURE PEUT ÊTRE DÉLÉGUÉE. Depuis 0.29.24 elle passe par `ouvrir_flux_audio()`,
        #   qui ouvre ET compare les canaux déclarés : exiger l'appel SDK en direct rendait
        #   cette garde rouge pour une amélioration. Ce qui compte est qu'une ouverture ait
        #   lieu DANS le chemin par trame, pas laquelle.
        maux.append("servir_audio() n'ouvre jamais le lecteur audio : un flux absent au "
                    "démarrage ne sera JAMAIS repris (sortie muette pour toujours)")

    # 2. Le `if` qui précède EnableAudioOutput doit porter sur les canaux, pas sur le lecteur.
    m = re.search(r"if\s*\(([^)]*)\)\s*\{[^{}]*?EnableAudioOutput", src, re.S)
    if not m:
        m = re.search(r"if\s*\(([^)]*)\)\s*\{(?:[^{}]|\{[^{}]*\})*?EnableAudioOutput", src, re.S)
    if not m:
        maux.append("EnableAudioOutput() introuvable sous une condition — garde à revoir")
    else:
        cond = m.group(1)
        if "rd_audio" in cond:
            maux.append("l'audio de la carte est configuré SOUS CONDITION d'un lecteur "
                        "(`%s`) : le flux pourra apparaître, il n'y aura pas de piste audio "
                        "sur le fil" % cond.strip())
        if "canaux_audio" not in cond:
            maux.append("la condition de EnableAudioOutput (`%s`) ne porte pas sur le nombre "
                        "de canaux demandé" % cond.strip())

    # 3. L'attente doit être VISIBLE de l'extérieur.
    if '"audio_pending"' not in src.replace("\\", ""):
        maux.append("la ligne de statistiques ne publie pas `audio_pending` : « pas de son "
                    "demandé » et « son demandé, flux pas encore là » se liraient pareil")

    return maux


def main():
    chemin = os.path.join(ICI, "decklink_tx.cpp")
    src = open(chemin, encoding="utf-8").read()
    maux = verifier(src)
    for m in maux:
        print("  ÉCHEC  %s" % m)
    if maux:
        return 1
    print("  ok   la sortie SDI reprend l'audio en cours de route (3 invariants)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
