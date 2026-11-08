// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 BOBI SAS, France
//
// Contrôle : l'UUID de flux calculé par `decklink_rx.cpp` est-il IDENTIQUE à celui de
// `bobimxl.flow_id()` ? Une divergence d'un seul bit rendrait nos flux introuvables par les
// consommateurs Python — sans aucune erreur, juste un flux que personne ne lit.
//
// Ne s'exécute pas seul : `verif_flow_id.py` lui donne les noms et compare.

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <openssl/sha.h>

static const uint8_t NS_BOBI[16] = {
    0xd4,0xe7,0x7c,0xba,0x0e,0x52,0x55,0xd9,0x82,0xed,0x72,0x26,0xb7,0xbd,0xa7,0x57};

static void flow_id_str(const char *name, char out[37]) {
    SHA_CTX c; unsigned char h[20];
    SHA1_Init(&c); SHA1_Update(&c, NS_BOBI, 16);
    SHA1_Update(&c, name, strlen(name)); SHA1_Final(h, &c);
    h[6] = (unsigned char)((h[6] & 0x0f) | 0x50);
    h[8] = (unsigned char)((h[8] & 0x3f) | 0x80);
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             h[0],h[1],h[2],h[3],h[4],h[5],h[6],h[7],h[8],h[9],h[10],h[11],h[12],h[13],h[14],h[15]);
}

int main(int argc, char **argv) {
    char id[37];
    for (int i = 1; i < argc; i++) { flow_id_str(argv[i], id); printf("%s\n", id); }
    return 0;
}
