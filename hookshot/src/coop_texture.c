/* SPDX-License-Identifier: GPL-3.0-or-later
   (oot-remake-multiplayer, our patch 0020) Texture overrides for the co-op module: hos_coop_texture.h.
   nvn_native.c tells this file each texture the game makes and finalizes (hos_coop_texture_made / _gone); the module
   asks for them (hos_coop_textures) and sets overrides on their storage (hos_coop_texture_override), each for one
   texture by its serial; the renderers read a texture's storage through hos_coop_texture_overlay, which lays the
   overrides over the guest's bytes. Guest memory is never written here: setting or taking an override marks its pages
   written (the soft write watch), so the renderers upload the texture again, from the overlay.
   The same file is in Longshot's host/ through our patch and in our repository (hookshot/src, where the unit tests
   build it with stand-ins for the two functions below): keep them equal (tests/hookshot/module_test.cpp checks it). */
#include "hos_coop_texture.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef SRWLOCK coop_lock;
#define COOP_LOCK_INIT SRWLOCK_INIT
#define coop_write_lock(l) AcquireSRWLockExclusive(l)
#define coop_write_unlock(l) ReleaseSRWLockExclusive(l)
#define coop_read_lock(l) AcquireSRWLockShared(l)
#define coop_read_unlock(l) ReleaseSRWLockShared(l)
#define coop_count_set(p, v) InterlockedExchange((p), (v))
#define coop_count_get(p) InterlockedCompareExchange((p), 0, 0)
#define COOP_TEXTURE_EXPORT __declspec(dllexport)
#else
#include <pthread.h>
typedef pthread_rwlock_t coop_lock;
#define COOP_LOCK_INIT PTHREAD_RWLOCK_INITIALIZER
#define coop_write_lock(l) pthread_rwlock_wrlock(l)
#define coop_write_unlock(l) pthread_rwlock_unlock(l)
#define coop_read_lock(l) pthread_rwlock_rdlock(l)
#define coop_read_unlock(l) pthread_rwlock_unlock(l)
#define coop_count_set(p, v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
#define coop_count_get(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define COOP_TEXTURE_EXPORT __attribute__((visibility("default")))
#endif

void hos_mark_written(const void *p, size_t n);   /* (hos_mem.c: the GPU mirror picks the pages up) */
uint8_t *hos_guest_base(void);                    /* (hos_main.c) */

/* A guest address in this process (the unit tests have no guest memory: a null base, so through integers). */
static const void *guest_at(uint64_t address) {
    return (const void *)((uintptr_t)hos_guest_base() + (uintptr_t)address);
}

typedef struct {
    const void *tex;   /* the game's nvn_tex (its key) */
    HosCoopTexture t;
} Made;
typedef struct {
    uint64_t address, n, serial;   /* serial: the texture it lies on */
    uint8_t *bytes;
} Override;

static coop_lock lock = COOP_LOCK_INIT;
static Made *made;
static size_t made_n, made_cap;
static Override *over;
static size_t over_n, over_cap;
static uint64_t generation, serials;   /* (lock held) */
static volatile long overrides;        /* over_n, read without the lock (the renderers' fast path) */

static void drop_at(size_t i) {   /* (lock held) */
    const Override o = over[i];
    free(o.bytes);
    over[i] = over[--over_n];
    coop_count_set(&overrides, (long)over_n);
    hos_mark_written(guest_at(o.address), (size_t)o.n);   /* (the game's own texels uploaded again) */
}

/* (lock held) every override of that texture (serial), or of any texture (0), that overlaps [a, a + n) */
static void drop_overlapping(uint64_t a, uint64_t n, uint64_t serial) {
    for (size_t i = over_n; i-- > 0;)
        if ((!serial || over[i].serial == serial) && over[i].address < a + n && a < over[i].address + over[i].n)
            drop_at(i);
}

static void drop_serial(uint64_t serial) {   /* (lock held) */
    for (size_t i = over_n; i-- > 0;)
        if (over[i].serial == serial) drop_at(i);
}

