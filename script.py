# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 BOBI SAS, France
# Auteur : Cyril Mazouer, pour le compte de BOBI SAS
# Distribué sous licence GNU GPL v3 (ou ultérieure) ; voir le fichier LICENSE.
#
# decklink_io — superviseur déployé par l'agent dans le conteneur.
#
# Il ne touche PAS au matériel lui-même : tout le chemin de données est dans le binaire natif
# `decklink_rx` (capture v210 10 bits → planar → grains MXL). Ce script le lance, le surveille,
# le relance s'il meurt, et publie ce qu'il rapporte : fps sur :8080, état détaillé sur :8082.
#
# ★ Chaque entrée est désignée par son PERSISTENT ID, jamais par un index : un changement de
#   profil de la carte renumérote les sous-périphériques et redistribue les connecteurs. Un
#   index mémorisé pointerait alors une autre prise, en silence.
#
# ⚠ Ce fichier est un gabarit `str.format` : toute accolade littérale est DOUBLÉE.

import json, os, re, signal, subprocess, threading, time
from http.server import HTTPServer, BaseHTTPRequestHandler

CONFIG         = {config}
HOSTNAME       = "{hostname}"
PLUGIN_VERSION = "{plugin_version}"

BINAIRE    = CONFIG.get("binary") or "/usr/local/bin/decklink_rx"
BINAIRE_TX = CONFIG.get("binary_tx") or "/usr/local/bin/decklink_tx"


def _liste(cfg, cle):
    """`inputs` / `outputs` d'une configuration, en liste.

    ★ `coerce_config` ne connaît pas de type LISTE : selon le chemin de configuration, la valeur
      arrive soit en liste, soit en CHAÎNE JSON. Accepter les deux évite un plugin qui démarre,
      ne déclare aucune entrée et n'explique rien."""
    v = cfg.get(cle) or []
    if isinstance(v, str):
        try:
            v = json.loads(v) or []
        except Exception:
            print("decklink_io: `%s` illisible (ni liste ni JSON valide)" % cle, flush=True)
            v = []
    return v if isinstance(v, list) else []


def _construire(cfg):
    """(entrées, sorties, voies, profondeur) d'une configuration — au démarrage COMME à chaud.

    ★ UNE SEULE CONSTRUCTION. La reconfiguration à chaud (`POST /config`) passe par ici, comme
      le démarrage : deux lectures de la même configuration finiraient par diverger, et une
      prise réglée à chaud ne serait pas celle que le prochain démarrage relirait."""
    # 8 ou 10 ; toute autre valeur retombe sur 10, le comportement historique.
    bits = cfg.get("bit_depth") if cfg.get("bit_depth") in (8, 10) else 10
    entrees = []
    # Le nom de flux par défaut suit la convention maison {{hostname}}_{{n}}, numérotée À PARTIR DE 1.
    for n, e in enumerate(_liste(cfg, "inputs"), start=1):
        entrees.append({{
            "persistent_id": int(e.get("persistent_id") or 0),
            "flow": e.get("flow") or "{{}}_{{}}".format(HOSTNAME, n),
            "audio": int(e.get("audio") or 0),
            "anc": bool(e.get("anc")),
            "numero": n,
        }})
    sorties = []
    for n, o in enumerate(_liste(cfg, "outputs"), start=1):
        # ⚠⚠ CETTE RECOPIE A PERDU UN CHAMP TROIS FOIS, ET LE COMMENTAIRE QUI LA SURPLOMBAIT
        #   DISAIT DÉJÀ « TOUT CHAMP OUBLIÉ ICI EST PERDU EN SILENCE ». Écrit après la première
        #   perte, il n'a empêché ni la deuxième ni la troisième :
        #     1. la PRÉ-CHARGE — normalisée, persistée, lue par la boucle de lancement, absente
        #        d'ici : deux pré-charges différentes donnaient le même retard ;
        #     2. le FLUX AUDIO, trois semaines plus tard : la ligne de commande partait sans
        #        `--audio` et l'exploitant n'entendait rien ;
        #     3. le SECOND GROUPE AUDIO (`audio_flow_2`), trouvé le 28/10 : posé par le hook,
        #        lu par `_args_voie`, jamais recopié — les canaux 9-16 ne partaient JAMAIS, quoi
        #        qu'on câble, et le compteur `audio_channels_declared_2` restait à 0.
        #   Une liste blanche tenue à la main se périme : c'est la FAMILLE qu'il faut fermer, pas
        #   le cas. On REPREND donc tout ce que le hook a posé, et on ne force que le TYPE des
        #   champs dont le reste du script dépend. Un champ ajouté demain arrive tout seul.
        sortie = dict(o)
        sortie.update({{
            "persistent_id": int(o.get("persistent_id") or 0),
            "flow": o.get("flow") or "",
            "mode": o.get("mode") or "",
            "preroll": int(o.get("preroll") or 0),
            # Attente des tranches, en ms (0 = le binaire garde son défaut). Cf. le pavé du
            # hook : le plafond réel est tenu par le binaire, qui seul connaît la durée d'une
            # trame du mode.
            "slice_wait_ms": int(o.get("slice_wait_ms") or 0),
            # Fill & Key : le connecteur et le flux de la DÉCOUPE, quand la sortie en déclare une.
            "key_persistent_id": int(o.get("key_persistent_id") or 0),
            "key_flow": o.get("key_flow") or "",
            "audio_flow": o.get("audio_flow") or "",
            "audio_flow_2": o.get("audio_flow_2") or "",
            "audio_channels": int(o.get("audio_channels") or 0),
            "numero": n,
        }})
        sorties.append(sortie)
    # ── VOIES ──────────────────────────────────────────────────────────────────────────────
    # Une sortie Fill & Key porte DEUX connecteurs. Chacun a son binaire, son fichier de
    # statistiques ET SA CASE D'ÉTAT : partager l'état ferait écrire les deux dans la même, et
    # la découpe aurait l'air saine parce que le remplissage l'est.
    voies = []
    for o in sorties:
        voies.append({{"sortie": o["numero"] - 1, "voie": "fill", "flow": o["flow"],
                      "persistent_id": o["persistent_id"]}})
        if o.get("key_persistent_id") and o.get("key_flow"):
            voies.append({{"sortie": o["numero"] - 1, "voie": "key", "flow": o["key_flow"],
                          "persistent_id": o["key_persistent_id"]}})
    return entrees, sorties, voies, bits


# ── Fichiers de statistiques : nommés par la PRISE, pas par le rang ───────────────────────
# ★ Ils étaient numérotés par le rang dans la liste (`decklink_tx_3.json`). Un rang change dès
#   qu'une prise est ajoutée ou retirée devant : à chaud, une sortie inchangée aurait vu son
#   fichier changer de nom, donc sa ligne de commande, donc elle aurait été relancée pour rien.
#   L'identifiant stable de la prise ne bouge pas.
def _stats_entree(pid):
    return "/tmp/decklink_in_{{}}.json".format(pid)


def _stats_voie(pid):
    return "/tmp/decklink_tx_{{}}.json".format(pid)


def _mode_fichier(pid):
    """Canal lateral du mode « image manquante », par PRISE (meme raison que les statistiques :
    un rang change, un identifiant de prise non)."""
    return "/tmp/decklink_mode_{{}}".format(pid)


def _ecrire_mode(pid, mode):
    """Pose le mode effectif dans son fichier. ★ CE N'EST PAS DANS `args` A DESSEIN : le mettre
    sur la ligne de commande ferait relancer le connecteur a chaque bascule (`_reconfigurer`
    compare les `args`), et sur la sortie PGM d'antenne, changer un reglage de MISE AU POINT
    couperait le direct. Le binaire relit ce fichier une fois par seconde.

    Ecriture ATOMIQUE : le binaire lit sans verrou, et un fichier tronque lu a mi-chemin le
    ferait retomber sur `repeat` sans rien dire."""
    mode = mode if mode in ("repeat", "black") else "repeat"
    chemin = _mode_fichier(pid)
    tmp = chemin + ".tmp"
    try:
        with open(tmp, "w") as f:
            f.write(mode + "\n")
        os.replace(tmp, chemin)
    except Exception as exc:
        print("decklink_io: missing-frame mode write failed: pid=%s mode=%s err=%s"
              % (pid, mode, exc), flush=True)


def _args_entree(e, bits):
    # ★ LA PROFONDEUR PUBLIÉE VIENT DE L'ORCHESTRATEUR, pas de la carte : celle-ci livre toujours
    #   du v210 (10 bits), mais `mxl_pipeline_bit_depth` arbitre la profondeur du BUS pour tout
    #   le produit. Le binaire l'ignorait et écrivait 10 en dur.
    args = [BINAIRE, str(e["persistent_id"]), e["flow"], "0",
            "--bits", str(bits), "--stats", _stats_entree(e["persistent_id"])]
    if e["audio"]:
        args += ["--audio", str(e["audio"])]
    if e["anc"]:
        args += ["--anc"]
    return args


