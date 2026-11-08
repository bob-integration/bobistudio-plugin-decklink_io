# `decklink_probe` — sonde d'énumération DeckLink

Jalon 2 du chantier `decklink_io` (`docs/chantiers/DECKLINK.md`). Liste en JSON les cartes Blackmagic
présentes, leurs sous-périphériques, leurs attributs et leurs **profils**.

```
make            # construit la sonde (aucun pilote requis)
./decklink_probe     # énumère
make verif      # vérifie la sonde SANS carte (cf. plus bas)
```

## Ce qu'elle ne fait pas

Aucun effet de bord : elle n'ouvre aucun flux, n'écrit nulle part, et **ne change aucun profil**
— le seul appel de l'API qui modifierait l'état de la carte (`IDeckLinkProfile::SetActive`) est
absent du code. Elle peut donc être lancée sur un nœud en production.

## Les sept réponses, et pourquoi elles sont séparées

« Aucune carte » et « sonde inopérante » se ressemblent trait pour trait. Une sonde qui les
confond a l'air de marcher jusqu'au jour du montage, où elle ne sert plus à rien. D'où quatre
états distincts, séparés aussi par le code de retour :

| `status` | rc | Sens |
|---|---|---|
| `ok` | 0 | le SDK répond, au moins une carte |
| `no_devices` | 0 | le SDK répond, zéro carte — **réponse correcte** sur une machine sans matériel |
| `sdk_absent` | 2 | `libDeckLinkAPI.so` introuvable : pilote Desktop Video non installé |
| `sdk_broken` | 3 | bibliothèque présente, mais l'itérateur ne se crée pas (symbole ou version) |
| `module_absent` | 4 | la bibliothèque répond, mais **aucun module noyau `blackmagic` n'est chargé** |
| `non_enumere` | 5 | le **noyau** expose des périphériques que le **SDK** n'énumère pas |
| `enumere_partiel` | 6 | le noyau en expose **plus** que le SDK n'en énumère : une carte au moins est invisible alors qu'une autre répond |

`non_enumere` a été ajouté le 2026-09-08, au montage de la première carte : une DeckLink Duo 2 au
micrologiciel périmé est activée par le pilote et peuple `/dev/blackmagic/`, mais le SDK refuse
de l'énumérer. La sonde répondait alors « aucune carte, réponse correcte sur une machine sans
matériel » — avec une carte dans le châssis. Le désaccord entre le noyau et le SDK *est*
l'information ; la sonde le dit au lieu de choisir le camp du SDK, et pointe
`DesktopVideoUpdateTool --list`.

`enumere_partiel` a été ajouté le 2026-10-20, au montage de la Quad 2 : posée à côté d'une Duo 2
à jour, son micrologiciel périmé la rendait invisible au SDK. Douze nœuds dans `/dev/blackmagic`,
quatre cartes énumérées — et la sonde a répondu **« ok, le SDK énumère au moins une carte »**.
Elle avait les deux chiffres sous les yeux sans les comparer, et l'exploitant a cherché la panne
dans le conteneur. La règle `non_enumere` ne couvrait que le cas TOTAL ; « au moins une » n'est
pas une réponse quand on en attend huit.

⚠ **Cet état n'a pas encore été exercé.** Le banc `verif_sans_carte.sh` ne sait pas simuler une
énumération partielle — son bouchon de SDK rend toujours zéro carte, et un bouchon qui en rendrait
une demanderait d'implémenter les interfaces d'attributs. Le cas réel, lui, a été constaté le 2026-10-20 :
la Quad 2 au micrologiciel périmé posée à côté d'une Duo 2 à jour. Si la sonde se tait encore la
prochaine fois, c'est elle qui aura tort.

