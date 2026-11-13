// SPDX-License-Identifier: GPL-3.0-or-later
// A copy's own AI left out (brain.hpp).
#include "brain.hpp"

#include <array>
#include <vector>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
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
    int grace = 0;          // updates left with the far rule widened, after it became ours
    bool held = true;       // held at its last update
    int kind = -1;          // -1 not looked at yet, 0 no Stalchild, 1 a Stalchild
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

// The Stalchild's far rule (brain.hpp, "The far rule"): the two distances in the
// parameter file the game reads while the AI runs, widened for the updates that
// need it. One file serves Enemy_Stalchild and Enemy_Stalchild_Big.
std::mutex g_far_mutex;
std::uint64_t g_far_at[2] = {0, 0};      // the value words: OutsideUserPercepReturnDistance, ReturnDistance
std::uint32_t g_far_was[2] = {0, 0};     // what they held
int g_far_busy = 0;                      // updates running with them widened
bool g_far_looked = false;
int g_far_tries = 0;
std::atomic<std::uint64_t> g_far_runs{0};
std::atomic<int> g_tracked{0};
std::atomic<bool> g_link_alive_for_aim{true};           // Seen slots of Stalchildren still to look at
bool g_far_on = true;                    // OOT_COOP_STALCHILD_FAR=0 turns the far rule off

bool IsStalchild(const Nx2PluginApi* api, std::uint64_t actor) {
    std::uint64_t name_at = 0;
    char name[16] = {};
    return api->read(actor + kActorName, &name_at, 8) == 1 && name_at >= 0x1000 &&
           api->read(name_at, name, 15) == 1 && std::memcmp(name, "Enemy_Stalchild", 15) == 0;
}

// Finds the parameter file's two words from an actor that has it (called with the
// far mutex held). The dictionary's entries are (key index | type, value) pairs;
// 0xd2 is a float: key 7 OutsideUserPercepReturnDistance (18), key 11 ReturnDistance (24).
void FindFar(const Nx2PluginApi* api, std::uint64_t actor) {
    if (g_far_looked) {
        return;
    }
    std::uint64_t a = 0, b = 0;
    if (api->read(actor + kFarPath0, &a, 8) != 1 || a < 0x1000 || api->read(a + kFarPath1, &b, 8) != 1 ||
        b < 0x1000) {
        return;  // not made far enough yet: asked again at its next update
    }
    if (++g_far_tries > 300) {
        g_far_looked = true;  // not this build's layout: no more looks
    }
    for (const std::uint64_t at : {kFarPath2, kFarPath2b}) {
        std::uint64_t block = 0;
        std::array<std::uint32_t, 0x800 / 4> w{};
        if (api->read(b + at, &block, 8) != 1 || block < 0x1000 || api->read(block, w.data(), sizeof(w)) != 1) {
            continue;
        }
        for (std::size_t i = 0; i + 9 < w.size(); i += 1) {
            if (w[i] == 0xd2000007u && w[i + 1] == kFarOutside && w[i + 2] == 0xd2000008u &&
                w[i + 8] == 0xd200000bu && w[i + 9] == kFarReturn) {
                g_far_at[0] = block + (i + 1) * 4;
                g_far_at[1] = block + (i + 9) * 4;
                g_far_was[0] = kFarOutside;
                g_far_was[1] = kFarReturn;
                g_far_looked = true;
                return;
            }
        }
    }
}

void FarWiden(const Nx2PluginApi* api) {
    std::lock_guard lock(g_far_mutex);
    if (g_far_at[0] != 0 && g_far_busy++ == 0) {
        const std::uint32_t wide = kFarWide;
        api->write(g_far_at[0], &wide, 4);
        api->write(g_far_at[1], &wide, 4);
    }
}

void FarRestore(const Nx2PluginApi* api) {
    std::lock_guard lock(g_far_mutex);
    if (g_far_at[0] != 0 && --g_far_busy == 0) {
        api->write(g_far_at[0], &g_far_was[0], 4);
        api->write(g_far_at[1], &g_far_was[1], 4);
    }
}

// The Seen slot of an actor (the seen mutex held); a new one when `make`.
Seen* SlotOf(std::uint64_t actor, bool make, std::uint64_t life, std::uint32_t id) {
    for (Seen& s : g_seen) {
        if (s.actor == actor) {
            return &s;
        }
    }
    if (!make) {
        return nullptr;
    }
    // Held for the first time: its first updates run (kWarmFrames, brain.hpp).
    Seen* slot = &g_seen[g_seen_next++ % g_seen.size()];
    *slot = Seen{actor, life, id, kWarmFrames};
    return slot;
}

