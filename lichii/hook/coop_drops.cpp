// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared drops (coop_drops.h, docs/objects.md "Drops"). Sync and Remover need
// no emulator code and are tested in tests/lichii/drops_test.cpp; the hook
// glue at the end is built into the emulator only (LICHII_COOP).

#include "coop_drops.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <variant>

namespace CoopDrops {

namespace proto = coop::protocol;

namespace {

// Every pick-up pack of build e0f3ccfaf50065ae that a kill or a break can
// leave. Not here: fairies (they fly off and heal one player), heart pieces
// and containers, tokens (one-time pickups: flags, docs/items.md).
constexpr std::array<std::string_view, 18> kDrops{{
    "Obj_Rupee_Green",
    "Obj_Rupee_Blue",
    "Obj_Rupee_Red",
    "Obj_Rupee_Purple",
    "Obj_Rupee_Silver",
    "Obj_Rupee_Gold",
    "Obj_Heart",
    "Obj_MagicJar_Small",
    "Obj_MagicJar_Large",
    "Obj_ArrowBundle_Small",
    "Obj_ArrowBundle_Medium",
    "Obj_ArrowBundle_Large",
    "Obj_Bomb",
    "Obj_Bombchu",
    "Obj_DekuNut",
    "Obj_DekuSeed",
    "Obj_DekuStick",
    "Obj_Arrow_Fire",
}};

float Distance(const Vec3& a, const Vec3& b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

bool Finite(const Vec3& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

std::uint64_t Apart(std::uint64_t a, std::uint64_t b) {
    return a > b ? a - b : b - a;
}

std::string Hex(std::uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%#llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string Tag(const proto::ObjectId& id) {
    return "P" + std::to_string(id.maker) + "#" + Hex(id.value);
}

// Main offsets (build e0f3ccfaf50065ae): docs/objects.md.
constexpr std::uint64_t kActorName = 0x260;
constexpr std::uint64_t kActorNode = 0x538;
constexpr std::uint64_t kNodeOwner = 0x10;
constexpr std::uint32_t kStateAlive = 4;

} // namespace

bool IsDrop(std::string_view name) {
    return std::find(kDrops.begin(), kDrops.end(), name) != kDrops.end();
}

// ---------------------------------------------------------------- Sync

void Sync::Receive(const proto::Message& message) {
    if (std::holds_alternative<proto::DropSpawn>(message) ||
        std::holds_alternative<proto::DropTake>(message) ||
        std::holds_alternative<proto::DropAsk>(message)) {
        std::lock_guard lock(inbox_mutex_);
        if (inbox_.size() < 1024) {
            inbox_.push_back(message);
        }
    }
}

void Sync::Reset() {
    scene_.clear();
    first_walk_ = true;
    old_.clear();
    drops_.clear();
    bound_.clear();
    deleting_.clear();
    ghosts_.clear();
    gone_unknown_.clear();
    sources_.clear();
    tell_at_ = 0;
    outbox_.clear();
    std::lock_guard lock(inbox_mutex_);
    inbox_.clear();
}

std::size_t Sync::Live() const {
    return static_cast<std::size_t>(
        std::count_if(drops_.begin(), drops_.end(),
                      [](const auto& e) { return e.second.state == State::live; }));
}

std::optional<proto::ObjectId> Sync::IdOf(std::uint64_t address) const {
    const auto it = bound_.find(address);
    return it == bound_.end() ? std::nullopt : std::optional(it->second);
}

void Sync::Note(std::string text) {
    if (notes_.size() < 64) {
        notes_.push_back(std::move(text));
    }
}

void Sync::Queue(proto::Message message) {
    if (outbox_.size() < 256) {
        outbox_.push_back(std::move(message));
    }
}

bool Sync::Matches(const Drop& a, const Drop& b) {
    return a.id.maker != b.id.maker && Apart(a.time_us, b.time_us) < kMatchUs &&
           Distance(a.origin, b.origin) < kMatchM;
}

bool Sync::Beats(const Drop& a, const Drop& b) {
    if (a.owned != b.owned) {
        return a.owned;
    }
    return a.id.maker < b.id.maker;
}

bool Sync::Beaten(const Drop& d) const {
    for (const auto& [id, other] : drops_) {
        if (id != d.id && Matches(other, d) && Beats(other, d)) {
            return true;
        }
    }
    return false;
}

void Sync::Bind(Drop& d, std::uint64_t address) {
    d.address = address;
    d.missed = 0;
    bound_[address] = d.id;
}

void Sync::Delete(std::uint64_t address, const std::string& name, std::uint64_t now_us,
                  std::vector<Remove>& removes) {
    if (deleting_.emplace(address, std::make_pair(name, now_us)).second) {
        removes.push_back({address, name});
    }
}

void Sync::Lose(Drop& d, Drop* winner, State state, std::uint64_t now_us,
                std::vector<Remove>& removes) {
    const std::uint64_t address = d.address;
    d.state = state;
    d.done_us = now_us;
    if (address != 0) {
        bound_.erase(address);
        d.address = 0;
    }
    if (address != 0 && winner != nullptr && winner->state == State::live &&
        winner->address == 0 && winner->name == d.name && (d.key == 0 || winner->key == 0)) {
        // The same kind of drop for the same thing: our actor is the winner's.
        Bind(*winner, address);
        winner->position = d.position;
        if (d.key != 0) {  // the director's slot goes with the actor
            winner->key = d.key;
            winner->made = d.made;
            winner->asked_us = d.asked_us;
        }
        Note(Tag(d.id) + " " + d.name + " lost to " + Tag(winner->id) + ", actor kept");
    } else if (address != 0) {
        if (d.key != 0) {
            // The director still holds it: no longer listed, it deletes it.
            deleting_.emplace(address, std::make_pair(d.name, now_us));
        } else {
            Delete(address, d.name, now_us, removes);
        }
        Note(Tag(d.id) + " " + d.name + (state == State::taken ? " taken" : " lost") +
             ", actor deleted");
    } else if (d.key != 0) {
        // A create on its way: the director deletes it once made. Until then
        // it must not be taken for a roll of ours.
        ghosts_.push_back({d.name, d.position, now_us});
    }
    d.key = 0;
    d.made = false;
}

void Sync::Learn(const proto::DropSpawn& spawn, std::uint64_t now_us,
                 std::vector<Remove>& removes) {
    Drop d;
    d.id = spawn.id;
    d.name = spawn.actor;
    d.origin = d.position = spawn.position;
    d.time_us = spawn.time_us;
    d.owned = (spawn.flags & proto::drop_spawn::owned) != 0;
    d.got_us = now_us;
    d.wait_us = (spawn.flags & proto::drop_spawn::again) != 0 ? 0 : kAdoptWaitUs;
    Drop& x = drops_.emplace(spawn.id, std::move(d)).first->second;
    if (Beaten(x)) {
        // A better roll for the same thing is known: this one never shows.
        x.state = State::lost;
        x.done_us = now_us;
        return;
    }
    for (auto& [id, other] : drops_) {
        if (other.state == State::live && id != x.id && Matches(x, other) && Beats(x, other)) {
            if (other.id.maker == self_) {
                x.wait_us = 0;  // our own roll was there: nothing to wait for
            }
            Lose(other, &x, State::lost, now_us, removes);
        }
    }
}

void Sync::Apply(std::uint8_t self, std::uint64_t now_us, std::vector<Remove>& removes) {
    std::vector<proto::Message> inbox;
    {
        std::lock_guard lock(inbox_mutex_);
        inbox.swap(inbox_);
    }
    for (const auto& m : inbox) {
        if (const auto* s = std::get_if<proto::DropSpawn>(&m)) {
            if (s->scene != scene_ || s->player_number == self || drops_.count(s->id) != 0 ||
                gone_unknown_.count(s->id) != 0 || !IsDrop(s->actor) || !Finite(s->position) ||
                drops_.size() >= kMaxDrops) {
                continue;
            }
            Learn(*s, now_us, removes);
        } else if (const auto* t = std::get_if<proto::DropTake>(&m)) {
            const auto it = drops_.find(t->id);
            if (it == drops_.end()) {
                if (gone_unknown_.size() < kMaxDrops) {
                    gone_unknown_[t->id] = now_us;
                }
            } else if (it->second.state == State::live || it->second.state == State::away) {
                Lose(it->second, nullptr, State::taken, now_us, removes);
            }
        } else if (const auto* a = std::get_if<proto::DropAsk>(&m)) {
            if (a->scene == scene_ && a->player_number != self && tell_at_ == 0) {
                tell_at_ = now_us + kTellAfterUs;
            }
        }
    }
}

void Sync::Tick(const std::string& scene, std::uint8_t self, std::uint64_t now_us,
                const std::vector<Seen>& seen, const std::optional<Vec3>& player,
                const std::vector<Vec3>& owned, const std::map<std::uint64_t, std::uint64_t>& made,
                std::vector<proto::Message>& send, std::vector<Create>& creates,
                std::vector<Remove>& removes) {
    if (scene != scene_) {
        Reset();
        scene_ = scene;
        tokens_ = kSendsPerSecond;
        tokens_at_ = now_us;
        Queue(proto::DropAsk{0, scene});
    }
    self_ = self;

    // The shared things we own, where they were last seen.
    for (const Vec3& p : owned) {
        if (!Finite(p)) {
            continue;
        }
        const auto it = std::find_if(sources_.begin(), sources_.end(), [&](const auto& s) {
            return Distance(s.first, p) < 1.0f;
        });
        if (it != sources_.end()) {
            *it = {p, now_us};
        } else if (sources_.size() < 512) {
            sources_.emplace_back(p, now_us);
        }
    }
    std::erase_if(sources_, [&](const auto& s) { return now_us - s.second > kSourceUs; });

    Apply(self, now_us, removes);

    std::map<std::uint64_t, const Seen*> on_list;
    for (const Seen& s : seen) {
        on_list.emplace(s.address, &s);
    }
    if (first_walk_) {
        for (const auto& [address, s] : on_list) {
            old_.insert(address);  // lay there when we came: nobody's roll now
        }
        first_walk_ = false;
    }

    // What the director made.
    for (auto& [id, d] : drops_) {
        if (d.key == 0) {
            continue;
        }
        const auto it = made.find(d.key);
        if (it == made.end() || it->second == 0) {
            if (d.made) {  // let go of, as asked (or its actor went)
                d.key = 0;
                d.made = false;
            }
            continue;
        }
        d.made = true;
        if (d.address == 0) {
            Bind(d, it->second);
        } else if (d.address != it->second) {
            // We kept another actor as this drop meanwhile: the director's
            // one is not listed any more, and it deletes it.
            deleting_.emplace(it->second, std::make_pair(d.name, now_us));
            d.key = 0;
            d.made = false;
        }
    }

    // Our actors: where they are, and the ones that left the list.
    std::vector<proto::ObjectId> left;
    for (auto& [id, d] : drops_) {
        if (d.address == 0) {
            continue;
        }
        if (const auto it = on_list.find(d.address); it != on_list.end()) {
            if (Finite(it->second->position)) {
                d.position = it->second->position;
            }
            d.missed = 0;
        } else if (++d.missed >= kGoneWalks) {
            left.push_back(id);
        }
    }
    const bool unloaded = !player || left.size() >= kUnloadCount;
    for (const auto& id : left) {
        Drop& d = drops_.at(id);
        bound_.erase(d.address);
        d.address = 0;
        d.key = 0;
        d.made = false;
        d.done_us = now_us;
        const float away = unloaded ? std::numeric_limits<float>::max() : Distance(*player, d.position);
        if (away < kTakeM || away < near_m_) {
            const auto reason =
                away < kTakeM ? proto::DropTakeReason::picked_up : proto::DropTakeReason::gone;
            d.state = State::taken;
            Queue(proto::DropTake{0, id, reason});
            Note(Tag(id) + " " + d.name +
                 (reason == proto::DropTakeReason::picked_up ? " picked up here" : " blinked out here"));
        } else {
            d.state = State::away;
            Note(Tag(id) + " " + d.name + " unloaded here");
        }
    }
    std::erase_if(old_, [&](std::uint64_t a) { return on_list.count(a) == 0; });
    std::erase_if(deleting_, [&](const auto& e) {
        return on_list.count(e.first) == 0 || now_us - e.second.second > kDeleteWaitUs;
    });
    std::erase_if(ghosts_, [&](const Ghost& g) { return now_us - g.since_us > kCreateWaitUs; });

    // Actors that are new on the list.
    for (const Seen& s : seen) {
        if (old_.count(s.address) != 0 || bound_.count(s.address) != 0 ||
            deleting_.count(s.address) != 0 || !Finite(s.position)) {
            continue;
        }
        // A create that was called off: the director deletes it.
        const auto ghost = std::find_if(ghosts_.begin(), ghosts_.end(), [&](const Ghost& g) {
            return g.name == s.name && Distance(g.position, s.position) < kMatchM;
        });
        if (ghost != ghosts_.end()) {
            deleting_.emplace(s.address, std::make_pair(s.name, now_us));
            ghosts_.erase(ghost);
            continue;
        }
        Drop mine;
        mine.id = {proto::ObjectKind::made, self, 0};
        mine.name = s.name;
        mine.origin = mine.position = s.position;
        mine.time_us = now_us;
        mine.got_us = now_us;
        mine.owned = std::any_of(sources_.begin(), sources_.end(), [&](const auto& src) {
            return Distance(src.first, s.position) < kSourceM;
        });
        // A copy being made, or a winner that waits for our own roll.
        Drop* fits = nullptr;
        float best = kMatchM;
        for (auto& [id, d] : drops_) {
            if (d.state != State::live || d.address != 0 || d.name != s.name) {
                continue;
            }
            const float apart = Distance(d.position, s.position);
            if (apart >= best) {
                continue;
            }
            if (d.key == 0 && (self == 0 || !Matches(d, mine) || Beats(mine, d))) {
                continue;
            }
            fits = &d;
            best = apart;
        }
        if (fits != nullptr) {
            if (fits->key == 0) {
                ++adopted_;
                Note("own " + s.name + " kept as " + Tag(fits->id));
            }
            Bind(*fits, s.address);
            fits->position = s.position;
            continue;
        }
        if (self == 0) {
            continue;
        }
        if (Beaten(mine)) {
            // Another game's roll for the same thing stays; ours goes.
            Delete(s.address, s.name, now_us, removes);
            ++refused_;
            Note("own " + s.name + " deleted: another game's roll stays");
            continue;
        }
        if (drops_.size() >= kMaxDrops) {
            continue;
        }
        if (counter_ == 0) {
            counter_ = now_us;  // ids stay unique across reconnects
        }
        mine.id.value = ++counter_;
        Drop& d = drops_.emplace(mine.id, mine).first->second;
        Bind(d, s.address);
        proto::DropSpawn out;
        out.id = d.id;
        out.scene = scene_;
        out.actor = d.name;
        out.position = d.position;
        out.time_us = d.time_us;
        out.flags = d.owned ? proto::drop_spawn::owned : std::uint8_t{0};
        Queue(std::move(out));
        for (auto& [id, other] : drops_) {
            if (other.state == State::live && id != d.id && Matches(d, other) && Beats(d, other)) {
                Lose(other, &d, State::lost, now_us, removes);
            }
        }
    }

    // A player came into the scene: the drops we still show, again.
    if (tell_at_ != 0 && now_us >= tell_at_) {
        tell_at_ = 0;
        for (const auto& [id, d] : drops_) {
            if (d.state != State::live || d.address == 0) {
                continue;
            }
            proto::DropSpawn out;
            out.id = id;
            out.scene = scene_;
            out.actor = d.name;
            out.position = d.position;
            out.time_us = d.time_us;
            out.flags = static_cast<std::uint8_t>(proto::drop_spawn::again |
                                                  (d.owned ? proto::drop_spawn::owned : 0));
            Queue(std::move(out));
        }
    }

    // Copies for the director.
    for (auto& [id, d] : drops_) {
        if (d.state != State::live) {
            continue;
        }
        if (d.address == 0 && d.key == 0) {
            if (now_us < d.got_us + d.wait_us || d.id.maker == self) {
                continue;
            }
            d.key = next_key_++;
            d.asked_us = now_us;
        }
        if (d.key == 0) {
            continue;
        }
        if (d.address == 0 && now_us - d.asked_us > kCreateWaitUs) {
            ghosts_.push_back({d.name, d.position, now_us});
            d.key = 0;
            d.state = State::away;
            d.done_us = now_us;
            Note(Tag(id) + " " + d.name + " could not be made here");
            continue;
        }
        creates.push_back({d.key, d.name, d.position, d.address != 0 && d.made});
    }

    // Old news.
    std::erase_if(drops_, [&](const auto& e) {
        return e.second.state != State::live && now_us - e.second.done_us > kKeepUs;
    });
    std::erase_if(gone_unknown_, [&](const auto& e) { return now_us - e.second > kKeepUs; });

    // At most kSendsPerSecond messages a second, so a field of cut grass
    // cannot use up the server's limit for this player.
    tokens_ = std::min<double>(kSendsPerSecond,
                               tokens_ + double(now_us - tokens_at_) / 1e6 * kSendsPerSecond);
    tokens_at_ = now_us;
    while (!outbox_.empty() && tokens_ >= 1.0) {
        send.push_back(std::move(outbox_.front()));
        outbox_.pop_front();
        tokens_ -= 1.0;
    }
}

// ---------------------------------------------------------------- Remover

void Remover::Want(std::uint64_t main_base, std::vector<Remove> removes) {
    std::lock_guard lock(mutex_);
    base_ = main_base;
    removes_ = std::move(removes);
}

std::size_t Remover::TakeCount() {
    std::lock_guard lock(mutex_);
    return std::exchange(count_, 0);
}

bool Remover::Alive(CoopPuppet::Guest& guest, const Remove& r) const {
    std::uint32_t state = 0;
    std::uint64_t owner = 0, text = 0;
    std::array<char, 65> name{};
    if (r.address == 0 || r.name.empty() || r.name.size() >= name.size() ||
        !guest.Read(r.address + CoopPuppet::addr::kActorState, &state, 4) || state != kStateAlive ||
        !guest.Read(r.address + kActorNode + kNodeOwner, &owner, 8) || owner != r.address ||
        !guest.Read(r.address + kActorName, &text, 8) || text == 0 ||
        !guest.Read(text, name.data(), r.name.size() + 1)) {
        return false;
    }
    return name[r.name.size()] == '\0' && r.name == name.data();
}

std::optional<CoopPuppet::Call> Remover::Step(CoopPuppet::Guest& guest, bool after_call,
                                              std::uint64_t /*result*/, std::uint64_t /*now_ms*/) {
    if (!after_call) {
        {
            std::lock_guard lock(mutex_);
            work_ = removes_;
            work_base_ = base_;
        }
        next_ = 0;
        calls_ = 0;
        std::erase_if(asked_, [&](std::uint64_t a) {
            return std::none_of(work_.begin(), work_.end(),
                                [&](const Remove& r) { return r.address == a; });
        });
    }
    if (work_base_ == 0) {
        return std::nullopt;
    }
    while (next_ < work_.size() && calls_ < kMaxCallsPerFrame) {
        const Remove& r = work_[next_++];
        if (asked_.count(r.address) != 0 || !Alive(guest, r)) {
            continue;  // not alive yet: looked at again next frame
        }
        asked_.insert(r.address);
        ++calls_;
        {
            std::lock_guard lock(mutex_);
            ++count_;
        }
        CoopPuppet::Call c;
        c.function = work_base_ + CoopPuppet::addr::kRequestDelete;
        c.x[0] = r.address;
        c.x[1] = 0;
        return c;
    }
    return std::nullopt;
}

} // namespace CoopDrops

#ifdef LICHII_COOP  // the hook, built into the emulator

#include <cstdlib>
#include <unordered_map>

#include "common/logging.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/loader/loader.h"
#include "core/memory.h"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop_objects.h"
#include "coop_spawn.h"
#include "coop_svc.h"

namespace CoopDrops {
namespace {

constexpr u64 kOcarinaProgramId = 0x0400486024DEC000ULL;
constexpr std::array<u8, 8> kGameBuildId{0xe0, 0xf3, 0xcc, 0xfa, 0xf5, 0x00, 0x65, 0xae};
// The live actor list, as coop_objects.cpp walks it (docs/objects.md).
constexpr u64 kManagerLists = 0x118;
constexpr u64 kListHead = 0x670;
constexpr u64 kListCount = 0x10;
constexpr u64 kActorPlacement = 0x3d8;
constexpr u64 kPlacementHash = 0x88;
constexpr std::size_t kMaxWalk = 8192;
constexpr unsigned kEveryFrames = 2;
constexpr u64 kDropKey = u64{1} << 62;  // the director's keys for drops
constexpr int kCallerOrder = 40;        // after the mover (30)

std::string Env(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return {};
    }
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
#endif
}

bool EnvOn(const char* name) {
    const std::string v = Env(name);
    return !v.empty() && v != "0";
}

// On unless LICHII_COOP_DROPS=0; needs the cave (coop_svc.cpp patches it for
// these switches) to create and delete.
bool On() {
    static const bool on = Env("LICHII_COOP_DROPS") != "0" &&
                           (EnvOn("LICHII_COOP_PUPPET") || EnvOn("LICHII_COOP_DEBUG_PUPPET") ||
                            EnvOn("LICHII_COOP_SPAWN"));
    return on;
}

Remover& TheRemover() {
    static Remover remover;
    return remover;
}

class Glue {
public:
    Glue() {
        if (const std::string near = Env("LICHII_COOP_DROPS_NEAR"); !near.empty()) {
            sync_.SetNear(std::strtof(near.c_str(), nullptr));
        }
        placed_ = EnvOn("LICHII_COOP_DROPS_PLACED");
    }

