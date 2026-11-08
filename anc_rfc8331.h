// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 BOBI SAS, France
// Auteur : Cyril Mazouer, pour le compte de BOBI SAS
// Distribué sous licence GNU GPL v3 (ou ultérieure) ; voir le fichier LICENSE.
//
// Empaquetage ANC RFC 8331 — MIROIR EXACT de `bobimxl.anc_pack_rfc8331`.
//
// ★ Pourquoi c'est risqué et pourquoi il y a un contrôle croisé (`verif_anc.py`) : un grain ANC
//   mal sérialisé se lit SANS ERREUR. Le consommateur y trouve « ANC count: 0 » ou des paquets
//   fantaisistes, et rien, nulle part, ne signale que l'octet 7 était faux. La seule défense est
//   de comparer octet pour octet à l'implémentation de référence.
//
// Représentation : les UDW sont sur 8 BITS de données ; les bits 8 et 9 du mot SMPTE 291 sont la
// parité et son inverse, CALCULÉS ici. Ce n'est pas une troncature — c'est la définition du mot.

#ifndef BOBI_ANC_RFC8331_H
#define BOBI_ANC_RFC8331_H

#include <stdint.h>
#include <string.h>
#include <stddef.h>

#define BOBI_ANC_HDR 6u        /* Length(2) + ANC_Count(1) + F/rsvd(1) + rsvd(2) */

typedef struct {
    uint8_t  did, sdid;
    uint16_t line;             /* 11 bits */
    uint16_t hori;             /* 12 bits ; 0xFFF = indéfini */
    uint8_t  c, s, stream_num;
    uint8_t  udw[256];
    uint16_t udw_len;          /* Data_Count, ≤ 255 */
} bobi_anc_packet;

/* Mot 10 bits SMPTE 291 : b8 = parité PAIRE de b0-b7, b9 = ~b8. */
static inline uint16_t bobi_anc_parity10(uint8_t v8) {
    unsigned p = 0, x = v8;
    while (x) { p ^= (x & 1u); x >>= 1; }
    return (uint16_t)(v8 | (p << 8) | ((p ^ 1u) << 9));
}

/* Checksum_Word : somme des 9 bits de poids faible des mots DÉJÀ paritaires, modulo 512, b9 = ~b8. */
static inline uint16_t bobi_anc_checksum10(const uint16_t *mots, size_t n) {
    unsigned s = 0;
    for (size_t i = 0; i < n; i++) s += (mots[i] & 0x1FFu);
    s &= 0x1FFu;
    return (uint16_t)(s | ((((s >> 8) & 1u) ^ 1u) << 9));
}

/* Écriture de bits MSB d'abord (ordre réseau du RFC). */
typedef struct { uint8_t *out; size_t cap, len; uint64_t acc; unsigned n; } bobi_bitw;

static inline void bobi_bw_write(bobi_bitw *w, uint32_t val, unsigned bits) {
    w->acc = (w->acc << bits) | (uint64_t)(val & ((1u << bits) - 1u));
    w->n += bits;
    while (w->n >= 8) {
        w->n -= 8;
        if (w->len < w->cap) w->out[w->len] = (uint8_t)((w->acc >> w->n) & 0xFF);
        w->len++;                            /* on compte même au-delà : détection de dépassement */
    }
}

static inline void bobi_bw_align32(bobi_bitw *w) {
    if (w->n) bobi_bw_write(w, 0, 8 - w->n);          /* d'abord compléter l'octet courant */
    while (w->len & 3u) bobi_bw_write(w, 0, 8);
}

/* Sérialise `n` paquets. Renvoie la taille écrite, ou 0 si ça ne tient pas dans `cap`.
 * `field` : 0b00 progressif/indéfini, 0b10 champ 1, 0b11 champ 2. */
static inline size_t bobi_anc_pack(const bobi_anc_packet *pk, size_t n, unsigned field,
                                   uint8_t *dst, size_t cap) {
    if (cap < BOBI_ANC_HDR) return 0;
    bobi_bitw w;
    w.out = dst + BOBI_ANC_HDR; w.cap = cap - BOBI_ANC_HDR; w.len = 0; w.acc = 0; w.n = 0;
    for (size_t i = 0; i < n; i++) {
        const bobi_anc_packet *p = &pk[i];
        if (p->udw_len > 255) return 0;
        bobi_bw_align32(&w);
        bobi_bw_write(&w, p->c & 1u, 1);
        bobi_bw_write(&w, p->line & 0x7FFu, 11);
        bobi_bw_write(&w, p->hori & 0xFFFu, 12);
        bobi_bw_write(&w, p->s & 1u, 1);
        bobi_bw_write(&w, p->stream_num & 0x7Fu, 7);
        uint16_t mots[3 + 256];
        size_t m = 0;
        mots[m++] = bobi_anc_parity10(p->did);
        mots[m++] = bobi_anc_parity10(p->sdid);
        mots[m++] = bobi_anc_parity10((uint8_t)p->udw_len);
        for (uint16_t k = 0; k < p->udw_len; k++) mots[m++] = bobi_anc_parity10(p->udw[k]);
        for (size_t k = 0; k < m; k++) bobi_bw_write(&w, mots[k], 10);
        bobi_bw_write(&w, bobi_anc_checksum10(mots, m), 10);
        bobi_bw_align32(&w);
    }
    if (w.len > w.cap) return 0;                     /* dépassement : on REFUSE, on ne tronque pas */
    dst[0] = (uint8_t)((w.len >> 8) & 0xFF);
    dst[1] = (uint8_t)(w.len & 0xFF);
    dst[2] = (uint8_t)(n & 0xFF);
    dst[3] = (uint8_t)((field & 0x3u) << 6);
    dst[4] = 0; dst[5] = 0;
    return BOBI_ANC_HDR + w.len;
}

/* ── ATC (SMPTE ST 12-2 / RP 188, DID 0x60 SDID 0x60) ──────────────────────────────────────
 * Timecode → les 16 UDW d'un paquet ATC. MIROIR EXACT de `bobimxl.anc_atc_encode` et de
 * `decode_atc` dans mtl_rx.c — les trois changent ENSEMBLE.
 *
 * ⚠ Disposition LTC : UN CHIFFRE BCD PAR QUARTET, placé dans le quartet **HAUT** de son UDW ;
 *   les UDW d'index impair portent les drapeaux. Deux erreurs successives ont été corrigées
 *   là-dessus le 2026-08-07 (quartets bas → timecode figé à 00:00:00:00 ; puis deux quartets
 *   recollés en un octet → DIZAINES D'IMAGES PERDUES, invisible sous dix images). Ne pas
 *   « simplifier » cette fonction sans relire ce commentaire.
 *
 * ⚠ `tools/bancs/anc_tc_gen.py` (dépôt principal) est RESTÉ sur l'ancienne disposition : il
 *   écrit dans les quartets bas. Il ne doit PAS servir de référence. */
static inline void bobi_atc_encode(int h, int m, int s, int f, int drop_frame, uint8_t udw[16]) {
    int q[16];
    for (int i = 0; i < 16; i++) q[i] = 0;
    q[0]  = f % 10;
    q[2]  = ((f / 10) & 0x3) | (drop_frame ? 0x4 : 0);
    q[4]  = s % 10;
    q[6]  = (s / 10) & 0x7;
    q[8]  = m % 10;
    q[10] = (m / 10) & 0x7;
    q[12] = h % 10;
    q[14] = (h / 10) & 0x3;
    for (int i = 0; i < 16; i++) udw[i] = (uint8_t)((q[i] & 0x0F) << 4);
}

#endif
