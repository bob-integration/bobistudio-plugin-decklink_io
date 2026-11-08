# Composants tiers — `decklink_io`

Ce plugin est publié sous **GPL-3.0-or-later** (voir `LICENSE`). Il embarque un composant tiers,
qui garde sa propre licence.

## En-têtes du SDK Blackmagic DeckLink

| | |
|---|---|
| Composant | **Blackmagic DeckLink SDK**, en-têtes seuls |
| Version | `12.2.2` |
| Emplacement | `decklink_sdk/` — 9 fichiers |
| Rôle | déclarations de l'API, nécessaires pour compiler `decklink_rx`, `decklink_tx` et la sonde d'énumération |
| Origine | source Debian `gst-plugins-bad1.0` 1.26.2 (`sys/decklink/linux/`), composant `main` |

### La licence autorise cette redistribution, sous condition

Le bloc `-LICENSE-START-` en tête de chaque fichier accorde le droit d'utiliser, reproduire,
distribuer et sous-licencier, ainsi que de préparer des travaux dérivés. Il le fait selon l'EULA
du SDK si les fichiers viennent de Blackmagic, ou **selon les termes notifiés par le tiers**
s'ils viennent d'ailleurs. C'est notre cas : ils viennent de Debian, qui les publie dans `main`.

Deux chemins indépendants mènent donc au même résultat. Le second est l'EULA elle-même : sa
**clause 0.1** exclut les clauses 1, 4.3, 4.4, 5, 7 et 8 pour les fichiers situés sous
`/Mac/Include`, `/Win/Include` et `/Linux/Include`. Ce sont précisément les clauses
restrictives, et nos 9 fichiers sont des `Include`.

⚠ **La condition à tenir est simple.** Les mentions de copyright et le bloc de licence entier
sont conservés en tête de chaque fichier. **Ne pas éditer ces fichiers.**

### Le pilote, lui, n'est pas redistribuable

Le pilote **Desktop Video** ne se trouve ni ici ni dans l'image d'exécution. Il s'installe à la
main sur la machine qui porte la carte. Seuls les en-têtes sont présents.

## Marques Blackmagic

« DeckLink » est une marque de **Blackmagic Design Pty. Ltd.** L'EULA du SDK définit les *Marks*
largement (clause 6.3) : toute marque de Blackmagic ou de ses affiliés, « including but not
limited to » DeckLink, Multibridge, Intensity, HDLink, Videohub, UltraScope. « Blackmagic » en
fait partie.

⚠ La clause 6 n'est **pas** dans la liste d'exclusions de la clause 0.1. Elle s'applique donc
pleinement, même à qui ne redistribue que les en-têtes.

**Ce qu'elle autorise.** La clause 6.1 interdit d'adopter une marque, une marque de service ou
un nom commercial qui *inclut* une Marque. Cette interdiction survit à la fin du contrat. La
clause 6.2 autorise deux désignations, et seulement elles, pendant la durée du contrat :
« XXX compatible with Blackmagic Design YYY » et « XXX for Blackmagic Design YYY ». Cette
autorisation est révocable.

**Ce que ce plugin fait.** Son libellé d'interface emploie la forme 6.2. Ailleurs, la Marque
n'apparaît qu'en texte descriptif, pour dire quel matériel le plugin pilote. Aucun logo.

**Les identifiants techniques l'emploient nominativement.** Le type `decklink_io`, le nom du
dépôt et celui des binaires natifs nomment la Marque. Ce n'est pas un nom commercial adopté
pour notre produit, qui s'appelle Bobi.Studio, mais un identifiant qui dit quel matériel le
composant pilote.

⚠ Cette distinction entre identifiant technique et nom commercial **n'a pas été validée par un
conseil juridique**.