    void Frame(Core::System& system, coop::host::Session* session,
               const std::optional<std::string>& scene, u64 now_us) {
        if (session == nullptr || !scene || scene->empty() || now_us == 0 || !Ready(system)) {
            if (active_) {
                sync_.Reset();
                CoopSpawn::TheDirector().SetDrops(0, {});
                TheRemover().Want(0, {});
                active_ = false;
            }
            return;
        }
        if (++frame_ % kEveryFrames != 0) {
            return;
        }
        auto& memory = system.ApplicationMemory();
        std::vector<Seen> seen;
        std::optional<Vec3> player;
        std::vector<Vec3> owned;
        if (!Walk(memory, seen, player, owned)) {
            return;  // the list changed under us
        }
        const u8 self =
            session->with_client([](coop::net::Client& c) { return c.player_number(); });
        if (self == 0) {
            return;
        }
        if (!active_) {
            LOG_INFO(Core, "coop: drops: on");
        }
        active_ = true;
        std::map<std::uint64_t, std::uint64_t> made;
        for (const auto& [key, actor] : CoopSpawn::TheDirector().Live()) {
            if ((key & kDropKey) != 0) {
                made[key & ~kDropKey] = actor;
            }
        }
        std::vector<coop::protocol::Message> send;
        std::vector<Create> creates;
        std::vector<Remove> removes;
        sync_.Tick(*scene, self, now_us, seen, player, owned, made, send, creates, removes);

        std::vector<CoopSpawn::Copy> copies;
        for (const Create& c : creates) {
            CoopSpawn::Copy copy;
            copy.key = c.key | kDropKey;
            copy.actor = c.name;
            copy.position = c.position;
            copy.act = c.release ? CoopSpawn::Act::release : CoopSpawn::Act::show;
            copy.place = false;  // it falls and lies by itself
            copies.push_back(std::move(copy));
        }
        CoopSpawn::TheDirector().SetDrops(main_, std::move(copies));

        // A delete is asked until the actor has left the list (or 3 s).
        for (const Remove& r : removes) {
            removing_.push_back(r);
        }
        std::erase_if(removing_, [&](const Remove& r) { return !listed_.contains(r.address); });
        if (removing_.size() > 256) {
            removing_.erase(removing_.begin(), removing_.end() - 256);
        }
        TheRemover().Want(main_, removing_);

        for (const auto& note : sync_.TakeNotes()) {
            LOG_INFO(Core, "coop: drops: {}", note);
        }
        if (!send.empty()) {
            session->with_client([&](coop::net::Client& c) {
                for (const auto& m : send) {
                    const bool sent = c.send(m);
                    if (const auto* s = std::get_if<coop::protocol::DropSpawn>(&m)) {
                        LOG_INFO(Core, "coop: drops: sent {} #{:#x} at {:.2f} {:.2f} {:.2f} flags {}{}",
                                 s->actor, s->id.value, s->position[0], s->position[1],
                                 s->position[2], s->flags, sent ? "" : " (not sent)");
                    } else if (const auto* t = std::get_if<coop::protocol::DropTake>(&m)) {
                        LOG_INFO(Core, "coop: drops: sent take P{}#{:#x} reason {}{}", t->id.maker,
                                 t->id.value, static_cast<int>(t->reason),
                                 sent ? "" : " (not sent)");
                    } else if (std::holds_alternative<coop::protocol::DropAsk>(m)) {
                        LOG_INFO(Core, "coop: drops: asked for the drops of {}{}", *scene,
                                 sent ? "" : " (not sent)");
                    }
                }
            });
        }
        const std::size_t deleted = TheRemover().TakeCount();
        if (sync_.Live() != live_logged_ || sync_.Refused() != refused_logged_ ||
            sync_.Adopted() != adopted_logged_ || deleted != 0) {
            live_logged_ = sync_.Live();
            refused_logged_ = sync_.Refused();
            adopted_logged_ = sync_.Adopted();
            LOG_INFO(Core, "coop: drops: {} lying, {} own rolls deleted, {} kept as another's, {} deletes asked now",
                     live_logged_, refused_logged_, adopted_logged_, deleted);
        }
    }

