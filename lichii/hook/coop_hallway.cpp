// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_hallway.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace CoopHallway {

namespace {

struct Box {
    float x0, x1, y0, y1, z0, z1;
};
// The trigger Area's box (half sizes 5, 5, 21 around its centre) grown by 0.5 to its walls; the height from just
// under the floor (124.5) to the turned floor's top.
constexpr std::array<Box, 2> kBoxes{{
    {-61.93f, -50.93f, 116.17f, 132.17f, -54.39f, -11.39f},  // west, centre x -56.43
    {51.07f, 62.07f, 116.17f, 132.17f, -54.39f, -11.39f},    // east, centre x 56.57
}};

bool Inside(const Box& b, const Vec& p) {
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) && p[0] >= b.x0 && p[0] <= b.x1 &&
           p[1] >= b.y0 && p[1] <= b.y1 && p[2] >= b.z0 && p[2] <= b.z1;
}

const char* Side(int hallway) {
    return hallway == 0 ? "west" : "east";
}

std::string Names(const std::vector<std::string>& who) {
    std::string out;
    for (std::size_t i = 0; i < who.size(); ++i) {
        if (i > 0) out += i + 1 == who.size() ? " and " : ", ";
        out += who[i];
    }
    return out;
}

} // namespace

std::optional<int> HallwayAt(std::string_view scene, const Vec& p) {
    if (scene != kScene) {
        return std::nullopt;
    }
    for (int i = 0; i < static_cast<int>(kBoxes.size()); ++i) {
        if (Inside(kBoxes[static_cast<std::size_t>(i)], p)) {
            return i;
        }
    }
    return std::nullopt;
}

int HallwayOfShooter(const Vec& shooter) {
    return shooter[0] < 0.0f ? 0 : 1;
}

bool IsTurn(std::string_view event) {
    // The eye's hit asks the room's setting (RSDB EventSetting ForestTemple_Room11_RoomSpin, _Room22_RoomSpin: run
    // hs-ft-twist-173 logged "the game asked the event 'ForestTemple_Room11_RoomSpin'"), whose flow plays the
    // twist (Dm_ForestTemple_0005/_0007, TwistCorridorEvent); those are asked by name too.
    return event == "ForestTemple_Room11_RoomSpin" || event == "ForestTemple_Room22_RoomSpin" ||
           event == "Dm_ForestTemple_0005" || event == "Dm_ForestTemple_0007";
}

Verdict Decide(std::string_view event, std::string_view scene, const Vec& self, std::span<const Other> others) {
    Verdict v;
    if (!IsTurn(event) || scene != kScene) {
        return v;
    }
    v.hallway = HallwayOfShooter(self);
    std::vector<const Other*> inside;
    for (const Other& o : others) {
        if (!o.can_move || !o.scene || *o.scene != kScene) {
            continue;
        }
        if (HallwayAt(*o.scene, o.position) == v.hallway) {
            inside.push_back(&o);
        }
    }
    std::sort(inside.begin(), inside.end(), [](const Other* a, const Other* b) { return a->number < b->number; });
    for (const Other* o : inside) {
        v.who.push_back(o->name.empty() ? "player " + std::to_string(o->number) : o->name);
    }
    v.refuse = !v.who.empty();
    return v;
}

std::string OurLine(const Verdict& v) {
    return "The " + std::string(Side(v.hallway)) + " hallway can't turn: " + Names(v.who) +
           (v.who.size() == 1 ? " is" : " are") + " in it.";
}

std::string TheirLine(const Verdict& v) {
    return Names(v.who) + ", step out of the " + std::string(Side(v.hallway)) + " hallway: I want to turn it.";
}

void Lines::Refused(const Verdict& v, std::uint64_t now_us) {
    if (!v.refuse || v.hallway < 0 || v.hallway > 1) {
        return;
    }
    std::uint64_t& at = said_at_[static_cast<std::size_t>(v.hallway)];
    if (at != 0 && now_us - at < kSayEveryUs) {
        return;
    }
    at = now_us;
    ours_.push_back(OurLine(v));
    chats_.push_back(TheirLine(v));
}

std::vector<std::string> Lines::TakeOurs() {
    return std::exchange(ours_, {});
}

std::vector<std::string> Lines::TakeChats() {
    return std::exchange(chats_, {});
}

} // namespace CoopHallway
