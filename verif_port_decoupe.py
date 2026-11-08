#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 BOBI SAS, France
#
# Garde sur le PORT DE DÉCOUPE de la page Câbles (hooks.py).
#
# Un Fill & Key est UNE sortie sur DEUX connecteurs. La page Câbles n'en montrait qu'un port —
# celui du remplissage — et rien n'y disait qu'une découpe existait derrière : le 20/10, le flux
# de découpe a été posé sur le port du remplissage, et la carte a émis la clé sur le connecteur
# de remplissage, plus rien sur celui de découpe, sans un mot. D'où un port par découpe.
#
# Trois choses à ne pas casser, et c'est tout l'objet de ce fichier :
#   · le report se fait par `persistent_id`, JAMAIS par rang — un rang se décale dès qu'une
#     sortie est ajoutée, et la découpe partirait sur une autre sortie, en silence ;
#   · une découpe SANS source reste déclarée, sinon le port n'existe pas et on ne peut rien y
#     câbler (il faudrait un flux pour avoir le port, et le port pour poser le flux) ;
#   · le libellé nomme le numéro de la SORTIE, pas le rang dans la liste des découpes.
import importlib.util, pathlib, sys

ici = pathlib.Path(__file__).parent
# `hooks.py` importe `app.numerotation` : il tourne DANS l'orchestrateur, pas isolément. On
# ajoute donc la racine du dépôt au chemin plutôt que de simuler le module — une simulation
# figerait ici la convention de nommage des flux, qui vit là-bas.
sys.path.insert(0, str(ici.parent.parent))
spec = importlib.util.spec_from_file_location("dl_hooks", ici / "hooks.py")
h = importlib.util.module_from_spec(spec); spec.loader.exec_module(h)

def sortie(pid, flow, kpid=None, kflow=None, mode="1080i50"):
    o = {"persistent_id": pid, "flow": flow, "mode": mode}
    if kpid: o["key_persistent_id"] = kpid
    if kflow is not None: o["key_flow"] = kflow
    return o

def passe(params):
    return h.before_deploy(dict(params), {"hostname": "dl"})

def libelle_port(params, liste="key_ports"):
    """Le libellé TEL QUE LA PAGE CÂBLES LE RENDRA — pas le champ brut de l'entrée.
    Le gabarit vit dans le manifeste (donc il est traduit) et c'est `derive_wiring` qui y
    substitue le numéro de sortie : vérifier le champ brut ne prouverait rien de ce que
    l'exploitant lit."""
    from app import plugins
    plugins._scan()
    w = plugins.derive_wiring("decklink_io", "dl", params)
    # ⚠ On cherche dans les DEUX sens : une entrée est un port `produces`, une sortie un port
    #   `consumes`. Ne regarder que l'un des deux rendait None pour l'autre — et un None se lit
    #   comme « pas de libellé » alors que c'est « pas cherché au bon endroit ».
    for c in list(w["consumes"]) + list(w["produces"]):
        if c.get("from_list") == liste:
            return c.get("label")
    return None

ko = 0
def verifier(titre, ok, detail=""):
    global ko
    if not ok: ko = 1
    print("  %s %-58s %s" % ("ok  " if ok else "ÉCHEC", titre, detail))

# ── 1. Le port existe dès que le connecteur de découpe est déclaré, même sans source ──────
r = passe({"outputs": [sortie(100, "fill", kpid=101, kflow="")]})
kp = r.get("key_ports") or []
verifier("une découpe SANS source a quand même son port", len(kp) == 1, "%d port(s)" % len(kp))
verifier("le connecteur de découpe reste déclaré sur la sortie",
         (r["outputs"][0].get("key_persistent_id")) == 101)
verifier("le libellé nomme la carte et la prise", libelle_port(r) == "DeckLink SDI 1 — découpe",
         repr(libelle_port(r)))

# ── 2. Report du câblage, par identifiant ────────────────────────────────────────────────
r = passe({"outputs": [sortie(100, "fill", kpid=101, kflow="")],
           "key_ports": [{"persistent_id": 101, "flow": "stills_key"}]})
verifier("ce que pose la page Câbles arrive sur la sortie",
         r["outputs"][0].get("key_flow") == "stills_key", r["outputs"][0].get("key_flow"))

# ── 3. LE cas qui justifie le report par identifiant : une sortie SANS découpe d'abord ───
#     Le rang de la découpe (0) n'est PAS le rang de sa sortie (1).
p = {"outputs": [sortie(100, "a"), sortie(200, "b", kpid=201, kflow="")],
     "key_ports": [{"persistent_id": 201, "flow": "k2"}]}
r = passe(p)
verifier("le report ne se trompe pas de sortie",
         r["outputs"][0].get("key_flow") in (None, "") and r["outputs"][1].get("key_flow") == "k2",
         "sortie1=%r sortie2=%r" % (r["outputs"][0].get("key_flow"), r["outputs"][1].get("key_flow")))