    void Receive(const coop::protocol::Message& m) {
        if (const auto* s = std::get_if<coop::protocol::DropSpawn>(&m)) {
            LOG_INFO(Core, "coop: drops: got {} P{}#{:#x} from P{} in {} flags {}", s->actor,
                     s->id.maker, s->id.value, s->player_number, s->scene, s->flags);
        } else if (const auto* t = std::get_if<coop::protocol::DropTake>(&m)) {
            LOG_INFO(Core, "coop: drops: got take P{}#{:#x} from P{} reason {}", t->id.maker,
                     t->id.value, t->player_number, static_cast<int>(t->reason));
        }
        sync_.Receive(m);
    }

private:
    bool Ready(Core::System& system) {
        auto* process = system.ApplicationProcess();
        if (process == nullptr || system.GetApplicationProcessProgramID() != kOcarinaProgramId) {
            return false;
        }
        const auto& build = system.GetApplicationProcessBuildID();
        if (std::memcmp(build.data(), kGameBuildId.data(), kGameBuildId.size()) != 0) {
            return false;
        }
        if (process != process_) {
            process_ = process;
            main_ = FindMain(system, process);
            known_.clear();
            listed_.clear();
            removing_.clear();
            sync_.Reset();
        }
        return main_ != 0;
    }