def _args_voie(v, o):
    """Ligne de commande d'une voie de sortie, ou un DIAGNOSTIC (chaîne) si elle ne peut pas partir.

    ★ DEUX SITUATIONS DISTINCTES, DEUX MESSAGES. Une sortie SANS SOURCE est un état normal : le
      connecteur est déclaré en sortie, la source sera posée par la page Câbles. La confondre
      avec une configuration incomplète envoie l'exploitant chercher une erreur là où il n'y en
      a pas."""
    if not o["persistent_id"] or not o["mode"]:
        return "configuration incomplète (identifiant ou format manquant)"
    if not v["flow"]:
        return "en attente d'une source"
    # Fill & Key : DEUX binaires, un par connecteur, même format et même pré-charge, tous deux
    # asservis à la référence de la carte. Un seul binaire pour les deux serait plus fragile :
    # une panne sur l'un emporterait l'autre.
    args = [BINAIRE_TX, str(v["persistent_id"]), v["flow"], o["mode"], "0",
            "--stats", _stats_voie(v["persistent_id"])]
    # ★ PRÉ-CHARGE : c'est LE levier de latence de la sortie.
    if o.get("preroll"):
        args += ["--preroll", str(o["preroll"])]
    # ★ ATTENTE DES TRANCHES : le levier de JUSTESSE. Trop courte, la sortie répète une image
    #   puis en saute une, à cadence parfaite — c'est ce qui a saccadé le PGM le 28/10.
    # ⚠ UN SEUL JETON, `--slice-wait=<ms>`, ET C'EST UNE QUESTION D'ANTENNE. Le binaire vit dans
    #   l'image de calcul : ce script peut être redéployé seul, devant un binaire d'avant 0.75.
    #   Un tel binaire lit tout jeton inconnu comme la durée en secondes — mesuré : avec deux
    #   jetons il comprend « arrête-toi dans 25 s », s'arrête, est relancé, et la sortie cycle
    #   toutes les 25 secondes. Avec la forme collée il lit 0, soit « sans limite », et ignore
    #   l'option. On ne passe donc JAMAIS la forme en deux jetons depuis ici.
    if o.get("slice_wait_ms"):
        args += ["--slice-wait=%s" % o["slice_wait_ms"]]
    # ★ TOUJOURS PASSE, ET LE CHEMIN NE DEPEND QUE DE LA PRISE : c'est ce qui garantit que
    #   basculer le mode ne modifie PAS `args`, donc ne relance pas la sortie. Forme COLLEE
    #   obligatoire, pour la raison ecrite juste au-dessus (un binaire d'avant lirait un jeton
    #   inconnu comme une duree en secondes et s'arreterait).
    args += ["--missing-frame-mode-file=%s" % _mode_fichier(v["persistent_id"])]
    # ★ L'AUDIO VA SUR LE REMPLISSAGE, PAS SUR LA DÉCOUPE : l'embarquer dans les deux doublerait
    #   l'audio sur la liaison aval (+6 dB dans un mélangeur qui somme les deux entrées).
    if v.get("voie") != "key" and o.get("audio_flow"):
        args += ["--audio", o["audio_flow"], str(o.get("audio_channels") or 8)]
        # ★ 16 CANAUX = DEUX FLUX DE 8 (21/10). Le bus porte des flux de 8 canaux ; demander 16
        #   sur un seul flux ouvrait 16 canaux sur la carte et n'en alimentait que 8, en
        #   silence. Le second groupe est un port de câblage à part : il peut venir d'une autre
        #   source que le premier (programme sur 1-8, retours sur 9-16).
        if o.get("audio_flow_2"):
            args += ["--audio-2", o["audio_flow_2"]]
    return args


ENTREES, SORTIES, VOIES = [], [], []
# Vues publiées : les cases d'état des travailleurs, dans l'ordre de la configuration.
_etats = []
_etats_voie = []
# ★★ LES ENTRÉES ÉCARTÉES PAR LE HOOK, pour qu'un refus ne soit pas MUET. Le hook écarte une
#    entrée dont le nom de flux est déjà pris (sinon deux captures se marchent dessus sur le
#    bus et la seconde reste à 0 i/s sans un mot). Il le PUBLIAIT dans la configuration —
#    et personne ne le lisait : ni ici, ni /state, ni la console. Une entrée qui disparaît
#    sans un mot, c'est le défaut du 23/10 sous une autre forme, et il a tenu une journée.
_refusees = []
_verrou = threading.Lock()
_stop = threading.Event()


# ── La vue PAR SORTIE est DÉRIVÉE, jamais stockée ─────────────────────────────────────────
# Il y avait ici une SECONDE copie de l'état des sorties (`_etats_out`), restée à « init » /
# 0 i/s À VIE. Mesuré le 2026-10-19 : une sortie qui avait tissé 5 618 078 trames sans un seul
# retard s'affichait MORTE dans la console. Deux copies d'un même état finissent toujours par
# diverger : on n'en garde qu'une (la voie) et on DÉRIVE l'autre à la lecture.
def _vue_sorties():
    """Une entrée par SORTIE, bâtie sur sa voie `fill`. À appeler sous `_verrou`."""
    vues = []
    for _o in SORTIES:
        _v = next((x for x in _etats_voie
                   if x["sortie"] == _o["numero"] and x["voie"] == "fill"), None)
        vue = {{"numero": _o["numero"], "flow": _o["flow"],
               "persistent_id": _o["persistent_id"], "video_mode": _o["mode"]}}
        if _v:
            vue.update({{k: _v[k] for k in _v
                        if k not in ("sortie", "voie", "flow", "persistent_id")}})
        vues.append(vue)
    return vues


def _lire_stats(chemin):
    """Lit le JSON publié par le binaire. Renvoie None si absent ou illisible — le binaire fait
    un rename atomique, donc un échec ici signifie vraiment « pas encore de nouvelle »."""
    try:
        with open(chemin) as f:
            return json.load(f)
    except Exception:
        return None


def _fin(t):
    """Ce travailleur doit-il s'arrêter ? (arrêt du conteneur, ou reconfiguration)"""
    return _stop.is_set() or t["stop"].is_set()


def _rendre_prise(t, proc):
    """Termine le binaire ET attend qu'il ait rendu la prise.

    ★ À CHAUD, ATTENDRE EST OBLIGATOIRE. Passer une prise d'entrée en sortie, c'est arrêter un
      binaire et en lancer un autre sur le MÊME sous-périphérique : si le premier tient encore la
      carte, le second échoue à l'ouverture et la prise reste muette. Au démarrage, le fil
      principal faisait ce ménage ; à chaud, c'est le travailleur lui-même."""
    try:
        proc.terminate()
        proc.wait(timeout=3)
    except Exception:
        try:
            proc.kill()
            proc.wait(timeout=2)
        except Exception:
            pass
    with _verrou_enfants:
        _enfants.discard(proc)


def _superviser(t):
    """Lance le binaire d'une ENTRÉE et le maintient en vie.

    ★ Le redémarrage est COMPTÉ et publié. Un binaire qui redémarre en boucle produit sinon une
      cadence qui a l'air seulement « instable », alors que la cause est ailleurs."""
    etat, args, stats = t["etat"], t["args"], t["stats"]
    while not _fin(t):
        try:
            os.remove(stats)
        except OSError:
            pass
        try:
            proc = _lancer(args)
        except Exception as exc:
            with _verrou:
                etat["mode"] = "binaire introuvable: {{}}".format(exc)
            t["stop"].wait(5)
            continue
        with _verrou:
            etat["mode"] = "en cours"
        while proc.poll() is None and not _fin(t):
            d = _lire_stats(stats)
            with _verrou:
                if d:
                    avant_signal = etat.get("signal")
                    etat.update({{
                        "fps": float(d.get("fps") or 0.0),
                        "frame_index": int(d.get("frame_index") or 0),
                        "signal": bool(d.get("signal")),
                        "width": int(d.get("width") or 0),
                        "height": int(d.get("height") or 0),
                        "interlaced": bool(d.get("interlaced")),
                        "no_source": int(d.get("no_source") or 0),
                        "format_changes": int(d.get("format_changes") or 0),
                        # ⚠⚠ L'ÉTAT D'UNE ENTRÉE PUBLIAIT 7 CHAMPS SUR LES 39 QUE LE BINAIRE ÉCRIT,
                        #    et c'est ce qui a rendu la panne audio du 28/10 indéchiffrable de
                        #    l'extérieur : la console #214 ne recevait rien des entrées SDI, le
                        #    binaire SAVAIT que son index audio dérivait, et il fallait un
                        #    `docker exec` pour le lire. La même faute avait déjà été commise
                        #    deux fois sur les SORTIES (22/10 et 24/10) ; la garde
                        #    `verif_stats_publiees` avait été écrite pour ça — mais elle ne
                        #    regardait que `decklink_tx`. Elle couvre maintenant les DEUX
                        #    binaires, donc cette liste ne peut plus se périmer en silence :
                        #    un compteur ajouté au binaire et oublié ici fait rougir la garde.
                        #
                        # ★ LE TÉMOIN D'HORLOGE AUDIO EST LE PREMIER DE LA LISTE, parce que c'est
                        #   lui qui décide : `audio_offset_ms` est SIGNÉ — négatif, on écrit en
                        #   retard sur l'horloge et un consommateur qui lit l'instant présent ne
                        #   trouve rien ; positif, on écrit en avance. `audio_resync_fwd` compte
                        #   les rattrapages vers l'avant, `audio_resync_back` les décrochages
                        #   francs : les confondre avait masqué le défaut.
                        # ★★ CÔTÉ VIDÉO, LA PHASE DANS LE CRÉNEAU — c'est elle qui décidait le
                        #    28/10 : la caméra perdue arrivait à 19,99 ms d'un créneau de 20, les
                        #    quatre saines entre 1,3 et 5,6 ms, et aucun compteur ne le disait.
                        #    `video_edge_ms` est la distance au bord le plus proche : PETIT = EN
                        #    DANGER, une gigue d'une milliseconde suffisant alors à changer de
                        #    créneau. `video_edge_min_ms` est la pire approche depuis le
                        #    démarrage, parce qu'un relevé ponctuel RATE la traversée (la phase
                        #    dérive de ~1,5 ms/h : elle dure quelques minutes).
                        #    `video_phase_ms` dit de quel côté du centre on dérive (origine au
                        #    CENTRE du créneau : convention de `mxlIndexToTimestamp`, MESURÉE).
                        # ⚠ `video_offset_ms` A ÉTÉ RETIRÉ, pas renommé par coquetterie : il
                        #    multipliait un nombre ENTIER de créneaux par la durée d'une trame,
                        #    donc il ne pouvait valoir que des multiples de 20 ms. Relevé sur la
                        #    Z8 le 30/10 : exactement ±20,00 ms sur les cinq entrées, min =
                        #    médiane = max. Il paraissait sain et ne mesurait pas la phase.
                        #    `video_grid_frames` le remplace en disant ce qu'il mesurait vraiment
                        #    (-1 en régime normal, +1 = la source devance le nœud).
                        "video_phase_ms": d.get("video_phase_ms"),
                        "video_edge_ms": d.get("video_edge_ms"),
                        "video_edge_min_ms": d.get("video_edge_min_ms"),
                        "video_grid_frames": d.get("video_grid_frames"),
                        "video_resync_fwd": int(d.get("video_resync_fwd") or 0),
                        "video_resync_back": int(d.get("video_resync_back") or 0),
                        "audio_offset_ms": d.get("audio_offset_ms"),
                        # ★ L'AUTRE ÉCART, et c'est celui que subit un consommateur : la TÊTE du
                        #   flux par rapport à maintenant. Échantillon de DENT DE SCIE — l'audio
                        #   arrive par paquets, donc la tête est à l'heure juste après une
                        #   écriture et a une période de paquet de retard juste avant la suivante.
                        #   C'est son MAXIMUM qui est la latence vraie, pas un relevé isolé.
                        "audio_head_lag_ms": d.get("audio_head_lag_ms"),
                        "audio_resync_fwd": int(d.get("audio_resync_fwd") or 0),
                        "audio_resync_back": int(d.get("audio_resync_back") or 0),
                        "audio_reanchor": int(d.get("audio_reanchor") or 0),
                        "audio_backward": int(d.get("audio_backward") or 0),
                        "audio_long_gap": int(d.get("audio_long_gap") or 0),
                        "audio_silence": int(d.get("audio_silence") or 0),
                        "audio_samples": int(d.get("audio_samples") or 0),
                        "audio_packets": int(d.get("audio_packets") or 0),
                        "audio_empty": int(d.get("audio_empty") or 0),
                        "audio_open_fail": int(d.get("audio_open_fail") or 0),
                        "audio_flows": int(d.get("audio_flows") or 0),
                        "audio_channels": int(d.get("audio_channels") or 0),
                        "audio_channels_2": int(d.get("audio_channels_2") or 0),
                        "audio_slices": int(d.get("audio_slices") or 0),
                        "audio_peak_raw": int(d.get("audio_peak_raw") or 0),
                        "audio_floats": int(d.get("audio_floats") or 0),
                        # Refus de trames : le détail par motif, déjà écrit par le binaire et
                        # jusqu'ici invisible (cf. l'entrée TODO du 28/10 sur la phase des
                        # captures). `diag` porte la DERNIÈRE cause en clair.
                        "refused": int(d.get("refused") or 0),
                        "refused_no_writer": int(d.get("refused_no_writer") or 0),
                        "refused_no_bytes": int(d.get("refused_no_bytes") or 0),
                        "refused_open_grain": int(d.get("refused_open_grain") or 0),
                        "refused_grain_short": int(d.get("refused_grain_short") or 0),
                        "refused_ahead_of_grid": int(d.get("refused_ahead_of_grid") or 0),
                        "diag": d.get("diag") or "",
                        # ANC et timecode.
                        "anc_grains": int(d.get("anc_grains") or 0),
                        "anc_packets": int(d.get("anc_packets") or 0),
                        "anc_oversize": int(d.get("anc_oversize") or 0),
                        "anc_open_fail": int(d.get("anc_open_fail") or 0),
                        "anc_tc_synth": int(d.get("anc_tc_synth") or 0),
                        "timecode": d.get("timecode") or "",
                        "sliced": int(d.get("sliced") or 0),
                        "whole": int(d.get("whole") or 0),
                    }})
                    # ★ « pas de signal » est un état publié, pas une cadence à zéro qu'on
                    #   laisse confondre avec un plantage.
                    etat["mode"] = "actif" if d.get("signal") else "pas de signal"
                    # ★★ L'INSTANT DE LA TRANSITION, et c'est lui qui porte l'ANTI-REBOND du
                    #    consommateur. Sur un relevé isolé, rien ne distingue une micro-coupure
                    #    de 200 ms d'une prise débranchée depuis la veille — cas vécu le 29/10 à
                    #    17:35 sur la Z8, où l'entrée 5 d'une carte était muette depuis des
                    #    heures pendant que les quatre autres tournaient à 50 fps, sans que rien
                    #    ne le dise : les lecteurs en ont accusé leur propre lecture.
                    #    Compter des lignes d'alerte ne mesure pas une durée ; cet horodatage se
                    #    lit en une soustraction, et le retour à la normale est EXPLICITE (le
                    #    compteur retombe à 0) au lieu d'être déduit d'une absence.
                    if bool(d.get("signal")) != bool(avant_signal):
                        etat["signal_change_t"] = time.time()
            t["stop"].wait(0.5)
        if _fin(t):
            _rendre_prise(t, proc)
            return
        rc = proc.poll()
        with _verrou:
            etat["redemarrages"] += 1
            etat["fps"] = 0.0
            etat["signal"] = False
            etat["mode"] = "arrêté (code {{}}) — relance".format(rc)
        t["stop"].wait(2)


