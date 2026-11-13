// SPDX-License-Identifier: GPL-3.0-or-later
// What a puppet shows and sounds beyond its pose: the numbers and the small
// messages, with no game or Windows code (unit-tested; upstream.cpp applies them).
//
// The red flash. The game makes a struck Link red through his model
// controller ([[actor + 0x270] + 0x260], the same object on a puppet): the
// float at +0x98 is the material parameter p_damage_ratio (0 normal, 1 all
// red), pushed to the model and every attached one (sword, shield) when bit
// 0x4000 of the u32 at +0x120 is set. Link's Player part counts 48 ticks of
// 1/60 s down after a hit and writes curve(1 - left / 48) each frame; the
// curve (sampled in the running game, launcher-test fl-r4-probe.txt) is at 1
// for the first 0.1 s and back at 0 after 0.2 s. A puppet has no Player
// part, so nothing writes these fields there but this mod.
//
// The game does not blink Link after a hit (nine hits watched: no field of
// the controller but the two above changed, and he is in every picture). A
// blink is still offered for a puppet (OOT_COOP_PUPPET_BLINK=1), made with
// the controller's own visibility switch: byte +0xe5 (1 shown) with dirty
// bit 0x800.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CoopNx2navi::PuppetFx {

namespace addr {
inline constexpr std::uint64_t kPartsModelController = 0x260;
inline constexpr std::uint64_t kDamageRatio = 0x98;  // float
inline constexpr std::uint64_t kDirty = 0x120;       // u32
inline constexpr std::uint32_t kDirtyDamage = 0x4000;
inline constexpr std::uint64_t kVisible = 0xe5;      // u8
inline constexpr std::uint32_t kDirtyVisible = 0x800;
// Every sound an actor's sound user starts goes through xlink2's emit
// (lichii/hook/coop_puppet_sound.h): x0 the user, x1 -> the asset, whose
// first word points at its name.
inline constexpr std::uint64_t kEmit = 0x440630;
}  // namespace addr

inline constexpr float kFlashTicks = 48.0f;  // PlayerGameParam.DamageColorCount, in 1/60 s
inline constexpr std::uint64_t kFlashMs = 800;
inline constexpr std::uint64_t kBlinkMs = 1000;
inline constexpr std::uint64_t kBlinkHalfMs = 66;  // two frames at 30 a second

// p_damage_ratio `ms` after the hit; 0 once the flash is over.
inline float DamageRatio(std::uint64_t ms) {
    const float x = static_cast<float>(ms) * 0.06f / kFlashTicks;  // ticks gone / 48
    if (x >= 0.25f) {
        return 0.0f;
    }
    static constexpr float kRise[5][2] = {{0.0f, 0.5f}, {0.0104f, 0.587f}, {0.0208f, 0.774f}, {0.0312f, 0.947f},
                                          {0.0417f, 1.0f}};
    if (x < kRise[4][0]) {
        for (int i = 0; i < 4; ++i) {
            if (x < kRise[i + 1][0]) {
                const float t = (x - kRise[i][0]) / (kRise[i + 1][0] - kRise[i][0]);
                return kRise[i][1] + t * (kRise[i + 1][1] - kRise[i][1]);
            }
        }
    }
    if (x <= 0.12f) {
        return 1.0f;
    }
    const float u = (x - 0.12f) / 0.13f;
    return 1.0f - u * u * (3.0f - 2.0f * u);
}

// Whether a blinking puppet is shown `ms` after the hit.
inline bool BlinkShown(std::uint64_t ms) { return ms >= kBlinkMs || (ms / kBlinkHalfMs) % 2 == 1; }

// --- PLAYER_EXTRA kinds of this port (coop/protocol messages.hpp) ---

inline constexpr std::uint8_t kExtraSound = 1;  // data: the asset's name
inline constexpr std::uint8_t kExtraMagic = 2;  // data: u16 now, u16 most (little endian)
inline constexpr std::size_t kMaxSoundName = 63;
// A sound older than this when the puppet can play it is not played.
inline constexpr std::uint64_t kSoundStaleMs = 400;
// At most this many of our own sounds are sent in a second (a run is two steps a second).
inline constexpr int kSoundsPerSecond = 24;

// An asset name as the game writes them: letters, digits and '_', 1 to 63.
inline bool SoundName(std::string_view name) {
    if (name.empty() || name.size() > kMaxSoundName) {
        return false;
    }
    for (const char c : name) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

inline std::vector<std::uint8_t> EncodeMagic(int now, int most) {
    const auto clamp = [](int v) { return static_cast<std::uint16_t>(v < 0 ? 0 : v > 0xffff ? 0xffff : v); };
    const std::uint16_t a = clamp(now), b = clamp(most);
    return {static_cast<std::uint8_t>(a & 0xff), static_cast<std::uint8_t>(a >> 8),
            static_cast<std::uint8_t>(b & 0xff), static_cast<std::uint8_t>(b >> 8)};
}

struct Magic {
    int now = 0;
    int most = 0;
};
inline std::optional<Magic> DecodeMagic(const std::vector<std::uint8_t>& data) {
    if (data.size() != 4) {
        return std::nullopt;
    }
    Magic m{data[0] | (data[1] << 8), data[2] | (data[3] << 8)};
    if (m.now > m.most) {
        m.now = m.most;
    }
    return m;
}

// Our own sounds on their way out: not more than kSoundsPerSecond in any second.
class SoundBudget {
public:
    bool Take(std::uint64_t now_ms) {
        if (now_ms - window_ms_ >= 1000) {
            window_ms_ = now_ms;
            sent_ = 0;
        }
        if (sent_ >= kSoundsPerSecond) {
            return false;
        }
        ++sent_;
        return true;
    }

private:
    std::uint64_t window_ms_ = 0;
    int sent_ = 0;
};

}  // namespace CoopNx2navi::PuppetFx