    static u64 FindMain(Core::System& system, Kernel::KProcess* process) {
        Loader::AppLoader::Modules modules;
        if (system.GetAppLoader().ReadNSOModules(modules) != Loader::ResultStatus::Success ||
            modules.empty()) {
            modules = Core::FindModules(process);
        }
        for (const auto& [address, name] : modules) {
            if (name == "main" || name.starts_with("main.")) {
                return address;
            }
        }
        return GetInteger(Core::FindMainModuleEntrypoint(process));
    }

    template <typename T>
    static std::optional<T> Get(Core::Memory::Memory& memory, u64 address) {
        if (address < 0x1000 || !memory.IsValidVirtualAddressRange(address, sizeof(T))) {
            return std::nullopt;
        }
        T value{};
        memory.ReadBlockUnsafe(address, &value, sizeof(T));
        return value;
    }

    static std::string Name(Core::Memory::Memory& memory, u64 actor) {
        const auto p = Get<u64>(memory, actor + kActorName);
        std::array<char, 64> buf{};
        if (!p || !memory.IsValidVirtualAddressRange(*p, buf.size())) {
            return "?";
        }
        memory.ReadBlockUnsafe(*p, buf.data(), buf.size() - 1);
        return std::string(buf.data(), strnlen(buf.data(), buf.size()));
    }

