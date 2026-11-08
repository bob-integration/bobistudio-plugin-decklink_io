# DeckLink — entrées et sorties SDI

Fait entrer un signal **SDI** sur le bus MXL, et l'en fait ressortir, au moyen d'une carte
**Blackmagic DeckLink**. Un seul conteneur porte les deux rôles pour une carte donnée.

Le plugin ne connaît **aucun modèle de carte** : tout — nombre de prises, formats, profils,
capacité d'entrée ou de sortie — est demandé à la carte au démarrage.

⚠ **Mais seuls deux modèles ont été essayés** : **DeckLink Duo 2** et **DeckLink Quad 2**, deux
cartes PCIe SDI. Les boîtiers UltraStudio (Thunderbolt, USB), les Intensity, les cartes IP et les
modèles 4K/8K n'ont jamais été branchés, et l'UHD n'a jamais été essayée. Ne rien connaître des
modèles donne une bonne chance qu'une autre carte marche, mais personne ne l'a vérifié : si vous
en installez une, attendez-vous à devoir relever son inventaire (`tools/probe`) et prévenez-nous
de ce que vous trouvez.

## Ce qu'il fait aujourd'hui

- **Entrée** : détection automatique du format, vidéo en 4:2:2 10 bits sur le bus, **audio
  jusqu'à 16 canaux**, et le **timecode** publié en ANC (RFC 8331).
- **Sortie** : rejoue un flux MXL sur une prise SDI, en v210, **à la cadence de la carte** — si
  la source décroche, la dernière image est ré-émise et les répétitions sont comptées, plutôt que
  de laisser la sortie se figer ou se trouer. Ce comportement **se choisit** : recopie (défaut,
  invisible à l'antenne) ou **noir** (pour voir exactement quand la source saute), dans
  **Réglages → MXL → Image manquante**, appliqué à chaud ; un conteneur réglé explicitement garde
  sa valeur. Audio embarqué jusqu'à 16 canaux, et **Fill & Key** sur deux connecteurs appairés.
- **Surveillance d'horloge**, publiée par sortie : dérive de la carte contre l'horloge du nœud,
  asservissement au genlock, et glissement net entre la source et la sortie.

## Avant tout : le pilote s'installe sur la MACHINE

Le pilote **Desktop Video** de Blackmagic ne vit pas dans ce conteneur mais sur le nœud, et sans
lui le noyau n'expose aucune carte : le conteneur est alors créé **en attente du pilote** au lieu
d'échouer. Le geste se fait dans **Réglages → Nœuds**, sur la fiche du nœud qui porte la carte,
après y avoir déclaré la capacité `decklink`. L'archive se **dépose** à la main — un nœud n'a pas
forcément d'accès à Internet, et l'installation se fait hors ligne.

## Désigner une prise : l'identifiant stable, jamais l'index

Une prise se désigne par son **`persistent_id`**, jamais par son index de sous-périphérique :
un changement de profil fait désigner à l'index une **autre prise physique**, sans que rien ne le
signale. En exploitation vous n'avez pas à le manipuler — on clique une prise du ruban dans la
console, qui pose l'identifiant elle-même. Il se lit à la main avec `tools/probe`, utile quand on
diagnostique une carte qu'on découvre.

## Trois pièges, tous rencontrés en vrai

**Le profil de la carte éteint des prises.** Une carte s'offre en plusieurs **profils** — les
deux cartes mesurées n'en proposent que deux, `1dfd` et `2dhd` — et, selon le profil, des prises
sont purement **inactives**. Un câble branché dessus ne donne rien, et **rien sur le châssis ne
le dit** : la console marque ces prises inactives, et c'est la première chose à vérifier quand
une source est annoncée présente et que la carte n'en voit aucune.

⚠ **Le chiffre dans le nom d'un profil compte des groupes de sous-périphériques, pas des
connecteurs.** Une Duo 2 en `2dhd` expose **quatre** prises, pas deux — mesuré. C'est la lecture
qui nous a trompés deux fois ; ne déduisez pas un nombre de prises d'un nom de profil.

**En half-duplex, un sous-périphérique fait entrée OU sortie, jamais les deux.** La prise de
sortie qui fait paire avec une entrée en cours de capture est donc **muette par construction**.
Là encore, aucun message : ça ne marche pas, voilà tout. C'est **le profil de la carte** qui le
décide, pas le plugin : en **full-duplex**, capturer et émettre en même temps sur le même
sous-périphérique marche, et c'est même le seul moyen de boucler une sortie sur une entrée de la
même carte (mesuré : 1 298 trames capturées pendant qu'il émettait). Le plugin déclare donc la
sortie sans la discuter — si la carte ne sait pas le faire, c'est elle qui refuse, et le refus
est rapporté.

**Un micrologiciel périmé rend la carte INVISIBLE.** Elle est bien dans la machine, `lspci` la
voit, `/dev/blackmagic` existe — et le SDK énumère zéro carte. Le diagnostic n'est lisible que
dans `dmesg` (« firmware version mismatch »). La mise à jour se fait avec l'outil Blackmagic et
exige une **extinction complète**, pas un redémarrage.

## La correspondance prise physique ↔ sous-périphérique

Elle n'est **pas déductible** : ni le châssis, ni le SDK ne la donnent. On l'établit par
l'essai — on émet sur un sous-périphérique et on regarde quelle entrée se verrouille. Prévoyez-le
au montage, et notez-la : c'est ce qui coûte le plus de temps sur une carte qu'on découvre.

## Les réglages d'une prise

Tout se règle dans la **console du conteneur** : on clique une prise du ruban, et on dit ce
qu'elle fait.

- **Entrée** : audio embarqué (2, 8 ou 16 canaux) et ANC. 16 canaux publient **deux flux de 8**
  (`…_audio` et `…_audio_2`) — le bus porte des flux de 8 canaux.
- **Sortie** : le **format est explicite**, il n'a pas de défaut (« le premier supporté » tombe
  sur du NTSC SD). Plus l'audio embarqué, la paire **Fill & Key** (deux connecteurs, une seule
  sortie logique, format et pré-charge partagés), la **pré-charge** — offerte avec son effet,
  « 1 trame, environ 160 ms », parce qu'elle gouverne la moitié du budget de latence — et
  l'attente des tranches (1 à 40 ms, 20 par défaut).
