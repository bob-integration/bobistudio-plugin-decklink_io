# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 BOBI SAS, France
# Auteur : Cyril Mazouer, pour le compte de BOBI SAS
# Distribué sous licence GNU GPL v3 (ou ultérieure) ; voir le fichier LICENSE.
#
# decklink_io — hook de déploiement.
#
# ⚠ CE QUE CE HOOK NE PEUT PAS FAIRE, et il ne faut pas s'y tromper : il ne peut pas REFUSER un
#   déploiement. `deploy.py` avale les exceptions du hook et poursuit (« un hook qui lève une
#   exception est ignoré ; le deploy continue »). Il ne reçoit pas non plus le NŒUD, donc il ne
#   peut pas interroger la carte. Toute validation qui exige le matériel appartient au
#   superviseur, dans le conteneur — c'est lui qui voit les périphériques.
#
#   D'où le partage : ici on NORMALISE (ce qui ne demande que les params) ; le superviseur
#   DIAGNOSTIQUE (ce qui demande la carte) et publie son verdict sur :8082.

from app.numerotation import flux_video
import json


def _entier(v):
    """Un persistent id peut arriver en entier, en chaîne décimale ou en hexa (« 0xF0D8AA03 ») :
    la sonde l'affiche en décimal, mais un exploitant peut recopier ce que dit `dmesg`."""
    if isinstance(v, bool):
        return 0
    if isinstance(v, int):
        return v
    s = str(v or "").strip()
    if not s:
        return 0
    try:
        return int(s, 16) if s.lower().startswith("0x") else int(s, 10)
    except ValueError:
        return 0


