#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Garde : deux captures ne visent jamais le même flux (23/10).

CE QUE CETTE GARDE PROTÈGE
    Deux entrées qui portent le même nom de flux se marchent dessus sur le bus : la première
    le tient, la seconde n'y écrit RIEN et reste à 0 i/s — sans erreur, sans message, au
    milieu de compteurs sains. Signalé en recette : « pourquoi l'entrée Quad SDI 2 ne process
    aucune image ? Il y a bien du signal en 1080p50 qui arrive ». La carte voyait bien le
    signal ; c'est le nom du flux qui était déjà pris.

    ⚠ LE COMMENTAIRE DU CODE DÉCRIVAIT DÉJÀ CE DÉFAUT — « les deux flux se marcheraient dessus
      sur le bus, sans erreur visible » — mais la garde qu'il accompagnait portait sur les
      IDENTIFIANTS DE CONNECTEUR, pas sur les noms de flux. Bonne crainte, mauvaise clé.
      D'où cette garde : une peur écrite dans un commentaire n'a jamais rien empêché.

CAUSE RACINE
    Le nom dérivé se fondait sur la LONGUEUR de la liste. Quatre entrées donnent …_1 à …_4 ;
    on retire la deuxième, la liste tombe à 3 ; la suivante déclarée reprend l'indice 4, déjà
    porté par une entrée restante. La dérivation avance maintenant jusqu'au premier indice
    LIBRE.
