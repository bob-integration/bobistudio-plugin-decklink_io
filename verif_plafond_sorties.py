#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Garde : le plafond de sorties simultanées ne redevient pas silencieux (22/10).

CE QUE CETTE GARDE PROTÈGE
    Au-delà de 7 sorties simultanées, des prises passent en retard permanent et la carte
    ÉCARTE LEUR AUDIO : l'image reste, le son disparaît, et aucun compteur ne bronche —
    50 i/s et genlock verrouillé sur toutes. Le testeur a cherché la panne une journée entière
    dans ses câbles, ses canaux et ses flux. Le défaut n'est pas la limite, c'est son SILENCE.

    Mesuré par bissection : 7 sorties → 0 trame tardive ; 8 → 4 sorties à 99 % de retard.
    Indépendant du connecteur, de la carte, du processeur, et de la durée (17 h 24 à 4 sorties
    sans une trame tardive).

POURQUOI UNE GARDE SUR LE TEXTE DU SOURCE
    L'avertissement ne se déclenche qu'avec huit connecteurs déclarés : le banc de console
    tourne sur un relevé de carte qui n'en a pas assez, et truquer son décor pour faire passer
    un test est précisément ce que ce banc s'interdit. On vérifie donc les quatre propriétés
    dont la perte rouvrirait le silence — chacune mutée pour vérifier qu'elle mord.
"""
import json
import os
import re
import sys

ICI = os.path.dirname(os.path.abspath(__file__))
JS = os.path.join(ICI, "control.js")


def _lire(p):
    with open(p, encoding="utf-8") as f:
        return f.read()


def verif_plafond_declare(js):
    """La limite est une CONSTANTE NOMMÉE, pas un 7 nu perdu dans une condition."""
    m = re.search(r"const\s+PLAFOND_SORTIES_PAR_CARTE\s*=\s*(\d+)\s*;", js)
    if not m:
        return "`PLAFOND_SORTIES_PAR_CARTE` n'est plus déclaré"
    if int(m.group(1)) != 5:
        return ("le plafond vaut %s au lieu de 5 — s'il a été REMESURÉ, mettre à jour cette "
                "garde et la mesure qui le justifie dans le commentaire" % m.group(1))
    # ⚠ LE COMPTE EST PAR CARTE, PAS UN TOTAL. Première version de cet avertissement : un
    #   total de 7, qui décrivait en fait une RÉPARTITION (5 Quad + 2 Duo) prise pour une
    #   limite. Sept sorties toutes sur la Quad échouent, six réparties passent.
    if "cartesEnSurcharge" not in js:
        return "le regroupement PAR CARTE a disparu : un total ne prédit pas la panne"
    return None


def verif_bandeau_avertit(js):
    """Le bandeau avertit tant que la configuration dépasse le plafond.

    C'est le seul endroit qui le dit SANS GESTE de l'exploitant : quelqu'un qui arrive devant
    une machine déjà mal configurée n'a pas cliqué sur « Appliquer » et ne verra jamais la note.
    """
    if not re.search(r"cartesEnSurcharge\(\s*Array\.from\(\s*cfg\.sorties\.values", js):
        return "le bandeau ne compte plus les sorties PAR CARTE"
    if "avert.trop_de_sorties" not in js:
        return "le bandeau n'émet plus l'avertissement `avert.trop_de_sorties`"
    return None


def verif_consentement_a_lapplication(js):
    """Déclarer la 8ᵉ sortie exige un SECOND clic.

    Une note qui défile pendant que l'application se fait n'est pas un avertissement. Le
    consentement doit aussi se REMETTRE À ZÉRO, sinon il vaut pour tous les gestes suivants.
    """
    # Depuis 0.29.53 (07/11) les prises s'appliquent EN LOT : c'est `appliquerLot()` qui envoie,
    # donc c'est lui qui doit compter le plafond avant d'envoyer.
    i = js.find("async function appliquerLot()")
    if i < 0:
        return "`appliquerLot()` est introuvable"
    corps = js[i:i + 3000]
    if "cartesEnSurcharge" not in corps:
        return "`appliquerLot()` ne contrôle plus le plafond par carte avant d'envoyer"
    if "plafondAccepte" not in corps:
        return "`appliquerLot()` n'exige plus de consentement explicite (second clic)"
    # ⚠ NE PAS CHERCHER « plafondAccepte = false » TOUT COURT : la DÉCLARATION
    #   (`let plafondAccepte = false;`) satisfait ce motif et l'assertion survit à la
    #   suppression de la remise à zéro. Première version de cette garde : verte, et INERTE —
    #   trouvée par mutation. On compte donc les affectations qui ne sont PAS la déclaration.
    remises = [m for m in re.finditer(r"(let\s+)?plafondAccepte\s*=\s*false", js)
               if not m.group(1)]
    if not remises:
        return ("le consentement de plafond ne se remet jamais à zéro (hors déclaration) : "
                "il vaudrait pour tous les gestes suivants")
    return None


def verif_textes_traduits(js):
    """Les deux messages existent dans LES DEUX langues, avec les mêmes marqueurs.

    Un marqueur oublié dans une langue donne « {max} sorties » à l'écran — et c'est l'anglais
    qui le prendrait, donc personne ici ne le verrait.
    """
    cles = ["plugin.decklink_io.avert.trop_de_sorties",
            "plugin.decklink_io.note.plafond_sorties"]
    cats = {}
    for lang in ("fr", "en"):
        p = os.path.join(ICI, "i18n", "%s.json" % lang)
        try:
            cats[lang] = json.load(open(p, encoding="utf-8"))
        except Exception as exc:
            return "catalogue %s illisible : %s" % (lang, exc)
    for cle in cles:
        for lang in ("fr", "en"):
            if cle not in cats[lang]:
                return "clé %s absente du catalogue %s" % (cle, lang)
        mf = sorted(re.findall(r"\{[a-z]+\}", cats["fr"][cle]))
        me = sorted(re.findall(r"\{[a-z]+\}", cats["en"][cle]))
        if mf != me:
            return "%s : marqueurs différents entre fr %s et en %s" % (cle, mf, me)
        if not mf:
            return "%s ne porte plus aucun marqueur : le message ne peut plus citer de chiffre" % cle
    return None


def main():
    js = _lire(JS)
    verifs = (
        ("le plafond est une constante nommée", lambda: verif_plafond_declare(js)),
        ("le bandeau avertit sans geste", lambda: verif_bandeau_avertit(js)),
        ("la 8ᵉ sortie exige un second clic", lambda: verif_consentement_a_lapplication(js)),
        ("les messages existent dans les deux langues", lambda: verif_textes_traduits(js)),
    )
    ko = 0
    for titre, f in verifs:
        motif = f()
        if motif:
            ko += 1
            print("ÉCHEC  %s : %s" % (titre, motif))
        else:
            print("ok     %s" % titre)
    if ko:
        print("\n%d vérification(s) en échec — le plafond redeviendrait SILENCIEUX, "
              "et c'est son silence qui a coûté une journée au testeur." % ko)
    return 1 if ko else 0


if __name__ == "__main__":
    sys.exit(main())
