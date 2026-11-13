/* SPDX-License-Identifier: GPL-3.0-or-later
   The mods list between the co-op module's Mods tab and Hookshot's mod loader (oot-remake-multiplayer, docs/mods.md).

   Hookshot indexes its mods folder once, at the start, before the module is loaded, and keeps the game's files open
   while it runs. So the tab never renames or moves a mod: it writes one small file into the mods folder, and Hookshot's
   host/mods.c (our patch 0011) reads it at the next start:

     <mods>/coop-mods.list      written by the module: the order and the switches
         coop-mods 1
         on <name>              first row = wins a file two mods have (it is indexed last)
         off <name>             left out
       <name> is the mod's name as Hookshot logs it: its path under the mods folder with '/'. A mod the list does not
       name is on and keeps Hookshot's own order (by name) under the named ones. Lines it does not know are skipped.

     <mods>/coop-mods.applied   written by Hookshot at each start: what this run indexed
         coop-mods-applied 1 <process id>
         on <files> <name>      in the order indexed (the last one wins)
         off <name>

   The same file is in Hookshot's host/ through our patch and in our repository (hookshot/src): keep them equal
   (tests/hookshot/mods_list_test.cpp checks it). Plain C, header only, no Hookshot or co-op include. */
#ifndef HOS_COOP_MODS_H
#define HOS_COOP_MODS_H
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define HOS_COOP_MODS_VERSION 1
#define HOS_COOP_MODS_LIST_FILE "coop-mods.list"
#define HOS_COOP_MODS_APPLIED_FILE "coop-mods.applied"
#define HOS_COOP_MODS_MAX 256    /* rows read; more are skipped */
#define HOS_COOP_MODS_NAME 260   /* a name with its 0 (MAX_PATH) */
/* The most a list file can hold that is read: a caller's buffer for it. */
#define HOS_COOP_MODS_TEXT_MAX (HOS_COOP_MODS_MAX * (HOS_COOP_MODS_NAME + 8) + 64)

typedef struct HosCoopModsRow {
    char name[HOS_COOP_MODS_NAME];
    int off;
} HosCoopModsRow;

typedef struct HosCoopModsList {
    int count;
    HosCoopModsRow rows[HOS_COOP_MODS_MAX];
} HosCoopModsList;

/* Names are compared as Hookshot sorts them: ASCII letters without case; '\' counts as '/'. */
static inline int hos_coop_mods_fold(int c) {
    if (c == '\\') return '/';
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}
static inline int hos_coop_mods_same(const char *a, const char *b) {
    for (;; a++, b++) {
        if (hos_coop_mods_fold((unsigned char)*a) != hos_coop_mods_fold((unsigned char)*b)) return 0;
        if (!*a) return 1;
    }
}

/* Reads a list file's text (n bytes; no 0 needed). 1 when it is a list of this version; else 0 and an empty list. */
static inline int hos_coop_mods_parse(const char *text, size_t n, HosCoopModsList *out) {
    size_t at = 0;
    int line_no = 0;
    out->count = 0;
    while (at < n) {
        size_t end = at, len;
        const char *line = text + at;
        while (end < n && text[end] != '\n') end++;
        len = end - at;
        at = end + 1;
        if (len && line[len - 1] == '\r') len--;
        if (line_no++ == 0) {
            if (len != 11 || memcmp(line, "coop-mods 1", 11)) return 0;
            continue;
        }
        {
            int off;
            int i, known = 0;
            if (len > 3 && !memcmp(line, "on ", 3)) off = 0, line += 3, len -= 3;
            else if (len > 4 && !memcmp(line, "off ", 4)) off = 1, line += 4, len -= 4;
            else continue;
            if (len >= HOS_COOP_MODS_NAME || out->count >= HOS_COOP_MODS_MAX) continue;
            memcpy(out->rows[out->count].name, line, len);
            out->rows[out->count].name[len] = 0;
            for (i = 0; i < out->count && !known; i++)   /* (a name twice: its first row counts) */
                known = hos_coop_mods_same(out->rows[i].name, out->rows[out->count].name);
            if (known) continue;
            out->rows[out->count++].off = off;
        }
    }
    return line_no > 0;
}

/* A mod's place: -1 = off; 0 = not in the list (Hookshot's own order, under the listed ones); else the higher wins
   (the list's first row has the highest). Mods are indexed by rising rank, then by name. */
static inline int hos_coop_mods_rank(const HosCoopModsList *list, const char *name) {
    int i;
    for (i = 0; i < list->count; i++)
        if (hos_coop_mods_same(list->rows[i].name, name)) return list->rows[i].off ? -1 : list->count - i;
    return 0;
}

/* The applied file's lines (each with its '\n'); what snprintf returns. */
static inline int hos_coop_mods_applied_head(char *out, size_t cap, unsigned long pid) {
    return snprintf(out, cap, "coop-mods-applied %d %lu\n", HOS_COOP_MODS_VERSION, pid);
}
static inline int hos_coop_mods_applied_row(char *out, size_t cap, const char *name, int files) {   /* files < 0: off */
    return files < 0 ? snprintf(out, cap, "off %s\n", name) : snprintf(out, cap, "on %d %s\n", files, name);
}

#endif
