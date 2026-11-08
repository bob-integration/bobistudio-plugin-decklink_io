#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 BOBI SAS, France
#
# Garde sur `_normaliser_carte` — la bascule de profil AU DÉMARRAGE.
#
# ★★ POURQUOI CETTE GARDE N'EXISTAIT PAS, ET CE QUE ÇA A COÛTÉ. La fonction abandonnait la
#    bascule dès qu'UNE SEULE prise portait un signal, en écrivant « switch left to the
#    operator » — alors que la bascule manuelle avait été retirée de la console le 16/10. Le
#    geste était donc laissé à quelqu'un qui n'avait plus aucun moyen de le faire. Résultat
#    constaté par Cyril le 30/10 sur sa Quad 2 : des prises ÉTEINTES par le profil, sept
#    connecteurs bloqués par un seul câble branché, et rien à faire sans débrancher.
#    AUCUNE garde ne couvrait cette branche : la suite est restée verte de part en part quand
#    on l'a retirée. Une branche non gardée est une branche qui peut changer de sens sans
#    qu'on le sache — dans les deux sens.
#
# ★ CE QU'ELLE TIENT MAINTENANT (arbitrage de Cyril, 2026-10-30) :
#   1. la bascule a lieu MÊME si des prises portent un signal ;
#   2. elle n'a PAS lieu si la carte est déjà dans le bon profil — une bascule inutile coupe
#      pour rien, et c'est une coupure gratuite à chaque démarrage ;
#   3. elle n'a PAS lieu sur une carte mono-profil (il n'y a rien à basculer) ;
#   4. elle n'est JAMAIS MUETTE : les prises coupées sont NOMMÉES dans le journal, et l'état
#      publié dit lesquelles. C'est la seule chose qui rend la coupure acceptable ;
#   5. l'avis sur :8080 porte des paramètres GROSSIERS (un compte, jamais la liste des
#      identifiants) : le relais limite à un avis par heure sur (vmid, clé, PARAMÈTRES), et des
#      paramètres qui varient contourneraient cette limite ;
#   6. l'avis s'ÉTEINT quand l'événement n'est plus frais, mais /state le garde — « pourquoi mon
#      signal a-t-il sauté » se pose parfois une heure après ;
#   7. le relevé des prises vivantes est pris AVANT de relâcher la carte (mesuré le 19/10 : une
#      carte relâchée ne rapporte plus ses entrées comme verrouillées).
#
# La fonction est extraite du GABARIT (`script.py` passe par str.format), jamais recopiée.
import copy, pathlib, re, sys, time

SRC = pathlib.Path(__file__).parent / "script.py"
src = SRC.read_text(encoding="utf-8")

ko = 0
n = 0


def ok(cond, titre, detail=""):
    global ko, n
    n += 1
    print("  %s %s%s" % ("ok   " if cond else "ÉCHEC", titre,
                         "" if cond or not detail else "  — %s" % detail))
    if not cond:
        ko = 1


def extraire(nom, source):
    """La fonction SEULE : sa ligne `def` puis les lignes indentées ou vides qui suivent.

    ⚠ S'ARRÊTER AU PROCHAIN `def` NE SUFFIT PAS : `_normaliser_carte` est la DERNIÈRE fonction
      du gabarit, et un tel motif embarquait les appels de module qui la suivent
      (`_nettoyer_orphelins()`, `_reconfigurer(...)`) — le harnais tentait alors d'exécuter le
      démarrage entier du plugin. C'est la deuxième fois que ce motif d'extraction mord ici.
    """
    m = re.search(r"^def %s\(.*?\):\n(?:[ \t].*\n|[ \t]*\n)*" % re.escape(nom),
                  source, re.M)
    if not m:
        raise RuntimeError("ancre périmée : %s" % nom)
    return m.group(0).rstrip() + "\n"