`module_absent` a été ajouté le 2026-09-07 après le redémarrage de dell-1, et c'est le cas le
plus retors : sans carte, les modules ne se rechargent pas au démarrage, et **dans cet état la
bibliothèque répond parfaitement — elle annonce « zéro carte »**. Sur une machine sans matériel
la réponse reste juste ; sur une machine AVEC une carte et le module déchargé, « zéro carte »
serait un diagnostic FAUX, impossible à distinguer de la panne réelle. La sonde lit donc
`/proc/modules` (chemin surchargeable par `BOBI_SDI_MODULES`, qui n'existe que pour permettre à
la vérification d'exercer les deux cas).

Le dispatch de Blackmagic rend `NULL` pour les deux derniers sans les distinguer (son
`IsDeckLinkAPIPresent` est neutralisé par un `#if 0`), d'où le `dlopen` fait en propre dans la
sonde, uniquement pour récupérer le message de `dlerror()`.

## Vérification sans carte

`make verif` construit deux fausses `libDeckLinkAPI.so` — l'une qui annonce zéro carte, l'autre
sans le symbole attendu — deux faux `/proc/modules` et deux faux `/dev/blackmagic`, et contrôle
que la sonde rend bien cinq réponses différentes. Les cinq cas sont **exercés**, pas affirmés.

Cette vérification a été **mutée** le 2026-09-07 pour s'assurer qu'elle mord : en retirant le
test de présence de la bibliothèque, le cas 1 échoue ; en faisant rapporter « zéro carte » comme
« SDK absent », le cas 2 échoue ; en supposant le module toujours chargé, le cas 4 échoue. Chaque
mutation est attrapée par le cas qu'elle concerne — et par lui seul.

Les cas `no_devices` et `module_absent` ont en outre été **constatés sur dell-1** contre la vraie
bibliothèque 16.4, en déchargeant puis rechargeant le module.

Elle vit ici (`verif_sans_carte.sh`) et **ne tourne pas en CI** : la sonde complète exige une
carte. La
vérification ci-dessus, elle, tourne partout où il y a un `g++` — à relancer à la main après
toute modification de la sonde.

## Couverture des en-têtes (`verif_entetes.cpp`)

`make couverture` compile un fichier qui **utilise** chaque constante, méthode et interface dont
le chantier dépend — horodatage de fin de trame, statut de genlock, groupes de capture/lecture,
`PersistentID`, keying, paquets ANC, profils, v210, conversions. Il n'est jamais exécuté : le
verdict *est* la compilation.

Un `grep` ne suffisait pas — il trouve un symbole cité dans un commentaire, ou une constante dont
le type d'énumération a changé. Ici le compilateur exige la signature entière. Vérifié le
2026-09-07 : les 19 symboles du chantier sont déclarés dans les en-têtes 12.2.2, donc **il ne
sera pas nécessaire de récupérer le SDK officiel** pour écrire le plugin. Contrôle de la
vérification elle-même : y ajouter `IDeckLinkIPExtensions`, absent de 12.2.2, la fait bien
échouer.

## Les en-têtes du SDK (`decklink_sdk/`)

Version **12.2.2**, reprise de la source Debian `gst-plugins-bad1.0` 1.26.2
(`sys/decklink/linux/`), donc de `main` — leur licence Blackmagic autorise explicitement la
redistribution par un tiers, à condition de conserver la notice, qui est en tête de chaque
fichier. **Ne pas les éditer.**

Deux conséquences à connaître :

- la bibliothèque n'est chargée qu'à l'exécution (`dlopen` dans `DeckLinkAPIDispatch.cpp`), donc
  tout se **compile sans pilote installé** — c'est ce qui a permis d'écrire et de vérifier cette
  sonde avant l'arrivée de la carte ;
- 12.2.2 couvre les profils, les attributs et le statut, mais **pas** les interfaces SMPTE 2110
  (`IDeckLinkIPExtensions`), apparues plus tard. Le jour où le §8 bis du chantier sera tranché
  dans ce sens, il faudra les en-têtes officiels d'une version récente.
