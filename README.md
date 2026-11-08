# `decklink_io` — entrées/sorties Blackmagic sur le bus MXL

Fait entrer un signal **SDI** sur le bus MXL et l'en fait ressortir, au moyen d'une carte
Blackmagic. La capture est faite par un **binaire natif** (`decklink_rx`), la sortie par un autre
(`decklink_tx`) — les deux sens sont nommés **depuis le fil SDI**, comme le `mtl_rx.c` de
`2110_io` l'est depuis le fil Ethernet ; un superviseur Python les lance, les relance et publie
leur état.

**Un conteneur par CARTE, les deux rôles dans le même conteneur.** Le profil de la carte décide
du rôle de chaque connecteur et vaut pour tout un groupe de sous-périphériques : il ne peut donc
pas appartenir à deux conteneurs.

**Aucune branche par modèle de carte.** Nombre de prises, formats, profils, capacité d'entrée ou
de sortie : tout est demandé à la carte au démarrage. C'est la première règle du chantier, et
elle est tenue par des gardes (`verif_profil_cible.py` vérifie que la règle de profil se CALCULE
au lieu de lister des modèles).

⚠ **DEUX MODÈLES ONT ÉTÉ MESURÉS, et deux seulement** : **DeckLink Duo 2** et **DeckLink
Quad 2** — toutes deux PCIe, toutes deux SDI, toutes deux n'offrant que les profils `1dfd` et
`2dhd`. N'ont JAMAIS été branchés : les boîtiers **UltraStudio** (Thunderbolt, USB) et les
**Intensity**, les cartes **IP**, les modèles **4K/8K** — et l'**UHD n'a jamais été essayée**,
même sur les deux cartes mesurées. L'absence de branche par modèle est une propriété du code,
vérifiée par une garde ; qu'une carte inconnue marche est une **prédiction**, pas une mesure. Le
chantier a déjà vu cette prédiction se tromper deux fois : un profil `4dhd` attendu sur la
Quad 2 qui n'existe pas, et un nombre de prises déduit du chiffre du profil alors que ce chiffre
compte des groupes de sous-périphériques (cf. `tools/console/README.md`, où les deux réfutations
sont datées). Ce qu'on peut dire d'une carte non listée ici : rien ne l'exclut, rien ne
l'atteste.

## Ce qu'il fait

| | |
|---|---|
| **Entrée** | détection automatique du format, vidéo 4:2:2 10 bits sur le bus, audio embarqué jusqu'à 16 canaux, timecode publié en ANC (RFC 8331) |
| **Sortie** | rejoue un flux MXL sur une prise, en v210, à la cadence de la CARTE ; audio embarqué jusqu'à 16 canaux ; Fill & Key sur deux connecteurs appairés |
| **Entrelacé** | capture et sortie, mesurés (cf. « Ce qui est mesuré ») |
| **Horloge** | par sortie : genlock, trames répétées, heures entre deux glissements, écart carte ↔ nœud en ppm |
| **Console** | le ruban des prises dans l'ordre PHYSIQUE de l'équerre, un éditeur par prise, les dispositions enregistrées, et la section d'installation (profil, pilote) |

