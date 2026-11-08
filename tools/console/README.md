# Bancs de la console

`banc_console.js` monte `control.js` dans un DOM simulé, le pilote, et vérifie le **corps posté**.
`node --check` ne voit que la syntaxe ; un champ oublié dans le corps posté n'affiche aucune erreur,
il efface un réglage — c'est arrivé deux fois sur ce plugin.

    node tools/console/banc_console.js                      # relevé Duo 2 réel
    DL_INV=inventaire_quad2_synthetique.json node tools/console/banc_console.js
    DL_INV=inventaire_quad2_sans_profil.json node tools/console/banc_console.js

⚠ **Il était MORT du 2026-10-24 au 2026-11-01, et vert nulle part pour le dire.** La légende des
couleurs (0.29.44) pose un `aria-hidden` sur chaque pastille ; le faux DOM du banc n'avait pas
`setAttribute`, et le banc s'ARRÊTAIT là — sur une exception, après huit contrôles sur
soixante-quinze. Quatre primitives manquaient en tout (`setAttribute`, `classList`,
`childElementCount`, `createTextNode`), toutes ajoutées en s'alignant sur le navigateur plutôt
qu'en inventant un comportement : `classList` opère sur `className`, et un attribut `data-*` va
dans `dataset`. **Un faux DOM incomplet n'affaiblit pas le verdict, il l'empêche** — c'est la
même leçon qu'`insertBefore`, notée dans le code. Vérifié par mutation après réparation : en
retirant la pré-charge du corps posté, le banc échoue ; restaurée, il repasse au vert, sur les
cinq relevés (75 à 81 contrôles selon la carte).

## Les cinq relevés

| Fichier | Origine | Ce qu'il prouve |
|---|---|---|
| `inventaire_duo2.json` | **relevé réel** sur dell-1 | le comportement observé sur une carte qui existe |
| `inventaire_duo2_2dhd.json` | **relevé réel**, profil `2dhd` | la même carte sous l'autre profil |
| `inventaire_duo2_quad2_reel.json` | **relevé réel** de dell-1, deux cartes | le découpage par carte : 12 prises à ranger en DEUX rubans, numérotés à partir de 1 sur chacun |
| `inventaire_quad2_synthetique.json` | **FABRIQUÉ** | que la console tient à 8 connecteurs — rien de plus |
| `inventaire_quad2_sans_profil.json` | **FABRIQUÉ** | que la console tient à 8 connecteurs **sans aucun gestionnaire de profil** |

## Le chiffre d'un profil ne compte PAS les connecteurs (mesuré le 2026-10-20)

**C'est la correction d'une erreur qui avait été écrite ici comme un fait.** On lisait dans ce
fichier que l'énumération des profils du SDK s'arrêtant à `4dhd`, « 8 prises indépendantes ne
peuvent venir d'aucun profil connu », et que la Quad 2 n'avait donc probablement aucun
gestionnaire de profil. La Duo 2 posée dans dell-1 réfutait déjà ce raisonnement, et personne ne
le lui avait demandé.

Relevé sur la carte réelle :

```
profil ACTIF    : 2dhd  (« 2 sous-périph. half-duplex »)
prises exposées : 4, toutes half-duplex, toutes affectables
profils offerts : 1dfd, 2dhd        ← il n'y a PAS de 4dhd sur une Duo 2
```

Quatre prises indépendantes sous un profil qui annonce « 2 » — et quatre étaient en service la
veille, trois entrées plus une sortie. **Le chiffre compte des groupes de sous-périphériques,
pas des connecteurs.** Par la même mesure, on en déduisait ici qu'une
Quad 2 en `4dhd` donnerait vraisemblablement 8 prises — **et cette déduction-là est tombée à son
tour** : la Quad 2 réelle n'offre que `1dfd` et `2dhd`, et expose ses 8 prises sous `1dfd`
(cf. section suivante). Ce qui reste, c'est le fait : le chiffre compte des groupes.

Ce que cela ne change PAS : l'ordonnancement de `_profil_cible`. Plus de groupes = plus de
prises, donc prendre le plus grand chiffre half-duplex reste la bonne décision. C'était
l'explication qui était fausse, pas le choix — et c'est pourquoi rien n'a cassé.