// Whether this update of a held copy runs because a hit came in for it (or it is
// one of its first ones).
bool RunsForHit(const Nx2PluginApi* api, std::uint64_t actor, std::uint64_t table, int& kind) {
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
    Seen* slot = SlotOf(actor, false, life, id);
    if (slot == nullptr) {
        slot = SlotOf(actor, true, life, id);
    } else if (slot->life != life) {
        slot->life = life;  // another Life component: what its header says is not a new hit
        slot->header_id = id;
    } else if (life != 0 && slot->header_id != id) {
        slot->header_id = id;  // the game put a hit in: our own Link's
        slot->left = slot->left > kHitFrames ? slot->left : kHitFrames;
    }
    slot->held = true;
    slot->grace = 0;
    for (Noted& n : g_noted) {
        if (life != 0 && n.life == life) {
            n.life = 0;  // the Mover put a hit in: the owner's game says so
            slot->left = slot->left > kHitFrames ? slot->left : kHitFrames;
        }
    }
    if (slot->kind < 0) {
        slot->kind = IsStalchild(api, actor) ? 1 : 0;
        if (slot->kind == 1) {
            g_tracked.fetch_add(1, std::memory_order_relaxed);
        }
    }
    kind = slot->kind;
    if (slot->left <= 0) {
        return false;
    }
    --slot->left;
    return true;
}

// An update of an actor that is not held now: whether it is a Stalchild that was a
// held copy until lately (it became ours), and then whether this update is one of
// the kGraceUpdates that run with the far rule widened.
bool InGrace(std::uint64_t actor) {
    std::lock_guard lock(g_seen_mutex);
    Seen* slot = SlotOf(actor, false, 0, 0);
    if (slot == nullptr || slot->kind != 1) {
        return false;
    }
    if (slot->held) {
        slot->held = false;
        slot->grace = kGraceUpdates;
    }
    if (slot->grace <= 0) {
        return false;
    }
    if (--slot->grace == 0) {
        slot->kind = 0;  // done: no more looks at it
        g_tracked.fetch_sub(1, std::memory_order_relaxed);
    }
    return true;
}

// Whether a Stalchild is still in its grace after it became ours (no change).
bool PeekGrace(std::uint64_t actor) {
    std::lock_guard lock(g_seen_mutex);
    const Seen* slot = SlotOf(actor, false, 0, 0);
    return slot != nullptr && slot->kind == 1 && !slot->held && slot->grace > 0;
}

std::atomic<std::uint64_t> g_burrows_refused{0};
std::atomic<std::uint64_t> g_burrows_redirected{0};
std::atomic<std::uint64_t> g_burrows_let{0};
std::atomic<std::uint64_t> g_burrows_held{0};
// Our own Link, from the session's frame: where, and whether alive (NoteOurLink).
std::mutex g_link_mutex;
float g_link[3] = {0, 0, 0};
bool g_link_known = false;
bool g_link_alive = true;
std::vector<std::array<float, 3>> g_others;  // living other players in our scene (their puppets' places)
bool g_debug_verbs = false;                      // OOT_COOP_DEBUG_VERBS=1
std::array<std::atomic<std::uint32_t>, 256> g_verb_seen{};
std::atomic<std::uint32_t> g_verb_other{0};
int g_verb_handle = 0;

// Our Link's actor (NoteOurPlayer) and a small block of the game's heap for the target call.
std::atomic<std::uint64_t> g_player{0};
std::atomic<std::uint64_t> g_main{0};
std::mutex g_target_mutex;
std::uint64_t g_target_block = 0;
std::atomic<std::uint64_t> g_targets_set{0};

