// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 BOBI SAS, France
//
// Sérialise des paquets ANC lus sur l'entrée standard (JSON simplifié) et rend le grain en hexa,
// pour comparaison octet pour octet avec `bobimxl.anc_pack_rfc8331`. Cf. verif_anc.py.
//
// Format d'entrée, une ligne par cas : field;did,sdid,line,hori,c,s,stream:UDW_hexa|did,...

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "anc_rfc8331.h"

// Mode « atc » : hh mm ss ff df sur l'entrée → les 16 UDW en hexa, pour comparaison avec
// bobimxl.anc_atc_encode.
static int mode_atc(void) {
    int h, m, s, f, df;
    while (scanf("%d %d %d %d %d", &h, &m, &s, &f, &df) == 5) {
        uint8_t udw[16];
        bobi_atc_encode(h, m, s, f, df, udw);
        for (int i = 0; i < 16; i++) printf("%02x", udw[i]);
        printf("\n");
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "atc")) return mode_atc();
    char ligne[65536];
    while (fgets(ligne, sizeof ligne, stdin)) {
        char *nl = strchr(ligne, '\n'); if (nl) *nl = 0;
        if (!*ligne) continue;
        char *sep = strchr(ligne, ';'); if (!sep) continue;
        *sep = 0;
        const unsigned field = (unsigned)atoi(ligne);
        bobi_anc_packet pk[32]; size_t n = 0;
        char *save = NULL;
        for (char *tok = strtok_r(sep + 1, "|", &save); tok && n < 32;
             tok = strtok_r(NULL, "|", &save)) {
            char *colon = strchr(tok, ':'); if (!colon) continue;
            *colon = 0;
            unsigned did, sdid, line, hori, c, s, sn;
            if (sscanf(tok, "%u,%u,%u,%u,%u,%u,%u", &did, &sdid, &line, &hori, &c, &s, &sn) != 7)
                continue;
            memset(&pk[n], 0, sizeof pk[n]);
            pk[n].did = (uint8_t)did; pk[n].sdid = (uint8_t)sdid;
            pk[n].line = (uint16_t)line; pk[n].hori = (uint16_t)hori;
            pk[n].c = (uint8_t)c; pk[n].s = (uint8_t)s; pk[n].stream_num = (uint8_t)sn;
            const char *h = colon + 1;
            size_t L = strlen(h) / 2;
            if (L > 255) L = 255;
            for (size_t k = 0; k < L; k++) {
                unsigned b; sscanf(h + 2 * k, "%2x", &b); pk[n].udw[k] = (uint8_t)b;
            }
            pk[n].udw_len = (uint16_t)L;
            n++;
        }
        uint8_t buf[65536];
        const size_t taille = bobi_anc_pack(pk, n, field, buf, sizeof buf);
        for (size_t i = 0; i < taille; i++) printf("%02x", buf[i]);
        printf("\n");
    }
    return 0;
}
