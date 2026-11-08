# Image d'ESSAI du chemin de données DeckLink → MXL (chantier decklink_io, jalon 4).
#
# Elle ne reconstruit PAS libmxl : l'image d'exécution la porte déjà, et la reprendre telle quelle
# garantit l'ABI. Seuls les EN-TÊTES sont apportés (dépôt MXL public, même version que l'image).
#
# ⚠ Rien de Blackmagic n'est embarqué : `libDeckLinkAPI.so` est montée depuis l'hôte au `docker
#   run`, conformément à la décision du §5 (le pilote s'installe à la main, rien ne se distribue).
#   Seuls les EN-TÊTES DeckLink sont dans le contexte, et leur licence l'autorise (§8).

ARG BASE=bobi-compute:0.34

FROM ${BASE} AS base

FROM debian:trixie-slim AS builder
RUN apt-get update && apt-get install -y --no-install-recommends \
      g++ libssl-dev ca-certificates && rm -rf /var/lib/apt/lists/*
COPY --from=base /usr/local/lib/libmxl.so* /usr/local/lib/
RUN ldconfig
COPY mxl/mxl /usr/local/include/mxl/
COPY src/ /src/
WORKDIR /src
RUN g++ -O2 -Wall -Wextra -Wno-deprecated-declarations -std=c++11 -fno-rtti \
      -I/src -I/usr/local/include \
      -o /decklink_rx decklink_rx.cpp decklink_sdk/DeckLinkAPIDispatch.cpp \
      -L/usr/local/lib -lmxl -lcrypto -ldl -lpthread
FROM ${BASE}
COPY --from=builder /decklink_rx /usr/local/bin/decklink_rx
# Contrôle dans l'image FINALE — c'est le seul endroit où il veut dire quelque chose : le binaire
# ne doit dépendre de rien qui y manque. `libDeckLinkAPI.so` n'y figure pas et c'est normal : elle
# est chargée par `dlopen`, donc invisible à ldd, et montée depuis l'hôte au `docker run`.
RUN ldd /usr/local/bin/decklink_rx | grep -q "not found" \
      && { echo "DÉPENDANCE MANQUANTE :"; ldd /usr/local/bin/decklink_rx | grep "not found"; exit 1; } \
      || echo "toutes les dépendances sont résolues dans l image finale"