L'aide de l'exploitant est dans **`help.md`** (rendue dans l'interface) ; le journal des versions,
avec les mesures et les pièges, dans **`meta.json`** ; la spécification et le journal du chantier
dans **`docs/chantiers/DECKLINK.md`** du dépôt principal.

## Où regarder quoi

| | |
|---|---|
| `decklink_rx.cpp` | capture — v210 10 bits → planar → grains MXL, audio, ANC. Pendant de `mtl_rx.c` chez `2110_io` |
| `decklink_tx.cpp` | sortie — flux MXL planar → v210 sur un connecteur, audio, pré-charge, horloge |
| `script.py` | le superviseur dans le conteneur : lance un processus PAR CONNECTEUR, publie `:8080` (fps) et `:8082` (`/state`, `/inventory`, `/config`, `/profile`) |
| `hooks.py` | ce qui tourne dans l'ORCHESTRATEUR : normalisation de la configuration, matérialisation des ports audio/ANC, dépliage du câblage |
| `control.html` / `control.js` / `control.css` | la console, avec ses libellés dans `i18n/` |
| `docker/Dockerfile` | image d'exécution : reprend `libmxl` de l'image compute, compile les binaires, n'embarque **rien** de Blackmagic |
| `decklink_sdk/` | en-têtes du SDK 12.2.2, vendorisés (cf. `THIRD-PARTY-NOTICES.md`) — **ne pas les éditer** |
| `tools/probe` | énumération : cartes, sous-périphériques, profils, connecteurs, signal. Lancée DANS le conteneur par `/inventory` |
| `tools/profile` | bascule de profil (opération destructive, avec ses garde-fous) |
| `tools/in` / `tools/out` | bancs de capture et de sortie/horloge, antérieurs au plugin |
| `tools/console` | banc de la console : monte `control.js` dans un DOM simulé et vérifie le corps POSTÉ |
| `verif_*.py` / `verif_*.cpp` | les gardes du plugin (cf. plus bas) |

## Configuration

Le manifeste porte cinq clés (`config_schema`), dont trois cachées. Ce qui compte :

```json
{
  "inputs":  [{"persistent_id": 4040731139, "flow": "sdi1", "audio": 16, "anc": true}],
  "outputs": [{"persistent_id": 4040731136, "flow": "pgm", "mode": "1080p50", "preroll": 3}]
}
```

- **`persistent_id`, jamais l'index de sous-périphérique.** L'index désigne une **autre prise
  physique** après un changement de profil, sans que rien ne le signale. L'identifiant stable est
  donné par `tools/probe` (et par la console, qui le lit via `/inventory`).
- **Le mode de sortie n'a pas de défaut** : « le premier supporté » tombe sur du NTSC SD (mesuré
  au banc). Une sortie sans mode est écartée ; une sortie sans FLUX, en revanche, est un état
  légitime — « connecteur déclaré, source pas encore câblée ».
- **Audio** : 2, 8 ou 16 canaux. Une autre valeur est ramenée à **0**, pas au plus proche —
  annoncer 6 canaux et en livrer 8 serait pire que de n'en livrer aucun.
- **Pré-charge** (`preroll`, 1 à 10) : trames en file devant la sortie. Elle **achète de la
  latence**, et la loi est mesurée : 120 ms + 40 ms par trame. Absente, le binaire garde son
  défaut (3), qui ne vit qu'à un seul endroit.
- `missing_frame_mode` (caché) : ce qu'une SORTIE émet quand la source ne fournit rien de frais —
  `repeat` ou `black`, vide = suit Réglages → MXL → Image manquante. Ne concerne jamais la
  capture : un trou y est créé honnêtement.
- `layouts` (caché, `scope: system`) : les dispositions enregistrées par carte, écrites par la
  console, jamais à la main.
- `binary` (caché) : chemin du binaire dans l'image.

**Reconfiguration à chaud** : `/config` compare la nouvelle configuration à celle qui tourne et ne
relance **que** les connecteurs dont la ligne de commande change — une prise d'antenne n'est pas
coupée parce qu'on en règle une autre. Refus 409 si la version du plugin ou le nom d'hôte
diffèrent (garde `verif_reconfig_a_chaud.py`).

## Câblage

Les ports se déplient depuis les listes (`from_list`) : **rien n'est codé en dur**.

| | |
|---|---|
| produit | une vidéo par entrée, plus l'audio et l'ANC des entrées qui les activent |
| consomme | une vidéo par sortie, la découpe d'une sortie Fill & Key, et l'audio des sorties qui en portent |

Les listes `audio_ports` / `anc_ports` / `audio_out_ports` / `key_ports` sont **matérialisées par
le hook** : `derive_wiring` déplie un port par entrée de liste et prend le nom tel quel, il ne
sait pas suffixer. Sans cette matérialisation, les flux audio et ANC existeraient sur le bus sans
être câblables depuis l'interface — une capacité morte.

⚠ **Le bus porte des flux de 8 canaux.** 16 canaux font donc **DEUX** flux : `<nom>_audio`
(canaux 1-8) et `<nom>_audio_2` (9-16), convention déjà en service ailleurs. Garde :
`verif_audio_groupes.py`.

## Les règles tenues, et ce qu'elles ont coûté