def harnais(source):
    """Rend (ns, journal, bascules) pour UNE version du gabarit."""
    bloc = extraire("_normaliser_carte", source)
    bloc_avis = extraire("_avis_normalisation", source)
    bloc_avis2 = extraire("_avis_prises_eteintes", source)
    journal, bascules = [], []

    def _print(*a, **k):
        journal.append(" ".join(str(x) for x in a))

    def _basculer_profil(profil, pid, consentement=False):
        """La VRAIE sémantique mesurée : la commande ne touche que le GROUPE du pid visé, et
        elle rend l'inventaire d'après (c'est ce que fait le code, qui invalide le cache de la
        sonde et relit). Une doublure qui normaliserait tout d'un coup laisserait passer
        exactement le défaut du 30/10."""
        bascules.append((profil, pid, consentement))
        devices = harnais.inv.get("devices") or []
        # L'inventaire d'AVANT est une copie : le code mesure les prises coupées en comparant
        # avant/après, et une référence partagée ferait les deux identiques — la mesure
        # rendrait toujours « rien de coupé », et l'assertion serait vacante.
        avant = {"devices": copy.deepcopy(devices)}
        # La carte peut REFUSER : un profil se demande, il ne se décrète pas. Sans ce cas, un
        # verdict codé en dur (`"ok": True`) serait indiscernable d'un verdict lu.
        if getattr(harnais, "echec", False):
            return {"ok": False, "code": "profile_switch_refused", "avant": avant,
                    "apres": avant}
        vise = next((d for d in devices if d.get("persistent_id") == pid), None)
        if vise is not None and not getattr(harnais, "sans_effet", False):
            for d in devices:
                if d.get("groupe") == vise.get("groupe"):
                    d["profil_actif"] = {"id": profil}
                    # La carte vient d'être relâchée et reprise : juste après, elle n'a pas
                    # encore repris ses verrous. ⚠ MESURÉ : la session Z8 a relevé 1 verrou sur
                    # 8 à 15:50, puis 4 sur 8 à 15:51:06 — des DIZAINES DE SECONDES. D'où
                    # `verrous_instantanes`, qui joue l'autre extrême : si `coupees` dépend de
                    # ce délai, les deux décors ne donneront pas la même réponse, et c'est
                    # exactement le défaut de 0.29.45.
                    if not getattr(harnais, "verrous_instantanes", False):
                        d["signal"] = {"entree_verrouillee": False, "mode_detecte": None}
        if getattr(harnais, "inventaire_muet", False):
            return {"ok": True, "code": "ok", "avant": avant, "apres": {"devices": []}}
        return {"ok": True, "code": "ok", "avant": avant,
                "apres": {"devices": copy.deepcopy(devices)}}

    ns = {"re": re, "time": time, "print": _print, "flush": True,
          "_basculer_profil": _basculer_profil, "_inventaire": lambda: harnais.inv,
          "_profil_cible": None, "_NORMALISATION": None}
    # `_profil_cible` vient du gabarit lui aussi : la cible et la décision doivent rester
    # d'accord, et une doublure écrite ici masquerait un désaccord entre les deux.
    rendu = lambda t: t.replace("{{", "{").replace("}}", "}")      # noqa: E731
    exec(rendu(extraire("_profil_cible", source)), ns)              # noqa: S102
    exec(rendu(bloc), ns)                                           # noqa: S102
    exec(rendu(bloc_avis), ns)                                      # noqa: S102
    exec(rendu(bloc_avis2), ns)                                     # noqa: S102
    ns["_NORMALISATION_AVIS_S"] = 600.0
    return ns, journal, bascules