def _superviser_sortie(t):
    """Lance `decklink_tx` pour une VOIE de sortie et le maintient en vie.

    ★ Une sortie SDI qui s'arrête est un incident d'antenne : le binaire ré-émet la dernière
      trame connue quand la source n'a rien de neuf, et COMPTE ces répétitions. Le superviseur
      les publie telles quelles — une sortie parfaitement cadencée sur une source morte doit se
      voir ici, pas se deviner."""
    etat, args, stats = t["etat"], t["args"], t["stats"]
    if isinstance(args, str):            # diagnostic : rien à lancer, on le DIT
        with _verrou:
            etat["mode"] = args
        return
    while not _fin(t):
        try:
            os.remove(stats)
        except OSError:
            pass
        try:
            proc = _lancer(args)
        except Exception as exc:
            with _verrou:
                etat["mode"] = "binaire introuvable: {{}}".format(exc)
            t["stop"].wait(5)
            continue
        with _verrou:
            # ★ Ne PAS écraser un diagnostic par « en cours » à chaque relance : sur une boucle
            #   d'échec, l'état oscillerait entre le vrai motif et un libellé rassurant.
            if not etat.get("erreur"):
                etat["mode"] = "en cours"
        while proc.poll() is None and not _fin(t):
            d = _lire_stats(stats)
            with _verrou:
                if d:
                    etat.update({{
                        "fps": float(d.get("fps") or 0.0),
                        "frame_index": int(d.get("frame_index") or 0),
                        "source_alive": bool(d.get("source_alive")),
                        "repeated": int(d.get("repeated") or 0),
                        "missed": int(d.get("missed") or 0),
                        "width": int(d.get("width") or 0),
                        "height": int(d.get("height") or 0),
                        # ── Grandeurs d'HORLOGE ────────────────────────────────────────────
                        # `genlock`          : cette sortie est-elle asservie à la référence.
                        # `slip_hours`       : heures entre deux glissements, MESURÉ sur les
                        #                      répétitions réellement survenues. null = pas encore
                        #                      de fenêtre close ; 0 = aucun glissement.
                        # `card_vs_node_ppm` : écart horloge de la carte ↔ horloge du nœud.
                        #   ⚠ Ne prédit PAS le glissement (contrôle du 2026-09-08) : publié pour
                        #   le diagnostic, pas pour l'alerte.
                        "genlock": bool(d.get("genlock")),
                        # ★ L'ATTENTE EFFECTIVE, pas celle configurée : le binaire la ramène à la
                        #   durée d'une trame s'il le faut, et c'est la valeur qui AGIT qu'il
                        #   faut pouvoir lire. Sans elle, on ne peut pas distinguer « le réglage
                        #   n'a pas pris » de « il a pris et ne suffit pas ».
                        "slice_wait_ms": d.get("slice_wait_ms"),
                        # ★ LES QUATRE MOTIFS DE REPLI. « `late_previous` monte et
                        #   `slice_timeouts` reste à 0 » a coûté un aller-retour entre deux
                        #   sessions le 28/10 : les trois sorties muettes vers le repli ne
                        #   comptaient pas. `slices_awaited` est le témoin inverse — la preuve
                        #   que la sortie a VRAIMENT suivi le producteur bande par bande.
                        "head_unreadable": int(d.get("head_unreadable") or 0),
                        "head_no_slices": int(d.get("head_no_slices") or 0),
                        "slice_geometry_refused": int(d.get("slice_geometry_refused") or 0),
                        "slices_awaited": int(d.get("slices_awaited") or 0),
                        # ★ `head_awaited` = répétitions ÉVITÉES en attendant, dans le budget
                        #   déjà accordé, que le producteur ouvre son grain suivant ; c'est le
                        #   chiffre qui mesure le correctif du 28/10. `head_frozen` dit
                        #   l'inverse : l'échéance a expiré sans que le head bouge, donc le
                        #   producteur est en retard — il désigne la SOURCE, pas la sortie.
                        "head_awaited": int(d.get("head_awaited") or 0),
                        "head_frozen": int(d.get("head_frozen") or 0),
                        # ★★ LA MARGE, réclamée par la Z8 le 30/10 en lisant son relevé : sur
                        #    la sortie PGM, `head_awaited` montait de 2994 en 60 s, soit une
                        #    attente À CHAQUE TRAME, toujours réussie. Savoir que le correctif
                        #    travaille ne dit pas de combien il tient : ce maximum couru se
                        #    compare au budget `slice_wait_ms` publié à côté. S'il s'en
                        #    approche, la trame suivante sera répétée — et aucun compteur
                        #    d'issue ne l'aurait annoncé.
                        "head_wait_max_ms": d.get("head_wait_max_ms"),
                        "slip_hours": d.get("slip_hours"),
                        "slip_window_s": int(d.get("slip_window_s") or 0),
                        "card_vs_node_ppm": d.get("card_vs_node_ppm"),
                        "drift_window_s": int(d.get("drift_window_s") or 0),
                        # ── DÉCROCHAGE DE GÉNÉRATION (22/10) ───────────────────────────────
                        # ★ SANS CES DEUX CHIFFRES, LA GARDE EST INVÉRIFIABLE : rien ne
                        #   distinguerait « elle n'a jamais eu à mordre » de « elle ne mord
                        #   pas ». Ils comptent les fois où le producteur a recréé son flux
                        #   sous le même nom et où la sortie s'est raccrochée seule. Une valeur
                        #   qui grimpe sans fin ne dit pas que la garde va mal, elle dit que le
                        #   PRODUCTEUR se recrée en boucle — c'est lui qu'il faut aller voir.
                        # Contrat PARTAGE avec le melangeur (30/10) : images emises qui ne
                        # sont pas une image FRAICHE de la source. Le mode effectif est
                        # recopie a cote — un compteur sans son mode ne dit pas ce que le
                        # spectateur a vu.
                        # ★ LES TROIS TEMOINS DE LA BORNE D'ATTENTE. `slack_min_ms` est le
                        #   PIRE temps restant observe avant l'instant de diffusion : s'il
                        #   approche de zero, la sortie est au bord de la trame JETEE (comptee
                        #   en `missed`), et c'est `preroll` qu'il faut monter. Sans ces trois
                        #   valeurs, le choix entre « monter la pre-charge » et « baisser
                        #   l'attente » se fait au raisonnement et non a la mesure.
                        # Chaque unite est UNE discontinuite volontaire a l'antenne : zero
                        # est le regime normal, un nombre qui grimpe dit que le processus se
                        # fait decaler et c'est LA qu'il faut chercher.
                        "schedule_reanchors": int(d.get("schedule_reanchors") or 0),
                        "slack_min_ms": d.get("slack_min_ms"),
                        "work_max_ms": d.get("work_max_ms"),
                        "wait_shortened": int(d.get("wait_shortened") or 0),
                        "missing_frames_repeated": int(d.get("missing_frames_repeated") or 0),
                        "missing_frame_mode": d.get("missing_frame_mode") or "repeat",
                        "flow_regens": int(d.get("flow_regens") or 0),
                        "audio_regens": int(d.get("audio_regens") or 0),
                        # ★ Ces deux-là manquaient à l'état alors qu'ils existaient dans les
                        #   statistiques : le 22/10 il a fallu ouvrir un terminal dans le
                        #   conteneur pour répondre au testeur. Un compteur qu'on doit aller
                        #   chercher à la main n'est pas publié.
                        "audio_group2_missing": bool(d.get("audio_group2_missing")),
                        "audio_resyncs": int(d.get("audio_resyncs") or 0),
                        # ── D'OÙ VIENT CHAQUE TRAME SERVIE (24/10) ─────────────────────────
                        # ★★ CETTE LISTE BLANCHE A REFAIT, POUR NEUF COMPTEURS, LE DÉFAUT QUE
                        #    DÉNONCE LE COMMENTAIRE JUSTE AU-DESSUS. Le binaire les publiait,
                        #    l'état les jetait, et il a fallu un `docker exec … cat` dans le
                        #    conteneur pour diagnostiquer une sortie qui répétait 15 trames par
                        #    seconde. C'est la deuxième fois : un compteur qu'on doit aller
                        #    chercher à la main n'est pas publié.
                        # Ils FERMENT LE COMPTE : sliced + late_previous + head_ontime +
                        # repeated ≈ la cadence nominale. Sans ça, « 15 répétitions/s » ne dit
                        # pas si c'est nous qui arrivons en retard (slice_timeouts), la carte
                        # qui refuse (dropped/late/flushed) ou la source qui ne commet pas
                        # (le reste) — trois pannes différentes derrière un seul chiffre.
                        "sliced": int(d.get("sliced") or 0),
                        "head_ontime": int(d.get("head_ontime") or 0),
                        "late_previous": int(d.get("late_previous") or 0),
                        "slice_timeouts": int(d.get("slice_timeouts") or 0),
                        "short_grains": int(d.get("short_grains") or 0),
                        # Côté CARTE, à ne pas confondre avec le côté SOURCE ci-dessus.
                        "dropped": int(d.get("dropped") or 0),
                        "late": int(d.get("late") or 0),
                        "flushed": int(d.get("flushed") or 0),
                        "queue_depth": int(d.get("queue_depth") or 0),
                        "queued_now": int(d.get("queued_now") or 0),
                        # ── MODE RÉELLEMENT APPLIQUÉ ───────────────────────────────────────
                        # Ce que la sortie FAIT, pas ce que la configuration demande : les deux
                        # ont divergé en silence plus d'une fois sur ce plugin.
                        "low_latency": bool(d.get("low_latency")),
                        "interlaced": bool(d.get("interlaced")),
                        "woven": int(d.get("woven") or 0),
                        # ── AUDIO : TOUTE LA MESURE ÉTAIT ÉCRITE ET JETÉE ICI ──────────────
                        # ★★ Les commentaires du binaire expliquent longuement pourquoi chacun
                        #    de ces compteurs est indispensable — « sans lui, les deux se lisent
                        #    audio_samples = 0, et une sortie qui attend son producteur
                        #    ressemble à une source muette », « un seul chiffre 16 canaux ne
                        #    distingue pas 16 portés de 16 ouverts, 8 alimentés ». Ils étaient
                        #    tous écrits par le binaire et AUCUN n'arrivait à l'état : la panne
                        #    du 21/10 tenait entière dans une confusion que ces chiffres
                        #    dissipent, et il fallait un terminal dans le conteneur pour les
                        #    voir. Garde : `verif_stats_publiees.py`.
                        "audio_samples": int(d.get("audio_samples") or 0),
                        "audio_clipped": int(d.get("audio_clipped") or 0),
                        "audio_skipped": int(d.get("audio_skipped") or 0),
                        "audio_skipped_2": int(d.get("audio_skipped_2") or 0),
                        "audio_pending": bool(d.get("audio_pending")),
                        "audio_opens": int(d.get("audio_opens") or 0),
                        "audio_flows": int(d.get("audio_flows") or 0),
                        "audio_channels_card": int(d.get("audio_channels_card") or 0),
                        "audio_channels_declared": int(d.get("audio_channels_declared") or 0),
                        "audio_channels_declared_2": int(d.get("audio_channels_declared_2") or 0),
                    }})
                    etat["erreur"] = ""          # ça tourne : le diagnostic tombe
                    etat["mode"] = ("émet" if d.get("source_alive")
                                    else "émet SANS source (trames répétées)")
            t["stop"].wait(0.5)
        if _fin(t):
            _rendre_prise(t, proc)
            return
        rc = proc.poll()
        with _verrou:
            etat["redemarrages"] += 1
            etat["fps"] = 0.0
            # 7 = flux source introuvable : la faute de CONFIGURATION la plus probable, NOMMÉE.
            # 8 = profondeur illisible : le binaire REFUSE d'émettre une image fausse.
            etat["erreur"] = (
                "flux « {{}} » introuvable sur le bus".format(t["flow"])
                if rc == 7 else
                "profondeur du flux « {{}} » illisible — la sortie refuse d'émettre une image "
                "fausse".format(t["flow"])
                if rc == 8 else "arrêté (code {{}}) — relance".format(rc))
            etat["mode"] = etat["erreur"]
        t["stop"].wait(2)