void hos_coop_texture_made(const void *tex, uint64_t address, uint64_t size, uint32_t width, uint32_t height,
                           uint32_t levels, uint32_t format) {
    if (!tex || !address || !size) return;
    coop_write_lock(&lock);
    size_t i = 0;
    while (i < made_n && made[i].tex != tex) i++;
    if (i == made_n) {
        if (made_n == made_cap) {
            const size_t cap = made_cap ? made_cap * 2 : 1024;
            Made *m = (Made *)realloc(made, cap * sizeof *m);
            if (!m) {
                coop_write_unlock(&lock);
                return;
            }
            made = m, made_cap = cap;
        }
        made_n++;
    } else {
        drop_serial(made[i].t.serial);   /* (made again without a finalize: another texture now) */
    }
    made[i].tex = tex;
    made[i].t.address = address, made[i].t.size = size, made[i].t.serial = ++serials;
    made[i].t.width = width, made[i].t.height = height, made[i].t.levels = levels, made[i].t.format = format;
    generation++;
    coop_write_unlock(&lock);
}

void hos_coop_texture_gone(const void *tex) {
    coop_write_lock(&lock);
    for (size_t i = 0; i < made_n; i++) {
        if (made[i].tex != tex) continue;
        drop_serial(made[i].t.serial);
        made[i] = made[--made_n];
        generation++;
        break;
    }
    coop_write_unlock(&lock);
}

COOP_TEXTURE_EXPORT int hos_coop_textures(uint64_t size_min, uint64_t size_max, HosCoopTexture *out, int max,
                                          uint64_t *gen) {
    int count = 0;
    coop_read_lock(&lock);
    for (size_t i = 0; i < made_n; i++) {
        if (made[i].t.size < size_min || made[i].t.size > size_max) continue;
        if (out && count < max) out[count] = made[i].t;
        count++;
    }
    if (gen) *gen = generation;
    coop_read_unlock(&lock);
    return count;
}

COOP_TEXTURE_EXPORT int hos_coop_texture_override(uint64_t address, uint64_t serial, const void *bytes, uint64_t n) {
    if (!n || !serial) return 0;
    int ok = 0;
    coop_write_lock(&lock);
    if (!bytes) {
        drop_overlapping(address, n, serial);
        ok = 1;
    } else {
        size_t i = 0;   /* only inside the storage of the texture of that serial, while the game has it */
        while (i < made_n && made[i].t.serial != serial) i++;
        const int inside = i < made_n && made[i].t.address <= address && address + n <= made[i].t.address + made[i].t.size;
        uint8_t *copy = inside ? (uint8_t *)malloc((size_t)n) : NULL;
        if (copy && over_n == over_cap) {
            const size_t cap = over_cap ? over_cap * 2 : 64;
            Override *o = (Override *)realloc(over, cap * sizeof *o);
            if (o) over = o, over_cap = cap;
        }
        if (copy && over_n < over_cap) {
            memcpy(copy, bytes, (size_t)n);
            drop_overlapping(address, n, 0);
            over[over_n].address = address, over[over_n].n = n, over[over_n].serial = serial, over[over_n].bytes = copy;
            over_n++;
            coop_count_set(&overrides, (long)over_n);
            hos_mark_written(guest_at(address), (size_t)n);   /* (uploaded again, from the overlay) */
            ok = 1;
        } else {
            free(copy);
        }
    }
    coop_write_unlock(&lock);
    return ok;
}

/* For the renderers (the Vulkan one finds it in the exe by name): whether any override overlaps [address, address+n) */
COOP_TEXTURE_EXPORT int hos_coop_texture_overridden(uint64_t address, uint64_t n) {
    if (!coop_count_get(&overrides)) return 0;
    int hit = 0;
    coop_read_lock(&lock);
    for (size_t i = 0; i < over_n && !hit; i++) hit = over[i].address < address + n && address < over[i].address + over[i].n;
    coop_read_unlock(&lock);
    return hit;
}

/* The overrides over `data`, which holds guest memory [address, address + n): how many lay on it */
COOP_TEXTURE_EXPORT int hos_coop_texture_overlay(uint64_t address, uint8_t *data, uint64_t n) {
    if (!coop_count_get(&overrides) || !data) return 0;
    int laid = 0;
    coop_read_lock(&lock);
    for (size_t i = 0; i < over_n; i++) {
        const Override *o = &over[i];
        const uint64_t lo = o->address > address ? o->address : address;
        const uint64_t hi = o->address + o->n < address + n ? o->address + o->n : address + n;
        if (lo >= hi) continue;
        memcpy(data + (lo - address), o->bytes + (lo - o->address), (size_t)(hi - lo));
        laid++;
    }
    coop_read_unlock(&lock);
    return laid;
}