def carte(profil_actif, profils, signaux, groupes=None):
    """Un relevé d'inventaire de la FORME que rend la sonde : une entrée par sous-périphérique.

    ★★ `groupes` EST LA CORRECTION DE CE HARNAIS, et il faut dire pourquoi. Dans sa première
       version, tous les sous-périphériques partageaient UN profil : le décor encodait ma
       supposition — « une bascule normalise la carte » — et la garde ne pouvait donc que la
       confirmer. Elle est restée verte pendant que le code laissait trois prises éteintes sur
       la Quad 2 de #228 (mesuré par la session Z8 le 2026-10-30 à 15:38). Une garde bâtie sur
       un modèle faux ne teste pas le code : elle teste l'accord du code avec l'erreur.
       Le profil se règle PAR GROUPE de sous-périphériques. `groupes` dit lequel appartient à
       quel groupe — par défaut un groupe unique, pour les cas où le groupement n'importe pas.
    """
    if groupes is None:
        groupes = [0] * len(signaux)
    devices = []
    for i, sig in enumerate(signaux):
        pa = profil_actif if isinstance(profil_actif, str) else profil_actif[groupes[i]]
        devices.append({
            "index": i, "persistent_id": 4040731136 + i, "model": "DeckLink Quad 2",
            "groupe": groupes[i],
            "duplex": "half", "profil_actif": {"id": pa},
            "profils": [{"id": p} for p in profils],
            "signal": {"entree_verrouillee": bool(sig), "mode_detecte": "Hp50" if sig else None},
        })
    return {"devices": devices}


def jouer(source, inv, echec=False, sans_effet=False, inventaire_muet=False,
          verrous_instantanes=False):
    harnais.inv = inv
    harnais.echec = echec
    harnais.sans_effet = sans_effet
    harnais.inventaire_muet = inventaire_muet
    harnais.verrous_instantanes = verrous_instantanes
    ns, journal, bascules = harnais(source)
    ns["_normaliser_carte"]()
    return ns, journal, bascules


