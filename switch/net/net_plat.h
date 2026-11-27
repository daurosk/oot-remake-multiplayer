// SPDX-License-Identifier: GPL-3.0-or-later
// What the freestanding net code (switch/net) needs from the place it runs.
// The ExeFS module implements these in switch/proto/main.cpp (debug-string log,
// the CPU's counter, nn::os::GenerateRandomBytes); the PC test in
// tools/switch_mod/net_host_harness.cpp. Plain C names: no function pointers
// (the module stays liftable, docs/exefs.md "Lifting").
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
void coop_plat_log(const char* text, size_t len);  // one line, no newline
uint64_t coop_plat_ms();                           // a millisecond clock
void coop_plat_random(void* out, size_t len);      // secret-grade random bytes
}

namespace coopnet {

// Text and numbers into one log line, written when it goes out of scope.
struct Line {
    char buf[240];
    size_t n = 0;
    Line& s(const char* t) {
        while (*t && n < sizeof(buf) - 1) buf[n++] = *t++;
        return *this;
    }
    Line& s(const char* t, size_t len) {
        for (size_t i = 0; i < len && n < sizeof(buf) - 1; ++i) buf[n++] = t[i];
        return *this;
    }
    Line& hex(uint64_t v) {
        s("0x");
        char tmp[16];
        int k = 0;
        do {
            tmp[k++] = "0123456789abcdef"[v & 0xf];
            v >>= 4;
        } while (v && k < 16);
        while (k && n < sizeof(buf) - 1) buf[n++] = tmp[--k];
        return *this;
    }
    Line& dec(uint64_t v) {
        char tmp[20];
        int k = 0;
        do {
            tmp[k++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v && k < 20);
        while (k && n < sizeof(buf) - 1) buf[n++] = tmp[--k];
        return *this;
    }
    ~Line() { coop_plat_log(buf, n); }
};

}  // namespace coopnet
