# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 BOBI SAS, France
# bobi-decklink image — Blackmagic data path of the decklink_io plugin.
#
# ★ WHY THIS IMAGE EXISTS (2026-11-01). These four binaries were compiled INSIDE bobi-compute,
#   that is, inside the base image shared by EVERY non-2110 plugin. Three consequences, all at
#   odds with what the product claims — you install what you need:
#     · a node with no DeckLink card carried the binaries anyway;
#     · the base image COULD NOT be built without this plugin's sources. Reported by an outside
#       user on 2026-09-23: the public repository ships the recipe but not the plugin (private
#       submodule), so the build context was refused, NO image could be built, and the product
#       did nothing at all;
#     · any fix to the DeckLink path rebuilt everyone's base image.
#   Same shape as bobi-gui and bobi-compute-gpu: a thin layer FROM bobi-compute, `node_only`,
#   built only on the nodes that DECLARE the "decklink" capability. The others pay nothing for
#   it, and the base image has no dependency on this plugin left.
#
# ★ NOTHING OF BLACKMAGIC'S IS SHIPPED HERE. Only the SDK HEADERS go in (their licence allows
#   third-party redistribution, see THIRD-PARTY-NOTICES.md); `libDeckLinkAPI.so` is mounted from
#   the host AT RUNTIME (docker_compute, `needs_decklink` trigger), the Desktop Video driver being
#   installed by hand on the node.
#
# The MXL headers come from the base image (bobi-compute >= 0.81 keeps them under
# /usr/local/include — that is what makes a thin layer compiling against the bus possible).
#
# Build (context = repository root, like the other _IMAGES; the bobi-compute:<ver> base image is
# already on the node):
#   docker build -t bobi-decklink:0.2.0 -f plugins/decklink_io/image/Dockerfile .
ARG BASE_IMAGE=bobi-compute:0.81
FROM ${BASE_IMAGE} AS decklink-builder
ENV DEBIAN_FRONTEND=noninteractive
# The -dev packages do not go into the shipped image: this stage is thrown away.
RUN apt-get update && apt-get install -y --no-install-recommends g++ libssl-dev \
 && rm -rf /var/lib/apt/lists/*
COPY plugins/decklink_io/decklink_sdk /src/decklink_sdk
COPY plugins/decklink_io/anc_rfc8331.h /src/anc_rfc8331.h
COPY plugins/decklink_io/decklink_rx.cpp /src/decklink_rx.cpp
COPY plugins/decklink_io/decklink_tx.cpp /src/decklink_tx.cpp
# ★ The PROBE goes into the image too (2026-09-21). A card's inventory — connectors, roles,
#   duplex, INACTIVE connectors — can only be read from inside the container: it is the one with
#   the devices mounted, and the controller never runs plugin code. Without it the operator had to
#   find the `persistent_id` values on the command line to fill a JSON field by hand. It is only
#   ever called by script.py (/usr/local/bin/decklink_probe), hence from THIS container: moving it
#   out of the shared base image takes nothing away from anyone.
COPY plugins/decklink_io/tools/probe/decklink_probe.cpp /src/decklink_probe.cpp
# The PROFILE tool goes in as well: changing a card's profile is an operator action, and it cannot
# require a command line on the node (decision of 2026-10-16). It carries its own safeguards —
# refusal when an input is locked, targeting by stable identity.
COPY plugins/decklink_io/tools/profile/decklink_profile.cpp /src/decklink_profile.cpp
WORKDIR /src
RUN for b in decklink_rx decklink_tx decklink_probe decklink_profile; do \
      g++ -O2 -Wall -Wextra -Wno-deprecated-declarations -std=c++11 -fno-rtti \
        -I/src -I/usr/local/include -o /$b $b.cpp decklink_sdk/DeckLinkAPIDispatch.cpp \
        -L/usr/local/lib -lmxl -lcrypto -ldl -lpthread || exit 1; \
    done

FROM ${BASE_IMAGE}
COPY --from=decklink-builder /decklink_rx /usr/local/bin/decklink_rx
COPY --from=decklink-builder /decklink_tx /usr/local/bin/decklink_tx
COPY --from=decklink-builder /decklink_probe /usr/local/bin/decklink_probe
COPY --from=decklink-builder /decklink_profile /usr/local/bin/decklink_profile
# ⚠ RUNTIME-STAGE SAFEGUARD — the builder stage proves NOTHING about the shipped image (same
# reason as the `ldd` check in _compute_runtime: a binary that compiles may still be missing a
# library at runtime). What is required here: libmxl and the rest resolve.
#
# ★ MEASURED on 2026-11-01 on r620-2 (a node WITHOUT a card): `ldd` reports NO missing library, not
#   even libDeckLinkAPI.so. That is expected, and the dispatch is why — `DeckLinkAPIDispatch.cpp`
#   loads the library with `dlopen` at runtime, so it is not a dynamic dependency of the binary.
#   The exclusion below therefore filters nothing today: it is a net for the day someone links the
#   API directly, in which case the library's absence on a card-less node must not fail the BUILD
#   (it is mounted from the host at `docker run`, see docker_compute/needs_decklink).
RUN for b in decklink_rx decklink_tx decklink_probe decklink_profile; do \
      if ldd /usr/local/bin/$b 2>&1 | grep "not found" | grep -qv "libDeckLinkAPI"; then \
        echo "FAILED: missing dependency in /usr/local/bin/$b" >&2; \
        ldd /usr/local/bin/$b 2>&1 | grep "not found" >&2; exit 1; \
      fi; \
    done