    static Vec3 Place(Core::Memory::Memory& memory, u64 actor) {
        Vec3 at{};
        for (std::size_t i = 0; i < 3; ++i) {
            at[i] = Get<float>(memory, actor + CoopPuppet::addr::kActorPosition + 4 * i)
                        .value_or(std::numeric_limits<float>::quiet_NaN());
        }
        return at;
    }

    enum class What : u8 { other, drop, player };

    // The drop actors on the list, our Link, and the places of the shared
    // things we own. False when the list changed under us.
    bool Walk(Core::Memory::Memory& memory, std::vector<Seen>& out, std::optional<Vec3>& player,
              std::vector<Vec3>& owned) {
        const auto manager = Get<u64>(memory, main_ + CoopPuppet::addr::kActorManagerPtr);
        const auto lists =
            manager && *manager != 0 ? Get<u64>(memory, *manager + kManagerLists) : std::nullopt;
        if (!lists || *lists == 0) {
            return false;
        }
        const u64 head = *lists + kListHead;
        const auto count = Get<u32>(memory, head + kListCount);
        if (!count) {
            return false;
        }
        std::unordered_map<u64, std::pair<What, std::string>> known;
        std::size_t walked = 0;
        auto node = Get<u64>(memory, head);
        while (node && *node != 0 && *node != head && walked < kMaxWalk) {
            ++walked;
            const auto owner = Get<u64>(memory, *node + kNodeOwner);
            if (owner && *owner + kActorNode == *node) {
                const u64 a = *owner;
                std::pair<What, std::string> entry{What::other, {}};
                if (const auto it = known_.find(a); it != known_.end()) {
                    entry = it->second;
                } else {
                    std::string name = Name(memory, a);
                    if (name == "Player_Child" || name == "Player_Adult") {
                        entry.first = What::player;
                    } else if (IsDrop(name)) {
                        const auto placement = Get<u64>(memory, a + kActorPlacement);
                        const u64 hash = placement && *placement != 0
                                             ? Get<u64>(memory, *placement + kPlacementHash).value_or(0)
                                             : 0;
                        if (hash == 0 || hash == ~u64{0} || placed_) {
                            entry = {What::drop, std::move(name)};
                        } else if (!first_) {
                            LOG_INFO(Core, "coop: drops: {} {:#x} is placed (hash {:#x}): not shared",
                                     name, a, hash);
                        }
                    }
                }
                if (entry.first == What::drop) {
                    out.push_back({a, entry.second, Place(memory, a)});
                } else if (entry.first == What::player) {
                    player = Place(memory, a);
                }
                known.emplace(a, std::move(entry));
            }
            node = Get<u64>(memory, *node);
        }
        if (!node || walked != *count) {
            out.clear();
            player.reset();
            return false;
        }
        known_ = std::move(known);
        first_ = false;
        listed_.clear();
        for (const auto& [a, entry] : known_) {
            listed_.insert(a);
        }
        for (const u64 a : CoopObjects::OwnedActors()) {
            if (listed_.contains(a)) {
                owned.push_back(Place(memory, a));
            }
        }
        return true;
    }

