// SPDX-License-Identifier: GPL-3.0-or-later
// The other player's Link as a real second Player (docs/native-player.md).
//
// Today another player is shown by a stripped actor from the puppet pack
// (CoopPuppet_<Age>), posed from outside. The target is the game's own
// Player_Child / Player_Adult, made by the game's create path and run by the
// game's own player code from the other player's input.
//
// Step 1, this file: LICHII_COOP_NATIVE_PLAYER=1 makes the Director create
// Player_<Age> instead of the pack actor and then leave it alone (no pose, no
// equip, no animation call, no write into it), and logs what the game does
// with a second Player: who "the player" is, whether the second one moves,
// which components it got. A test switch; off, nothing changes.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace CoopNativePlayer {

using Vec3 = std::array<float, 3>;
// Guest memory, as CoopPuppet::Guest::Read.
using ReadFn = std::function<bool(std::uint64_t address, void* out, std::size_t size)>;

// The game's own player actors (the preload walker's name table, main+0x944a44).
inline constexpr std::string_view kChild = "Player_Child";
inline constexpr std::string_view kAdult = "Player_Adult";

inline bool IsPlayerActor(std::string_view actor) {
    return actor == kChild || actor == kAdult;
}

inline std::string ActorFor(bool adult) {
    return std::string(adult ? kAdult : kChild);
}

namespace addr {
inline constexpr std::uint64_t kActorState = 0x2c;      // 4: present
inline constexpr std::uint64_t kActorPosition = 0x31c;
inline constexpr std::uint64_t kActorParts = 0x270;     // the component table
// Slots of the component table (8 bytes each; reports/hs-puppets-alt-b.md).
inline constexpr std::uint64_t kPartPhysics = 0x50;     // slot 10
inline constexpr std::uint64_t kPartLife = 0x2b8;       // slot 87
inline constexpr std::uint64_t kPartPlayer = 0x358;     // slot 107
// The player preload lists (the docs' "player manager"): how many records.
inline constexpr std::uint64_t kSceneModulePtr = 0x4e97648;
inline constexpr std::uint64_t kSceneTable = 0x1f0;
inline constexpr std::uint64_t kTableArray = 0x58;
inline constexpr std::uint64_t kPlayerManagerSlot = 0x78;
inline constexpr std::uint64_t kListOneCount = 0x28;
inline constexpr std::uint64_t kListTwoCount = 0x38;
}  // namespace addr

// What one look at the game found.
struct Facts {
    std::uint64_t second = 0;        // the second Player's actor
    std::uint32_t state = 0;         // its actor state
    Vec3 at{};                       // where it is
    std::uint64_t parts = 0;         // its component table
    std::uint64_t player = 0;        // its Player component (0: none)
    std::uint64_t life = 0;
    std::uint64_t physics = 0;
    std::uint64_t local = 0;         // the local Link, as the game's own lookup (0x37cca0) finds it
    Vec3 local_at{};
    std::int32_t list_one = -1;      // preload records, list 1 and 2 (-1: not read)
    std::int32_t list_two = -1;
};

template <typename T>
inline bool ReadValue(const ReadFn& read, std::uint64_t address, T& out) {
    return address != 0 && read(address, &out, sizeof(T));
}

inline Facts Look(const ReadFn& read, std::uint64_t main_base, std::uint64_t second,
                  std::uint64_t local) {
    Facts f;
    f.second = second;
    f.local = local;
    ReadValue(read, second + addr::kActorState, f.state);
    ReadValue(read, second + addr::kActorPosition, f.at);
    if (ReadValue(read, second + addr::kActorParts, f.parts) && f.parts != 0) {
        ReadValue(read, f.parts + addr::kPartPlayer, f.player);
        ReadValue(read, f.parts + addr::kPartLife, f.life);
        ReadValue(read, f.parts + addr::kPartPhysics, f.physics);
    }
    if (local != 0) {
        ReadValue(read, local + addr::kActorPosition, f.local_at);
    }
    std::uint64_t scene = 0, table = 0, array = 0, manager = 0;
    std::uint32_t one = 0, two = 0;
    if (ReadValue(read, main_base + addr::kSceneModulePtr, scene) &&
        ReadValue(read, scene + addr::kSceneTable, table) &&
        ReadValue(read, table + addr::kTableArray, array) &&
        ReadValue(read, array + addr::kPlayerManagerSlot, manager) &&
        ReadValue(read, manager + addr::kListOneCount, one) &&
        ReadValue(read, manager + addr::kListTwoCount, two)) {
        f.list_one = static_cast<std::int32_t>(one);
        f.list_two = static_cast<std::int32_t>(two);
    }
    return f;
}