def before_deploy(params, ctx):
    """Normalise `inputs` avant le rendu du script.

    Ce que ça évite concrètement : un `inputs` mal typé produit un conteneur qui démarre, ne
    déclare aucune entrée et n'explique rien — la panne la plus coûteuse à diagnostiquer parce
    qu'elle ne ressemble pas à une panne.
    """
    hostname = (ctx or {}).get("hostname") or params.get("hostname") or "decklink"

    brut = params.get("inputs") or []
    # `coerce_config` ne connaît pas de type LISTE : selon le chemin de configuration, `inputs`
    # arrive en liste ou en chaîne JSON. Les deux sont acceptés.
    if isinstance(brut, str):
        try:
            brut = json.loads(brut) or []
        except Exception:
            brut = []
    if not isinstance(brut, list):
        brut = []

    propres, vus = [], set()
    # ★★ LES NOMS DE FLUX DÉJÀ PRIS, parce que deux captures qui visent le même nom se marchent
    #    dessus sur le bus : la première le tient, la seconde n'y écrit RIEN et reste à 0 i/s,
    #    sans erreur, au milieu de compteurs sains. Signalé en recette le 23/10 (« pourquoi
    #    l'entrée Quad SDI 2 ne process aucune image ? Il y a bien du signal »).
    noms_vus, refusees = set(), []
    for e in brut:
        if not isinstance(e, dict):
            continue                       # une entrée illisible est ÉCARTÉE, pas devinée
        pid = _entier(e.get("persistent_id"))
        if not pid or pid in vus:
            # Un persistent id absent ou répété désignerait deux fois le même connecteur : les
            # deux flux se marcheraient dessus sur le bus, sans erreur visible.
            continue
        vus.add(pid)
        # Le SDK n'accepte que 2, 8 ou 16 canaux : une autre valeur est ramenée à AUCUN audio,
        # pas « au plus proche ». Annoncer 6 canaux et en livrer 8 serait pire que d'en livrer 0.
        canaux = _entier(e.get("audio"))
        if canaux not in (2, 8, 16):
            canaux = 0
        # Numérotation À PARTIR DE 1 (cf. app/numerotation.py) — le 0 n'existe pas.
        # ★ MODÈLE ET NUMÉRO DE PRISE : posés par la console, qui seule les connaît (ils viennent
        #   du SDK, dans le conteneur). L'orchestrateur ne peut pas les deviner — il ne voit que
        #   des identifiants stables. On les TRANSPORTE pour que les ports de la page Câbles
        #   s'appellent « DeckLink Quad 2 SDI 1 » et non « Entrée SDI 3 », qui ne désigne rien
        #   dès qu'il y a deux cartes dans la machine.
        # ⚠ Repli NEUTRE, jamais un modèle inventé : « DeckLink » tout court vaut mieux qu'un
        #   nom de carte faux, et se lit correctement dans les deux langues.
        # ── NOM DU FLUX ─────────────────────────────────────────────────────────────────
        # ⚠⚠ LA DÉRIVATION PAR LA LONGUEUR DE LA LISTE N'EST PAS SANS COLLISION, et c'est la
        #    CAUSE RACINE du défaut du 23/10. Reproduit en trois lignes : quatre entrées
        #    donnent …_1 à …_4 ; on retire la deuxième, la liste tombe à 3 ; la suivante
        #    déclarée reprend donc l'indice 4 — déjà porté par une entrée restante. Deux
        #    captures visaient `decklink-io_4`, l'une tournait à 50 i/s et l'autre à ZÉRO.
        #    On avance donc jusqu'au premier indice LIBRE, au lieu de compter les présents.
        nom = str(e.get("flow") or "").strip()
        if not nom:
            k = len(propres)
            nom = flux_video(hostname, k)
            while nom in noms_vus:
                k += 1
                nom = flux_video(hostname, k)
        elif nom in noms_vus:
            # ★ UN DOUBLON EXPLICITE EST ÉCARTÉ ET NOMMÉ, jamais accepté en silence. L'accepter
            #   donnait une capture muette qu'aucun message ne désignait ; l'écarter sans le
            #   dire serait la même faute sous une autre forme.
            refusees.append({"persistent_id": pid, "flow": nom, "raison": "flow_duplicate"})
            continue
        noms_vus.add(nom)
        propres.append({
            "persistent_id": pid,
            "flow": nom,
            "audio": canaux,
            "anc": bool(e.get("anc")),
            "carte": str(e.get("carte") or "DeckLink").strip() or "DeckLink",
            # ⚠⚠ ON N'INVENTE PAS UN NUMÉRO DE CONNECTEUR (22/10). Le repli valait
            #    `len(propres) + 1`, c'est-à-dire le RANG DANS LA LISTE DE CONFIGURATION —
            #    pas le connecteur physique. Et comme le libellé du port est « {carte} SDI
            #    {prise_label} », l'exploitant lisait « SDI 2 » pour un signal qui sort du
            #    connecteur 5 : il pose son câble dans le mauvais trou, et rien ne le
            #    contredit. Constaté sur la Quad le 22/10 (prise enregistrée « 2 » pour
            #    « Quad (5) »), signalé au testeur.
            #    L'orchestrateur ne PEUT PAS connaître ce rang : il se lit dans l'inventaire
            #    de la carte, donc dans le conteneur. La console le joint à la déclaration
            #    (cf. `reperesDe`) ; quand il manque, le seul geste honnête est de dire qu'on
            #    ne sait pas. « SDI ? » envoie vérifier ; « SDI 2 » envoie se tromper.
            "prise": _entier(e.get("prise")) or 0,
        })

    # ── Sorties ──
    brut_out = params.get("outputs") or []
    if isinstance(brut_out, str):
        try:
            brut_out = json.loads(brut_out) or []
        except Exception:
            brut_out = []
    if not isinstance(brut_out, list):
        brut_out = []
    # ── Report du câblage de la DÉCOUPE ────────────────────────────────────────────────────
    # La page Câbles écrit dans `key_ports[slot].flow` (le port que nous matérialisons plus bas).
    # Il faut le reverser sur la sortie qui porte ce connecteur AVANT de reconstruire les
    # sorties, sinon le câblage serait écrasé au déploiement suivant — le port existerait et ne
    # servirait à rien.
    # ★ LA CORRESPONDANCE PASSE PAR LE `persistent_id`, PAS PAR LE RANG. Un rang se décale dès
    #   qu'une sortie est ajoutée ou retirée, et un report décalé enverrait la découpe d'une
    #   sortie sur une autre — en silence, et seulement à l'image.
    # ★ DEUX GROUPES DE 8, DONC DEUX PORTS (21/10). Le bus porte des flux de 8 canaux : les
    #   16 canaux d'une sortie SDI se câblent en deux flux, 1-8 et 9-16. `groupe` dit lequel —
    #   absent ou 1 = le premier, pour que les câbles déjà posés gardent leur sens.
    _cable_audio, _cable_audio2 = {}, {}
    for ap in (params.get("audio_out_ports") or []):
        if isinstance(ap, dict):
            _pid = _entier(ap.get("persistent_id"))
            if _pid:
                _cible = _cable_audio2 if _entier(ap.get("groupe")) == 2 else _cable_audio
                _cible[_pid] = str(ap.get("flow") or "").strip()

    _cable_key = {}
    for kp in (params.get("key_ports") or []):
        if not isinstance(kp, dict):
            continue
        _kpid = _entier(kp.get("persistent_id"))
        if _kpid:
            _cable_key[_kpid] = str(kp.get("flow") or "").strip()

    sorties, vus_out = [], set()
    for e in brut_out:
        if not isinstance(e, dict):
            continue
        pid = _entier(e.get("persistent_id"))
        flux = str(e.get("flow") or "").strip()
        mode = str(e.get("mode") or "").strip()
        # Une sortie sans flux à rejouer, sans mode, ou en double : ÉCARTÉE. Le mode n'a pas de
        # défaut — « le premier supporté » tombe sur du NTSC SD, mesuré au banc.
        #
        # ⚠ ON N'ÉCARTE PLUS UN CONNECTEUR DÉJÀ UTILISÉ EN ENTRÉE (2026-10-02). Cette règle
        #   venait du half-duplex, où un sous-périphérique fait entrée OU sortie. Sur un
        #   sous-périphérique FULL-DUPLEX, capturer et émettre en même temps est exactement ce
        #   qu'on veut — c'est même le seul moyen de boucler une sortie sur une entrée de la
        #   même carte. Mesuré le même jour : le full-duplex capture 1298 trames pendant qu'il
        #   émet. La règle faisait donc disparaître la sortie EN SILENCE, et le bouclage était
        #   intestable sans qu'aucun message ne le dise.
        #
        #   C'est au MATÉRIEL de dire ce qu'il sait faire, pas à nous de le deviner ici : en
        #   half-duplex la carte refusera elle-même, et le binaire le rapportera. Inférer la
        #   règle dans le hook, c'est exactement la branche par modèle que ce plugin s'interdit.
        # ★ UNE SORTIE SANS FLUX EST UN ÉTAT LÉGITIME : « connecteur déclaré en sortie, source
        #   pas encore câblée » (2026-10-19). On l'écartait, et ça créait un cercle vicieux :
        #   pour câbler une source depuis la page Câbles il faut un PORT, le port vient de la
        #   sortie déclarée, et la sortie était supprimée tant qu'elle n'avait pas de source.
        #   L'exploitant ne pouvait donc jamais partir d'une prise vide. Le mode, lui, reste
        #   OBLIGATOIRE : il n'a pas de défaut acceptable (« le premier supporté » tombe sur du
        #   NTSC SD, mesuré au banc), et c'est un choix, pas une conséquence du câblage.
        if not pid or not mode or pid in vus_out:
            continue
        vus_out.add(pid)
        # Pré-charge : nombre de trames en file devant la sortie. Bornée 1..10 — en dessous la
        # carte se trouve à court et saute des trames, au-dessus on paie de la latence pour rien.
        # Absente → le binaire garde son défaut (3), qu'on ne réécrit pas ici : une valeur par
        # défaut dupliquée à deux endroits finit toujours par diverger.
        pre = _entier(e.get("preroll"))
        sortie = {"persistent_id": pid, "flow": flux, "mode": mode,
                  "carte": str(e.get("carte") or "DeckLink").strip() or "DeckLink",
                  # Même raison qu'en entrée : pas de numéro de connecteur inventé. Cf. le pavé
                  # du 22/10 plus haut.
                  "prise": _entier(e.get("prise")) or 0}
        # ── AUDIO EMBARQUÉ ────────────────────────────────────────────────────────────────
        # ★ UNE SORTIE SDI N'ACCEPTAIT QUE DE LA VIDÉO (20/10, signalé en recette). On pouvait
        #   capturer 16 canaux d'une source et n'en émettre aucun. Le flux audio est un port de
        #   plus sur la MÊME sortie — pas une sortie séparée : il part sur le même connecteur,
        #   embarqué dans le signal SDI.
        _fa = str(e.get("audio_flow") or "").strip()
        if pid in _cable_audio:
            _fa = _cable_audio[pid]
        _fa2 = str(e.get("audio_flow_2") or "").strip()
        if pid in _cable_audio2:
            _fa2 = _cable_audio2[pid]
        _ca = _entier(e.get("audio_channels"))
        if _ca not in (2, 8, 16):
            # ⚠ LE DÉFAUT ÉTAIT 16, ET C'ÉTAIT LA PANNE. Le raisonnement d'alors — « nos
            #   captures publient 16 canaux, mieux vaut une fenêtre trop large » — ouvrait 16
            #   canaux sur la carte avec un seul flux de 8 câblé : les canaux 9-16 sortaient
            #   muets sans un mot, et le testeur a cherché la panne dans son câble (21/10,
            #   Quad SDI 1). Le défaut suit maintenant CE QUI EST CÂBLÉ : deux flux → 16, un
            #   seul → 8. Le réglage explicite, lui, est toujours respecté.
            _ca = 16 if _fa2 else 8
        if _fa:
            sortie["audio_flow"] = _fa
            sortie["audio_channels"] = _ca
            # Un second flux n'a de sens qu'au-delà de 8 canaux ouverts : le garder en deçà
            # ferait croire à un câblage actif dont aucun échantillon ne sortirait.
            if _fa2 and _ca > 8:
                sortie["audio_flow_2"] = _fa2
        if pre:
            sortie["preroll"] = max(1, min(10, pre))
        # ── ATTENTE DES TRANCHES ──────────────────────────────────────────────────────────
        # Combien de temps la sortie attend que le producteur FINISSE l'image en cours avant de
        # retomber sur la précédente. Trop court, et le repli ré-émet l'image déjà sortie : une
        # répétée, une sautée (mesuré le 28/10 sur le PGM de la Z8, mélangeur qui finit vers
        # 9-10 ms contre une échéance de 10 ms).
        # Bornée 1..40 ms ICI, et PAS à la durée d'une trame : le hook ne connaît le mode que
        # comme une chaîne (« 1080p50 »), et en déduire une durée serait deviner. C'est le
        # binaire qui tient le plafond réel — il a le mode de la carte — et qui publie la valeur
        # EFFECTIVE. Absente → le binaire garde son défaut (20 ms), qu'on ne réécrit pas ici.
        att = _entier(e.get("slice_wait_ms"))
        if att:
            sortie["slice_wait_ms"] = max(1, min(40, att))

        # ── FILL & KEY ────────────────────────────────────────────────────────────────────
        # Deux signaux vidéo ORDINAIRES sur deux connecteurs : le remplissage et la découpe.
        # Pas de keyer matériel, pas d'alpha, pas de conversion : le bus MXL porte deux flux
        # 4:2:2 et la carte émet deux images. Ce qu'on modélise ici, c'est le LIEN — les deux
        # connecteurs partagent le format et la pré-charge, et vivent ou tombent ensemble.
        #
        # ★ Le lien est déclaré sur UNE entrée plutôt qu'en appairant deux entrées libres : une
        #   paire dont les deux moitiés peuvent être réglées séparément finit toujours par
        #   diverger d'un format, et une découpe au mauvais format n'est pas une découpe.
        kpid = _entier(e.get("key_persistent_id"))
        kflux = str(e.get("key_flow") or "").strip()
        if kpid in _cable_key:
            kflux = _cable_key[kpid]          # ce que la page Câbles vient de poser
        # ★ UNE DÉCOUPE SANS SOURCE RESTE DÉCLARÉE (2026-10-20), pour la même raison qu'une
        #   sortie sans source la veille : le port de câblage NAÎT du connecteur déclaré. Exiger
        #   un flux pour garder le connecteur rendait la découpe incâblable depuis la page
        #   Câbles — il fallait un flux pour avoir le port, et le port pour poser le flux. Sans
        #   flux la voie n'est simplement pas créée côté binaire : rien n'est émis, et
        #   l'appairage des deux connecteurs reste visible dans la console.
        if kpid and kpid != pid and kpid not in vus_out:
            sortie["key_persistent_id"] = kpid
            sortie["key_flow"] = kflux
            vus_out.add(kpid)        # le connecteur de découpe est pris, lui aussi
        sorties.append(sortie)

    # ── Ports dérivés ──────────────────────────────────────────────────────────────────────
    # `derive_wiring` déplie un port par entrée d'une liste et prend le nom TEL QUEL : il ne sait
    # pas suffixer. On matérialise donc ici les listes des flux audio et ANC, pour que le CÂBLAGE
    # les propose. Sans ça, ces flux existeraient sur le bus sans être câblables depuis
    # l'interface — une capacité morte.
    # Ces listes sont persistées avec le reste des params (deploy.py réécrit le deploy_config
    # après le hook), donc le câblage les retrouve bien après le déploiement.
    # ★ LES PORTS SORTENT DANS L'ORDRE DES PRISES, PAS DE LEUR CRÉATION (20/10, demandé en
    #   recette : « SDI 7 doit se situer au-dessus du SDI 8 »). La liste était celle des
    #   déclarations successives — un ordre qui n'a de sens que pour celui qui les a faites, et
    #   aucun pour celui qui cherche une prise.
    # ⚠ LE TRI NE RENOMME RIEN : le flux voyage AVEC son entrée, il n'est pas dérivé du rang.
    #   Réordonner la liste ne déplace donc aucun câblage — seul l'affichage change.
    def _rang(e):
        return (str(e.get("carte") or ""), _entier(e.get("prise")) or 0,
                _entier(e.get("persistent_id")) or 0)
    propres.sort(key=_rang)
    sorties.sort(key=_rang)

    params = dict(params)
    params["inputs"]      = propres
    params["outputs"]     = sorties
    # ★★ CE QUI A ÉTÉ ÉCARTÉ EST PUBLIÉ, SINON J'AURAIS REMPLACÉ UNE PANNE MUETTE PAR UN REFUS
    #    MUET. ⚠ CETTE PHRASE A ÉTÉ FAUSSE PENDANT UNE JOURNÉE : la clé était posée ici et
    #    personne ne la lisait — ni le superviseur, ni /state, ni la console. Le superviseur la
    #    journalise maintenant à CHAQUE application (démarrage et reconfiguration à chaud) et
    #    l'expose dans /state ; garde `verif_flux_uniques.py`, qui suit la clé jusqu'à son
    #    LECTEUR. Une entrée qui disparaît sans un mot, c'est le défaut du 23/10 sous une
    #    autre forme — et publier dans un dictionnaire que personne n'ouvre, ce n'est pas
    #    publier.
    params["entrees_refusees"] = refusees
    # ★ UN PORT PAR GROUPE DE 8 (21/10). La capture publiait UN flux de 16 canaux : sur les 28
    #   flux audio du domaine, 25 en portaient 8 et les 3 seuls à en porter 16 étaient les
    #   nôtres. Nous étions donc seuls à produire cette forme et seuls à savoir la lire, et le
    #   reste du produit n'en lisait que la moitié sans le dire. Le 2e groupe prend le suffixe
    #   `_audio_2`, convention déjà en service (`rl-micros_audio_1`…). Le 1er garde `_audio` :
    #   le renommer casserait tous les câbles déjà posés.
    _ap = []
    for e in propres:
        _ca = _entier(e.get("audio"))
        if not _ca:
            continue
        # `canaux_label` porte la PLAGE, parce que le libellé vit dans le manifeste (donc il
        # est traduit) et qu'une plage n'est pas une phrase : « 1-8 » et « 9-16 » se lisent
        # dans toutes les langues. Écrire « audio 9 à 16 » ici l'aurait figé en français.
        _ap.append({"flow": e["flow"] + "_audio", "carte": e["carte"], "prise": e["prise"],
                    "groupe": 1, "canaux": min(_ca, 8),
                    "canaux_label": "1-%d" % min(_ca, 8)})
        if _ca > 8:
            _ap.append({"flow": e["flow"] + "_audio_2", "carte": e["carte"],
                        "prise": e["prise"], "groupe": 2, "canaux": _ca - 8,
                        "canaux_label": "9-%d" % _ca})
    params["audio_ports"] = _ap
    params["anc_ports"]   = [{"flow": e["flow"] + "_anc", "carte": e["carte"],
                              "prise": e["prise"]} for e in propres if e["anc"]]
    # ★ LA DÉCOUPE A SON PROPRE PORT (2026-10-20, demandé par le testeur). Un Fill & Key est UNE
    #   sortie sur DEUX connecteurs : la page Câbles n'en montrait donc qu'un seul port, celui du
    #   remplissage, et rien n'y disait qu'une découpe existait derrière. Conséquence vécue le
    #   jour même : le flux de découpe a été posé sur le port du remplissage, la carte a émis la
    #   clé sur le connecteur de remplissage et plus rien sur celui de découpe — sans un mot.
    #   L'entrée porte le NUMÉRO de sa sortie (`sortie`), pas une phrase : le libellé vit dans
    #   le manifeste, donc il est traduit, et `{sortie}` le rend juste — `{i1}` donnerait le
    #   rang dans la liste des découpes, qui diffère du numéro de sortie dès qu'une sortie sans
    #   découpe la précède. Écrire la phrase ici l'aurait figée en français.
    # Le second port n'apparaît QUE si la sortie ouvre plus de 8 canaux : un port de câblage
    # qu'aucun échantillon ne traverse est une invitation à se tromper.
    _aop = []
    for n, s_ in enumerate(sorties, 1):
        _aop.append({"flow": s_.get("audio_flow") or "",
                     "persistent_id": s_["persistent_id"],
                     "carte": s_.get("carte") or "DeckLink", "prise": s_.get("prise") or n,
                     "sortie": n, "groupe": 1,
                     "canaux_label": "1-%d" % min(_entier(s_.get("audio_channels")) or 8, 8)})
        if _entier(s_.get("audio_channels")) > 8:
            _aop.append({"flow": s_.get("audio_flow_2") or "",
                         "persistent_id": s_["persistent_id"],
                         "carte": s_.get("carte") or "DeckLink", "prise": s_.get("prise") or n,
                         "sortie": n, "groupe": 2,
                         "canaux_label": "9-%d" % _entier(s_.get("audio_channels"))})
    params["audio_out_ports"] = _aop
    params["key_ports"] = [
        {"flow": s_.get("key_flow") or "",
         "persistent_id": s_["key_persistent_id"],
         "carte": s_.get("carte") or "DeckLink", "prise": s_.get("prise") or n,
         "sortie": n}
        for n, s_ in enumerate(sorties, 1) if s_.get("key_persistent_id")
    ]
    _etiqueter_prises(params)
    _repliquer_si_distant(params, ctx)
    return params