"""
import os
import sys

ICI = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, "/opt/bobistudio")
sys.path.insert(0, ICI)

import hooks as h  # noqa: E402

ko = 0


def verifier(titre, ok, detail="", sinon=""):
    """`detail` s'affiche dans les deux cas ; `sinon` SEULEMENT en échec.

    Les deux ont existé mélangés : le motif d'échec s'affichait sur les lignes vertes
    (« ok  il les EXPOSE dans /state   la console ne peut pas les montrer »), ce qui se lit
    exactement comme un échec. Une garde qui s'affiche de travers finit par être mal crue.
    """
    global ko
    if ok:
        print("  ok   %-62s %s" % (titre, detail))
    else:
        ko += 1
        print("  ÉCHEC %-61s %s" % (titre, detail or sinon))


def passe(params):
    return h.before_deploy(dict(params), {"hostname": "dl"})


def noms(res):
    return [e.get("flow") for e in (res.get("inputs") or [])]


# ── 1. LA CAUSE RACINE : une dérivation qui ne collisionne pas ────────────────────────────
# Le scénario EXACT de la panne : des entrées déjà nommées …_3 et …_4 (une entrée du milieu a
# été retirée un jour), puis une nouvelle sans nom. L'ancienne dérivation lui donnait …_4.
r = passe({"inputs": [{"persistent_id": 11, "flow": "dl_3"},
                      {"persistent_id": 12, "flow": "dl_4"},
                      {"persistent_id": 13}]})
verifier("un nom dérivé ne reprend jamais un nom déjà pris",
         len(noms(r)) == len(set(noms(r))), noms(r))
verifier("et les trois entrées sont conservées", len(noms(r)) == 3, noms(r))

# ── 2. UN DOUBLON EXPLICITE EST ÉCARTÉ… ───────────────────────────────────────────────────
r = passe({"inputs": [{"persistent_id": 21, "flow": "meme"},
                      {"persistent_id": 22, "flow": "meme"}]})
verifier("deux entrées ne gardent jamais le même flux",
         len(noms(r)) == len(set(noms(r))) and len(noms(r)) == 1, noms(r))

# ── 3. …ET IL EST DIT ─────────────────────────────────────────────────────────────────────
# Écarter en silence serait le défaut du 23/10 sous une autre forme : une entrée qui disparaît
# sans un mot vaut une entrée muette sans un mot.
ref = r.get("entrees_refusees") or []
verifier("le refus est PUBLIÉ, pas silencieux", len(ref) == 1, ref)
verifier("et il nomme le connecteur et le motif",
         bool(ref) and ref[0].get("persistent_id") == 22
         and ref[0].get("raison") == "flow_duplicate", ref[0] if ref else None)

# ── 4. LE CAS SAIN NE DÉCLENCHE RIEN ──────────────────────────────────────────────────────
# Une garde qui refuse aussi le cas normal serait pire que l'absence de garde : elle ferait
# disparaître des entrées légitimes.
r = passe({"inputs": [{"persistent_id": 31, "flow": "a"}, {"persistent_id": 32, "flow": "b"},
                      {"persistent_id": 33}]})
verifier("trois entrées distinctes passent toutes", len(noms(r)) == 3, noms(r))
verifier("et rien n'est refusé", not (r.get("entrees_refusees") or []),
         r.get("entrees_refusees"))

# ── 5. LE REFUS ARRIVE JUSQU'À L'EXPLOITANT ───────────────────────────────────────────────
# ⚠⚠ L'ASSERTION 3 CI-DESSUS A ÉTÉ VERTE PENDANT QUE LA CHAÎNE ÉTAIT COUPÉE. Elle vérifiait
#    que le hook POSE la clé `entrees_refusees` dans la configuration — et rien ne la lisait :
#    ni le superviseur, ni /state, ni la console. Publier dans un dictionnaire que personne
#    n'ouvre, ce n'est pas publier. Une garde qui s'arrête au producteur ne mesure pas ce
#    qu'elle prétend protéger ; on suit donc la clé jusqu'à son LECTEUR.
import ast  # noqa: E402

SCRIPT = os.path.join(ICI, "script.py")
src = open(SCRIPT, encoding="utf-8").read()

verifier("le superviseur LIT les entrées refusées",
         '_liste(cfg, "entrees_refusees")' in src,
         sinon="la clé posée par le hook n'est lue par personne")

# Le rendu est du `str.format` : on raisonne sur le texte, mais on exige les DEUX extrémités.
verifier("il les JOURNALISE (en anglais, décision du 15/10)",
         "input dropped" in src, sinon="aucun journal au moment de l'application")

verifier("il les EXPOSE dans /state",
         '"entrees_refusees": list(_refusees)' in src,
         sinon="la console ne peut pas les montrer")

# Et le journal doit tourner à CHAQUE application, pas seulement au démarrage : une entrée peut
# être écartée par une reconfiguration à chaud, et c'est là que quelqu'un regarde.
# ⚠⚠ DEUX VERSIONS DE CETTE ASSERTION ONT ÉTÉ INERTES, toutes deux trouvées par mutation :
#    « "input dropped" apparaît dans _reconfigurer » survivait à une boucle neutralisée en
#    `for _r in []`, et « dans les 400 caractères qui suivent la boucle » survivait au journal
#    SORTI de la boucle (une fenêtre de texte ne sait pas ce qu'est un corps de boucle).
#    On lit donc l'ARBRE : une boucle qui parcourt `_ref`, et le journal DANS son corps.
rendu = src.format(config=repr({"inputs": []}), hostname="dl", plugin_version="0")
arbre = ast.parse(rendu)
fn = next((n for n in ast.walk(arbre)
           if isinstance(n, ast.FunctionDef) and n.name == "_reconfigurer"), None)


def _journal_dans_boucle(fonction):
    """Vrai si un `for` qui parcourt `_ref` journalise le refus DANS son corps."""
    if fonction is None:
        return False
    for n in ast.walk(fonction):
        if not isinstance(n, ast.For):
            continue
        if not (isinstance(n.iter, ast.Name) and n.iter.id == "_ref"):
            continue
        for d in n.body:
            for c in ast.walk(d):
                if (isinstance(c, ast.Call) and isinstance(c.func, ast.Name)
                        and c.func.id == "print"
                        and any(isinstance(x, ast.Constant) and isinstance(x.value, str)
                                and "input dropped" in x.value
                                for x in ast.walk(c))):
                    return True
    return False


verifier("le journal est DANS la boucle qui parcourt les entrées refusées",
         _journal_dans_boucle(fn), sinon="boucle absente, vide, ou journal hors de son corps")
verifier("et la lecture de la clé est dans `_reconfigurer` (démarrage ET à chaud)",
         fn is not None and any(
             isinstance(x, ast.Constant) and x.value == "entrees_refusees"
             for x in ast.walk(fn)), sinon="hors du chemin commun")

print()
if ko:
    print("→ ÉCHEC : deux captures pourraient viser le même flux, et l'une resterait muette.")
sys.exit(1 if ko else 0)
