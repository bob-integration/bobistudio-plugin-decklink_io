# Sonde d'énumération DeckLink — chantier `decklink_io` (docs/chantiers/DECKLINK.md).
#
# Se compile SANS pilote Blackmagic installé : les en-têtes suffisent, et la bibliothèque
# libDeckLinkAPI.so n'est chargée qu'à l'exécution (dlopen). C'est ce qui permet d'écrire et de
# vérifier la sonde avant que la carte n'arrive.

CXX      ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++11 -fno-rtti
LDLIBS   := -ldl -lpthread

all: decklink_probe

decklink_probe: decklink_probe.cpp decklink_sdk/DeckLinkAPIDispatch.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

# Vérification sans matériel : construit une fausse libDeckLinkAPI.so qui répond « zéro carte »,
# et contrôle que la sonde la DISTINGUE de l'absence de bibliothèque. Cf. verif_sans_carte.sh.
verif: decklink_probe couverture
	./verif_sans_carte.sh

# Couverture des en-têtes vendorisés : tout ce dont le chantier dépend est-il DÉCLARÉ, avec les
# bons types ? Jamais exécuté — le verdict est la compilation elle-même.
couverture: verif_entetes.cpp
	$(CXX) -c -O0 -Wall -Wextra -std=c++11 -fno-rtti -o /dev/null $<
	@echo "  ok   couverture des en-têtes 12.2.2"

clean:
	rm -f decklink_probe stub/libDeckLinkAPI.so
	rmdir stub 2>/dev/null || true

.PHONY: all verif clean