verifier("et il porte le numéro de la PRISE, pas le rang dans la liste",
         libelle_port(r) == "DeckLink SDI 2 — découpe", repr(libelle_port(r)))

# ── 4. Décâbler la découpe ne supprime pas le connecteur ────────────────────────────────
r = passe({"outputs": [sortie(100, "fill", kpid=101, kflow="stills_key")],
           "key_ports": [{"persistent_id": 101, "flow": ""}]})
verifier("décâbler vide la source, garde le connecteur",
         r["outputs"][0].get("key_persistent_id") == 101 and not r["outputs"][0].get("key_flow"),
         repr(r["outputs"][0].get("key_flow")))

# ── 5. Une découpe ne peut pas voler un connecteur déjà pris ────────────────────────────
r = passe({"outputs": [sortie(100, "a", kpid=100, kflow="k")]})
verifier("la découpe ne peut pas être le connecteur de sa propre sortie",
         not r["outputs"][0].get("key_persistent_id"))

# ── AUDIO SUR UNE SORTIE (20/10) ─────────────────────────────────────────────────────────
# Une sortie SDI n'acceptait que de la vidéo : on pouvait capturer 16 canaux d'une source et
# n'en émettre aucun. Le son part sur le MÊME connecteur, embarqué dans le signal — c'est donc
# un port de plus sur la sortie, pas une sortie de plus.
r = passe({"outputs": [sortie(100, "fill")]})
ap = r.get("audio_out_ports") or []
verifier("toute sortie a son port audio", len(ap) == 1, "%d port(s)" % len(ap))
verifier("vide tant que rien n'est câblé", (ap[0].get("flow") if ap else "?") == "",
         repr(ap[0].get("flow") if ap else None))
verifier("le port audio nomme lui aussi la carte, la prise ET sa plage de canaux",
         libelle_port(r, "audio_out_ports") == "DeckLink SDI 1 — audio 1-8",
         repr(libelle_port(r, "audio_out_ports")))

r = passe({"outputs": [sortie(100, "fill")],
           "audio_out_ports": [{"persistent_id": 100, "flow": "console_pgm"}]})
verifier("ce que pose la page Câbles arrive sur la sortie",
         r["outputs"][0].get("audio_flow") == "console_pgm", r["outputs"][0].get("audio_flow"))
verifier("et le nombre de canaux a un défaut valide pour le SDK",
         r["outputs"][0].get("audio_channels") in (2, 8, 16),
         r["outputs"][0].get("audio_channels"))

# Le SDK n'accepte que 2, 8 ou 16 : une valeur fausse n'est jamais « rapprochée » de la plus
# proche — annoncer 6 canaux et en livrer 8 décalerait toutes les pistes d'une régie.
# ⚠ LE DÉFAUT ÉTAIT 16, ET CE DÉFAUT ÉTAIT LA PANNE (corrigé le 21/10). Le raisonnement
#   d'alors — « nos captures publient 16 canaux, une fenêtre trop large laisse juste sortir du
#   muet » — tenait tant que nous publiions un flux de 16. Depuis que 16 canaux se câblent en
#   DEUX flux de 8, ce défaut ouvrait 16 canaux sur la carte avec un seul flux alimenté : les
#   canaux 9-16 sortaient muets, tous compteurs au vert, et le testeur a cherché son son dans
#   le câblage pendant une heure (Quad SDI 1). Le défaut suit donc CE QUI EST CÂBLÉ.
r = passe({"outputs": [dict(sortie(100, "fill"), audio_flow="a", audio_channels=6)]})
verifier("une valeur refusée par le SDK retombe sur ce que le câblage justifie, "
         "jamais sur la valeur voisine",
         r["outputs"][0].get("audio_channels") == 8, r["outputs"][0].get("audio_channels"))
r = passe({"outputs": [dict(sortie(100, "fill"), audio_flow="a", audio_channels=6,
                            audio_flow_2="b")]})
verifier("et à 16 dès qu'un second flux est câblé",
         r["outputs"][0].get("audio_channels") == 16, r["outputs"][0].get("audio_channels"))
r = passe({"outputs": [dict(sortie(100, "fill"), audio_flow="a", audio_channels=2)]})
verifier("mais une valeur VALIDE est respectée",
         r["outputs"][0].get("audio_channels") == 2, r["outputs"][0].get("audio_channels"))

# ⚠ LE REPORT PASSE PAR L'IDENTIFIANT, comme pour la découpe : une sortie sans audio en tête de
#   liste décalerait tout un report positionnel.
r = passe({"outputs": [sortie(100, "a"), sortie(200, "b")],
           "audio_out_ports": [{"persistent_id": 200, "flow": "son2"}]})
verifier("l'audio ne se trompe pas de sortie",
         not r["outputs"][0].get("audio_flow") and r["outputs"][1].get("audio_flow") == "son2",
         "s1=%r s2=%r" % (r["outputs"][0].get("audio_flow"), r["outputs"][1].get("audio_flow")))