    Sync sync_;
    bool active_ = false;
    bool placed_ = false;
    bool first_ = true;
    unsigned frame_ = 0;
    Kernel::KProcess* process_ = nullptr;
    u64 main_ = 0;
    std::unordered_map<u64, std::pair<What, std::string>> known_;
    std::set<u64> listed_;
    std::vector<Remove> removing_;
    std::size_t live_logged_ = 0;
    std::size_t refused_logged_ = 0;
    std::size_t adopted_logged_ = 0;
};

Glue& TheGlue() {
    static Glue glue;
    return glue;
}

} // namespace

void Frame(Core::System& system, coop::host::Session* session,
           const std::optional<std::string>& scene, std::uint64_t server_now_us) {
    if (!On()) {
        return;
    }
    static const bool registered = [] {
        CoopSpawn::UseDirector();
        CoopSvc::AddCaller(kCallerOrder, [](CoopPuppet::Guest& guest, bool after_call,
                                            std::uint64_t result, std::uint64_t now_ms) {
            return TheRemover().Step(guest, after_call, result, now_ms);
        });
        return true;
    }();
    (void)registered;
    TheGlue().Frame(system, session, scene, server_now_us);
}

void OnEvent(const coop::net::Event& event) {
    if (On() && event.kind == coop::net::EventKind::message) {
        TheGlue().Receive(event.message);
    }
}

} // namespace CoopDrops

#endif  // LICHII_COOP
