// SPDX-License-Identifier: GPL-3.0-or-later
// Longshot's texture overrides (patch 0020): hookshot/src/coop_texture.c, the
// same file as the patch's host/coop_texture.c, built here with stand-ins for
// the two Longshot functions it calls.
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "hos_coop_texture.h"

extern "C" {
void hos_coop_texture_made(const void* tex, std::uint64_t address, std::uint64_t size, std::uint32_t width,
                           std::uint32_t height, std::uint32_t levels, std::uint32_t format);
void hos_coop_texture_gone(const void* tex);
int hos_coop_textures(std::uint64_t size_min, std::uint64_t size_max, HosCoopTexture* out, int max,
                      std::uint64_t* generation);
int hos_coop_texture_override(std::uint64_t address, std::uint64_t serial, const void* bytes, std::uint64_t n);
int hos_coop_texture_overridden(std::uint64_t address, std::uint64_t n);
int hos_coop_texture_overlay(std::uint64_t address, std::uint8_t* data, std::uint64_t n);

// Stand-ins: the guest's base is null here, so a marked page's pointer is its guest address.
static std::vector<std::pair<std::uint64_t, std::size_t>> g_marked;
std::uint8_t* hos_guest_base(void) {
    return nullptr;
}
void hos_mark_written(const void* p, std::size_t n) {
    g_marked.push_back({reinterpret_cast<std::uintptr_t>(p), n});
}
}

namespace {

// The texture of `tex` as the exe lists it (size kSize).
constexpr std::uint64_t kSize = 0x1000;
HosCoopTexture Listed(std::uint64_t address) {
    HosCoopTexture out[64];
    const int n = hos_coop_textures(kSize, kSize, out, 64, nullptr);
    for (int i = 0; i < n && i < 64; ++i) {
        if (out[i].address == address) {
            return out[i];
        }
    }
    return HosCoopTexture{};
}
std::vector<std::uint8_t> Seen(std::uint64_t address, std::size_t n, std::uint8_t guest) {
    std::vector<std::uint8_t> v(n, guest);
    hos_coop_texture_overlay(address, v.data(), n);
    return v;
}

}  // namespace

TEST_CASE("texture overrides: set on a texture the game has, read over the guest's bytes, taken away") {
    int a = 0;
    const std::uint64_t at = 0x10000000;
    std::uint64_t gen0 = 0, gen1 = 0;
    hos_coop_textures(1, 0, nullptr, 0, &gen0);
    hos_coop_texture_made(&a, at, kSize, 64, 64, 1, 0x42);
    hos_coop_textures(1, 0, nullptr, 0, &gen1);
    CHECK(gen1 != gen0);
    const HosCoopTexture t = Listed(at);
    REQUIRE(t.serial != 0);
    CHECK(t.width == 64);
    CHECK(t.format == 0x42);

    const std::uint8_t red[16] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
    g_marked.clear();
    // Outside its storage, or with another serial: refused.
    CHECK(hos_coop_texture_override(at + kSize - 8, t.serial, red, 16) == 0);
    CHECK(hos_coop_texture_override(at, t.serial + 1000, red, 16) == 0);
    CHECK(hos_coop_texture_overridden(at, kSize) == 0);
    CHECK(g_marked.empty());
    // Inside: the overlay lays it at its offset of a range read part of the way in; the pages are marked.
    REQUIRE(hos_coop_texture_override(at + 0x100, t.serial, red, 16) == 1);
    CHECK(g_marked.size() == 1);
    CHECK(g_marked.back().first == at + 0x100);
    CHECK(hos_coop_texture_overridden(at, kSize) == 1);
    CHECK(hos_coop_texture_overridden(at + 0x200, 0x10) == 0);
    const auto seen = Seen(at + 0x108, 16, 1);   // the override's second half, then the guest's
    CHECK(seen == std::vector<std::uint8_t>{9, 9, 9, 9, 9, 9, 9, 9, 1, 1, 1, 1, 1, 1, 1, 1});
    // A new override over the same bytes replaces it.
    const std::uint8_t blue[8] = {5, 5, 5, 5, 5, 5, 5, 5};
    REQUIRE(hos_coop_texture_override(at + 0x104, t.serial, blue, 8) == 1);
    CHECK(Seen(at + 0x100, 16, 1) == std::vector<std::uint8_t>{1, 1, 1, 1, 5, 5, 5, 5, 5, 5, 5, 5, 1, 1, 1, 1});
    // Taken away: the guest's bytes again, marked.
    g_marked.clear();
    CHECK(hos_coop_texture_override(at, t.serial, nullptr, kSize) == 1);
    CHECK(hos_coop_texture_overridden(at, kSize) == 0);
    CHECK(g_marked.size() == 1);
    hos_coop_texture_gone(&a);
    CHECK(Listed(at).serial == 0);
}

TEST_CASE("texture overrides: a texture finalized or made again takes its overrides, a late call never lands on the next") {
    int a = 0, b = 0;
    const std::uint64_t at = 0x20000000;
    const std::uint8_t red[16] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};

    hos_coop_texture_made(&a, at, kSize, 64, 64, 1, 0);
    const std::uint64_t first = Listed(at).serial;
    REQUIRE(hos_coop_texture_override(at, first, red, 16) == 1);
    hos_coop_texture_gone(&a);
    CHECK(hos_coop_texture_overridden(at, kSize) == 0);

    // Another texture over the same storage: the old serial is refused there.
    hos_coop_texture_made(&b, at, kSize, 64, 64, 1, 0);
    const std::uint64_t second = Listed(at).serial;
    CHECK(second != first);
    CHECK(hos_coop_texture_override(at, first, red, 16) == 0);
    CHECK(hos_coop_texture_overridden(at, kSize) == 0);

    // Made again without a finalize (nvnTextureInitialize twice): a new serial, the override gone.
    REQUIRE(hos_coop_texture_override(at, second, red, 16) == 1);
    hos_coop_texture_made(&b, at, kSize, 64, 64, 1, 0);
    CHECK(Listed(at).serial != second);
    CHECK(hos_coop_texture_overridden(at, kSize) == 0);
    // Taking away with a stale serial leaves the new texture's override.
    const std::uint64_t third = Listed(at).serial;
    REQUIRE(hos_coop_texture_override(at, third, red, 16) == 1);
    CHECK(hos_coop_texture_override(at, second, nullptr, kSize) == 1);
    CHECK(hos_coop_texture_overridden(at, kSize) == 1);
    hos_coop_texture_gone(&b);
    CHECK(hos_coop_texture_overridden(at, kSize) == 0);
}
