# Chemin de données DeckLink → bus MXL — chantier `decklink_io` (docs/chantiers/DECKLINK.md).
#
# ⚠ NE SE CONSTRUIT PAS ICI. Il lui faut les en-têtes ET la bibliothèque libmxl, qui vivent dans
# l'image d'exécution (`plugins/_compute_runtime`), pas sur l'orchestrateur. La construction
# définitive se fera dans l'image du plugin. Ce Makefile sert au contrôle de syntaxe et à la
# vérification d'UUID, qui n'ont besoin ni de l'une ni de l'autre.
#
#   make verif        → l'UUID de flux natif est-il identique à celui de bobimxl ?
#   tools/en_tetes_mxl.sh → récupère les en-têtes libmxl au tag épinglé par le Dockerfile
#   make syntaxe      → les DEUX chemins de données compilent-ils ?

CXX      ?= g++
# ★ `-Werror=format` : un format qui ne correspond pas à ses arguments est un comportement
#   INDÉFINI, pas un détail de style. En simple avertissement il passait inaperçu — la
#   cible affichait « ok » juste en dessous. Vécu le 20/10 en ajoutant quatre compteurs
#   audio à la ligne de statistiques : le format était allongé, pas la liste d'arguments.
CXXFLAGS ?= -Wall -Wextra -std=c++11 -fno-rtti -Wno-deprecated-declarations -Werror=format
# Les en-têtes de libmxl n'existent pas sur l'orchestrateur : `tools/en_tetes_mxl.sh` les
# récupère au tag que le Dockerfile épingle, et la cible les trouve toute seule.
MXL_REF     := $(shell sed -n 's/^ARG MXL_REF=\(.*\)$$/\1/p' ../_compute_runtime/Dockerfile 2>/dev/null | head -1)
MXL_INCLUDE ?= $(if $(wildcard tools/.mxl-include/$(MXL_REF)/mxl/flow.h),tools/.mxl-include/$(MXL_REF),/usr/local/include)

verif: verif_flow_id verif_anc
	./verif_flow_id.py
	./verif_anc.py

verif_anc: verif_anc.cpp anc_rfc8331.h
	$(CXX) -O1 $(CXXFLAGS) -o $@ $<

verif_flow_id: verif_flow_id.cpp
	$(CXX) -O1 $(CXXFLAGS) -o $@ $< -lcrypto

# ★ LES DEUX BINAIRES, PAS UN SEUL. `decklink_tx.cpp` était absent de cette cible — et c'est
#   précisément là qu'une chaîne coupée par un « \n » non échappé est passée inaperçue le 19/10,
#   pour un aller-retour complet de construction d'image. Une cible qui ne couvre qu'une moitié
#   du code donne la même confiance qu'une cible complète, sans la rendre.
syntaxe: $(wildcard tools/.mxl-include/$(MXL_REF)/mxl/flow.h)
	@test -f "$(MXL_INCLUDE)/mxl/flow.h" || { \
	    echo "en-têtes libmxl absentes — lancez d'abord : tools/en_tetes_mxl.sh"; exit 1; }
	$(CXX) -fsyntax-only $(CXXFLAGS) -I. -Idecklink_sdk -I$(MXL_INCLUDE) decklink_rx.cpp
	@echo "  ok   decklink_rx.cpp compile"
	$(CXX) -fsyntax-only $(CXXFLAGS) -I. -Idecklink_sdk -I$(MXL_INCLUDE) decklink_tx.cpp
	@echo "  ok   decklink_tx.cpp compile"

clean:
	rm -f verif_flow_id verif_anc

.PHONY: verif syntaxe clean