def _etiqueter_prises(params):
    """Dérive `prise_label` de `prise` pour TOUTE liste de ports, en un seul endroit.

    ★ POURQUOI UN LIBELLÉ SÉPARÉ DU NOMBRE. Les libellés du manifeste valent « {carte} SDI
      {prise_label} » : un gabarit ne sait pas écrire « sauf si on ne sait pas ». Avec le
      nombre seul, un connecteur inconnu s'affichait « SDI 0 » — aussi faux que « SDI 2 », et
      aussi propre à faire poser un câble au mauvais endroit.
    ★ POURQUOI ICI ET PAS AUX SIX SITES DE CONSTRUCTION. Les ports se fabriquent à six
      endroits de ce fichier ; y recopier la même dérivation, c'est accepter qu'elle diverge au
      septième. Une seule passe, à la fin, sur tout ce qui porte une `prise`.
    """
    for cle, liste in list(params.items()):
        if not isinstance(liste, list):
            continue
        for port in liste:
            if not isinstance(port, dict) or "prise" not in port:
                continue
            n = port.get("prise")
            # 0 / None / illisible → « ? ». Dire qu'on ne sait pas envoie vérifier ; inventer
            # un numéro envoie se tromper (cf. le pavé du 22/10 sur `prise`).
            port["prise_label"] = str(n) if isinstance(n, int) and n > 0 else "?"
    return params