# ── Travailleurs : un par CONNECTEUR, désigné par sa prise ─────────────────────────────────
# ★★ RECONFIGURATION À CHAUD (21/10, demandé en recette). Chaque réglage, chaque câble
#    redéployait le conteneur, donc relançait TOUS les binaires : un câble audio sur une sortie
#    de la Quad coupait aussi les quatre prises de la Duo. Chaque connecteur est maintenant un
#    travailleur avec son propre arrêt, désigné par (rôle, prise). Une nouvelle configuration
#    est COMPARÉE à celle qui tourne : seuls les travailleurs dont la ligne de commande change
#    sont arrêtés et relancés ; les autres ne voient rien passer, compteurs compris.
_travailleurs = {{}}                  # (role, persistent_id) -> travailleur
_verrou_reconf = threading.Lock()


def _plan(entrees, sorties, voies, bits):
    """Ce qui DOIT tourner : clé -> (rôle, ligne de commande ou diagnostic, métadonnées)."""
    plan = {{}}
    for e in entrees:
        plan[("in", e["persistent_id"])] = {{
            "role": "in", "args": _args_entree(e, bits),
            "stats": _stats_entree(e["persistent_id"]), "flow": e["flow"],
            "meta": {{"numero": e["numero"], "flow": e["flow"],
                     "persistent_id": e["persistent_id"]}}}}
    for v in voies:
        o = sorties[v["sortie"]]
        plan[("out", v["persistent_id"])] = {{
            "role": "out", "args": _args_voie(v, o),
            "stats": _stats_voie(v["persistent_id"]), "flow": v["flow"],
            "meta": {{"sortie": v["sortie"] + 1, "voie": v["voie"], "flow": v["flow"],
                     "persistent_id": v["persistent_id"]}}}}
    return plan


def _etat_neuf(role, meta):
    if role == "in":
        base = {{"fps": 0.0, "frame_index": 0, "signal": False, "mode": "init",
                "width": 0, "height": 0, "interlaced": False, "redemarrages": 0,
                # ⚠ 0.0 et non `time.time()` : à l'amorce, on ne SAIT PAS depuis quand l'entrée
                #   est muette. Dater la transition de maintenant ferait repartir le compteur à
                #   chaque redéploiement du script et effacerait une panne vieille de la veille
                #   — exactement celle du 29/10. `no_signal_s` vaut alors null, pas 0.
                "signal_change_t": 0.0}}
    else:
        base = {{"fps": 0.0, "frame_index": 0, "source_alive": False, "repeated": 0, "missed": 0,
                "mode": "init", "redemarrages": 0, "genlock": False, "erreur": ""}}
    base.update(meta)
    return base


