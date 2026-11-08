#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 BOBI SAS, France
#
# Garde sur les GROUPES AUDIO DE 8 CANAUX (hooks.py + manifeste).
#
# ★★ POURQUOI. Le bus MXL porte des flux de 8 canaux : sur les 28 flux audio du domaine de
#    dell-1 le 21/10, 25 en portaient 8 et les 3 seuls à en porter 16 étaient NOS captures
#    DeckLink. Nous étions donc seuls à produire cette forme et seuls à savoir la lire — et
#    elle ratait en SILENCE dans les deux sens :
#      · une sortie réglée sur 16 canaux devant un flux de 8 remplissait 8 canaux et laissait
#        les 8 autres muets, tous compteurs au vert. C'est la panne de Quad SDI 1 : le testeur
#        a cherché son son dans le câblage pendant une heure ;
#      · un consommateur ordinaire branché sur notre flux de 16 n'en lisait que les 8 premiers,
#        sans rien dire non plus.
#    D'où : 16 canaux = DEUX flux de 8, donc DEUX ports de câblage.
#
# Quatre choses à ne pas casser :
#   · le PREMIER groupe garde le nom historique `…_audio` — le renommer casserait tous les
#     câbles déjà posés, et un câble cassé ne se voit pas avant l'antenne ;
#   · le second port n'existe QUE au-delà de 8 canaux : un port que rien ne traverse est une
#     invitation à se tromper ;
#   · le défaut de canaux d'une sortie suit CE QUI EST CÂBLÉ (deux flux → 16, un seul → 8) et
#     n'est plus 16 en dur — ce défaut-là ÉTAIT la panne ;
#   · le libellé que lit l'exploitant porte la plage (1-8 / 9-16), et il vient du manifeste
#     donc il reste traduisible.
import importlib.util, pathlib, sys

ici = pathlib.Path(__file__).parent
sys.path.insert(0, str(ici.parent.parent))
spec = importlib.util.spec_from_file_location("dl_hooks_audio", ici / "hooks.py")
h = importlib.util.module_from_spec(spec); spec.loader.exec_module(h)

ko = []


def verifier(titre, ok, detail=""):
    print(("  ok   " if ok else " ÉCHEC ") + titre + (" — " + str(detail) if detail else ""))
    if not ok:
        ko.append(titre)


def passe(params):
    return h.before_deploy(dict(params), {"hostname": "dl"})


def ports(params, liste):
    return [p for p in (params.get(liste) or [])]


def libelles(params, liste):
    """Les libellés TELS QUE LA PAGE CÂBLES LES RENDRA, dans l'ordre des ports.

    ⚠ `derive_wiring` SUBSTITUE déjà les champs de l'entrée : vérifier le gabarit du manifeste
      ne prouverait rien de ce que l'exploitant lit — et c'est l'erreur qu'a faite la première
      version de cette garde, qui attendait « {canaux_label} » non substitué."""
    from app import plugins
    plugins._scan()
    w = plugins.derive_wiring("decklink_io", "dl", params)
    return [c.get("label") for c in list(w["consumes"]) + list(w["produces"])
            if c.get("from_list") == liste]


# ── ENTRÉES ────────────────────────────────────────────────────────────────────────────────
p8 = passe({"inputs": [{"persistent_id": 11, "flow": "in1", "audio": 8}], "outputs": []})
ap = ports(p8, "audio_ports")
verifier("entrée 8 canaux : UN seul port audio", len(ap) == 1, [x["flow"] for x in ap])
verifier("et il garde le nom historique « …_audio »",
         ap and ap[0]["flow"] == "in1_audio", ap and ap[0]["flow"])

p16 = passe({"inputs": [{"persistent_id": 11, "flow": "in1", "audio": 16}], "outputs": []})
ap = ports(p16, "audio_ports")
verifier("entrée 16 canaux : DEUX ports de 8", len(ap) == 2, [x["flow"] for x in ap])
verifier("le 1er reste « …_audio » (les câbles posés survivent)",
         ap and ap[0]["flow"] == "in1_audio", ap and ap[0]["flow"])