def _producteurs():
    """{flux: (vmid, node_id)} de TOUT ce que le studio produit, en un seul passage.

    Même dérivation que `/api/sources` : les manifestes, via `derive_wiring`. Aucun appel
    réseau, uniquement la base.

    ★ UNE FOIS PAR DÉPLOIEMENT, PAS UNE FOIS PAR FLUX (21/10). La version précédente relisait
      tous les conteneurs et redérivait leur câblage pour CHAQUE flux de chaque sortie — jusqu'à
      trois par sortie, neuf sorties. Mesuré pendant un réglage de recette : ~30 s de ce seul
      crochet, sous la charge de l'orchestrateur, à chaque geste sur la carte.
    """
    table = {}
    try:
        from app import plugins as _pl
        from app.database import db_get_containers
        for c in db_get_containers():
            try:
                dc = json.loads(c.get("deploy_config") or "{}")
            except Exception:
                continue
            t = dc.get("type")
            if not t or not _pl.is_plugin(t):
                continue
            pr = dc.get("params") or {}
            hn = pr.get("hostname") or c.get("hostname") or ""
            try:
                w = _pl.derive_wiring(t, hn, pr)
            except Exception:
                continue
            for port in w.get("produces") or []:
                if port.get("shm"):
                    table.setdefault(port["shm"], (c["vmid"], c.get("node_id")))
    except Exception:
        pass
    return table