def passe(source):
    """Rend la liste [(booléen, titre, détail)] pour une version du gabarit."""
    r = []
    OK = lambda c, t, d="": r.append((bool(c), t, d))                # noqa: E731

    # ── 0. ★★ LA QUAD 2 RÉELLE DE #228, telle que la session Z8 l'a relevée le 2026-10-30 ──
    # Huit prises, QUATRE groupes de deux (les `topological_id` consécutifs les laissent voir),
    # toutes en `1dfd` dont quatre `inactive`, et les deux premières portant un signal. Une
    # seule bascule n'en réveille que deux : c'est le cas qui a manqué à cette garde.
    inv0 = carte("1dfd", ["1dfd", "2dhd"], [True, True, False, False, False, False, False, False],
                 groupes=[0, 1, 2, 3, 0, 1, 2, 3])
    ns0, j0, b0 = jouer(source, inv0)
    OK(len(b0) == 4, "Z1 ★ les QUATRE groupes sont basculés, pas seulement le premier",
       "%d bascule(s)" % len(b0))
    OK(all((d.get("profil_actif") or {}).get("id") == "2dhd"
           for d in inv0["devices"]),
       "Z2 et plus aucun sous-périphérique ne reste hors du profil cible")
    e0 = ns0["_NORMALISATION"]
    OK(e0 and e0.get("restantes") == [], "Z3 l'état publié ne déclare aucune prise restante",
       repr(e0 and e0.get("restantes")))
    OK(e0 and e0.get("ok") is True, "Z4 et `ok` vaut vrai — la carte EST normalisée")
    OK(e0 and len(e0.get("bascules") or []) == 4,
       "Z5 chaque bascule est consignée séparément")
    # Le relevé des vivantes est repris À CHAQUE TOUR : les deux prises qui portaient un signal
    # sont nommées, et aucune autre. C'est le champ que /state a affirmé à tort le 30/10.
    OK(e0 and sorted(e0.get("coupees") or []) == [4040731136, 4040731137],
       "Z6 ★ `coupees` ne nomme QUE les prises qui portaient vraiment un signal",
       repr(e0 and e0.get("coupees")))
    OK(ns0["_avis_prises_eteintes"]() is None,
       "Z7 aucun avis de prises éteintes : il n'en reste aucune")
    # ★ L'AVERTISSEMENT EST REFAIT À CHAQUE TOUR. La prise 4040731136 a été coupée au premier
    #   tour : la réannoncer au second comme « va être coupée » serait faux, et c'est la forme
    #   exacte du mensonge que /state a produit le 30/10. Un relevé pris une seule fois au
    #   départ le reproduirait tour après tour.
    annonces = [l for l in j0 if "Connectors carrying a signal:" in l]
    OK(len(annonces) == 2, "Z17 deux tours seulement avertissent — ceux qui risquent une coupure",
       str(len(annonces)))
    OK(annonces and "4040731136" not in annonces[-1] and "4040731137" in annonces[-1],
       "Z18 ★ et le dernier n'annonce QUE ce qui est encore vivant à ce moment-là",
       annonces[-1][-70:] if annonces else "-")

    # ── 0 quater. ★★ `coupees` NE DOIT PAS DÉPENDRE DU DÉLAI DE REPRISE DES VERROUS ──────
    # Même carte, même décor, une seule différence : la carte reprend ses verrous
    # instantanément. La réponse doit être IDENTIQUE. C'est la mesure de la session Z8 à
    # 15:50 puis 15:51:06 (1 verrou sur 8, puis 4) qui impose cette assertion : lire juste
    # après la bascule déclarait coupé tout ce qui n'avait pas encore eu le temps de revenir.
    invR = carte("1dfd", ["1dfd", "2dhd"], [True, True, False, False, False, False, False, False],
                 groupes=[0, 1, 2, 3, 0, 1, 2, 3])
    nsR, _, bR = jouer(source, invR, verrous_instantanes=True)
    eR = nsR["_NORMALISATION"]
    OK(len(bR) == 4, "Z19 les quatre groupes sont basculés dans les deux cas")
    OK(eR and sorted(eR.get("coupees") or []) == [4040731136, 4040731137],
       "Z20 ★★ et `coupees` est le MÊME, que les verrous reviennent vite ou lentement",
       repr(eR and eR.get("coupees")))

    # ── 0 quinquies. Une prise vivante dont le groupe n'est PAS basculé n'est pas coupée ───
    # C'est l'erreur exacte de 0.29.44 : `coupees` nommait 4037714289 alors que son groupe
    # n'avait pas bougé. Ici le groupe 1 est DÉJÀ au profil cible et porte un signal.
    invP = carte(["1dfd", "2dhd"], ["1dfd", "2dhd"], [False, True, False, True],
                 groupes=[0, 1, 0, 1])
    nsP, _, bP = jouer(source, invP)
    eP = nsP["_NORMALISATION"]
    OK(len(bP) == 1, "Z21 seul le groupe hors cible est basculé", "%d bascule(s)" % len(bP))
    OK(eP and (eP.get("coupees") or []) == [],
       "Z22 ★ la prise vivante du groupe DÉJÀ bon n'est pas déclarée coupée",
       repr(eP and eP.get("coupees")))

    # ── 0 bis. Le DÉFAUT du 30/10 reproduit : la carte n'accepte qu'un groupe ─────────────
    # `sans_effet` sur les tours suivants n'est pas simulable ici ; on mesure donc l'autre
    # moitié du défaut : quand rien ne progresse, on s'arrête ET on le dit.
    inv1 = carte("1dfd", ["1dfd", "2dhd"], [True, False, False, False],
                 groupes=[0, 1, 0, 1])
    ns1, j1, b1 = jouer(source, inv1, sans_effet=True)
    OK(len(b1) == 1, "Z8 une carte qui n'applique rien n'est PAS rebasculée en rond",
       "%d bascule(s)" % len(b1))
    OK("switching in circles" in "\n".join(j1),
       "Z9 et le journal dit pourquoi on s'arrête", "\n".join(j1)[-80:])
    e1 = ns1["_NORMALISATION"]
    OK(e1 and e1.get("ok") is False,
       "Z10 ★★ `ok` vaut FAUX : c'est l'affirmation qui a rendu le défaut invisible",
       repr(e1 and e1.get("ok")))
    OK(e1 and len(e1.get("restantes") or []) == 4,
       "Z11 les prises restées hors profil sont NOMMÉES",
       repr(e1 and e1.get("restantes")))
    av1 = ns1["_avis_prises_eteintes"]()
    OK(av1 and av1.get("key", "").endswith("profile_partially_normalised"),
       "Z12 un avis annonce la normalisation partielle", repr(av1))
    OK(av1 and av1["params"].get("left") == 4 and av1["params"].get("switches") == 1,
       "Z13 avec le compte de prises restantes et de bascules tentées")
    if e1:
        e1["t"] = time.time() - 40000.0
    OK(ns1["_avis_prises_eteintes"]() is not None,
       "Z14 ★ cet avis N'EXPIRE PAS : il décrit un ÉTAT qui dure, pas un événement passé")

    # ── 0 ter. Inventaire illisible après la bascule : on s'arrête et on le dit ───────────
    inv2 = carte("1dfd", ["1dfd", "2dhd"], [False, False], groupes=[0, 1])
    ns2b, j2b, b2b = jouer(source, inv2, inventaire_muet=True)
    OK(len(b2b) == 1, "Z15 inventaire muet après la bascule : on ne continue pas à l'aveugle")
    OK(ns2b["_NORMALISATION"] and ns2b["_NORMALISATION"].get("ok") is False,
       "Z16 et on ne prétend pas que la carte est normalisée")

    # ── 1. Le cas de la Quad 2 de Cyril : un câble vivant, des prises éteintes ────────────
    inv = carte("1dfd", ["1dfd", "2dhd", "4dhd"], [True, False, False, False])
    ns, journal, bascules = jouer(source, inv)
    OK(len(bascules) >= 1, "A1 la bascule a lieu MALGRÉ une prise qui porte un signal",
       "%d bascule(s)" % len(bascules))
    OK(bascules and bascules[0][0] == "4dhd",
       "A2 et elle vise le profil qui donne le plus de prises",
       bascules[0][0] if bascules else "-")
    OK(bascules and bascules[0][2] is True,
       "A3 avec consentement explicite (la carte est relâchée)")
    jt = "\n".join(journal)
    OK("ANYWAY" in jt, "A4 le journal dit que la bascule a lieu quand même", jt[:80])
    # ⚠ PAS « le pid apparaît quelque part » : le pid du GROUPE est imprimé dans les deux
    #   branches du journal, donc une telle assertion restait verte quand la liste des prises
    #   vivantes était vidée. On exige la liste elle-même, derrière son étiquette.
    OK(re.search(r"Connectors carrying a signal: \S", jt),
       "A5 et il NOMME les prises qu'il risque de couper — un avertissement anonyme n'en est pas un",
       jt[-90:])
    OK("2026-10-30" in jt, "A6 la décision est datée dans la trace")
    etat = ns["_NORMALISATION"]
    OK(etat and etat.get("coupees") == [4040731136],
       "A7 l'état publié porte la liste des prises coupées", repr(etat and etat.get("coupees")))
    OK(etat and etat.get("de") == "1dfd" and etat.get("vers") == "4dhd",
       "A8 et le profil d'avant comme celui d'après")
    OK(etat and etat.get("ok") is True, "A9 ainsi que le verdict de la bascule")
    avis = ns["_avis_normalisation"]()
    OK(avis and avis.get("key", "").endswith("profile_switched_live"),
       "A10 un avis est publié sur :8080", repr(avis))
    OK(avis and avis["params"].get("cut") == 1,
       "A11 avec un COMPTE de prises coupées, pas leur liste")
    OK(avis and not any(isinstance(v, (list, dict)) for v in avis["params"].values()),
       "A12 paramètres grossiers : rien qui varie à chaque relevé (limite horaire du relais)")

    # ── 2. Aucun signal : la bascule a lieu, et le journal ne crie pas ────────────────────
    ns2, j2, b2 = jouer(source, carte("1dfd", ["1dfd", "4dhd"], [False, False]))
    OK(len(b2) == 1, "B1 sans signal non plus, rien n'empêche la bascule")
    OK("ANYWAY" not in "\n".join(j2),
       "B2 et le journal ne parle pas de coupure quand il n'y en a pas")
    OK(ns2["_NORMALISATION"] and ns2["_NORMALISATION"].get("coupees") == [],
       "B3 aucune prise coupée n'est déclarée")
    OK(ns2["_avis_normalisation"]() is None,
       "B4 donc AUCUN avis : on n'alerte pas sur une bascule qui n'a rien coupé")

    # ── 3. Déjà dans le bon profil : surtout ne pas couper pour rien ──────────────────────
    ns3, j3, b3 = jouer(source, carte("4dhd", ["1dfd", "4dhd"], [True, True, True, True]))
    OK(len(b3) == 0, "C1 carte déjà dans le bon profil : AUCUNE bascule",
       "%d bascule(s)" % len(b3))
    OK(ns3["_NORMALISATION"] is None, "C2 et rien n'est annoncé")

    # ── 4. Carte mono-profil : il n'y a rien à basculer, et on le DIT ─────────────────────
    ns4, j4, b4 = jouer(source, carte("1dfd", [], [True]))
    OK(len(b4) == 0, "D1 carte mono-profil : aucune bascule")
    OK("single-profile" in "\n".join(j4),
       "D2 et le journal distingue « rien à faire » de « rien tenté »", "\n".join(j4)[:70])

    # ── 5. Carte qui n'offre que du full-duplex : jamais choisie ──────────────────────────
    ns5, j5, b5 = jouer(source, carte("1dfd", ["1dfd", "2dfd"], [True]))
    OK(len(b5) == 0, "E1 aucun profil half-duplex offert : on ne bascule pas",
       "%d bascule(s)" % len(b5))

    # ── 6. L'avis s'éteint, /state garde ──────────────────────────────────────────────────
    ns6, _, _ = jouer(source, carte("1dfd", ["1dfd", "4dhd"], [True, False]))
    # Null-safe À DESSEIN : une mutation qui n'annonce plus rien doit rougir sur l'assertion,
    # pas planter le harnais. Une mutation attrapée par un PLANTAGE ne dit pas laquelle des
    # propriétés a cédé — c'est ce qui m'a fait croire une garde mordante alors qu'elle
    # mesurait l'absence d'un attribut.
    if ns6["_NORMALISATION"]:
        ns6["_NORMALISATION"]["t"] = time.time() - 4000.0
    OK(ns6["_avis_normalisation"]() is None,
       "F1 passé la fraîcheur, l'avis s'éteint — on n'alerte pas sur un fait d'il y a une heure")
    OK(ns6["_NORMALISATION"] is not None,
       "F2 mais l'état le garde : « pourquoi mon signal a sauté » se pose plus tard")

    # ── 7. Bascule REFUSÉE par la carte : le dire, et ne pas prétendre que c'est fait ─────
    ns7, j7, b7 = jouer(source, carte("1dfd", ["1dfd", "4dhd"], [True, False]), echec=True)
    OK(len(b7) == 1, "G1 la bascule est bien tentée")
    OK(ns7["_NORMALISATION"] and ns7["_NORMALISATION"].get("ok") is False,
       "G2 un refus de la carte est publié COMME un refus, jamais supposé réussi",
       repr(ns7["_NORMALISATION"] and ns7["_NORMALISATION"].get("ok")))
    OK(ns7["_NORMALISATION"] and ns7["_NORMALISATION"].get("code") == "profile_switch_refused",
       "G3 et son motif est repris tel quel")
    OK("normalisation failed" in "\n".join(j7),
       "G4 le journal le dit aussi", "\n".join(j7)[-70:])
    bs7 = (ns7["_NORMALISATION"] or {}).get("bascules") or []
    OK(bs7 and bs7[0].get("ok") is False and bs7[0].get("code") == "profile_switch_refused",
       "G5 et chaque bascule garde SON propre verdict, lu du retour de la carte", repr(bs7))
    corps = extraire("_normaliser_carte", source)

    # ── 8. Le relevé précède la bascule (mesuré le 19/10) ────────────────────────────────
    i_releve = corps.find("vivantes = _vivantes(subs)")
    i_bascule = corps.find("r = _basculer_profil(")
    OK(0 <= i_releve < i_bascule,
       "H1 les prises vivantes sont relevées AVANT de relâcher la carte",
       "relevé@%d bascule@%d" % (i_releve, i_bascule))

    return r


