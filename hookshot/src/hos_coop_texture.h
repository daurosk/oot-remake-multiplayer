/* SPDX-License-Identifier: GPL-3.0-or-later
   Texture overrides (oot-remake-multiplayer, docs/hookshot.md "Texture overrides"; our patch 0020): a module may have
   Longshot's renderer draw a texture with other texels, while the game's memory keeps the game's own. Nothing of the
   game is written: the renderer takes the module's bytes where it would read the texture's storage, and uploads the
   texture again when an override is set or taken away. The Link customizer paints Link's gear and tunic with it.

   Exports of the game's executable, found by name (GetProcAddress of the exe, dlsym on Linux), any thread; for a
   module:
   - hos_coop_textures: the textures the game has made (nvnTextureInitialize, until nvnTextureFinalize) whose storage
     size is in [size_min, size_max]: their storage's guest address, size, width, height, levels and NVN format, and a
     serial, new each time a texture is made (one made again at the same address has another). Fills
     at most `max` (out may be NULL with max 0), returns how many there are; *generation (when not NULL) = a number that
     changes whenever a texture is made or finalized, so a caller asks again only then.
   - hos_coop_texture_override: from now on the renderer reads `n` bytes from `bytes` (copied: the caller may free
     them) in place of guest memory [address, address + n); bytes NULL takes away every override that overlaps that
     range. 1 when done; 0 when the range is not inside a texture's storage the game has (nothing set). An override
     lies on the storage of one texture: when the game finalizes that texture, the override goes with it (the memory
     may hold something else next).
   For the renderers (host/coop_texture.c): hos_coop_texture_overridden(address, n), whether any override overlaps that
   range, and hos_coop_texture_overlay(address, data, n), the overrides laid over `data` (guest memory of that range).
   Generic: any module, any texture, with no further Longshot build. Both renderers take them (Vulkan, WebGPU).
   The same file is in Longshot's host/ through our patch and in our repository (hookshot/src): keep them equal
   (tests/hookshot/module_test.cpp checks it). Plain C. */
#ifndef HOS_COOP_TEXTURE_H
#define HOS_COOP_TEXTURE_H
#include <stdint.h>

#define HOS_COOP_TEXTURE_VERSION 1

typedef struct HosCoopTexture {
    uint64_t address;   /* the storage's guest address (where its texels start) */
    uint64_t size;      /* the storage's size in bytes, every level and layer */
    uint64_t serial;    /* new each time a texture is made */
    uint32_t width, height, levels, format;   /* format: the NVN format number */
} HosCoopTexture;

typedef int (*hos_coop_textures_fn)(uint64_t size_min, uint64_t size_max, HosCoopTexture *out, int max,
                                    uint64_t *generation);
typedef int (*hos_coop_texture_override_fn)(uint64_t address, const void *bytes, uint64_t n);

#endif