| Règle | Raison, payée ailleurs |
|---|---|
| le sous-périphérique se désigne par son **persistent id** | un changement de profil renumérote ET redistribue : les groupes de la Duo 2 sont {0,2} et {1,3}, pas des paires consécutives |
| `duplex != inactive` testé **avant** `VideoIOSupport` | `VideoIOSupport` reste vrai sur un sous-périphérique désactivé. Et `modes_sortie` ne dit rien non plus : une prise éteinte annonce les mêmes 16 modes que les autres (mesuré) |
| reconfiguration **seulement si le mode diffère** | sinon `EnableVideoInput` re-déclenche la notification : 66 « changements » en 3 s sur une source stable |
| une trame `bmdFrameHasNoInputSource` n'est **jamais** écrite | pas de trame fabriquée en silence. Sans câble, la carte livre quand même 118 trames en 4 s |
| capture en **v210 10 bits**, convertie en planar | capturer en 8 bits aurait jeté deux bits sans que rien ne le signale |
| aucune **branche par modèle**, aucune disposition déduite | le SDK ne décrit pas la disposition des connecteurs d'un profil : un schéma inventé est pire qu'un schéma absent, on s'y fie |
| le plugin n'expose **aucune action** de macro | quel flux part sur quel connecteur est un **câblage**, pas de l'exploitation (arbitré le 2026-09-08). Il expose de la **condition** : `/state` publie signal, cadence, format et répétitions, de quoi déclencher sur « l'entrée 2 a perdu son signal » |
| cinq sorties au plus par carte | plafond mesuré, et c'est un compte **par carte**, pas un total (`verif_plafond_sorties.py`) |

## Ce qui est mesuré

Les chiffres ci-dessous viennent de bancs ou de la carte en service, pas d'une estimation ; leur
contexte est dans `meta.json`.

| | |
|---|---|
| **latence jusqu'au fil** | bouclage physique : 160 ms à pré-charge 1, 200 à 2, 240 à 3, sans une trame perdue → **120 ms + 40 ms × pré-charge** (0.13.0) |
| **sortie entrelacée** | 861 trames tissées, `interlaced=true`, 0 grain court, 25,0 i/s (0.12.0) ; la capture entrelacée tourne depuis 0.10.0 |
| **ordre des prises** | le SDK énumère les sous-périphériques dans un ordre **entrelacé**, pas celui des BNC. Deux sources concordantes : l'équerre d'une Quad 2 donnée par l'exploitant et le `BMDDeckLinkTopologicalID` de la Duo 2 réelle |
| **cœurs** | ~0,49 cœur par **prise active**, pointe/moyenne 1,22 → ceil((0,1 + 0,49 × prises) × 1,22) : 1 prise = 1 cœur, 4 = 3, 8 = 5. Cœurs **physiques entiers** (frères HT fermés à tout autre conteneur), sur le socket de la carte. Changer de prises recrée le conteneur : la page les applique **en lot** |
| **mémoire** | 153 Mio par connecteur + 41 Mio fixes → 2048 Mio. À 1024, le cgroup entrait en récupération DIRECTE et bloquait les fils de capture |
| **audio** | 284 175 échantillons sur 16 canaux, 4 546 800 flottants écrits (aucun perdu), −11 dBFS retrouvés côté consommateur |
| **retard de la tête audio** | c'est un échantillon de **dent de scie**, pas une constante : son maximum vaut la période du paquet (25/s → 40 ms). Un relevé isolé ne veut rien dire |

## Limites connues

- **ANC : seul le timecode** est transporté. Le pilote ne le livre pas en paquet ANC — il ne
  l'expose que par son API dédiée — donc le paquet ATC est **synthétisé** et son écriture
  contrôlée octet à octet contre notre propre décodeur. Sous-titres, sADM, données privées : non
  transportés.
- **L'index d'un grain dit l'instant où NOUS traitons la trame**, pas celui où elle est entrée :
  il est ancré sur `mxlGetCurrentIndex` et non sur l'horodatage matériel, dont le compteur ne
  porte pas l'époque. Depuis 0.29.33 il est au moins **contigu** — il ne se ré-ancre qu'à la
  première trame et aux décrochages.
- **Les chiffres de ressources sont relevés en 8 bits** : la conversion v210 → planar n'y est pas
  isolée. À remesurer en 10 bits avant d'en déduire quoi que ce soit.