def _reconfigurer(cfg):
    """Applique une configuration en ne relançant QUE ce qui a changé. Rend le bilan."""
    global ENTREES, SORTIES, VOIES, _etats, _etats_voie, _refusees
    with _verrou_reconf:
        entrees, sorties, voies, bits = _construire(cfg)
        # ⚠ JOURNALISÉ À CHAQUE APPLICATION, pas seulement au démarrage : une entrée peut
        #   être écartée par une reconfiguration à chaud, et c'est le moment où l'exploitant
        #   regarde. En ANGLAIS, comme tout nouveau journal (décision du 2026-10-15).
        _ref = [r for r in _liste(cfg, "entrees_refusees") if isinstance(r, dict)]
        for _r in _ref:
            print("decklink_io: input dropped: flow=%s persistent_id=%s reason=%s"
                  % (_r.get("flow"), _r.get("persistent_id"), _r.get("raison")), flush=True)
        with _verrou:
            _refusees = _ref
        plan = _plan(entrees, sorties, voies, bits)
        # ★★ LE MODE EST POSE POUR TOUTES LES VOIES, Y COMPRIS CELLES QU'ON NE RELANCE PAS.
        #    C'est le cas qui compte : une bascule du reglage global arrive par `hot_config`
        #    sans modifier un seul `args`, donc sans relancer personne — et c'est ce fichier,
        #    relu une fois par seconde par le binaire, qui la transmet. Le poser seulement pour
        #    les connecteurs relances aurait rendu le reglage inoperant sur l'antenne, qui est
        #    justement celle qu'on ne relance jamais.
        _mode = str(cfg.get("missing_frame_mode_effective") or "repeat")
        for _k in plan:
            if _k[0] == "out":
                _ecrire_mode(_k[1], _mode)
        # Une prise qui CHANGE DE RÔLE change de clé : elle est arrêtée côté ancien rôle et
        # relancée côté nouveau — c'est voulu, et c'est l'ordre qui compte (cf. _rendre_prise).
        a_arreter = [k for k, t in _travailleurs.items()
                     if k not in plan or t["args"] != plan[k]["args"]]
        for k in a_arreter:
            _travailleurs[k]["stop"].set()
        for k in a_arreter:
            th = _travailleurs[k]["thread"]
            if th is not None:
                th.join(timeout=8)
            _travailleurs.pop(k, None)
        relances, gardes = [], []
        for k, p in plan.items():
            t = _travailleurs.get(k)
            if t is not None:
                gardes.append(k[1])
                with _verrou:
                    t["etat"].update(p["meta"])     # rang ou numéro de sortie, à jour
                t["flow"] = p["flow"]
                continue
            t = {{"args": p["args"], "stats": p["stats"], "flow": p["flow"],
                 "stop": threading.Event(), "thread": None,
                 "etat": _etat_neuf(p["role"], p["meta"])}}
            _travailleurs[k] = t
            relances.append(k[1])
        with _verrou:
            ENTREES, SORTIES, VOIES = entrees, sorties, voies
            _etats = [_travailleurs[("in", e["persistent_id"])]["etat"] for e in entrees]
            _etats_voie = [_travailleurs[("out", v["persistent_id"])]["etat"] for v in voies]
        _nettoyer_stats_perimees()
        for k, p in plan.items():
            t = _travailleurs[k]
            if t["thread"] is None:
                cible = _superviser if p["role"] == "in" else _superviser_sortie
                t["thread"] = threading.Thread(target=cible, args=(t,), daemon=True)
                t["thread"].start()
        return {{"ok": True, "relances": sorted(set(relances)),
                "arretes": sorted(set(k[1] for k in a_arreter) - set(relances)),
                "gardes": len(gardes)}}


def _format_detecte(e):
    """« 1080p50 », « 1080i50 », « 1280x720p60 »… tel que la CARTE l'a détecté, ou "" sans signal.

    ⚠ `fps` est la cadence CHAMP sur une source entrelacée (le chemin entrelacé compte ses deux
      champs dans la fenêtre) et la cadence TRAME sur une progressive. C'est précisément la
      convention des libellés de la profession — 1080i50 porte 50 champs, 1080p50 50 trames —
      donc l'arrondi direct est juste dans les deux cas. Le noter, parce que « corriger » cette
      asymétrie en doublant la cadence entrelacée donnerait 1080i100.
    """
    if not e.get("signal"):
        return ""
    w, h = int(e.get("width") or 0), int(e.get("height") or 0)
    if not (w and h):
        return ""
    hauteur = str(h) if h in (480, 576, 720, 1080, 2160, 4320) else "%dx%d" % (w, h)
    cadence = int(round(float(e.get("fps") or 0.0)))
    return "%s%s%s" % (hauteur, "i" if e.get("interlaced") else "p",
                       cadence if cadence else "")


# ── Avis « des images sont recopiées sur une sortie » ────────────────────────────────────
# ★★ POURQUOI UN AVIS EST OBLIGATOIRE ICI, et pas seulement un compteur. Le mode `repeat` est
#    choisi PRÉCISÉMENT parce qu'il passe inaperçu à l'antenne (décision de Cyril, 30/10). C'est
#    son intérêt et c'est son danger : sans avis, une chaîne qui perd des images donne une image
#    propre et plus personne ne le sait. Le compteur seul ne suffit pas — il faut que quelqu'un
#    le regarde, et personne ne regarde un compteur sain 23 heures sur 24.
#
# ⚠ LES PARAMÈTRES RESTENT GROSSIERS, ET C'EST VOULU. Le relais d'avis de l'orchestrateur
#   throttle sur (vmid, clé, PARAMÈTRES) pendant une heure : y mettre le compte exact ferait
#   varier la clé de throttle à chaque relevé, donc une ligne de journal par relevé. L'avis dit
#   QU'il se passe quelque chose ; `missing_frames_repeated` sur /state dit COMBIEN.
# ── Ce que la normalisation du profil a fait ──────────────────────────────────────────────
# Une bascule de profil RELÂCHE et REPREND la carte : tout signal qui passait est coupé le temps
# de l'opération. Quand elle a lieu malgré des prises vivantes (voir `_normaliser_carte`), le
# taire serait un échec silencieux de la pire espèce — une coupure dont personne ne connaît la
# cause. On le publie donc, sur :8080 en avis et dans /state en détail.
_NORMALISATION = None             # dict, ou None si rien n'a été tenté
_NORMALISATION_AVIS_S = 600.0     # au-delà, l'événement est passé : il reste dans /state


def _avis_prises_eteintes():
    """Des prises restent HORS de leur profil cible après la normalisation.

    ★ CET AVIS N'EXPIRE PAS, contrairement à celui de la bascule : il ne décrit pas un
      ÉVÉNEMENT (« la carte a été basculée, voilà ce que ça a coupé ») mais un ÉTAT qui dure
      — des connecteurs inutilisables. Un avis qui s'éteint sur un état persistant est
      exactement ce qui a permis au défaut du 2026-10-30 de passer : `ok: true` publié avec
      trois prises `inactive`, et rien pour contredire.
    """
    n = _NORMALISATION
    if not n or not n.get("restantes"):
        return None
    return {{"key": "plugin.decklink_io.advisory.profile_partially_normalised",
            "params": {{"model": n.get("model") or "?", "to": n.get("vers") or "?",
                       "left": len(n.get("restantes") or []),
                       "switches": len(n.get("bascules") or [])}}}}


def _avis_normalisation():
    """L'avis de bascule, tant que l'événement est FRAIS. Params GROSSIERS (un compte, jamais
    la liste des identifiants) : le relais limite à un avis par heure sur (vmid, clé, PARAMS),
    et des paramètres qui varient contourneraient cette limite."""
    n = _NORMALISATION
    if not n or not n.get("coupees"):
        return None
    if time.time() - float(n.get("t") or 0.0) > _NORMALISATION_AVIS_S:
        return None
    return {{"key": "plugin.decklink_io.advisory.profile_switched_live",
            "params": {{"model": n.get("model") or "?", "from": n.get("de") or "?",
                       "to": n.get("vers") or "?", "cut": len(n.get("coupees") or [])}}}}


_AVIS_SEUIL = 5          # recopies dans la fenêtre — même seuil que le mélangeur, par cohérence
_AVIS_FENETRE_S = 10.0
_avis_reperes = {{}}       # persistent_id → (t, compteur vu)


def _avis_sorties():
    """Avis à publier sur :8080, en LISTE (le relais en accepte plusieurs depuis le 30/10)."""
    avis = []
    maintenant = time.time()
    for e in _etats_voie:
        pid = e.get("persistent_id")
        n = e.get("missing_frames_repeated")
        if pid is None or not isinstance(n, int):
            continue
        t0, n0 = _avis_reperes.get(pid, (None, None))
        if t0 is None or n < n0:          # amorce, ou connecteur relancé (compteur reparti à 0)
            _avis_reperes[pid] = (maintenant, n)
            continue
        if maintenant - t0 < _AVIS_FENETRE_S:
            continue
        _avis_reperes[pid] = (maintenant, n)
        if n - n0 > _AVIS_SEUIL:
            avis.append({{"key": "plugin.decklink_io.advisory.missing_frames_repeated",
                         "params": {{"flow": e.get("flow") or "?",
                                    "mode": e.get("missing_frame_mode") or "repeat"}}}})
    return avis


def _vue_entrees_signal():
    """Ce que :8080 publie par ENTRÉE — le strict nécessaire pour qu'un lecteur sache POURQUOI
    un flux n'est plus écrit, sans ouvrir un terminal dans le conteneur.

    ★★ CE QUE SON ABSENCE A COÛTÉ (29/10, 17:35, Z8) : l'entrée 5 d'une carte a perdu son signal
       SDI et est restée muette des heures, pendant que les quatre autres tournaient à 50 fps.
       `:8080` ne publiait que la cadence AGRÉGÉE, `frame_index` et la version : le conteneur
       paraissait sain, et les lecteurs (pyramides, mur) ont accusé leur propre lecture. La
       panne était écrite noir sur blanc dans le JSON du binaire, que personne ne lit depuis
       l'extérieur.
    ★ `no_signal_s` porte l'anti-rebond du consommateur : sur un relevé isolé, rien ne distingue
      une micro-coupure de 200 ms d'une prise débranchée depuis la veille. Il vaut None quand le
      signal est là (et non 0, qui se lirait comme « vient de revenir ») et None aussi quand on
      ne sait pas depuis quand (amorce).
    """
    vue = []
    for e in _etats:
        sig = bool(e.get("signal"))
        t0 = float(e.get("signal_change_t") or 0.0)
        vue.append({{"flow": e.get("flow") or "",
                    "connector": e.get("numero"),
                    "persistent_id": e.get("persistent_id"),
                    "signal": sig,
                    "fps": round(float(e.get("fps") or 0.0), 2),
                    "format": _format_detecte(e),
                    "no_signal_s": None if (sig or not t0) else int(time.time() - t0)}})
    return vue


class Metriques(BaseHTTPRequestHandler):
    """Contrat :8080 de la flotte — {{fps, frame_index}}. La cadence AGRÉGÉE est celle de
    l'entrée la plus lente : une carte à quatre entrées dont une décroche ne doit pas afficher
    la moyenne, qui masquerait le décrochage."""

    def do_GET(self):
        with _verrou:
            # La cadence agrégée est celle du maillon le plus LENT — entrées ET sorties
            # confondues. Une moyenne masquerait le décrochage d'un seul connecteur.
            vivants = ([s["fps"] for s in _etats if s["mode"] in ("actif", "pas de signal")]
                       + [s["fps"] for s in _etats_voie if s["mode"].startswith("émet")])
            fps = min(vivants, default=0.0)
            idxs = [s["frame_index"] for s in _etats] + [s["frame_index"] for s in _etats_voie]
            # ★ Le contrat de flotte ne bouge pas, il s'ENRICHIT : `fps` et `frame_index`
            #   gardent exactement le sens qu'ils avaient (la surveillance les lit), et
            #   `inputs` dit ce que la cadence agrégée ne peut pas dire.
            entrees = _vue_entrees_signal()
            avis = _avis_sorties()
            for _f in (_avis_prises_eteintes, _avis_normalisation):
                _an = _f()
                if _an:
                    avis = [_an] + avis
        corps = json.dumps({{"fps": round(fps, 2),
                            "frame_index": max(idxs) if idxs else 0,
                            "plugin_version": PLUGIN_VERSION,
                            "inputs": entrees,
                            # LISTE : `metrics._relay_advisory` en relaie plusieurs depuis le
                            # 30/10, et une carte a plusieurs sorties. Absente quand il n'y a
                            # rien à dire — pas une liste vide publiée en permanence.
                            **({{"advisory": avis}} if avis else {{}})}}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(corps)))
        self.end_headers()
        self.wfile.write(corps)

    def log_message(self, *_a):
        pass