def _repliquer_si_distant(params, ctx):
    """Provisionne la réplication RDMA d'un flux de SORTIE produit sur un AUTRE nœud.

    ★ POURQUOI ICI. Le bus mémoire est LOCAL à une machine : une sortie qui rejoue un flux
      produit ailleurs émet À VIDE — mesuré le 2026-10-02, « trames répétées », sans qu'aucun
      compteur ne désigne la cause. La page Câbles provisionne la réplication toute seule pour
      les câbles inter-nœuds ; la console du plugin passe par un AUTRE chemin (redéploiement),
      donc elle ne bénéficiait pas de ce filet. On offrait alors soit un choix qui échoue en
      silence, soit — ce qu'on a fait un temps — une liste amputée des autres machines, ce que
      l'exploitant a signalé comme une limitation arbitraire (19/10). Les deux étaient
      mauvais : on offre le choix ET on pose la réplication.

    Best-effort, comme côté câblage : un échec (pas de NIC RDMA) ne doit pas empêcher le
    déploiement. Il est en revanche JOURNALISÉ — une réplication absente explique une sortie
    muette, et c'est précisément ce qu'on refuse de laisser deviner.
    """
    try:
        from app.database import db_get_container
        vmid = (ctx or {}).get("vmid")
        moi = db_get_container(vmid) if vmid else None
        dst = (moi or {}).get("node_id")
        if not dst:
            return
        producteurs = None               # construite au premier flux, une seule fois
        for sortie in params.get("outputs") or []:
            for flux in (sortie.get("flow"), sortie.get("key_flow"),
                         sortie.get("audio_flow")):
                flux = str(flux or "").strip()
                if not flux:
                    continue
                if producteurs is None:
                    producteurs = _producteurs()
                src_vmid, src_node = producteurs.get(flux, (None, None))
                if not src_node or int(src_node) == int(dst):
                    continue
                from services import rdma as _rdma
                ok, info = _rdma.ensure_cable_link(src_node, src_vmid, flux, dst, kind="video")
                print("decklink_io: replication %s (node %s -> %s): %s %s"
                      % (flux, src_node, dst, "ok" if ok else "FAILED", info))
    except Exception as exc:
        print("decklink_io: replication check failed: %s" % exc)