- **La correspondance prise physique ↔ sous-périphérique n'est pas déductible** : ni le châssis ni
  le SDK ne la donnent. Elle s'établit par l'essai, et c'est ce qui coûte le plus de temps sur une
  carte qu'on découvre.
- **`enumere_partiel` de la sonde n'a jamais été exercé** par un banc (cf.
  `tools/probe/README.md`).
- **Dette** : le calcul d'UUID de flux et la construction du flowDef sont **dupliqués** depuis
  `mtl_rx.c`. `make verif` couvre l'UUID ; le flowDef, non.

## Gardes et bancs

Les gardes sont des fichiers autonomes, à lancer à la main depuis la racine du plugin. Elles
mordent par **mutation** : chacune a été vérifiée en cassant le code qu'elle surveille.

```
./venv/bin/python plugins/decklink_io/verif_<nom>.py    # depuis la racine du dépôt
make verif                                              # construit les témoins C++ et lance les deux comparaisons
node tools/console/banc_console.js                      # la console, corps POSTÉ vérifié
```

| | |
|---|---|
| `verif_flow_id` (`make verif`) | l'UUIDv5 natif est-il identique à celui de `bobimxl` ? S'il diverge, **rien n'échoue** : le flux existe sans que personne ne le trouve |
| `verif_anc` (`make verif`) | l'empaqueteur RFC 8331 et la charge ATC, octet pour octet, sur 66 + 16 cas. Un grain ANC mal sérialisé **se lit sans erreur** |
| `verif_profil_cible` | la règle de profil se CALCULE (jamais une liste de modèles), et ne choisit jamais un full-duplex |
| `verif_normalisation_profil` | la normalisation au démarrage, et qu'elle boucle jusqu'à ce que TOUS les groupes y soient |
| `verif_audio_groupes` / `verif_audio_reprise` | les deux flux de 8, et la recopie des champs du hook vers le binaire — trois champs y ont déjà été perdus EN SILENCE |
| `verif_flux_uniques`, `verif_port_decoupe`, `verif_plafond_sorties`, `verif_reconfig_a_chaud`, `verif_stats_publiees`, `verif_garde_generation` | le reste : unicité des flux, découpe, plafond, reconfiguration à chaud, statistiques publiées |

Côté dépôt principal : `tests/verif_decklink_pilote_absent.js`,
`tests/verif_blackmagic_firmware.py` et `tests/verif_pilote_hote_reglages.py` tiennent la chaîne
du **pilote hôte** (déclaration de la capacité, porte dans Réglages, bandeau de la page).

## Construction

Elle ne se fait **pas** ici : les binaires ont besoin des en-têtes et de la bibliothèque `libmxl`,
qui vivent dans l'image d'exécution (`docker/Dockerfile`). `make syntaxe` contrôle la compilation
en pointant `MXL_INCLUDE` sur des en-têtes MXL ; `make verif` n'a besoin que d'OpenSSL ; la sonde
`tools/probe` se compile **sans pilote installé** (le SDK n'est chargé qu'à l'exécution).

## Prérequis sur le nœud

Le pilote **Desktop Video** de Blackmagic s'installe sur la MACHINE, pas dans le conteneur, et
sans lui le noyau n'expose aucun `/dev/blackmagic` : le conteneur est alors créé *en attente du
pilote* plutôt que mis en échec. Le geste se fait dans **Réglages → Nœuds**, après avoir déclaré
la capacité `decklink` sur le nœud qui porte la carte. L'archive se **dépose** à la main : un nœud
n'a pas forcément d'accès sortant, et l'installation se fait hors ligne.

⚠ **Un micrologiciel périmé rend la carte INVISIBLE** au SDK alors que `lspci` la voit et que
`/dev/blackmagic` existe. Le diagnostic n'est lisible que dans `dmesg` (« firmware version
mismatch »), et la mise à jour exige une **extinction complète**, pas un redémarrage.

## Journal

`meta.json` porte le journal des versions. `docs/banc-2026-09-08.md` garde les premiers relevés
de banc — **journal daté, périmé par endroits**, conservé pour les mesures et les pièges qui les
ont précédées.