- **Dispositions** : l'affectation des connecteurs d'une carte s'enregistre et se rappelle
  (rôles, format, pré-charge, appairage Fill & Key), s'exporte et s'importe. Elle ne retient
  **pas** les flux : ça, c'est du câblage, et ça reste la page Câbles.
- **Cinq sorties au plus par carte** : au-delà, l'avertissement est affiché et demande une
  confirmation. Le compte est par **carte**, pas un total sur le nœud.
- Le flux d'une sortie se pose dans la page **Câbles**. Une sortie sans flux est un état
  légitime : il faut qu'elle existe pour pouvoir y câbler quelque chose.

**Le profil de la carte est rangé dans « Installation »**, replié : ce n'est pas un choix
d'exploitation. La carte est **normalisée au démarrage** vers le profil qui rend le plus de prises
indépendantes, et le changement est **signalé** dans les alertes. Une carte déjà dans le bon
profil n'est pas touchée — une bascule coupe les prises. En revanche, quand la bascule est due,
elle a lieu **même si des câbles sont branchés** : attendre qu'aucune prise ne soit occupée
revenait à laisser un seul câble bloquer les sept autres prises.

## Ce qui est mesuré, et ce qui reste ouvert

- **La latence jusqu'au fil est chiffrée**, par bouclage physique sortie → entrée : 160 ms à
  pré-charge 1, 200 à 2, 240 à 3, sans une trame perdue. Soit **120 ms + 40 ms par trame de
  pré-charge**. Les 120 ms de socle sont la capture, le bus, la sortie de carte, le fil et
  l'entrée de carte.
- **L'entrelacé tourne**, des deux côtés : la capture depuis la version 0.10.0 — le chemin par
  champ n'avait jamais été atteint, un index calculé à la cadence trame le faisant repartir avant
  lui — et la sortie depuis 0.12.0, mesurée à 861 trames tissées, 0 grain court, 25,0 i/s. Le
  défaut à guetter reste traître : une parité de trame inversée donne une image **fixe
  parfaitement correcte**, et ne se voit que sur du **mouvement**.
- **ANC : seul le timecode** est transporté. Le pilote ne le livre pas en paquet ANC — il ne
  l'expose que par son API dédiée — donc le paquet ATC est **synthétisé** par le plugin, et son
  écriture est vérifiée octet à octet contre notre propre décodeur. Les autres données ANC
  (sous-titres, sADM, privées) ne sont pas transportées.
- **L'instant qu'un grain porte est celui où nous le traitons**, pas celui où la trame est
  entrée dans la carte : son compteur matériel ne porte pas l'époque. Cela ne change rien à la
  latence ci-dessus, qui est mesurée au fil et non déduite de l'index, mais un consommateur ne
  peut pas en déduire l'heure de capture.
- **L'UHD n'a jamais été essayée**, et les relevés de ressources (5 cœurs, 2 Gio) ont été pris en
  **8 bits** : à remesurer en 10 bits avant d'en déduire quoi que ce soit.
