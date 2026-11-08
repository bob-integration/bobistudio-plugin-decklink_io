#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Garde : l'échéance d'une sortie se confronte au TEMPS DE FLUX, jamais à l'horloge matérielle.

CE QUE CETTE GARDE PROTÈGE
    `prochaine_` est l'échéance passée à `ScheduleVideoFrame` : elle vit dans le temps de FLUX,
    qui repart de l'instant donné à `StartScheduledPlayback` (0 chez nous). `GetHardwareReference
    Clock` vit dans un AUTRE domaine : il court depuis l'activation du sous-périphérique et ne
    repart pas. Les deux ne coïncident qu'au premier démarrage — donc un banc, qui part toujours
    d'une carte fraîche, ne voit JAMAIS la différence.

    Mesuré sur la Z8 les 2026-11-01 et 02 (#228 puis #237) : une sortie relancée à chaud sous le
    même `persistent_id` lisait son avance comme −T, où T était le temps écoulé depuis son
    démarrage précédent (−10 s, −439 s, −580 s selon le cas). Elle ré-ancrait UNE fois à T +
    preroll trames, puis attendait T secondes sur sa dernière image : « émet », `source_alive`
    vrai, genlock verrouillé, 0 i/s. Aucune erreur, aucun code de retour.

POURQUOI UNE GARDE SUR LE TEXTE DU SOURCE
    Le défaut n'était pas dans la formule `prochaine_echeance` — elle est juste, et une garde
    qui l'exerce seule passe au vert avec les deux domaines. Il était dans l'UNITÉ de son
    argument, c'est-à-dire chez l'appelant. C'est donc l'appelant qu'on vérifie, et il tient
    dans un binaire qui exige la carte : la seule lecture possible en CI est celle du source.
"""
import os
import re
import sys

ICI = os.path.dirname(os.path.abspath(__file__))
TX = os.path.join(ICI, "decklink_tx.cpp")

# Portée : le CODE, pas les pavés d'explication — qui doivent pouvoir nommer le défaut corrigé.
_COMMENTAIRE = re.compile(r"^\s*(///|//|\*)")


def _lignes_de_code(tx):
    return [(n, l) for n, l in enumerate(tx.splitlines(), 1) if not _COMMENTAIRE.match(l)]


def _variable_du_temps_de_flux(code, i, portee=8):
    """Nom de la variable que `GetScheduledStreamTime` remplit, lue dans les lignes qui précèdent.

    On rend le NOM, pas un booléen : exiger la présence de l'appel ne suffit pas — il peut rester
    là pendant que le calcul se fait sur une autre variable, et c'est précisément ce qu'une
    mutation du 02/11 a fait passer au vert.
    """
    for _, l in reversed(code[max(0, i - portee):i]):
        m = re.search(r"GetScheduledStreamTime\s*\([^,]+,\s*&(\w+)", l)
        if m:
            return m.group(1)
    return None


def verif_aucune_horloge_materielle_contre_lecheance(tx):
    """Aucun appel à l'horloge matérielle au voisinage de `prochaine_`.

    C'est la forme EXACTE du défaut : `GetHardwareReferenceClock` lu dans un bloc qui soustrait
    ou compare `prochaine_`. On refuse le voisinage plutôt que l'appel lui-même — mesurer la
    dérive de la carte contre l'horloge du nœud reste un usage légitime de cette fonction, et
    une garde qui l'interdirait partout serait contournée le jour où on en aura besoin.
    """
    code = _lignes_de_code(tx)
    for i, (n, l) in enumerate(code):
        if "GetHardwareReferenceClock" not in l:
            continue
        voisinage = " ".join(x for _, x in code[i:i + 12])
        if "prochaine_" in voisinage:
            return ("ligne %d : l'horloge matérielle est confrontée à `prochaine_`, qui est un "
                    "temps de FLUX — c'est le gel des sorties relancées (02/11)" % n)
    return None


def verif_le_reancrage_lit_le_temps_de_flux(tx):
    """L'argument de `prochaine_echeance` vient de `GetScheduledStreamTime`.

    Vérifié dans ce sens-là, et pas seulement par l'absence de l'autre appel : remplacer la
    lecture par une constante, ou par notre propre horloge, rouvrirait la panne sans réintroduire
    le nom interdit.
    """
    code = _lignes_de_code(tx)
    for i, (n, l) in enumerate(code):
        if "prochaine_echeance(" not in l or "static BMDTimeValue" in l:
            continue
        # L'appel peut tenir sur deux lignes : on prend l'argument là où il est écrit.
        appel = l + " " + (code[i + 1][1] if i + 1 < len(code) else "")
        var = _variable_du_temps_de_flux(code, i)
        if not var:
            return ("ligne %d : le ré-ancrage ne lit pas `GetScheduledStreamTime` juste avant — "
                    "son argument doit être un temps de FLUX" % n)
        if not re.search(r"\b%s\b" % re.escape(var), appel):
            return ("ligne %d : le ré-ancrage n'utilise pas `%s`, la variable que "
                    "`GetScheduledStreamTime` vient de remplir" % (n, var))
        return None
    return "aucun appel à `prochaine_echeance` : le ré-ancrage a disparu (cf. le défaut du 30/10)"


def verif_la_reserve_lit_le_temps_de_flux(tx):
    """La réserve avant l'instant visé (`reste_s`) se mesure dans le même domaine.

    Elle décide du budget d'attente des tranches, donc d'une image perdue à l'antenne. Mesurée
    contre l'horloge matérielle, elle vaut −T à chaque relance et interdit toute attente — et
    c'est aussi elle que publie `slack_min_ms`, le témoin par lequel le défaut a été vu.
    """
    code = _lignes_de_code(tx)
    for i, (n, l) in enumerate(code):
        if "reste_s" not in l or "=" not in l or "prochaine_" not in l:
            continue
        var = _variable_du_temps_de_flux(code, i)
        if not var:
            return ("ligne %d : `reste_s` ne se mesure pas sur le temps de flux" % n)
        if not re.search(r"\b%s\b" % re.escape(var), l):
            return ("ligne %d : `reste_s` se mesure contre autre chose que `%s`, la variable "
                    "que `GetScheduledStreamTime` vient de remplir" % (n, var))
        return None
    return "`reste_s` ne se calcule plus depuis `prochaine_` : la réserve n'est plus mesurée"


def verif_le_temoin_reste_publie(tx):
    """`slack_min_ms` et `schedule_reanchors` restent publiés.

    Ce sont les deux seuls témoins par lesquels ce gel se voit de l'extérieur — la sortie
    annonce par ailleurs un état parfaitement sain. Les retirer rendrait le prochain défaut
    de ce genre invisible, comme celui-ci l'a été jusqu'à ce qu'on lise ces deux chiffres.
    """
    # ⚠ DANS LE CODE SEUL. Cherchés dans tout le fichier, les deux noms se trouvaient dans le
    #   pavé qui RACONTE le défaut : la vérification restait verte après leur retrait de `/state`
    #   (mutée le 02/11, et c'est comme ça qu'on l'a su).
    code = " ".join(l for _, l in _lignes_de_code(tx))
    manquants = [c for c in ("slack_min_ms", "schedule_reanchors") if c not in code]
    if manquants:
        return "témoin(s) retiré(s) de `/state` : %s" % ", ".join(manquants)
    return None


def main():
    with open(TX, encoding="utf-8") as f:
        tx = f.read()
    verifications = (
        ("l'horloge matérielle n'est pas confrontée à l'échéance",
         lambda: verif_aucune_horloge_materielle_contre_lecheance(tx)),
        ("le ré-ancrage lit le temps de flux", lambda: verif_le_reancrage_lit_le_temps_de_flux(tx)),
        ("la réserve lit le temps de flux", lambda: verif_la_reserve_lit_le_temps_de_flux(tx)),
        ("les témoins du gel restent publiés", lambda: verif_le_temoin_reste_publie(tx)),
    )
    echecs = 0
    for titre, f in verifications:
        motif = f()
        if motif:
            echecs += 1
            print("ÉCHEC  %s : %s" % (titre, motif))
        else:
            print("ok     %s" % titre)
    if echecs:
        print("\n%d vérification(s) en échec — cf. le pavé « `t_flux` EST UN *STREAM TIME* » "
              "dans decklink_tx.cpp" % echecs)
    return 1 if echecs else 0


if __name__ == "__main__":
    sys.exit(main())
