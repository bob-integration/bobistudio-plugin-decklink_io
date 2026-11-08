# `decklink_in` — banc de capture

Chantier `decklink_io` (`docs/chantiers/DECKLINK.md`), jalon 4. Essaie **chaque sous-périphérique
actif capable de capture** et dit, pour chacun : verrou de signal, format détecté, trames reçues.

```
make && ./decklink_in [secondes]      # défaut : 4 s par sous-périphérique
```

## Deux choses qu'il départage

**1. Un « pas de verrou » lu à froid est-il fiable ?** La sonde lit
`bmdDeckLinkStatusVideoInputSignalLocked` *sans* activer l'entrée, alors que le manuel ne fait
cette lecture qu'**après** `EnableVideoInput` (§2.4.13.1). Ce banc lit le même statut **avant et
après** activation, pour que « pas de signal » ne puisse pas être un artefact de méthode.
Constaté le 2026-09-08 : les deux lectures concordent — la lecture à froid est fiable sur cette
carte.

**2. L'exigence du §1 : pas de trame fabriquée en silence.** Quand rien n'entre, la carte livre
quand même des trames, marquées `bmdFrameHasNoInputSource`. Elles sont comptées **à part** et ne
doivent jamais passer pour valides. Vérifié : sans câble, 118 trames reçues en 4 s, **0 valide**.

## Détection automatique du format

Le banc part du premier mode déclaré puis laisse `VideoInputFormatChanged` corriger — et
reconfigure l'entrée sur le format annoncé, sans quoi les trames restent invalides. Le format
n'est jamais supposé.