verifier("le 2e prend « …_audio_2 » (convention déjà en service)",
         len(ap) > 1 and ap[1]["flow"] == "in1_audio_2", len(ap) > 1 and ap[1]["flow"])
verifier("chaque port annonce 8 canaux, pas 16",
         all(x.get("canaux") == 8 for x in ap), [x.get("canaux") for x in ap])
verifier("et sa plage, pour que l'exploitant sache laquelle il câble",
         [x.get("canaux_label") for x in ap] == ["1-8", "9-16"],
         [x.get("canaux_label") for x in ap])

p2 = passe({"inputs": [{"persistent_id": 11, "flow": "in1", "audio": 2}], "outputs": []})
verifier("entrée 2 canaux : un seul port, et sa plage le dit",
         [x.get("canaux_label") for x in ports(p2, "audio_ports")] == ["1-2"],
         [x.get("canaux_label") for x in ports(p2, "audio_ports")])

p0 = passe({"inputs": [{"persistent_id": 11, "flow": "in1", "audio": 0}], "outputs": []})
verifier("entrée sans audio : aucun port", ports(p0, "audio_ports") == [])

# ── SORTIES ────────────────────────────────────────────────────────────────────────────────
base = {"persistent_id": 21, "flow": "src", "mode": "1080p50"}
un_cable = passe({"inputs": [], "outputs": [dict(base)],
                  "audio_out_ports": [{"persistent_id": 21, "flow": "a1", "groupe": 1}]})
s = (un_cable.get("outputs") or [{}])[0]
verifier("sortie avec UN flux câblé : 8 canaux ouverts (et non 16 en dur)",
         s.get("audio_channels") == 8, s.get("audio_channels"))
verifier("un seul port de câblage, donc aucun port vide à remplir par erreur",
         len(ports(un_cable, "audio_out_ports")) == 1,
         [x.get("groupe") for x in ports(un_cable, "audio_out_ports")])

deux_cables = passe({"inputs": [], "outputs": [dict(base)],
                     "audio_out_ports": [{"persistent_id": 21, "flow": "a1", "groupe": 1},
                                         {"persistent_id": 21, "flow": "a2", "groupe": 2}]})
s = (deux_cables.get("outputs") or [{}])[0]
verifier("sortie avec DEUX flux câblés : 16 canaux ouverts",
         s.get("audio_channels") == 16, s.get("audio_channels"))
verifier("et les deux flux sont transmis au binaire, pas seulement le premier",
         (s.get("audio_flow"), s.get("audio_flow_2")) == ("a1", "a2"),
         (s.get("audio_flow"), s.get("audio_flow_2")))
verifier("deux ports de câblage", len(ports(deux_cables, "audio_out_ports")) == 2,
         [x.get("groupe") for x in ports(deux_cables, "audio_out_ports")])

# Réglage EXPLICITE : il commande, le câblage ne le contredit pas.
force8 = passe({"inputs": [], "outputs": [dict(base, audio_channels=8)],
                "audio_out_ports": [{"persistent_id": 21, "flow": "a1", "groupe": 1},
                                    {"persistent_id": 21, "flow": "a2", "groupe": 2}]})
s = (force8.get("outputs") or [{}])[0]
verifier("réglage explicite à 8 : le second flux n'est PAS passé au binaire",
         s.get("audio_channels") == 8 and not s.get("audio_flow_2"),
         (s.get("audio_channels"), s.get("audio_flow_2")))
verifier("et le second port de câblage disparaît avec lui",
         len(ports(force8, "audio_out_ports")) == 1,
         [x.get("groupe") for x in ports(force8, "audio_out_ports")])

force16 = passe({"inputs": [], "outputs": [dict(base, audio_channels=16)],
                 "audio_out_ports": [{"persistent_id": 21, "flow": "a1", "groupe": 1}]})
s = (force16.get("outputs") or [{}])[0]
verifier("réglage explicite à 16 avec un seul flux : 16 ouverts, port 9-16 OFFERT et vide",
         s.get("audio_channels") == 16
         and [x.get("flow") for x in ports(force16, "audio_out_ports")] == ["a1", ""],
         [x.get("flow") for x in ports(force16, "audio_out_ports")])