# ── Inventaire de la carte ────────────────────────────────────────────────────────────────
# ★ L'inventaire se lit ICI, dans le conteneur, et nulle part ailleurs : c'est lui qui a les
#   peripheriques montes, et le controleur n'execute jamais de code plugin. Sans cet endpoint,
#   l'exploitant devait trouver les identifiants en ligne de commande pour remplir un champ JSON
#   a la main — et il n'avait AUCUN moyen de voir qu'une prise etait INACTIVE, ce qui est le piege
#   le plus couteux de cette carte (une prise eteinte par un profil ne se voit sur aucun chassis).
_SONDE = "/usr/local/bin/decklink_probe"
_inv_cache = [0.0, None]        # (instant, resultat) — la sonde ouvre les peripheriques
_INV_TTL_S = 5.0


def _inventaire():
    """Sortie de la sonde, telle quelle, avec un cache court.

    ⚠ La sonde OUVRE les peripheriques pour lire leur etat. L'appeler a chaque rafraichissement
    d'une page la ferait entrer en concurrence avec nos propres captures : d'ou le cache.
    """
    maintenant = time.time()
    if _inv_cache[1] is not None and (maintenant - _inv_cache[0]) < _INV_TTL_S:
        return _inv_cache[1]
    try:
        r = subprocess.run([_SONDE], capture_output=True, timeout=20)
        out = (r.stdout or b"").decode("utf-8", "replace")
        res = json.loads(out) if out.strip() else {{"erreur": "sonde muette"}}
    except FileNotFoundError:
        res = {{"erreur": "sonde absente de l'image (decklink_probe)"}}
    except Exception as exc:
        res = {{"erreur": "sonde: {{}}".format(exc)}}
    _inv_cache[0], _inv_cache[1] = maintenant, res
    return res


_OUTIL_PROFIL = "/usr/local/bin/decklink_profile"
_verrou_profil = threading.Lock()


#: Code de sortie de `decklink_profile` quand il refuse parce qu'une entrée porte un signal
#: VERROUILLÉ. Un code, pas une phrase à relire : le texte de l'outil est en français et
#: changerait sous nos pieds (cf. la règle « l'interface traduit un CODE, jamais une phrase »).
_RC_SIGNAL_VIVANT = 4


def _basculer_profil(profil, pid, consentement=False):
    """Relâche la carte, bascule, puis la reprend. Rend la disposition RÉELLE observée après.

    L'ordre compte : tant que nos binaires tiennent la carte, l'outil refuse (à raison). On ne
    force donc pas d'emblée — on libère, on bascule, on relance. Si la bascule échoue, les
    binaires sont relancés quand même : laisser une carte sans capture parce qu'un réglage a raté
    serait remplacer un problème de configuration par une panne d'antenne.

    ★ LE REFUS « SIGNAL VIVANT » EST UNE QUESTION, PAS UN MUR (2026-10-19). L'outil refuse dès
      qu'une entrée porte un signal verrouillé — pas dès que quelqu'un la CAPTURE, et nous venons
      justement d'arrêter nos binaires. Résultat mesuré : avec un câble branché, le profil
      n'était plus réglable depuis la console, et il fallait `--force` en ligne de commande —
      exactement ce que la décision du 16/10 voulait supprimer. Un garde-fou qui interdit la
      fonction qu'il protège ne protège plus rien. On rend donc la main à l'exploitant : premier
      appel sans consentement → on remonte `confirmation_requise` + les prises concernées ;
      second appel avec `consentement` → on passe outre. Le refus silencieux disparaît, la
      protection reste (on ne coupe JAMAIS une source sans l'avoir dit).
    """
    if not _verrou_profil.acquire(blocking=False):
        return {{"ok": False, "code": "switch_in_progress"}}
    try:
        avant = _inventaire()
        _tuer_enfants()                      # on relâche la carte AVANT de parler de --force
        time.sleep(1.0)
        cmd = [_OUTIL_PROFIL, profil, "--device", str(pid)]
        if consentement:
            cmd.append("--force")
        rc = None
        try:
            r = subprocess.run(cmd, capture_output=True, timeout=90)
            sortie = ((r.stdout or b"") + (r.stderr or b"")).decode("utf-8", "replace")
            rc, ok = r.returncode, (r.returncode == 0)
        except FileNotFoundError:
            sortie, ok = "outil de profil absent de l'image", False
        except Exception as exc:
            sortie, ok = "bascule: {{}}".format(exc), False
        _inv_cache[1] = None                 # l'inventaire a changé : le cache ne vaut plus rien
        apres = _inventaire()
        reponse = {{"ok": ok, "journal": sortie[-2000:], "avant": avant, "apres": apres}}
        if not ok and rc == _RC_SIGNAL_VIVANT and not consentement:
            # Les prises qu'on s'apprête à couper, NOMMÉES : « une source va être coupée » sans
            # dire laquelle met l'exploitant devant un consentement qu'il ne peut pas évaluer.
            vivantes = [d.get("persistent_id") for d in (avant or {{}}).get("devices") or []
                        if ((d.get("signal") or {{}}).get("entree_verrouillee")
                            or (d.get("signal") or {{}}).get("mode_detecte"))]
            reponse.update({{"code": "live_input_signal", "confirmation_requise": True,
                             "prises_vivantes": vivantes}})
        elif not ok and rc is not None:
            reponse["code"] = "switch_failed"
        return reponse
    finally:
        # Les superviseurs relancent d'eux-mêmes leurs binaires : ils bouclent sur `_stop`.
        _verrou_profil.release()


class Controle(BaseHTTPRequestHandler):
    """:8082 — état détaillé, lisible par l'orchestrateur et par la page du plugin."""

    def _repondre(self, obj, code=200):
        corps = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(corps)))
        self.end_headers()
        self.wfile.write(corps)

    def do_POST(self):
        """POST /profile {{"profile": "<four-cc>", "persistent_id": <id>}} — bascule de profil.

        ★ POURQUOI C'EST LE SUPERVISEUR QUI LE FAIT, ET PAS L'ORCHESTRATEUR. L'outil doit tourner
          LÀ OÙ EST LA CARTE, et le contrôleur n'exécute jamais de code de plugin. C'est aussi lui
          qui tient nos binaires : il doit les ARRÊTER avant la bascule, sinon l'outil refuse —
          notre propre capture verrouille l'entrée, et le garde-fou se déclencherait contre nous.
          Passer `--force` pour contourner notre propre verrou serait une tricherie : on relâche
          la carte, on bascule, on la reprend.

        ⚠ Le rayon d'action annoncé par la carte SOUS-ESTIME (mesuré le 2026-09-21 : 1 annoncé,
          2 emportés). L'appelant doit avoir prévenu l'exploitant AVANT, et le résultat rend la
          disposition RÉELLE relevée après coup — c'est elle qui fait foi, pas la prévision.
        """
        route = self.path.split("?")[0]
        if route == "/config":
            self._reconfigurer_a_chaud()
            return
        if route != "/profile":
            self.send_response(404); self.end_headers(); return
        try:
            n = int(self.headers.get("Content-Length") or 0)
            corps = json.loads(self.rfile.read(n).decode() or "{{}}")
        except Exception as exc:
            self._repondre({{"ok": False, "erreur": "corps illisible: {{}}".format(exc)}}, 400); return
        profil = str(corps.get("profile") or "").strip()
        pid = corps.get("persistent_id")
        if len(profil) != 4 or not pid:
            self._repondre({{"ok": False, "erreur": "profile (4 caractères) et persistent_id requis"}}, 400)
            return
        # `consentement` n'est JAMAIS déduit : il doit être posté explicitement, et il ne vaut
        # que pour cet appel-ci. Un drapeau mémorisé ferait d'un accord ponctuel une permission
        # permanente, ce qui est précisément la faute que la confirmation veut éviter.
        self._repondre(_basculer_profil(profil, int(pid),
                                        consentement=bool(corps.get("consentement"))))

    def _reconfigurer_a_chaud(self):
        """POST /config {{plugin_version, hostname, config}} — nouvelle configuration, À CHAUD.

        Appelé par l'orchestrateur à la place d'un arrêt/démarrage (`control.hot_config`). Le
        fichier du script vient d'être réécrit avec la MÊME configuration : un redémarrage
        ultérieur repartira donc sur ce qui est appliqué ici.

        ⚠ ON REFUSE PLUTÔT QUE D'APPLIQUER À MOITIÉ. Une autre version de plugin, c'est un autre
          CODE que celui qui tourne ; un autre nom d'hôte, ce sont d'autres noms de flux par
          défaut. Ni l'un ni l'autre ne se change à chaud : on répond 409, et l'orchestrateur
          redémarre le script comme avant."""
        try:
            n = int(self.headers.get("Content-Length") or 0)
            corps = json.loads(self.rfile.read(n).decode() or "{{}}")
        except Exception as exc:
            self._repondre({{"ok": False, "code": "unreadable_body", "detail": str(exc)}}, 400)
            return
        if str(corps.get("plugin_version") or "") != PLUGIN_VERSION:
            self._repondre({{"ok": False, "code": "version_mismatch",
                            "running": PLUGIN_VERSION}}, 409)
            return
        if str(corps.get("hostname") or "") != HOSTNAME:
            self._repondre({{"ok": False, "code": "hostname_mismatch"}}, 409)
            return
        cfg = corps.get("config")
        if not isinstance(cfg, dict):
            self._repondre({{"ok": False, "code": "config_missing"}}, 400)
            return
        # Une bascule de profil relâche et reprend toute la carte : on ne mêle pas les deux.
        if not _verrou_profil.acquire(blocking=False):
            self._repondre({{"ok": False, "code": "switch_in_progress"}}, 409)
            return
        try:
            bilan = _reconfigurer(cfg)
        except Exception as exc:
            self._repondre({{"ok": False, "code": "reconfigure_failed", "detail": str(exc)}}, 500)
            return
        finally:
            _verrou_profil.release()
        print("decklink_io: live reconfiguration — restarted %s, stopped %s, kept %d"
              % (bilan["relances"], bilan["arretes"], bilan["gardes"]), flush=True)
        self._repondre(bilan)

    def do_GET(self):
        route = self.path.split("?")[0]
        if route == "/inventory":
            self._repondre(_inventaire())
            return
        if route != "/state":
            self.send_response(404)
            self.end_headers()
            return
        with _verrou:
            corps = json.dumps({{
                "hostname": HOSTNAME,
                "plugin_version": PLUGIN_VERSION,
                "inputs": list(_etats),
                "outputs": _vue_sorties(),
                "voies": list(_etats_voie),
                "entrees_refusees": list(_refusees),
                # Ce que la bascule de profil a fait au démarrage, y compris les prises qu'elle
                # a coupées. Reste lisible toute la vie du conteneur : c'est la réponse à
                # « pourquoi mon signal a-t-il sauté au démarrage ».
                "normalisation": _NORMALISATION,
            }}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(corps)))
        self.end_headers()
        self.wfile.write(corps)

    def log_message(self, *_a):
        pass


