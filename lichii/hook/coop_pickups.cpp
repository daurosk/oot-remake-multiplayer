// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_pickups.h"

#include <mutex>
#include <set>

namespace CoopPickups {

namespace {

constexpr std::size_t kMaxKeys = 4096;

struct Noted {
    std::mutex mutex;
    std::set<std::uint64_t> known;
    std::vector<std::uint64_t> order;
};

Noted& TheNoted() {
    static Noted noted;
    return noted;
}

} // namespace

bool IsPickup(std::string_view name) {
    if (name == "Enemy_Skulltula_Gold" || name == "Obj_Token_Gold" ||
        name == "Obj_Heart_Container" || name.starts_with("Obj_RupeeToken_")) {
        return true;
    }
    // Placed rupees, recovery hearts and small keys (breakables, issue 102):
    // only placed ones are noted (a drop has no placement hash), and a
    // placement without a 64-bit key in the game is never shared (the
    // watcher cannot read it), so respawning ones stay each game's own.
    // The minigames' keys and rupees are each player's own game.
    if (name.starts_with("Obj_Rupee_") || name == "Obj_Heart" || name == "Obj_Heart_InWater" ||
        name.starts_with("Obj_Dungeon_SmallKey")) {
        return true;
    }
    // Obj_Heart_Piece, its digging-hole and minigame kinds, the Gerudo Valley one.
    return name.starts_with("Obj_") && name.find("Heart_Piece") != std::string_view::npos;
}

void Note(std::string_view name, std::uint64_t hash) {
    if (hash == 0 || hash == ~std::uint64_t{0} || !IsPickup(name)) {
        return;
    }
    Noted& n = TheNoted();
    std::lock_guard lock(n.mutex);
    if (n.order.size() < kMaxKeys && n.known.insert(hash).second) {
        n.order.push_back(hash);
    }
}

std::vector<std::uint64_t> Since(std::size_t from) {
    Noted& n = TheNoted();
    std::lock_guard lock(n.mutex);
    if (from >= n.order.size()) {
        return {};
    }
    return {n.order.begin() + static_cast<std::ptrdiff_t>(from), n.order.end()};
}

void Reset() {
    Noted& n = TheNoted();
    std::lock_guard lock(n.mutex);
    n.known.clear();
    n.order.clear();
}

} // namespace CoopPickups
