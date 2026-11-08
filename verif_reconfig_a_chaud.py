#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Banc : la reconfiguration À CHAUD ne relance QUE les connecteurs modifiés (0.29.23, 21/10).

Avant : chaque réglage ou câble redéployait le conteneur, donc relançait les douze binaires des
deux cartes — un câble audio sur une sortie de la Quad coupait aussi la Duo.

Le banc exécute le VRAI script rendu (`app.plugins.render_script`), avec de FAUX binaires de
capture et de sortie (ils écrivent leurs statistiques comme les vrais, journalisent leurs
démarrages et arrêts, et mettent 0,5 s à rendre la prise au signal d'arrêt — sans ce délai, un
superviseur qui n'attend PAS la fin de l'ancien binaire passait le contrôle 2c par chance). Il tourne dans un espace réseau et un /tmp PRIVÉS (`unshare`), pour ne
toucher ni aux ports :8080/:8082 ni aux fichiers de la machine. Il vérifie, par PID :

  1. un changement sur UNE sortie ne relance qu'elle ; les autres gardent leur processus ET leurs
     compteurs ;
  2. une prise qui passe de sortie à entrée : l'ancien binaire a RENDU la prise avant que le
     nouveau la rouvre (sinon l'ouverture échouerait sur la vraie carte) ;
  3. ajouter une sortie DEVANT les autres ne relance pas les autres (fichiers nommés par prise) ;
  4. une autre version de plugin ou un autre nom d'hôte : refus 409, rien ne bouge ;
  5. /state reste cohérent (numéros de sortie, voies).

Lancement : `sudo ./venv/bin/python plugins/decklink_io/verif_reconfig_a_chaud.py` (root : unshare).
"""
import json
import os
import subprocess
import sys
import time
import urllib.request

RACINE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

if "--dedans" not in sys.argv:
    if os.geteuid() != 0:
        print("SKIP : ce banc a besoin de root (unshare réseau + /tmp privé)")
        sys.exit(0)
    cmd = ("ip link set lo up && mount -t tmpfs banc /tmp && exec %s %s --dedans"
           % (sys.executable, os.path.abspath(__file__)))
    sys.exit(subprocess.call(["unshare", "-n", "-m", "--propagation", "private",
                              "sh", "-c", cmd]))

sys.path.insert(0, RACINE)
from app import plugins  # noqa: E402

D = "/tmp/banc"
os.makedirs(D, exist_ok=True)
JOURNAL = D + "/journal"
FAUX = '''import json, os, signal, sys, time
args = sys.argv[1:]
stats = args[args.index("--stats") + 1]
role, pid = sys.argv[0].rsplit("/", 1)[1], args[0]
with open("%s", "a") as j:
    j.write("%%s start %%s %%d %%.6f %%s\\n" %% (role, pid, os.getpid(), time.time(), " ".join(args)))
def fin(*_a):
    time.sleep(0.5)          # une vraie carte met du temps à rendre la prise : on l'imite
    with open("%s", "a") as j:
        j.write("%%s end %%s %%d %%.6f\\n" %% (role, pid, os.getpid(), time.time()))
    sys.exit(0)
signal.signal(signal.SIGTERM, fin)
n = 0
while True:
    n += 1
    tmp = stats + ".tmp"
    with open(tmp, "w") as f:
        json.dump({"fps": 50.0, "frame_index": n, "signal": True, "source_alive": True}, f)
    os.replace(tmp, stats)
    time.sleep(0.05)
''' % (JOURNAL, JOURNAL)
for nom in ("faux_rx", "faux_tx"):
    with open(D + "/" + nom, "w") as f:
        f.write("#!" + sys.executable + "\n" + FAUX)
    os.chmod(D + "/" + nom, 0o755)

VERSION = plugins.get("decklink_io")["version"]
HOTE = "decklink-banc"


def config(outputs, inputs):
    return {"hostname": HOTE, "binary": D + "/faux_rx", "binary_tx": D + "/faux_tx",
            "inputs": inputs, "outputs": outputs}


def sortie(pid, **kw):
    o = {"persistent_id": pid, "flow": "player", "mode": "1080p50"}
    o.update(kw)
    return o


CFG0 = config([sortie(2), sortie(3)], [{"persistent_id": 1, "flow": "in1"}])
script = plugins.render_script("decklink_io", CFG0, HOTE)
with open(D + "/main.py", "w") as f:
    f.write(script)
proc = subprocess.Popen([sys.executable, D + "/main.py"], stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL)

fails = []


def check(cond, what):
    print(("OK   " if cond else "FAIL ") + what)
    if not cond:
        fails.append(what)


def http(methode, chemin, corps=None):
    req = urllib.request.Request("http://127.0.0.1:8082" + chemin, method=methode,
                                 data=json.dumps(corps).encode() if corps is not None else None,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read().decode() or "{}")


def vivants():
    """(role, prise) -> PID des faux binaires vivants, d'après le journal."""
    v = {}
    for ligne in open(JOURNAL).read().splitlines():
        r, quoi, prise, pid = ligne.split()[:4]
        if quoi == "start":
            v[(r, prise)] = int(pid)
        elif v.get((r, prise)) == int(pid):
            del v[(r, prise)]
    return v


def attendre(cond, s=10):
    fin = time.time() + s
    while time.time() < fin:
        try:
            if cond():
                return True
        except Exception:
            pass
        time.sleep(0.1)
    return False


def pousser(cfg, version=VERSION, hote=HOTE):
    return http("POST", "/config", {"plugin_version": version, "hostname": hote,
                                    "config": plugins.render_config("decklink_io", cfg)})


try:
    check(attendre(lambda: len(vivants()) == 3), "démarrage : 1 entrée + 2 sorties lancées")
    avant = vivants()
    attendre(lambda: all(o.get("frame_index", 0) > 5 for o in http("GET", "/state")[1]["outputs"]))
    fi2 = [o for o in http("GET", "/state")[1]["outputs"] if o["persistent_id"] == 2][0]["frame_index"]

    # 1. Audio posé sur la sortie 3 seulement.
    st, b = pousser(config([sortie(2), sortie(3, audio_flow="player_audio", audio_channels=8)],
                           [{"persistent_id": 1, "flow": "in1"}]))
    check(st == 200 and b.get("relances") == [3], "1. seul le connecteur 3 est relancé — %s %s" % (st, b))
    attendre(lambda: vivants().get(("faux_tx", "3")) not in (None, avant[("faux_tx", "3")]))
    apres = vivants()
    check(apres.get(("faux_rx", "1")) == avant[("faux_rx", "1")]
          and apres.get(("faux_tx", "2")) == avant[("faux_tx", "2")],
          "1b. l'entrée 1 et la sortie 2 gardent LEUR processus")
    check(apres.get(("faux_tx", "3")) not in (None, avant[("faux_tx", "3")]),
          "1c. la sortie 3 tourne sur un NOUVEAU processus")
    # Le superviseur relit les statistiques toutes les 0,5 s : on attend qu'il l'ait fait, sinon
    # on compare deux fois la même lecture (première version de ce contrôle, fausse alerte).
    def _s2():
        return [o for o in http("GET", "/state")[1]["outputs"] if o["persistent_id"] == 2][0]
    attendre(lambda: _s2()["frame_index"] > fi2, 5)
    s2 = _s2()
    check(s2["frame_index"] > fi2 and s2.get("redemarrages", 0) == 0,
          "1d. la sortie 2 garde ses compteurs (%d > %d, 0 redémarrage)" % (s2["frame_index"], fi2))
    jl = open(JOURNAL).read()
    check("--audio player_audio 8" in jl, "1e. la nouvelle ligne de commande porte bien l'audio")

    # 2. La prise 3 passe de SORTIE à ENTRÉE.
    st, b = pousser(config([sortie(2)], [{"persistent_id": 1, "flow": "in1"},
                                          {"persistent_id": 3, "flow": "in3"}]))
    check(st == 200 and 3 in b.get("relances", []), "2. prise 3 basculée en entrée — %s %s" % (st, b))
    check(attendre(lambda: ("faux_rx", "3") in vivants() and ("faux_tx", "3") not in vivants()),
          "2b. plus de binaire de SORTIE sur la prise 3, un binaire d'ENTRÉE à la place")
    lignes = [x.split() for x in open(JOURNAL).read().splitlines()]
    fin_tx3 = max(float(x[4]) for x in lignes if x[0] == "faux_tx" and x[1] == "end" and x[2] == "3")
    debut_rx3 = min(float(x[4]) for x in lignes if x[0] == "faux_rx" and x[1] == "start" and x[2] == "3")
    check(debut_rx3 > fin_tx3,
          "2c. la prise est RENDUE avant d'être rouverte (fin sortie %.3f < début entrée %.3f)"
          % (fin_tx3, debut_rx3))
    check(vivants().get(("faux_tx", "2")) == avant[("faux_tx", "2")],
          "2d. la sortie 2, elle, n'a toujours pas bougé")

    # 3. Une sortie ajoutée DEVANT les autres.
    avant3 = vivants()
    st, b = pousser(config([sortie(9), sortie(2)], [{"persistent_id": 1, "flow": "in1"},
                                                     {"persistent_id": 3, "flow": "in3"}]))
    check(st == 200 and b.get("relances") == [9], "3. ajout devant : seule la nouvelle part — %s" % b)
    attendre(lambda: ("faux_tx", "9") in vivants())
    check(all(vivants().get(k) == v for k, v in avant3.items()),
          "3b. les rangs ont changé, aucun processus existant n'a été relancé")

    # 4. Refus.
    v4 = vivants()
    st, b = pousser(config([sortie(2)], []), version="0.0.0")
    check(st == 409 and b.get("code") == "version_mismatch", "4. autre version : 409 version_mismatch")
    st, b = pousser(config([sortie(2)], []), hote="autre")
    check(st == 409 and b.get("code") == "hostname_mismatch", "4b. autre nom d'hôte : 409")
    time.sleep(0.5)
    check(vivants() == v4, "4c. après refus, rien n'a bougé")

    # 5. /state cohérent.
    st, etat = http("GET", "/state")
    outs = [(o["numero"], o["persistent_id"]) for o in etat["outputs"]]
    check(outs == [(1, 9), (2, 2)], "5. /state : sorties numérotées dans le nouvel ordre — %s" % outs)
    check([i["persistent_id"] for i in etat["inputs"]] == [1, 3], "5b. /state : entrées 1 et 3")
    check([(v["sortie"], v["persistent_id"]) for v in etat["voies"]] == [(1, 9), (2, 2)],
          "5c. /state : voies rattachées à leur sortie")
finally:
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except Exception:
        proc.kill()

print("\n%d échec(s)" % len(fails))
sys.exit(1 if fails else 0)