# ── Enfants : les terminer ET les faucher ─────────────────────────────────────────────────
# DEFAUT MESURE LE 2026-09-09 : treize enfants ZOMBIES accumules (onze decklink_rx, deux
#   decklink_tx) pour un seul processus vivant. Deux causes qui se cumulent : les fils
#   superviseurs sont des threads DEMONS, donc a l'arret du processus ils sont supprimes d'office
#   et leur `terminate()` n'a aucune chance de s'executer ; et sur le chemin d'arret normal, aucun
#   `wait()` ne suivait le `terminate()` — or c'est `wait()` qui fauche.
#
# ATTENTION A NE PAS SE TROMPER DE CAUSE : un zombie ne tient AUCUNE ressource. Ce n'est donc pas
#   lui qui rendait une prise de la carte indisponible, contrairement a ce que j'ai d'abord conclu.
#   Ce qui la rendait indisponible, c'est que l'ORCHESTRATEUR relance ce script 4 s apres un arret
#   — mesure. Liberer le materiel demande une intention PERSISTEE, pas un POST /stop.
_enfants = set()
_verrou_enfants = threading.Lock()


def _lancer(args):
    """Demarre un enfant ET l'enregistre. Tout lancement passe par ici."""
    proc = subprocess.Popen(args)
    with _verrou_enfants:
        _enfants.add(proc)
    return proc


def _tuer_enfants(delai=3.0):
    """SIGTERM a tous les enfants, puis SIGKILL a ceux qui trainent. Appele par le fil PRINCIPAL,
    jamais par un thread demon : c'est le seul qui survive assez longtemps pour le faire."""
    with _verrou_enfants:
        procs = list(_enfants)
    for pr in procs:
        try:
            pr.terminate()
        except Exception:
            pass
    fin = time.time() + delai
    for pr in procs:
        try:
            pr.wait(timeout=max(0.1, fin - time.time()))
        except Exception:
            try:
                pr.kill()          # le binaire n'a pas rendu la main : on ne negocie plus
            except Exception:
                pass
    with _verrou_enfants:
        _enfants.clear()


def _nettoyer_orphelins():
    """Tue les binaires de ce plugin restes d'une execution PRECEDENTE. Sans ce menage, un
    orphelin garde sa prise et le demarrage echoue de facon incomprehensible — la panne se
    manifeste sur la prise, pas sur le processus."""
    moi = str(os.getpid())
    # ⚠ L'ANCIEN NOM RESTE DANS CETTE LISTE (renommage decklink_mxl → decklink_rx, 0.29.51).
    #   Cette fonction fauche les binaires d'une execution PRECEDENTE : au premier demarrage
    #   apres la montee de version, l'orphelin qui tient une prise porte encore l'ANCIEN nom,
    #   et c'est precisement le cas que cette fonction existe pour eviter — sinon le demarrage
    #   echoue sur la prise, pas sur le processus. A retirer quand plus aucun conteneur ne
    #   tourne sur une image anterieure a bobi-decklink:0.2.0.
    noms = ("decklink_rx", "decklink_mxl", "decklink_tx")
    for pid in os.listdir("/proc"):
        if not pid.isdigit() or pid == moi:
            continue
        try:
            with open("/proc/" + pid + "/comm") as f:
                nom = f.read().strip()
        except Exception:
            continue
        if nom in noms:
            try:
                os.kill(int(pid), signal.SIGKILL)
                print("decklink_io: orphelin " + nom + " (pid " + pid + ") supprime", flush=True)
            except Exception:
                pass


def _arret(*_a):
    _stop.set()


signal.signal(signal.SIGTERM, _arret)
signal.signal(signal.SIGINT, _arret)

threading.Thread(target=lambda: HTTPServer(("0.0.0.0", 8080), Metriques).serve_forever(),
                 daemon=True).start()
threading.Thread(target=lambda: HTTPServer(("0.0.0.0", 8082), Controle).serve_forever(),
                 daemon=True).start()



def _nettoyer_stats_perimees():
    """Supprime les fichiers de statistiques d'une configuration PRÉCÉDENTE.

    ⚠ ILS MENTENT DE LA PIRE FAÇON : ils contiennent des chiffres plausibles pour des voies qui
    n'existent plus. Relevé le 2026-10-10 : `/tmp/decklink_tx_2.json` annonçait une file de 1 et
    307 trames perdues sur 308, vestige d'un essai de huit jours plus tôt — j'ai cru à une sortie
    en panne avant de voir qu'aucun de ses compteurs ne bougeait. Deux fois que ça me coûte un
    détour. Un fichier périmé n'est pas une donnée ancienne, c'est une donnée FAUSSE.
    """
    gardes = {{t["stats"] for t in _travailleurs.values()}}
    for _nom in os.listdir("/tmp"):
        _ch = "/tmp/" + _nom
        if not (_nom.startswith("decklink_") and _nom.endswith(".json")):
            continue
        if _ch in gardes:
            continue
        try:
            os.remove(_ch)
            print("decklink_io: statistiques périmées supprimées : " + _ch, flush=True)
        except OSError:
            pass


def _profil_cible(sub):
    """Le profil half-duplex au plus grand chiffre parmi ceux offerts, ou None.

    ★ LA RÈGLE SE CALCULE, ELLE NE SE CODE PAS PAR MODÈLE (décidé le 19/10). On ne veut jamais
      entrer ET sortir sur la même prise : chaque connecteur doit être affectable
      indépendamment, dans un seul sens. Il n'existe PAS de `4dfd` — le maximum de prises
      indépendantes vient donc toujours d'un profil half-duplex, et entre deux half-duplex
      c'est le plus grand chiffre qui en donne le plus. Aucune branche par modèle de carte.

    ⚠ LE CHIFFRE DU FOUR-CC NE COMPTE PAS LES CONNECTEURS. Mesuré sur la Duo 2 réelle le
      20/10 : profil ACTIF `2dhd`, et la carte expose **quatre** prises half-duplex, toutes
      affectables — quatre étaient en service la veille. Le chiffre compte des groupes de
      sous-périphériques, pas des prises. J'avais écrit ici l'inverse, et j'en avais tiré pour
      la Quad 2 une prédiction fausse (« 8 prises ne peuvent venir d'aucun profil connu »)
      alors que la carte sous la main la réfutait déjà. Par la même mesure, une Quad 2 en
      `4dhd` donnerait vraisemblablement 8 prises.
      Ce que cela ne change PAS : l'ordonnancement. Plus de groupes = plus de prises, donc
      prendre le plus grand chiffre half-duplex reste la bonne décision — c'est l'explication
      qui était fausse, pas le choix. Cette carte n'offre d'ailleurs que `1dfd` et `2dhd`.
    """
    meilleur, n_max = None, -1
    for p in (sub.get("profils") or []):
        m = re.match(r"^(\d)d(fd|hd)$", str(p.get("id") or ""))
        if not m or m.group(2) != "hd":
            continue
        if int(m.group(1)) > n_max:
            meilleur, n_max = p.get("id"), int(m.group(1))
    return meilleur


