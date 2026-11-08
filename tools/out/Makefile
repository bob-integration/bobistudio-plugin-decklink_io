# Banc de sortie DeckLink — mesure le pont d'horloge, la cadence et la santé de la lecture
# ordonnancée. Exige une carte : ne tourne pas en CI. Cf. docs/chantiers/DECKLINK.md §3 et §4.
CXX      ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++11 -fno-rtti
LDLIBS   := -ldl -lpthread -lm

all: decklink_out

decklink_out: decklink_out.cpp decklink_sdk/DeckLinkAPIDispatch.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f decklink_out

.PHONY: all clean
