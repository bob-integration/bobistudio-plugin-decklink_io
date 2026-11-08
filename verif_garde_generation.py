#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Garde : une sortie SDI se raccroche quand son producteur recrée son flux (22/10).

CE QUE CETTE GARDE PROTÈGE
    Un producteur qui détruit puis recrée son flux sous le même nom laisse nos lecteurs
    accrochés à la génération MORTE. Elle reste LISIBLE — grains servis, index figé — donc
    aucune erreur, aucun SIGBUS, aucun code de retour : la sortie rejoue la dernière image en
    boucle en annonçant 50 i/s et genlock verrouillé. Mesuré le 22/10 : six sorties sur neuf
    figées depuis le redéploiement du player à 07 h 38, et le producteur accusé à tort.

POURQUOI UNE GARDE SUR LE TEXTE DU SOURCE
    Le correctif est en C++, dans un binaire qui exige la carte et libmxl : il n'est pas
    exécutable en CI. Ce qu'on vérifie ici, ce sont donc les TROIS propriétés dont la perte
    rouvrirait la panne en silence, et seulement elles. Chacune a été mutée pour vérifier
    qu'elle mord (cf. `verifications` plus bas) — une garde qu'on n'a pas vue échouer ne
    prouve rien.
"""
import os
import re
import sys

ICI = os.path.dirname(os.path.abspath(__file__))
TX = os.path.join(ICI, "decklink_tx.cpp")
SCRIPT = os.path.join(ICI, "script.py")


def _lire(p):
    with open(p, encoding="utf-8") as f:
        return f.read()


def verif_retente_quand_le_lecteur_est_nul(tx):
    """`rd` nul doit être traité comme « à retenter », pas comme sain.

    `lecteur_perime(NULL)` rend faux. Sans le `!e->rd`, une première réouverture ratée laisse
    `rd` nul, la garde ne voit plus rien de périmé et la sortie reste figée POUR TOUJOURS —
    la panne même qu'elle corrige. Et ce premier échec est le cas NORMAL : pendant un
    redéploiement le producteur se tait 5 à 15 s contre un seuil à 5 s.
    """
    m = re.search(r"const bool\s+video_morte\s*=\s*([^;]+);", tx)
    if not m:
        return "l'affectation de `video_morte` est introuvable"
    expr = " ".join(m.group(1).split())
    if "!e->rd" not in expr:
        return ("`video_morte` ne traite plus le lecteur nul comme à retenter "
                "(attendu `!e->rd || …`, lu `%s`)" % expr)
    if "lecteur_perime" not in expr:
        return "`video_morte` ne teste plus la péremption (lu `%s`)" % expr
    return None


def verif_la_garde_passe_avant_la_lecture(tx):
    """Placée après la lecture, la garde ne corrigerait qu'à la trame SUIVANTE.

    C'est la faute de placement déjà payée le 2026-10-02 (garde de taille en aval du site
    qu'elle protégeait). On vérifie aussi le garde-fou de déréférencement : la garde peut
    laisser `rd` nul, et la bibliothèque serait appelée sur une poignée nulle.
    """
    i = tx.find("void remplir(")
    if i < 0:
        return "`remplir()` est introuvable"
    corps = tx[i:i + 4000]
    p_garde = corps.find("garde_generation(e_)")
    p_nul = corps.find("if (!e_->rd)")
    p_lecture = corps.find("mxlFlowReaderGetInfo(e_->rd")
    if p_garde < 0:
        return "`remplir()` n'appelle plus `garde_generation()`"
    if p_lecture < 0:
        return "la lecture `mxlFlowReaderGetInfo(e_->rd` a disparu de `remplir()`"
    if p_nul < 0:
        return "`remplir()` ne se protège plus d'un lecteur nul avant d'appeler la bibliothèque"
    if not (p_garde < p_nul < p_lecture):
        return ("ordre rompu dans `remplir()` : attendu garde(%d) < contrôle-nul(%d) < "
                "lecture(%d)" % (p_garde, p_nul, p_lecture))
    return None


def verif_lacher_avant_ramasser(tx):
    """L'ordre des trois gestes de reprise n'est pas libre.

    Tant que notre poignée est tenue, le flux reste RÉFÉRENCÉ et le ramasse-miettes ne le
    réclame pas ; rouvrir avant de ramasser retombe sur l'orphelin et la garde boucle. Ordre
    établi au banc par `_StaleGuard` (bobimxl) : lâcher → ramasser → rouvrir.
    """
    i = tx.find("static void garde_generation(")
    if i < 0:
        return "`garde_generation()` est introuvable"
    corps = tx[i:i + 4000]
    p_lacher = corps.find("mxlReleaseFlowReader")
    p_ramasser = corps.find("mxlGarbageCollectFlows")
    p_rouvrir = corps.find("mxlCreateFlowReader")
    for nom, p in (("lâcher", p_lacher), ("ramasser", p_ramasser), ("rouvrir", p_rouvrir)):
        if p < 0:
            return "l'étape « %s » a disparu de `garde_generation()`" % nom
    if not (p_lacher < p_ramasser < p_rouvrir):
        return ("ordre de reprise rompu : attendu lâcher(%d) < ramasser(%d) < rouvrir(%d)"
                % (p_lacher, p_ramasser, p_rouvrir))
    return None


def verif_audio_juge_sur_lindex(tx):
    """L'audio ne se juge PAS sur `lastWriteTime` — ce détecteur-là coupe le son.

    Les producteurs audio ne mettent pas `lastWriteTime` à jour : une garde qui les juge dessus
    les croit morts en permanence, relâche le lecteur une fois par seconde, et chaque reprise
    ré-ancre la lecture. Mesuré en direct le 22/10 : 1 300 à 1 700 échantillons/s au lieu de
    48 000, sur les quatre sorties porteuses de son. L'avertissement existait dans `_StaleGuard`
    et n'avait pas été appliqué — d'où cette garde, pour que l'oubli ne puisse pas se répéter.
    """
    i = tx.find("static void garde_generation(")
    if i < 0:
        return "`garde_generation()` est introuvable"
    corps = tx[i:i + 4000]
    for poignee in ("e->rd_audio", "e->rd_audio2"):
        if re.search(r"lecteur_perime\(\s*%s\b" % re.escape(poignee), corps):
            return ("`%s` est de nouveau jugé par `lecteur_perime` (témoin VIDÉO) — "
                    "c'est la coupure de son du 22/10" % poignee)
    if corps.count("lecteur_audio_perime(") < 2:
        return ("les deux groupes audio ne sont plus jugés par `lecteur_audio_perime` "
                "(%d appel(s) trouvé(s), 2 attendus)" % corps.count("lecteur_audio_perime("))
    # Le détecteur audio doit lire l'INDEX, pas la date d'écriture.
    j = tx.find("static bool lecteur_audio_perime(")
    if j < 0:
        return "`lecteur_audio_perime()` est introuvable"
    det = tx[j:j + 1600]
    if "headIndex" not in det:
        return "`lecteur_audio_perime()` ne juge plus sur `headIndex`"
    if "lastWriteTime" in det:
        return "`lecteur_audio_perime()` s'est remis à lire `lastWriteTime`"
    return None


def verif_compteurs_publies(tx, script):
    """Une garde qu'on ne peut pas observer est invérifiable.

    Sans ces compteurs, rien ne distingue « la garde n'a jamais eu à mordre » de « la garde
    ne mord pas » : les deux se lisent `flow_regens` absent.
    """
    # ⚠ Dans le C++ la clé vit DANS une chaîne littérale : le fichier contient `\"flow_regens\"`,
    #   guillemets échappés compris. Chercher `"flow_regens"` n'y correspond jamais — c'est ce
    #   qui a fait échouer cette garde à sa première exécution, sur du code pourtant correct.
    manquants = [c for c in ("flow_regens", "audio_regens") if '\\"%s\\"' % c not in tx]
    if manquants:
        return "le binaire ne publie plus %s dans ses statistiques" % ", ".join(manquants)
    manquants = [c for c in ("flow_regens", "audio_regens") if '"%s"' % c not in script]
    if manquants:
        return "`script.py` ne remonte plus %s dans /state" % ", ".join(manquants)
    return None


def main():
    tx, script = _lire(TX), _lire(SCRIPT)
    verifications = (
        ("le lecteur nul est retenté", lambda: verif_retente_quand_le_lecteur_est_nul(tx)),
        ("la garde passe avant la lecture", lambda: verif_la_garde_passe_avant_la_lecture(tx)),
        ("lâcher avant ramasser avant rouvrir", lambda: verif_lacher_avant_ramasser(tx)),
        ("l'audio est jugé sur l'index", lambda: verif_audio_juge_sur_lindex(tx)),
        ("les compteurs sont publiés", lambda: verif_compteurs_publies(tx, script)),
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
        print("\n%d vérification(s) en échec — cf. le pavé « DÉCROCHAGE DE GÉNÉRATION » "
              "dans decklink_tx.cpp" % echecs)
    return 1 if echecs else 0


if __name__ == "__main__":
    sys.exit(main())