def _normaliser_carte():
    """Met la carte en « toutes les prises indépendantes », AU DÉMARRAGE ET SANS CONDITION.

    ★ POURQUOI AUTOMATIQUE. Une carte qui a servi ailleurs arrive avec le profil qu'on y a
      laissé ; le profil vit dans la carte, pas dans notre base. La normaliser d'office évite
      que l'exploitant ait à connaître le sujet.

    ★★ POURQUOI SANS CONDITION, ET CE QUE ÇA COÛTE (arbitrage de Cyril, 2026-10-30). Jusqu'ici
       la bascule était abandonnée dès qu'UNE SEULE prise portait un signal, et le journal
       disait « switch left to the operator ». Mais la bascule manuelle avait été retirée de la
       console le 16/10, à sa demande : l'opérateur à qui on laissait le geste n'avait plus
       aucun moyen de le faire. Le résultat mesuré est exactement celui qu'il a constaté sur sa
       Quad 2 — des prises ÉTEINTES par le profil, que rien ne pouvait réveiller sans
       débrancher le seul câble vivant de la carte. Un câble branché bloquait les sept autres
       prises. Le ratio décide : une coupure brève au démarrage d'un conteneur contre des
       connecteurs inutilisables en permanence.
    ⚠ LE PRIX EST RÉEL ET IL EST ASSUMÉ : une bascule relâche et reprend la carte, donc tout
      signal qui passait est COUPÉ le temps de l'opération. Démarrer ce conteneur sur une carte
      qui diffuse coupe ce qu'elle diffuse. Ce n'est acceptable que parce que ça ne se produit
      qu'au démarrage, jamais à chaud, et que ce n'est JAMAIS MUET : les prises coupées sont
      nommées dans le journal, publiées dans /state et annoncées en avis sur :8080.
    ⚠ LE RELEVÉ SE PREND AVANT DE RELÂCHER LA CARTE. Mesuré le 19/10 : une fois nos binaires
      arrêtés, la carte ne rapporte PLUS les entrées comme verrouillées, même avec deux sources
      branchées et détectées. Le relevé ne conditionne plus la décision, mais il est toujours
      la seule occasion de savoir CE QU'ON COUPE — et c'est maintenant sa seule raison d'être.
    """
    try:
        inv = _inventaire()
    except Exception as exc:
        print("decklink_io: inventaire illisible, pas de normalisation (%s)" % exc, flush=True)
        return
    subs = (inv or {{}}).get("devices") or []
    if not subs:
        return
    # Une carte MONO-PROFIL n'offre aucune cible : il n'y a rien à basculer, et c'est normal.
    # On le DIT, au lieu de ne rien écrire. Le silence se lit comme « la normalisation a été
    # tentée et n'a rien trouvé à faire », ce qui n'est pas la même information que « ce
    # matériel n'a pas de profil du tout » — et c'est la première question que pose un
    # installateur devant une carte dont les prises ne se comportent pas comme attendu.
    if not any(_profil_cible(d) for d in subs):
        print("decklink_io: single-profile card (%s), nothing to normalise: the %d connector(s) "
              "are as the card exposes them" % (subs[0].get("model") or "?", len(subs)),
              flush=True)
        return
    global _NORMALISATION
    # ★★ UNE BASCULE NE NORMALISE PAS LA CARTE ENTIÈRE, et c'est ce que je supposais (0.29.44,
    #    corrigé en 0.29.45). Mesuré par la session Z8 sur la Quad 2 de #228 le 2026-10-30 à
    #    15:38 : `decklink_profile` a bien posé `2dhd`, mais sur UN SEUL groupe de
    #    sous-périphériques. Avant, les huit prises étaient en `1dfd` dont quatre `inactive` ;
    #    après, DEUX prises étaient passées en `2dhd`/half et TROIS restaient `inactive`. Le
    #    profil se règle par GROUPE (sur cette carte, des paires que les `topological_id`
    #    consécutifs laissent voir), pas par carte — et mon `return` après la première bascule
    #    déclarait le travail fini. Pire : il publiait `ok: true`.
    # ★ LA CORRECTION NE DEVINE PAS LE GROUPEMENT : elle MESURE. On bascule, on relit
    #   l'inventaire que la bascule rend déjà (`apres` — elle invalide le cache de la sonde), et
    #   on recommence tant qu'un sous-périphérique n'est pas dans son profil cible. Une carte
    #   dont une seule bascule suffit ne fait qu'un tour ; une Quad 2 en fait quatre. Aucune
    #   hypothèse sur le modèle, aucune arithmétique sur les index — c'est la règle du chantier,
    #   et c'est aussi la seule forme qui survivra à une carte qu'on n'a jamais vue.
    # ⚠ LE PROGRÈS EST EXIGÉ À CHAQUE TOUR. Sans ça, une carte qui accepte la commande sans rien
    #   changer serait rebasculée en rond — et chaque tour relâche la carte, donc coupe l'antenne.
    #   On s'arrête au premier tour qui ne réduit pas le reste à faire, et on le DIT.
    coupees, bascules, echecs = [], [], []

    def _a_faire(ds):
        return [x for x in ds
                if _profil_cible(x)
                and (x.get("profil_actif") or {{}}).get("id") != _profil_cible(x)]

    def _vivantes(ds):
        return [x.get("persistent_id") for x in ds
                if (x.get("signal") or {{}}).get("entree_verrouillee")
                or (x.get("signal") or {{}}).get("mode_detecte")]

    modele = subs[0].get("model") or "?"
    de_premier, vers_premier = None, None
    for _tour in range(len(subs) + 1):
        reste = _a_faire(subs)
        if not reste:
            break
        d = reste[0]
        cible = _profil_cible(d)
        actuel = (d.get("profil_actif") or {{}}).get("id")
        if de_premier is None:
            de_premier, vers_premier = actuel, cible
        # Les prises vivantes sont relues À CHAQUE TOUR, juste avant de couper : au premier tour
        # c'est le relevé pris avant tout arrêt (cf. la mesure du 19/10), aux suivants c'est
        # l'inventaire que la bascule précédente a rendu. Une liste prise une seule fois au
        # début nommerait comme « coupées » des prises qu'on n'a pas touchées — c'est ce que
        # /state a affirmé à tort le 30/10 (4037714289, restée en 1dfd).
        vivantes = _vivantes(subs)
        # La bascule a lieu MÊME si des prises portent un signal — mais on dit lesquelles, et on
        # le dit AVANT de couper, pour que la trace existe si la bascule tourne mal.
        if vivantes:
            print("decklink_io: normalising group of %s %s -> %s ANYWAY: %d connector(s) carry "
                  "a signal and will be cut for the duration of the switch (operator decision "
                  "2026-10-30; without it the connectors the profile keeps off cannot be "
                  "woken at all). Connectors carrying a signal: %s"
                  % (d.get("persistent_id"), actuel, cible, len(vivantes),
                     ", ".join(str(x) for x in vivantes)), flush=True)
        else:
            print("decklink_io: normalising group of %s %s -> %s (no signal on any connector)"
                  % (d.get("persistent_id"), actuel, cible), flush=True)
        r = _basculer_profil(cible, int(d.get("persistent_id") or 0), consentement=True)
        bascules.append({{"pid": d.get("persistent_id"), "de": actuel, "vers": cible,
                         "ok": bool(r.get("ok")), "code": r.get("code")}})
        # ★★ CE QU'EST UNE PRISE COUPÉE, EN TROIS ESSAIS — les deux premiers étaient faux et
        #    chacun l'était d'une façon instructive.
        #    (1) « Tout ce qui était vivant sur la carte » (0.29.44) : faux, parce qu'une
        #        bascule ne touche qu'UN groupe. Il a nommé 4037714289 alors que son groupe
        #        n'avait pas bougé — une prise qui n'avait rien subi, donnée pour victime.
        #    (2) « Vivante avant, plus vivante après » (0.29.45) : faux aussi, et plus
        #        subtilement — LA MESURE EST PRISE TROP TÔT. Mesuré par la session Z8 le
        #        2026-10-30 : à 15:50, 40 s après le démarrage, UNE seule prise sur huit était
        #        verrouillée ; à 15:51:06, QUATRE l'étaient. La carte met des dizaines de
        #        secondes à reprendre ses verrous après avoir été relâchée. Lire juste après la
        #        bascule, c'est donc déclarer coupé tout ce qui n'a pas encore eu le temps de
        #        revenir.
        #    (3) CE QUI EST VRAI ET MESURABLE : une prise est coupée si elle portait un signal
        #        AVANT et si son PROFIL a changé — le changement de profil est l'événement qui
        #        relâche la carte, et il se lit dans les deux inventaires que `_basculer_profil`
        #        rend déjà. Ça désigne exactement le groupe réellement basculé, sans inférer le
        #        groupement, et c'est INSENSIBLE au délai de reprise des verrous.
        d_av = {{x.get("persistent_id"): (x.get("profil_actif") or {{}}).get("id")
                for x in (r.get("avant") or {{}}).get("devices") or []}}
        d_ap = {{x.get("persistent_id"): (x.get("profil_actif") or {{}}).get("id")
                for x in (r.get("apres") or {{}}).get("devices") or []}}
        touchees = {{k for k, v in d_av.items() if k in d_ap and d_ap[k] != v}}
        for x in sorted(set(_vivantes((r.get("avant") or {{}}).get("devices") or [])) & touchees):
            if x not in coupees:
                coupees.append(x)
        if not r.get("ok"):
            echecs.append(r.get("code") or "switch_failed")
            print("decklink_io: normalisation failed on group of %s (%s)"
                  % (d.get("persistent_id"), r.get("code")), flush=True)
            break
        suivant = ((r.get("apres") or {{}}).get("devices")) or []
        if not suivant:
            echecs.append("inventory_unreadable_after_switch")
            print("decklink_io: inventory unreadable after the switch, stopping", flush=True)
            break
        if len(_a_faire(suivant)) >= len(reste):
            # La carte a accepté la commande et rien n'a changé. Continuer, c'est relâcher la
            # carte à chaque tour pour rien — donc couper l'antenne en boucle.
            subs = suivant
            echecs.append("no_progress")
            print("decklink_io: switch reported success but %d sub-device(s) are still off "
                  "target — stopping instead of switching in circles"
                  % len(_a_faire(suivant)), flush=True)
            break
        subs = suivant
    if not bascules:
        return
    restantes = [x.get("persistent_id") for x in _a_faire(subs)]
    _NORMALISATION = {{"t": time.time(), "model": modele,
                      "de": de_premier, "vers": vers_premier, "coupees": coupees,
                      "bascules": bascules, "restantes": restantes,
                      # ★ `ok` NE DIT PLUS « la dernière commande a réussi » mais « la carte
                      #   EST normalisée », et il se lit sur l'ÉTAT MESURÉ, pas sur le code de
                      #   retour. C'est la différence entre les deux qui a rendu le défaut du
                      #   30/10 invisible : trois prises éteintes sous un `ok: true`. Une seule
                      #   clause, et elle est celle qui compte — `echecs` dit POURQUOI, dans
                      #   `code`, et n'a pas à trancher le verdict.
                      "ok": not restantes,
                      "code": echecs[0] if echecs else None}}
    if restantes:
        print("decklink_io: %d sub-device(s) are STILL not in the target profile after %d "
              "switch(es): %s" % (len(restantes), len(bascules),
                                  ", ".join(str(x) for x in restantes)), flush=True)
    else:
        print("decklink_io: card normalised in %d switch(es), every connector is independent"
              % len(bascules), flush=True)


_nettoyer_orphelins()
_normaliser_carte()

# Le démarrage passe par le MÊME chemin que la reconfiguration à chaud : partant de rien, tout
# est « nouveau » et tout est lancé.
_reconfigurer(CONFIG)

if not ENTREES and not SORTIES:
    # Aucune entrée déclarée : on le DIT et on reste en vie pour que l'état soit interrogeable.
    # Sortir en silence laisserait un conteneur mort dont personne ne saurait pourquoi.
    print("decklink_io: aucune entrée NI sortie déclarée dans la configuration", flush=True)

while not _stop.is_set():
    time.sleep(1)

# Le fil principal, et lui seul, peut faire ce menage : les superviseurs sont des demons.
_tuer_enfants()