# ── LIBELLÉS ───────────────────────────────────────────────────────────────────────────────
lp = libelles(p16, "audio_ports")
verifier("les deux ports d'entrée se DISTINGUENT à l'écran par leur plage",
         len(lp) == 2 and lp[0].endswith("audio 1-8") and lp[1].endswith("audio 9-16"), lp)
ls = libelles(deux_cables, "audio_out_ports")
verifier("idem en sortie", len(ls) == 2 and ls[0].endswith("audio 1-8")
         and ls[1].endswith("audio 9-16"), ls)
verifier("une plage non substituée ne doit JAMAIS atteindre l'écran",
         not any("{" in (x or "") for x in lp + ls), lp + ls)

# ── LES BINAIRES ───────────────────────────────────────────────────────────────────────────
# ★★ POURQUOI DES CONTRÔLES DE STRUCTURE, ET PAS UN VRAI ESSAI. Ces deux binaires exigent le
#    SDK DeckLink, libmxl et une carte : l'essai réel ne peut pas tourner en intégration
#    continue, or c'est là que la faute du 21/10 serait passée. On vérifie donc les invariants
#    qui rendent les deux groupes possibles — chacun était faux avant ce jour-là.
cap = (ici / "decklink_rx.cpp").read_text(encoding="utf-8")
tx = (ici / "decklink_tx.cpp").read_text(encoding="utf-8")

verifier("capture : un SECOND écrivain existe", "a_writer2" in cap)
verifier("capture : le 2e flux prend le suffixe « _audio_2 »", '"_audio_2"' in cap)
verifier("capture : aucun flux n'est créé avec PLUS de 8 canaux",
         "build_audio_flowdef(e.a_nom.c_str(), e.canaux_g1)" in cap
         and "build_audio_flowdef(e.a_nom2.c_str(), e.canaux_g2)" in cap)
verifier("capture : les canaux 9-16 partent dans le 2e flux (décalage de 8)",
         "ecrire_groupe(idx, n, ech, 8," in cap)
verifier("capture : le comblement de silence couvre LES DEUX flux "
         "(sinon les groupes se désalignent à chaque trou)",
         "combler_un(e, e->a_writer2" in cap)
verifier("capture : un refus du 1er groupe n'écrit PAS le 2e "
         "(un seul groupe servi vaut pire qu'un trou dans les deux)",
         "if (!ecrire_groupe(idx, n, ech, 0, e_->canaux_g1, e_->a_writer)) {" in cap)

verifier("sortie : le 2e flux a son propre argument", '"--audio-2"' in tx)
verifier("sortie : il remplit les canaux 9-16 (décalage de 8)",
         "remplir_groupe(e, e->rd_audio2, n, 8," in tx)
verifier("sortie : le 2e groupe se rattrape SÉPARÉMENT du 1er",
         "flux_audio2_voulu" in tx and "audio_attentes2" in tx)
verifier("sortie : son absence ne fait pas taire les canaux 1-8",
         "audio_sautes2++" in tx)
verifier("sortie : ce que le bus DÉCLARE est comparé à ce qu'on attend "
         "(le contrôle manquant qui a envoyé chercher la panne dans le câble)",
         "*declares = canaux_declares_du_flux(" in tx
         and "*declares < canaux_voulus" in tx)
verifier("sortie : 16 canaux ouverts sans 2e flux câblé est NOMMÉ",
         "AUCUN second flux" in tx)
# ⚠ Les clés sont ÉCHAPPÉES dans la source C (`\"audio_flows\"`) : les chercher telles
#   quelles ne trouve rien, et la garde serait passée verte sans ces compteurs.
tx_nu = tx.replace("\\", "")
verifier("sortie : l'état publie de quoi distinguer ouvert / déclaré / câblé",
         all(k in tx_nu for k in ('"audio_flows"', '"audio_channels_card"',
                                  '"audio_channels_declared"', '"audio_group2_missing"')))

print()
if ko:
    print("%d ÉCHEC(S)" % len(ko))
    sys.exit(1)
print("tout est vert")