// Points the enemy's brain target slot 0 at our Link, tracked without perception
// (brain.hpp, "The target"). On the thread running its AI update, before it runs.
void AimAtOurLink(const Nx2PluginApi* api, std::uint64_t actor) {
    const std::uint64_t player = g_player.load(std::memory_order_relaxed);
    const std::uint64_t main = g_main.load(std::memory_order_relaxed);
    if (player < 0x1000 || main == 0 || !NX2_HAS(api, Nx2PluginApi, guest_call) || api->guest_call == nullptr) {
        return;
    }
    std::uint64_t table = 0, target = 0, handle = 0, slot_handle = 0;
    std::uint32_t id = 0, slot_id = 0;
    std::uint8_t state = 0;
    if (api->read(actor + kActorComponents, &table, 8) != 1 || table < 0x1000 ||
        api->read(table + kComponentTarget, &target, 8) != 1 || target < 0x1000 ||
        api->read(player + kPlayerHandle, &handle, 8) != 1 || api->read(player + kPlayerId, &id, 4) != 1) {
        return;
    }
    const std::uint64_t slot = target + kTargetSlot0;
    api->read(slot + kSlotState, &state, 1);
    api->read(slot + kSlotHandle, &slot_handle, 8);
    api->read(slot + kSlotId, &slot_id, 4);
    if (state == kSlotValid && slot_handle == handle && slot_id == id) {
        std::uint8_t flags = 0, tracked = 0;
        api->read(slot + kSlotFlags, &flags, 1);
        api->read(slot + kSlotTracked, &tracked, 1);
        flags |= 4;
        tracked |= 1;
        api->write(slot + kSlotFlags, &flags, 1);
        api->write(slot + kSlotTracked, &tracked, 1);
        return;
    }
    std::lock_guard lock(g_target_mutex);
    if (g_target_block == 0) {
        const std::uint64_t size = 0x40;
        g_target_block = api->guest_call(main + kGameMalloc, &size, 1);
        if (g_target_block == 0) {
            return;
        }
    }
    std::uint8_t link[0x18] = {};
    std::memcpy(link + 8, &handle, 8);
    std::memcpy(link + 0x10, &id, 4);
    const std::uint8_t opts[8] = {0, 1, 1, 0, 1, 0, 0, 0};  // keep 0, track 1, always 1, type 1
    if (api->write(g_target_block, link, sizeof(link)) != 1 || api->write(g_target_block + 0x20, opts, 8) != 1) {
        return;
    }
    const std::uint64_t args[3] = {slot, g_target_block, g_target_block + 0x20};
    api->guest_call(main + kTargetSet, args, 3);
    g_targets_set.fetch_add(1, std::memory_order_relaxed);
}

