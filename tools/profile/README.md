# `decklink_profile` — bascule de profil

Chantier `decklink_io` (`docs/chantiers/DECKLINK.md`), jalon 3.

```
./decklink_profile              # liste les profils et le rayon d'action de chacun
./decklink_profile 2dhd         # active ce profil sur TOUS les groupes
./decklink_profile 1dfd --force # passe outre le garde-fou « entrée vivante »
```

⚠ **Le plugin fait désormais cette bascule lui-même** : il **normalise** la carte au démarrage
vers le profil qui rend le plus de prises indépendantes, et la console l'offre dans sa section
« Installation » (endpoint `/profile`). Cet outil reste le banc — pratique sur un nœud où rien
n'est encore déployé — et la règle de décision, elle, est tenue par `verif_profil_cible.py` et
`verif_normalisation_profil.py` à la racine du plugin.

## C'est l'opération destructive du chantier, et l'outil la traite comme telle

- il annonce le **rayon d'action** (`GetPeers`) avant d'agir ;
- il **refuse** si un sous-périphérique a un signal d'entrée verrouillé — on ne coupe pas une
  source vivante par inadvertance. `--force` pour passer outre ;
- il **attend `ProfileActivated`** : le manuel prévient que l'activation n'est pas terminée au
  retour de `SetActive`. Croire ce retour, c'est ré-énumérer trop tôt.

## Deux choses apprises sur le matériel le 2026-09-08

**Un appareil porte plusieurs groupes.** Une Duo 2 en a deux, de deux sous-périphériques chacun ;
activer un profil sur l'un ne touche pas l'autre. L'outil balaie donc jusqu'à ce que tous les
groupes soient sur le profil visé, en repartant d'un **itérateur neuf** à chaque passe — le
nombre de sous-périphériques change sous les pieds à chaque activation.

**Le groupe n'est pas fait d'index voisins** : sur la Duo 2 c'est {0, 2} et {1, 3}, pas {0, 1} et
{2, 3}. Seul `GetPeers` dit qui va avec qui — l'index ne le dit pas, et le supposer bascule le
mauvais connecteur.