# ─── Mutations ────────────────────────────────────────────────────────────────────────────
def remplacer(s, de, vers):
    if s.count(de) != 1:
        raise RuntimeError("ancre périmée (%dx) : %s" % (s.count(de), de[:60]))
    return s.replace(de, vers, 1)


MUTATIONS = [
    # ── LE DÉFAUT D'ORIGINE : abandonner dès qu'une prise porte un signal ────────────────
    ("abandon_si_signal", lambda s: remplacer(
        s, "        vivantes = _vivantes(subs)",
        "        vivantes = _vivantes(subs)\n        if vivantes:\n            return")),
    # ── ★★ LE DÉFAUT DU 30/10 : s'arrêter après la PREMIÈRE bascule ─────────────────────
    # La mutation qui compte. Elle remet exactement le code de 0.29.44, et la garde d'alors ne
    # la voyait pas : son décor donnait un profil unique à toute la carte, donc une bascule
    # suffisait. Le décor encodait l'erreur, la garde ne pouvait que la confirmer.
    ("une_seule_bascule", lambda s: remplacer(
        s, "        subs = suivant\n    if not bascules:",
        "        subs = suivant\n        break\n    if not bascules:")),
    # ⚠ PAS DE MUTATION « ok = not echecs » ICI, ET C'EST DÉLIBÉRÉ. Je l'avais écrite ; elle ne
    #   mord sur rien, parce que sous ce flot de contrôle `restantes` non vide implique toujours
    #   au moins un échec (toute sortie prématurée en ajoute un). Les deux formulations sont donc
    #   équivalentes, et garder la mutation ferait croire à une propriété vérifiée qui ne l'est
    #   pas. C'est aussi pourquoi le code ne garde plus qu'UNE clause : `ok = not restantes`,
    #   l'état MESURÉ. Une clause redondante n'est pas une sécurité, c'est une assertion qu'on
    #   ne peut pas tester.
    ("ok_toujours_vrai", lambda s: remplacer(
        s, '"ok": not restantes,', '"ok": True,')),
    ("restantes_non_publiees", lambda s: remplacer(
        s, '"restantes": restantes,', '"restantes": [],')),
    ("progres_non_exige", lambda s: remplacer(
        s, "        if len(_a_faire(suivant)) >= len(reste):", "        if False:")),
    ("inventaire_dapres_ignore", lambda s: remplacer(
        s, '        suivant = ((r.get("apres") or {{}}).get("devices")) or []',
        "        suivant = subs")),
    # ── Les prises coupées ──────────────────────────────────────────────────────────────
    ("vivantes_relevees_une_fois", lambda s: remplacer(
        s, "        vivantes = _vivantes(subs)",
        "        vivantes = _vivantes(subs) if not bascules else []")),
    ("coupees_tout_le_monde", lambda s: remplacer(
        s, '        return [x.get("persistent_id") for x in ds\n'
           '                if (x.get("signal") or {{}}).get("entree_verrouillee")\n'
           '                or (x.get("signal") or {{}}).get("mode_detecte")]',
        '        return [x.get("persistent_id") for x in ds]')),
    # ★ LA RÈGLE DE 0.29.45 REMISE : coupée = vivante avant, plus vivante après. Elle dépend du
    #   délai de reprise des verrous, que la session Z8 a mesuré à des dizaines de secondes.
    ("coupees_par_diff_de_signal", lambda s: remplacer(
        s, "        for x in sorted(set(_vivantes((r.get(\"avant\") or {{}}).get(\"devices\") "
           "or [])) & touchees):",
        '        _ap_v = set(_vivantes((r.get("apres") or {{}}).get("devices") or []))\n'
        '        for x in sorted(set(_vivantes((r.get("avant") or {{}}).get("devices") '
        "or [])) - _ap_v):")),
    # ★ LA RÈGLE DE 0.29.44 REMISE : tout ce qui était vivant sur la carte, groupe basculé ou non.
    ("coupees_sans_le_groupe_touche", lambda s: remplacer(
        s, "or [])) & touchees):", "or []))):")),
    ("coupees_non_nommees", lambda s: remplacer(
        s, '                     ", ".join(str(x) for x in vivantes)), flush=True)',
        '                     ""), flush=True)')),
    ("etat_sans_les_coupees", lambda s: remplacer(
        s, '"coupees": coupees,', '"coupees": [],')),
    ("etat_non_publie", lambda s: remplacer(
        s, '    _NORMALISATION = {{"t": time.time(), "model": modele,',
        '    _inutile = {{"t": time.time(), "model": modele,')),
    # ── Déjà bon / mono-profil ──────────────────────────────────────────────────────────
    ("bascule_meme_si_deja_bon", lambda s: remplacer(
        s, '                and (x.get("profil_actif") or {{}}).get("id") != _profil_cible(x)]',
        "                ]")),
    ("mono_profil_bascule", lambda s: remplacer(
        s, "    if not any(_profil_cible(d) for d in subs):", "    if False:")),
    # ── Les avis ────────────────────────────────────────────────────────────────────────
    ("avis_toujours_publie", lambda s: remplacer(
        s, '    if time.time() - float(n.get("t") or 0.0) > _NORMALISATION_AVIS_S:\n'
           "        return None",
        "    if False:\n        return None")),
    ("avis_sans_coupure", lambda s: remplacer(
        s, '    if not n or not n.get("coupees"):', "    if not n:")),
    ("avis_params_fins", lambda s: remplacer(
        s, '"cut": len(n.get("coupees") or [])', '"cut": list(n.get("coupees") or [])')),
    ("avis_eteintes_retire", lambda s: remplacer(
        s, '    if not n or not n.get("restantes"):', "    if True:")),
    # L'avis de prises éteintes ne doit PAS expirer : il décrit un état qui dure.
    ("avis_eteintes_expire", lambda s: remplacer(
        s, '    if not n or not n.get("restantes"):\n        return None',
        '    if not n or not n.get("restantes"):\n        return None\n'
        '    if time.time() - float(n.get("t") or 0.0) > _NORMALISATION_AVIS_S:\n'
        "        return None")),
    # ── Le verdict de la carte ──────────────────────────────────────────────────────────
    ("verdict_suppose", lambda s: remplacer(
        s, '"ok": bool(r.get("ok")), "code": r.get("code")}})',
        '"ok": True, "code": None}})')),
    ("releve_apres_la_bascule", lambda s: remplacer(
        s, "        vivantes = _vivantes(subs)\n        # La bascule a lieu",
        "        vivantes = []\n        # La bascule a lieu")),
]

print("decklink_io — normalisation du profil au démarrage\n")
for cond, titre, detail in passe(src):
    ok(cond, titre, detail)

print("\nMutations (chacune DOIT faire rougir au moins une assertion) :")
for nom, f in MUTATIONS:
    try:
        mute = f(src)
    except RuntimeError as exc:
        n += 1
        ko = 1
        print("  ÉCHEC %s — %s" % (nom, exc))
        continue
    try:
        res = passe(mute)
    except Exception as exc:                                        # noqa: BLE001
        n += 1
        print("  ok    %s (rejetée : %s)" % (nom, str(exc)[:60]))
        continue
    rouges = [t.split(" ")[0] for c, t, _ in res if not c]
    n += 1
    if rouges:
        print("  ok    %s — attrapée par %s" % (nom, ", ".join(rouges)))
    else:
        ko = 1
        print("  ÉCHEC %s — AUCUNE assertion n'a rougi" % nom)

print("\n%s" % ("→ ÉCHEC" if ko else "→ %d vérifications : la carte est normalisée au "
                                    "démarrage, et jamais en silence." % n))
sys.exit(ko)
