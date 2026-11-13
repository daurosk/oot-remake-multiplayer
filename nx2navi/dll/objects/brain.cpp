// SPDX-License-Identifier: GPL-3.0-or-later
// A copy's own AI left out (brain.hpp).
#include "brain.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>

#include "hold.hpp"
#include "plugin_api_v2.h"

namespace CoopNx2navi::Objects::Brain {
namespace {

std::atomic<const Nx2PluginApi*> g_api{nullptr};
std::mutex g_mutex;
int g_handle = 0;
std::atomic<std::uint64_t> g_calls{0}, g_skipped{0}, g_for_hits{0};

// Held copies a hit came in for: how many more updates each one runs. The
// actors' updates run on several of the game's threads at once.
struct Seen {
    std::uint64_t actor = 0;
    std::uint64_t life = 0;
    std::uint32_t header_id = 0;
    int left = 0;
};
std::mutex g_seen_mutex;
std::array<Seen, 96> g_seen{};
std::size_t g_seen_next = 0;
// Life components the Mover wrote a hit record into, not yet matched to a copy.
struct Noted {
    std::uint64_t life = 0;
};
std::array<Noted, 16> g_noted{};
std::size_t g_noted_next = 0;

// Whether this update of a held copy runs because a hit came in for it.
bool RunsForHit(const Nx2PluginApi* api, std::uint64_t actor, std::uint64_t table) {
    // The Life component and the id in its last attack's header; 0 for an
    // actor without one (it still gets its first updates).
    std::uint64_t life = 0;
    std::uint32_t id = 0;
    if (api->read(table + kComponentLife, &life, 8) != 1 || life < 0x1000 ||
        api->read(life + kHitHeaderId, &id, 4) != 1) {
        life = 0;
        id = 0;
    }
    std::lock_guard lock(g_seen_mutex);
    Seen* slot = nullptr;
    for (Seen& s : g_seen) {
        if (s.actor == actor) {
            slot = &s;
            break;
        }
    }
    if (slot == nullptr) {
        // Held for the first time: its first updates run (kWarmFrames, brain.hpp).
        slot = &g_seen[g_seen_next++ % g_seen.size()];
        *slot = Seen{actor, life, id, kWarmFrames};
    } else if (slot->life != life) {
        slot->life = life;  // another Life component: what its header says is not a new hit
        slot->header_id = id;
    } else if (life != 0 && slot->header_id != id) {
        slot->header_id = id;  // the game put a hit in: our own Link's
        slot->left = slot->left > kHitFrames ? slot->left : kHitFrames;
    }
    for (Noted& n : g_noted) {
        if (life != 0 && n.life == life) {
            n.life = 0;  // the Mover put a hit in: the owner's game says so
            slot->left = slot->left > kHitFrames ? slot->left : kHitFrames;
        }
    }
    if (slot->left <= 0) {
        return false;
    }
    --slot->left;
    return true;
}

}  // namespace

void OnActorAi(Nx2HookCall* call, void*) {
    const Nx2PluginApi* api = g_api.load(std::memory_order_acquire);
    if (api == nullptr) {
        return;
    }
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (Hold::TheTable().Count() == 0) {
        return;  // nothing is held: no read at all
    }
    const std::uint64_t actor = call->x[0];
    std::uint64_t table = 0, bsa = 0;
    if (actor < 0x1000 || api->read(actor + kActorComponents, &table, 8) != 1 || table < 0x1000 ||
        api->read(table + kComponentBsa, &bsa, 8) != 1) {
        return;
    }
    std::uint32_t bits = 0;
    if (!Hold::TheTable().Alter(bsa, bits)) {
        return;
    }
    if (RunsForHit(api, actor, table)) {
        g_for_hits.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    call->skip = 1;
    g_skipped.fetch_add(1, std::memory_order_relaxed);
}

bool Install(const Nx2PluginApi* api, std::uint64_t main_base) {
    if (api == nullptr || main_base == 0 || !NX2_HAS(api, Nx2PluginApi, hook_function) ||
        api->hook_function == nullptr) {
        return false;
    }
    std::lock_guard lock(g_mutex);
    if (g_handle > 0) {
        return true;
    }
    g_api = api;
    std::uint64_t function = kActorAi;
    if (const char* other = std::getenv("OOT_COOP_BRAIN_SKIP"); other != nullptr && *other != 0) {
        function = std::strtoull(other, nullptr, 16);
    }
    g_handle = api->hook_function(main_base + function, OnActorAi, nullptr);
    if (g_handle <= 0) {
        g_handle = 0;
        return false;
    }
    return true;
}

void Remove() {
    std::lock_guard lock(g_mutex);
    const Nx2PluginApi* api = g_api.load();
    if (api != nullptr && g_handle > 0) {
        api->remove(g_handle);
    }
    g_handle = 0;
    std::lock_guard seen(g_seen_mutex);
    g_seen = {};
    g_noted = {};
}

void NoteWrite(std::uint64_t address, std::size_t size) {
    if (size != kHitRecordSize || address < kHitRecord + 0x1000) {
        return;
    }
    std::lock_guard lock(g_seen_mutex);
    g_noted[g_noted_next++ % g_noted.size()].life = address - kHitRecord;
}

Counts Take() {
    Counts c;
    c.calls = g_calls.exchange(0);
    c.skipped = g_skipped.exchange(0);
    c.for_hits = g_for_hits.exchange(0);
    return c;
}

}  // namespace CoopNx2navi::Objects::Brain