inline float Distance(const Vec3& a, const Vec3& b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

// The log of one second Player: a line when something changed (at most one a
// second), and one every kEveryMs anyway.
class Probe {
public:
    static constexpr std::uint64_t kQuietMs = 1000;
    static constexpr std::uint64_t kEveryMs = 5000;
    static constexpr float kMovedM = 0.05f;

    void Reset() { *this = Probe(); }

    // `local`: the local Link's actor now (0: not found). The first call
    // keeps where both stood and who the local Link was.
    std::optional<std::string> Step(const ReadFn& read, std::uint64_t main_base,
                                    std::uint64_t second, std::uint64_t local,
                                    std::uint64_t now_ms) {
        const Facts f = Look(read, main_base, second, local);
        if (!started_) {
            started_ = true;
            first_ = f;
            last_ = f;
            last_ms_ = now_ms;
            return "native player: second " + Text(f) + "; first look";
        }
        std::string what;
        if (f.local != first_.local) {
            what += f.local == f.second ? "; THE GAME'S PLAYER IS NOW THE SECOND ONE"
                                    : "; the game's player changed";
        }
        if (f.state != last_.state) {
            what += "; state changed";
        }
        if (f.player != last_.player || f.life != last_.life || f.physics != last_.physics) {
            what += "; components changed";
        }
        if (f.list_one != last_.list_one || f.list_two != last_.list_two) {
            what += "; preload lists changed";
        }
        const float moved = Distance(f.at, last_.at);
        const float local_moved = Distance(f.local_at, last_.local_at);
        if (moved > kMovedM) {
            // Both moving the same way: it reads the local pad (controller 0).
            const Vec3 d{f.at[0] - last_.at[0], f.at[1] - last_.at[1], f.at[2] - last_.at[2]};
            const Vec3 l{f.local_at[0] - last_.local_at[0], f.local_at[1] - last_.local_at[1],
                         f.local_at[2] - last_.local_at[2]};
            const bool same = local_moved > kMovedM && Distance(d, l) < 0.25f * (moved + local_moved);
            what += same ? "; the second one moved WITH the local Link" : "; the second one moved";
        }
        const bool due = now_ms - last_ms_ >= kEveryMs;
        if (what.empty() ? !due : now_ms - last_ms_ < kQuietMs) {
            return std::nullopt;
        }
        char total[64];
        std::snprintf(total, sizeof(total), "; %.2f m from where it was made",
                      static_cast<double>(Distance(f.at, first_.at)));
        last_ = f;
        last_ms_ = now_ms;
        return "native player: second " + Text(f) + total + (what.empty() ? "; no change" : what);
    }

    static std::string Text(const Facts& f) {
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "0x%llx state %u at (%.2f, %.2f, %.2f), components: player 0x%llx life 0x%llx "
                      "physics 0x%llx; the game's player 0x%llx at (%.2f, %.2f, %.2f); preload lists %d/%d",
                      static_cast<unsigned long long>(f.second), f.state, static_cast<double>(f.at[0]),
                      static_cast<double>(f.at[1]), static_cast<double>(f.at[2]),
                      static_cast<unsigned long long>(f.player), static_cast<unsigned long long>(f.life),
                      static_cast<unsigned long long>(f.physics),
                      static_cast<unsigned long long>(f.local), static_cast<double>(f.local_at[0]),
                      static_cast<double>(f.local_at[1]), static_cast<double>(f.local_at[2]), f.list_one,
                      f.list_two);
        return buf;
    }

private:
    bool started_ = false;
    Facts first_;
    Facts last_;
    std::uint64_t last_ms_ = 0;
};

}  // namespace CoopNativePlayer