| forme | ce que la sonde écrirait | ce que la console doit faire |
|---|---|---|
| **avec profils** (constaté sur la Quad 2) | `profils: ["1dfd", "2dhd"]`, 8 sous-périph. | choisir le plus grand half-duplex, quel que soit son chiffre |
| sans gestionnaire de profil | `profils: null`, `profil_actif: null` | ne proposer AUCUNE bascule, garder « Installation » masqué |

Le banc couvre les deux : `inventaire_quad2_synthetique.json` et `inventaire_quad2_sans_profil.json`.
La règle elle-même est vérifiée par `verif_profil_cible.py`, à la racine du plugin, qui porte
désormais le cas **mesuré** (`1dfd` + `2dhd` → `2dhd`) à côté des cas fabriqués.

## ⚠ Ce que le relevé synthétique ne prouve PAS

Il est construit par extrapolation du relevé Duo 2 : le nombre de sous-périphériques, les profils
offerts et la position de l'entrée de référence y sont des HYPOTHÈSES. Il garde sa valeur — tenir
à 8 connecteurs sans aucun gestionnaire de profil, par exemple, reste un cas qu'aucune carte
réelle ne nous a encore présenté — mais ce n'est pas lui qui dit ce qu'est une Quad 2.

**Une Quad 2 RÉELLE a été relevée depuis** (`inventaire_duo2_quad2_reel.json`, dell-1), et elle
tranche deux des attentes écrites ici :

```
sous-périphériques : 8
profil ACTIF       : 1dfd  (« 1 sous-périph. full-duplex »)
profils offerts    : 1dfd, 2dhd        ← les MÊMES que la Duo 2 ; il n'y a pas de 4dhd
BMDDeckLinkTopologicalID : 5277184, 5277186, 5277440, 5277442,
                           5277185, 5277187, 5277441, 5277443
```

- **« une Quad 2 en 4dhd donnerait vraisemblablement 8 prises » était une extrapolation, et ce profil
  n'existe pas sur cette carte.** La carte expose ses 8 sous-périphériques sous `1dfd`, et n'offre que les
  deux profils de la Duo 2. Le chiffre d'un profil compte les sous-périphériques d'un GROUPE, et
  une Quad 2 a quatre groupes là où une Duo 2 en a deux — ce qui reste exactement ce que dit la
  section précédente, mais mesuré au lieu d'être déduit.
- **`_profil_cible` n'en est pas affectée** : plus de groupes = plus de prises, prendre le plus
  grand chiffre half-duplex reste la bonne décision.

✔ **Et ce relevé CONFIRME l'entrelacement de l'équerre**, mesuré cette fois sur huit prises.
Trié, le `topological_id` donne les positions physiques 1 à 8 ; l'ordre dans lequel le SDK
énumère la carte parcourt ces positions dans l'ordre 1, 3, 5, 7, 2, 4, 6, 8 — ce qui, avec
l'équerre `Ref, 1, 5, 2, 6, 3, 7, 4, 8` donnée par l'exploitant, rend exactement les BNC 1 à 8
dans l'ordre. Les deux sources, relevées indépendamment et à un mois d'écart, se recoupent au
connecteur près.

## Ordre des connecteurs — ce qui est ÉTABLI (2026-10-19)

Le SDK **énumère** les sous-périphériques dans un ordre qui n'est pas celui des BNC sur
l'équerre : les deux moitiés de la carte sont **entrelacées**.

| carte | ordre d'équerre, depuis le côté carte mère |
|---|---|
| Duo 2 | Ref, 1, 3, 2, 4 |
| Quad 2 | Ref, 1, 5, 2, 6, 3, 7, 4, 8 |

Deux sources indépendantes et concordantes :

1. l'ordre d'équerre de la Quad 2, donné par l'exploitant ;
2. `BMDDeckLinkTopologicalID` relevé sur la Duo 2 réelle : 696, 698, 697, 699 — qui trié rend
   exactement le même entrelacement.

Le relevé synthétique de Quad 2 porte désormais ces identifiants, au lieu de l'extrapolation
qu'il contenait. ✔ **Une Quad 2 réelle a depuis été relevée** (`inventaire_duo2_quad2_reel.json`)
et elle confirme l'entrelacement sur huit prises — cf. la section précédente. Ce qui était une
hypothèse ne l'est plus ; ce que ce relevé réel a en revanche réfuté, c'est le profil `4dhd`
qu'on attendait.