// Runs the update now with the far rule widened (the call is not left out).
void RunWidened(const Nx2PluginApi* api, Nx2HookCall* call) {
    {
        std::lock_guard lock(g_far_mutex);
        FindFar(api, call->x[0]);
    }
    FarWiden(api);
    api->call_original(call);
    FarRestore(api);
    g_far_runs.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

void OnActorAi(Nx2HookCall* call, void*) {
    const Nx2PluginApi* api = g_api.load(std::memory_order_acquire);
    if (api == nullptr) {
        return;
    }
    g_calls.fetch_add(1, std::memory_order_relaxed);
    const bool any_held = Hold::TheTable().Count() != 0;
    const bool tracked = g_far_on && g_tracked.load(std::memory_order_relaxed) > 0;
    if (!any_held && !tracked) {
        return;  // nothing is held and no Stalchild became ours lately: no read at all
    }
    const std::uint64_t actor = call->x[0];
    std::uint64_t table = 0, bsa = 0;
    if (actor < 0x1000 || api->read(actor + kActorComponents, &table, 8) != 1 || table < 0x1000 ||
        api->read(table + kComponentBsa, &bsa, 8) != 1) {
        return;
    }
    std::uint32_t bits = 0;
    if (!any_held || !Hold::TheTable().Alter(bsa, bits)) {
        if (tracked && InGrace(actor)) {
            if (g_link_alive_for_aim.load(std::memory_order_relaxed)) {
                AimAtOurLink(api, actor);  // it just became ours: our Link is its target now
            }
            RunWidened(api, call);
        }
        return;
    }
    int kind = 0;
    if (RunsForHit(api, actor, table, kind)) {
        g_for_hits.fetch_add(1, std::memory_order_relaxed);
        if (kind == 1 && g_far_on) {
            RunWidened(api, call);
        }
        return;
    }
    call->skip = 1;
    g_skipped.fetch_add(1, std::memory_order_relaxed);
}

void OnActionVerb(Nx2HookCall* call, void*) {
    const Nx2PluginApi* api = g_api.load(std::memory_order_acquire);
    if (api == nullptr || !g_far_on) {
        return;
    }
    const std::uint64_t request = call->x[1];
    std::uint32_t verb = 0;
    if (request < 0x1000 || api->read(request + kVerbRequestVerb, &verb, 4) != 1) {
        return;
    }
    if (g_debug_verbs) {
        std::uint64_t n = 0, a0 = 0, b0 = 0, act = 0;
        if (api->read(request + kVerbRequestNode, &n, 8) == 1 && n >= 0x1000 && api->read(n + 8, &a0, 8) == 1 &&
            a0 >= 0x1000 && api->read(a0 + 8, &b0, 8) == 1 && b0 >= 0x1000 &&
            api->read(b0 + kNodeActor, &act, 8) == 1 && act >= 0x1000 && IsStalchild(api, act)) {
            g_verb_seen[verb & 255].fetch_add(1, std::memory_order_relaxed);
        } else {
            g_verb_other.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (verb != kVerbDisappear) {
        return;
    }
    std::uint64_t node = 0, a = 0, b = 0, actor = 0;
    if (api->read(request + kVerbRequestNode, &node, 8) != 1 || node < 0x1000 || api->read(node + 8, &a, 8) != 1 ||
        a < 0x1000 || api->read(a + 8, &b, 8) != 1 || b < 0x1000 || api->read(b + kNodeActor, &actor, 8) != 1 ||
        actor < 0x1000 || !IsStalchild(api, actor)) {
        return;
    }
    bool held = false;
    std::uint64_t table = 0, bsa = 0;
    std::uint32_t bits = 0;
    if (Hold::TheTable().Count() != 0 && api->read(actor + kActorComponents, &table, 8) == 1 && table >= 0x1000 &&
        api->read(table + kComponentBsa, &bsa, 8) == 1) {
        held = Hold::TheTable().Alter(bsa, bits);
    }
    if (held) {
        call->skip = 1;  // a copy: its owner's game decides when it burrows
        g_burrows_refused.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    float link[3];
    bool alive = false, known = false;
    std::vector<std::array<float, 3>> others;
    {
        std::lock_guard lock(g_link_mutex);
        std::memcpy(link, g_link, sizeof(link));
        alive = g_link_alive;
        known = g_link_known;
        others = g_others;
    }
    float at[3];
    if (api->read(actor + kActorPos, at, sizeof(at)) != 1) {
        return;
    }
    auto near = [&at](const float* p) {
        const float dx = at[0] - p[0], dy = at[1] - p[1], dz = at[2] - p[2];
        return dx * dx + dy * dy + dz * dz <= kChaseRange * kChaseRange;
    };
    const bool ours_near = known && alive && near(link);
    bool other_near = false;
    for (const auto& o : others) {
        other_near = other_near || near(o.data());
    }
    if (!ours_near && other_near) {
        // Our Link is dead or out of its reach, another living player is in it: it
        // waits for the server to give it to that player's game (owner rule), it does
        // not burrow here. Asked again each frame, so it goes once nobody is near.
        call->skip = 1;
        g_burrows_held.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!PeekGrace(actor)) {
        return;  // ours for a while: the game's own rule
    }
    // It just became ours. If our living Link is within its chase range, it is sent
    // walking to him instead (its brain takes him as its target on the way); else
    // it burrows as the game wants (nobody it could chase is near).
    if (!ours_near) {
        g_burrows_let.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::uint32_t walk = kVerbWalk;
    api->write(request + kVerbRequestVerb, &walk, 4);
    api->write(request + kVerbRequestTargetPos, link, sizeof(link));
    g_burrows_redirected.fetch_add(1, std::memory_order_relaxed);
}

void NoteOthers(const std::vector<std::array<float, 3>>& living) {
    std::lock_guard lock(g_link_mutex);
    g_others = living;
}

void NoteOurLink(const float* position, bool alive) {
    std::lock_guard lock(g_link_mutex);
    if (position != nullptr) {
        std::memcpy(g_link, position, sizeof(g_link));
        g_link_known = true;
    } else {
        g_link_known = false;
    }
    g_link_alive = alive;
    g_link_alive_for_aim.store(alive && position != nullptr, std::memory_order_relaxed);
}

std::uint64_t FindPlayer(const Nx2PluginApi* api, std::uint64_t main_base) {
    std::uint64_t manager = 0, lists = 0, node = 0;
    std::uint32_t count = 0;
    if (api == nullptr || main_base == 0 || api->read(main_base + 0x4e91888, &manager, 8) != 1 || manager < 0x1000 ||
        api->read(manager + 0x118, &lists, 8) != 1 || lists < 0x1000) {
        return 0;
    }
    const std::uint64_t head = lists + 0x670;
    if (api->read(head + 0x10, &count, 4) != 1 || count > 8192 || api->read(head, &node, 8) != 1) {
        return 0;
    }
    for (std::uint32_t i = 0; i < count && node >= 0x1000 && node != head; ++i) {
        std::uint64_t owner = 0, name_at = 0;
        char name[16] = {};
        if (api->read(node + 0x10, &owner, 8) == 1 && owner + 0x538 == node &&
            api->read(owner + kActorName, &name_at, 8) == 1 && name_at >= 0x1000 &&
            api->read(name_at, name, 13) == 1 &&
            (std::memcmp(name, "Player_Adult", 13) == 0 || std::memcmp(name, "Player_Child", 13) == 0)) {
            return owner;
        }
        if (api->read(node, &node, 8) != 1) {
            break;
        }
    }
    return 0;
}

void NoteOurPlayer(std::uint64_t main_base, std::uint64_t actor) {
    g_main.store(main_base, std::memory_order_relaxed);
    g_player.store(actor, std::memory_order_relaxed);
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
    if (const char* dv = std::getenv("OOT_COOP_DEBUG_VERBS"); dv != nullptr && *dv == '1') {
        g_debug_verbs = true;
    }
    if (const char* far = std::getenv("OOT_COOP_STALCHILD_FAR"); far != nullptr && *far == '0') {
        g_far_on = false;
    }
    std::uint64_t function = kActorAi;
    if (const char* other = std::getenv("OOT_COOP_BRAIN_SKIP"); other != nullptr && *other != 0) {
        function = std::strtoull(other, nullptr, 16);
    }
    g_handle = api->hook_function(main_base + function, OnActorAi, nullptr);
    if (g_handle <= 0) {
        g_handle = 0;
        return false;
    }
    if (g_far_on) {
        g_verb_handle = api->hook_function(main_base + kActionVerbInvoke, OnActionVerb, nullptr);
        if (g_verb_handle < 0) {
            g_verb_handle = 0;
        }
    }
    return true;
}

void Remove() {
    std::lock_guard lock(g_mutex);
    const Nx2PluginApi* api = g_api.load();
    if (api != nullptr && g_handle > 0) {
        api->remove(g_handle);
    }
    if (api != nullptr && g_verb_handle > 0) {
        api->remove(g_verb_handle);
    }
    g_handle = 0;
    g_verb_handle = 0;
    std::lock_guard seen(g_seen_mutex);
    g_seen = {};
    g_noted = {};
    g_tracked = 0;
    std::lock_guard far(g_far_mutex);
    if (api != nullptr && g_far_at[0] != 0 && g_far_busy > 0) {
        api->write(g_far_at[0], &g_far_was[0], 4);
        api->write(g_far_at[1], &g_far_was[1], 4);
    }
    g_far_busy = 0;
    g_far_looked = false;
    g_far_tries = 0;
    g_far_at[0] = g_far_at[1] = 0;
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
    c.far_runs = g_far_runs.exchange(0);
    c.burrows_refused = g_burrows_refused.exchange(0);
    c.burrows_redirected = g_burrows_redirected.exchange(0);
    c.burrows_let = g_burrows_let.exchange(0);
    c.burrows_held = g_burrows_held.exchange(0);
    c.targets_set = g_targets_set.exchange(0);
    if (g_debug_verbs) {
        char t[32];
        std::snprintf(t, sizeof(t), "other %u;", g_verb_other.exchange(0));
        c.verbs = t;
        for (std::size_t v = 0; v < g_verb_seen.size(); ++v) {
            if (const std::uint32_t k = g_verb_seen[v].exchange(0); k != 0) {
                std::snprintf(t, sizeof(t), " %zu:%u", v, k);
                c.verbs += t;
            }
        }
    }
    c.far_tries = g_far_tries;
    {
        std::lock_guard lock(g_far_mutex);
        c.far_found = g_far_at[0] != 0;
    }
    return c;
}

}  // namespace CoopNx2navi::Objects::Brain
