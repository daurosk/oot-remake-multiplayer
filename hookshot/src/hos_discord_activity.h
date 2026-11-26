/* SPDX-License-Identifier: GPL-3.0-or-later
   The Discord status's activity text (oot-remake-multiplayer's patch 0026, Longshot's host/discord_rpc.c).

   The same file is in Longshot's host/ through the patch and in our repository (hookshot/src): keep them equal
   (tests/hookshot/module_test.cpp checks it; tests/hookshot/discord_activity_test.cpp checks what it writes).
   Plain C with static inline functions only: no state, no I/O, so the tests drive it as discord_rpc.c does.

   With no setting and no module line the SET_ACTIVITY frame is byte for byte the one Longshot wrote before the patch:
   details "<name>", state and the art's hover text "<name> <version>", the art <default image>. A setting replaces one
   of them; discord_image=none sends no art at all (Discord then shows the application's own icon). */
#ifndef HOS_DISCORD_ACTIVITY_H
#define HOS_DISCORD_ACTIVITY_H
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* The settings' words: each "" when longshot_options.ini has no usable line for it. */
typedef struct HosDiscordWords {
    char image[256];        /* discord_image=: an art asset key or an https image; "none": no art */
    char details[128];      /* discord_details= */
    char state[128];        /* discord_state= */
    char large_text[128];   /* discord_large_text= */
} HosDiscordWords;

/* 1 for an art asset key or an image's https address: letters, digits and _ - . : / ? = & %, 1 to 255 of them */
static inline int hos_discord_valid_image(const char *s) {
    const size_t n = strlen(s);
    if (!n || n > 255) return 0;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || strchr("_-.:/?=&%", *s)))
            return 0;
    return 1;
}

/* The length of the whole UTF-8 sequence at s (1 to 4), 0 when it is not one (a stray or cut byte, an overlong form,
   a surrogate, past U+10FFFF) */
static inline size_t hos_discord_utf8_len(const unsigned char *s) {
    const unsigned char c = s[0];
    if (c < 0x80) return 1;
    size_t n;
    unsigned min;
    unsigned v;
    if (c >= 0xc2 && c <= 0xdf) n = 2, min = 0x80, v = c & 0x1fu;
    else if (c >= 0xe0 && c <= 0xef) n = 3, min = 0x800, v = c & 0x0fu;
    else if (c >= 0xf0 && c <= 0xf4) n = 4, min = 0x10000, v = c & 0x07u;
    else return 0;
    for (size_t i = 1; i < n; i++) {
        if ((s[i] & 0xc0) != 0x80) return 0;
        v = v << 6 | (s[i] & 0x3fu);
    }
    if (v < min || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) return 0;
    return n;
}

/* 1 for words Discord takes: 2 to 127 bytes of UTF-8 with no control character (Discord refuses a line shorter than 2
   or longer than 128 characters) */
static inline int hos_discord_valid_text(const char *s) {
    const size_t n = strlen(s);
    if (n < 2 || n > 127) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if (*p < 0x20 || *p == 0x7f) return 0;
        const size_t k = hos_discord_utf8_len(p);
        if (!k) return 0;
        p += k;
    }
    return 1;
}

/* s as the inside of a JSON string: " and \ escaped, control characters and bytes that are no UTF-8 left out, cut
   before a character that would not fit (out always ended) */
static inline void hos_discord_json_text(char *out, size_t cap, const char *s) {
    size_t o = 0;
    if (!cap) return;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        const size_t k = hos_discord_utf8_len(p);
        if (!k || *p < 0x20 || *p == 0x7f) {   /* (dropped: one byte) */
            p++;
            continue;
        }
        const size_t escape = (*p == '"' || *p == '\\') ? 1 : 0;   /* (only " and \: k is 1 for them) */
        if (o + escape + k + 1 > cap) break;
        if (escape) out[o++] = '\\';
        memcpy(out + o, p, k);
        o += k, p += k;
    }
    out[o] = 0;
}

/* Joins the strings of parts (NULL-ended) into out. All or nothing: 1 when the whole fits (out ended), else 0 and out "" (a cut
   would leave half an escape or a JSON object without its end). No snprintf of a string of unknown length. */
static inline int hos_discord_join(char *out, size_t cap, const char *const *parts) {
    size_t o = 0;
    if (!cap) return 0;
    for (; *parts; parts++) {
        const size_t n = strlen(*parts);
        if (n >= cap - o) {
            out[0] = 0;
            return 0;
        }
        memcpy(out + o, *parts, n);
        o += n;
    }
    out[o] = 0;
    return 1;
}

/* The activity object: name ("Longshot") and version ("v3.0.0-rc.4") make the default words, default_image the default
   art; w (may be NULL) the settings; module_line ("" or NULL: none) the second line, before the settings' state.
   1 when it fits in cap (1536 always does: each word is at most 254 bytes escaped, the image 255), else 0 and out "". */
static inline int hos_discord_activity(char *out, size_t cap, const char *name, const char *version, const char *default_image,
                                       const HosDiscordWords *w, const char *module_line, long long started) {
    static const HosDiscordWords none_set = {"", "", "", ""};
    if (!w) w = &none_set;
    char own[256], details[256], state[256], large[256], image[300], start[24];
    const char *const own_parts[] = {name, " ", version, NULL}, *const name_only[] = {name, NULL};
    if (!hos_discord_join(own, sizeof own, own_parts)) hos_discord_join(own, sizeof own, name_only);
    hos_discord_json_text(details, sizeof details, *w->details ? w->details : name);
    hos_discord_json_text(state, sizeof state, module_line && *module_line ? module_line : *w->state ? w->state : own);
    hos_discord_json_text(large, sizeof large, *w->large_text ? w->large_text : own);
    hos_discord_json_text(image, sizeof image, *w->image ? w->image : default_image);
    snprintf(start, sizeof start, "%lld", started);   /* (at most 20 characters) */
    const int art = strcmp(image, "none") != 0;
    const char *const parts[] = {"{\"details\":\"", details, "\",\"state\":\"", state, "\",\"timestamps\":{\"start\":", start,
                                 art ? "},\"assets\":{\"large_image\":\"" : "}}", art ? image : NULL, "\",\"large_text\":\"",
                                 large, "\"}}", NULL};
    return hos_discord_join(out, cap, parts);
}

/* The SET_ACTIVITY frame's JSON around an activity object; 1 when it fits, else 0 and out "" */
static inline int hos_discord_set_activity(char *out, size_t cap, unsigned nonce, unsigned long pid, const char *activity) {
    char head[96];
    snprintf(head, sizeof head, "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"%u\",\"args\":{\"pid\":%lu,\"activity\":", nonce, pid);
    const char *const parts[] = {head, activity, "}}", NULL};
    return hos_discord_join(out, cap, parts);
}

#endif