# ── LE NOM DE LA CARTE DANS LES PORTS (20/10) ────────────────────────────────────────────
# Les ports s'appelaient « Entrée SDI 1, 2, 3… » : une incrémentation dans l'ordre de la liste,
# qui ne dit NI de quelle carte il s'agit NI quel connecteur. Avec deux cartes dans la machine,
# « SDI 2 » ne désigne plus rien. Le modèle ne peut venir que de la console (il est lu du SDK,
# dans le conteneur) : il voyage avec la déclaration.
r = passe({"inputs": [{"persistent_id": 10, "flow": "a", "carte": "DeckLink Quad 2", "prise": 5}],
           "outputs": [dict(sortie(100, "fill"), carte="DeckLink Duo 2", prise=2)]})
verifier("un port d'entrée porte le nom de SA carte et son numéro",
         libelle_port(r, "inputs") == "DeckLink Quad 2 SDI 5", repr(libelle_port(r, "inputs")))
verifier("un port de sortie aussi",
         libelle_port(r, "outputs") == "DeckLink Duo 2 SDI 2", repr(libelle_port(r, "outputs")))
verifier("et la découpe hérite des repères de sa sortie",
         libelle_port(r, "key_ports") in (None, "DeckLink Duo 2 SDI 2 — découpe"),
         repr(libelle_port(r, "key_ports")))

# ⚠ REPLI NEUTRE, JAMAIS UN MODÈLE INVENTÉ. Une déclaration d'avant cette version n'a pas de
#   modèle : le port doit rester lisible sans pour autant nommer une carte au hasard.
# ★ ATTENDU CORRIGÉ LE 22/10 : « DeckLink SDI 1 » auparavant. Ce « 1 » était le RANG DANS LA
#   LISTE de configuration, pas le connecteur — cette garde figeait donc un numéro inventé sous
#   le titre « ne ment pas ». Constaté en exploitation le même jour : une prise enregistrée
#   « 2 » pour le connecteur physique 5 de la Quad, soit un libellé qui envoie poser le câble
#   dans le mauvais trou. L'orchestrateur ne PEUT pas connaître ce rang (il se lit dans
#   l'inventaire de la carte, donc dans le conteneur), et la seule réponse honnête est « ? ».
r = passe({"inputs": [{"persistent_id": 10, "flow": "a"}]})
_lp = libelle_port(r, "inputs")
verifier("sans modèle connu, le port reste lisible et ne ment pas",
         _lp == "DeckLink SDI ?", repr(_lp))
# ★ Et surtout : AUCUN CHIFFRE ne doit apparaître là où le connecteur est inconnu. L'assertion
#   d'égalité ci-dessus passerait encore si le repli devenait « SDI 0 » puis « SDI 1 » au fil
#   d'une réécriture ; celle-ci nomme la propriété, pas la chaîne.
verifier("et il n'affiche AUCUN numéro de connecteur inventé",
         not any(c.isdigit() for c in (_lp or "").split("SDI", 1)[-1]), repr(_lp))

# ── ORDRE DES PORTS (20/10) ──────────────────────────────────────────────────────────────
# Les ports sortaient dans l'ordre des DÉCLARATIONS — « SDI 8 » avant « SDI 7 » parce qu'il
# avait été déclaré avant. Un ordre qui n'a de sens que pour celui qui a fait les déclarations.
r = passe({"inputs": [
    {"persistent_id": 18, "flow": "h", "carte": "DeckLink Quad 2", "prise": 8},
    {"persistent_id": 17, "flow": "g", "carte": "DeckLink Quad 2", "prise": 7},
    {"persistent_id": 11, "flow": "a", "carte": "DeckLink Quad 2", "prise": 1},
]})
verifier("les entrées sortent dans l'ordre des PRISES",
         [e["prise"] for e in r["inputs"]] == [1, 7, 8],
         [e.get("prise") for e in r["inputs"]])
verifier("et le flux reste attaché à SA prise — le tri ne recâble rien",
         [e["flow"] for e in r["inputs"]] == ["a", "g", "h"],
         [e.get("flow") for e in r["inputs"]])

# Deux cartes : on regroupe par carte avant de numéroter, sinon les prises s'entrelacent.
r = passe({"inputs": [
    {"persistent_id": 40, "flow": "d2", "carte": "DeckLink Duo 2", "prise": 2},
    {"persistent_id": 13, "flow": "q3", "carte": "DeckLink Quad 2", "prise": 3},
    {"persistent_id": 39, "flow": "d1", "carte": "DeckLink Duo 2", "prise": 1},
]})
verifier("deux cartes : chacune groupée, puis par prise",
         [e["flow"] for e in r["inputs"]] == ["d1", "d2", "q3"],
         [e.get("flow") for e in r["inputs"]])

print("→ le port de découpe est câblable et va au bon endroit." if not ko
      else "→ ÉCHEC : la découpe serait incâblable ou irait sur la mauvaise sortie.")
sys.exit(ko)
