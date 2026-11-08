# `decklink_out` — banc de sortie et de mesure d'horloge

Chantier `decklink_io` (`docs/chantiers/DECKLINK.md`, §3 et §4). Émet un motif sur un
sous-périphérique et mesure, trame par trame, la relation entre l'horodatage de la carte et
l'horloge système.

```
make && ./decklink_out [secondes] [pré-charge]     # défaut : 10 s, pré-charge 3
```

**N'exige aucune entrée SDI** — c'est ce qui permet de le passer avant tout câblage.

## Ce qu'il mesure

- le **pont d'horloge** : `GetFrameCompletionReferenceTimestamp` contre `CLOCK_REALTIME`, lus
  dans le même callback → écart, amplitude, dérive en ppm ;
- la **santé de la lecture ordonnancée** : trames complétées, en retard, perdues, vidangées ;
- l'état du **genlock** (`GetReferenceStatus`), rappelé en tête pour que la mesure se lise dans
  son contexte : sans référence, la carte tourne sur son quartz.

## Ce qu'il ne mesure pas

Il compare l'instant où le pilote **rapporte** la trame à l'instant où nous lisons l'horloge. Il
ne dit **rien** de l'instant où la trame sort électriquement du connecteur. Le décalage
« soumission → front de sortie » demande un bouclage physique sortie → entrée.

> ✔ **Ce bouclage a été fait depuis** (plugin 0.13.0, cf. `meta.json`) : 160 ms à pré-charge 1,
> 200 à 2, 240 à 3, soit **120 ms + 40 ms par trame de pré-charge**. Ce banc-ci reste aveugle à
> cette grandeur — c'est la chaîne complète du plugin qui la mesure, pas lui.

## Choix du sous-périphérique et du format

Le banc prend le premier sous-périphérique **actif** (`duplex != inactive` — testé AVANT
`VideoIOSupport`, qui reste vrai sur un sous-périphérique désactivé) et capable de lecture, puis
le **premier mode que la carte déclare supporter**.

⚠ Ce dernier choix est une commodité de banc, **pas un modèle pour le plugin** : sur une Duo 2 il
tombe sur du `525i59.94 NTSC`. Le plugin devra choisir explicitement ou suivre le format détecté.

## Effet de bord

Il **émet** sur le connecteur de sortie du sous-périphérique retenu. Il ne change aucun profil et
ne touche à aucune entrée.
