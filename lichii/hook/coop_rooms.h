// SPDX-License-Identifier: GPL-3.0-or-later
//
// Dungeon rooms (issue 145): which room of a dungeon a position is in.
//
// A dungeon is one scene; its rooms are not scenes of their own. The game's
// own "Link is in room N" is not mapped, so the room is guessed from where
// the room's placements stand. That table is made on the player's PC from
// their own game, with the puppet pack (tools/puppet_pack/dungeon_rooms.py
// writes coop_rooms.txt beside coop_puppet.json); nothing of it is in this
// repo. A position is in the room of the nearest placement, with height
// counted twice (rooms are stacked on floors), and only within kFar of one.
// docs/puppet.md, Fairy, "Only in our room".
//
// Plain logic, no emulator: tested in tests/lichii/rooms_test.cpp.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace CoopRooms {

using Vec3 = std::array<float, 3>;

struct Point {
    std::string scene;
    int room = 0;  // DefaultDungeonRoomIndex
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// coop_rooms.txt: one placement a line, "scene room x y z" (spaces); "#"
// starts a comment. Bad lines are skipped and counted in `bad` if given.
std::vector<Point> Parse(std::string_view text, int* bad = nullptr);

// Height counts this many times over a step across (floors are stacked).
inline constexpr float kHeightWeight = 2.0f;
// Farther than this (weighted, game units) from every placement of the
// scene: not known.
inline constexpr float kFar = 25.0f;
// A tracked position changes room only when the new room's nearest
// placement is this much nearer than the old room's (no flicker in a door).
inline constexpr float kMargin = 1.5f;

struct Guess {
    int room = -1;
    float distance = 0.0f;  // weighted, to the room's nearest placement
};

// The points, indexed by scene once.
class Table {
public:
    Table() = default;
    explicit Table(std::vector<Point> points);

    bool empty() const { return scenes_.empty(); }
    bool Has(std::string_view scene) const { return Find(scene) != nullptr; }
    std::size_t size() const { return count_; }

    // The room of `scene` nearest `at`, or nothing (no room in that scene,
    // or none within kFar). `distance_to`: if set, also the distance to that
    // room's nearest placement (a large number if it has none).
    std::optional<Guess> Nearest(std::string_view scene, const Vec3& at, int distance_to = -1,
                                 float* distance_out = nullptr) const;

private:
    const std::vector<Point>* Find(std::string_view scene) const;
    std::map<std::string, std::vector<Point>, std::less<>> scenes_;
    std::size_t count_ = 0;
};

// Keeps a room for each key (a player number) with the kMargin slack. A key
// whose guess is not known is forgotten, so its next known guess has no
// slack against the room it had before.
class Tracker {
public:
    explicit Tracker(Table table = {}) : table_(std::move(table)) {}

    const Table& table() const { return table_; }

    // The room `key` is in now; nothing if not known (forgotten then).
    std::optional<int> Update(std::uint32_t key, std::string_view scene, const Vec3& at);
    void Forget(std::uint32_t key) { rooms_.erase(key); }
    void Clear() { rooms_.clear(); }
    template <typename Keep>
    void ForgetUnless(Keep keep) {
        for (auto it = rooms_.begin(); it != rooms_.end();) {
            it = keep(it->first) ? std::next(it) : rooms_.erase(it);
        }
    }

private:
    struct Kept {
        std::string scene;
        int room = -1;
    };
    Table table_;
    std::map<std::uint32_t, Kept> rooms_;
};

}  // namespace CoopRooms
