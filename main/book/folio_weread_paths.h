#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifndef FOLIO_SD_ROOT
#define FOLIO_SD_ROOT "/sdcard"
#endif
/* Stable book identity across the legacy pointer and public EPUB filename. */
static inline bool folio_wr_path_id(const char *path, char *out, size_t cap) {
    const char prefix[] = FOLIO_SD_ROOT "/books/";
    if (!path || !out || !cap || strncmp(path, prefix, sizeof(prefix) - 1))
        return false;
    const char *base = path + sizeof(prefix) - 1;
    if (strchr(base, '/') || strchr(base, '\\'))
        return false;
    const char *start = NULL;
    size_t hexlen = 0, n = strlen(base);
    if (n > 7 && !strncmp(base, "wr-", 3) && !strcmp(base + n - 4, ".wrb")) {
        start = base + 3;
        hexlen = n - 7;
    } else if (n > 11 && !strcmp(base + n - 6, "].epub")) {
        for (const char *p = base; (p = strstr(p, " [wr-")) != NULL; ++p)
            start = p + 5;
        if (start)
            hexlen = (size_t)(base + n - 6 - start);
    }
    if (!start || !hexlen || (hexlen & 1) || hexlen > 126 || hexlen / 2 >= cap)
        return false;
    for (size_t i = 0; i < hexlen / 2; ++i) {
        unsigned v = 0;
        for (unsigned j = 0; j < 2; ++j) {
            char c = start[2 * i + j];
            unsigned d = c >= '0' && c <= '9'   ? (unsigned)(c - '0')
                         : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
                                                : 99;
            if (d > 15)
                return false;
            v = v * 16 + d;
        }
        if (!((v >= 'a' && v <= 'z') || (v >= 'A' && v <= 'Z') || (v >= '0' && v <= '9') ||
              v == '_' || v == '-'))
            return false;
        out[i] = (char)v;
    }
    out[hexlen / 2] = 0;
    return true;
}
static inline bool folio_wr_identity(const char *path, char *out, size_t cap) {
    char id[64], hex[127];
    const char *digits = "0123456789abcdef";
    if (!folio_wr_path_id(path, id, sizeof(id)))
        return false;
    size_t n = strlen(id);
    for (size_t i = 0; i < n; ++i) {
        hex[2 * i] = digits[(uint8_t)id[i] >> 4];
        hex[2 * i + 1] = digits[(uint8_t)id[i] & 15];
    }
    hex[2 * n] = 0;
    int k = snprintf(out, cap, FOLIO_SD_ROOT "/books/wr-%s.wrb", hex);
    return k > 0 && (size_t)k < cap;
}
static inline bool folio_wr_aux(const char *path, const char *name, char *out, size_t cap) {
    char identity[256];
    if (!folio_wr_identity(path, identity, sizeof(identity)))
        return false;
    const char *hex = identity + strlen(FOLIO_SD_ROOT "/books/wr-");
    int k =
        snprintf(out, cap, FOLIO_SD_ROOT "/.folio/weread/%.*s/%s", (int)strlen(hex) - 4, hex, name);
    return k > 0 && (size_t)k < cap;
}
