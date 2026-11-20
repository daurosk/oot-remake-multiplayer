// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_boss_beaten.h"

#include <algorithm>
#include <utility>
#include <variant>

#include "coop_boss.h"
#include "coop_quests.h"
#include "coop/protocol/codec.hpp"

namespace CoopBossBeaten {

namespace {

#include "coop_boss_beaten.inc"

bool Has(std::span<const std::uint8_t> rooms, std::uint8_t room) {
    return std::find(rooms.begin(), rooms.end(), room) != rooms.end();
}

bool StartsWith(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

bool EndsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

// Row 138 (4): the alive value of a set's Bool for a quest at `quest_step` (a step in the set's `below`), or nothing
// when it is left alone (see Apply::HostReset).
std::optional<bool> AliveValue(const BeatenSet& set, const BeatenBool& b, std::uint32_t quest_step) {
    const std::string_view name(b.name);
    if (b.value) {
        if (EndsWith(name, "_BossIn")) {
            return std::nullopt;  // the intro: a save from after it, before the kill, has it
        }
        return false;
    }
    // `<quest>_IsBefore_<Step>`: true while the quest is before that step.
    const std::string prefix = std::string(set.quest_name) + "_IsBefore_";
    if (!StartsWith(name, prefix)) {
        return std::nullopt;
    }
    const CoopQuests::Quest* quest = CoopQuests::Find(set.quest);
    if (quest == nullptr) {
        return std::nullopt;
    }
    const std::optional<std::int32_t> now = CoopQuests::Rank(*quest, quest_step);
    const std::optional<std::int32_t> at =
        CoopQuests::Rank(*quest, coop::protocol::flag_hash(name.substr(prefix.size())));
    if (!now || !at) {
        return std::nullopt;
    }
    return *now < *at;
}

// The last set of `set`'s quest (Ganon for Ganondorf: the one whose `below` holds the other's step), or `set`.
const BeatenSet& LastOfQuest(const BeatenSet& set) {
    const BeatenSet* last = &set;
    for (const BeatenSet& s : kSets) {
        if (s.quest == set.quest && std::find(s.below.begin(), s.below.end(), last->step) != s.below.end()) {
            last = &s;
        }
    }
    return *last;
}

// Row 159: our quest holds `set`'s boss just beaten: its step, or a later one up to the quest's last boss's.
bool InBeatenRange(const BeatenSet& set, std::uint32_t step) {
    if (std::find(set.below.begin(), set.below.end(), step) != set.below.end()) {
        return false;  // alive
    }
    const BeatenSet& last = LastOfQuest(set);
    return step == set.step || step == last.step ||
           std::find(last.below.begin(), last.below.end(), step) != last.below.end();
}

} // namespace

std::span<const BeatenSet> Sets() {
    return kSets;
}

const BeatenSet* ForRoom(std::uint8_t room) {
    for (const BeatenSet& s : kSets) {
        if (s.room == room) {
            return &s;
        }
    }
    return nullptr;
}

std::optional<std::string_view> DeathScene(std::uint8_t room) {
    if (ForRoom(room) == nullptr) {
        return std::nullopt;
    }
    if (const CoopBoss::Room* where = CoopBoss::RoomByIndex(room)) {
        return std::string_view(where->scene);
    }
    switch (room) {
    case 8:
        return std::string_view("InsideGanonCastle");  // Ganondorf's room (Room_26, Create_Ganondorf)
    case 9:
        return std::string_view("FinalBattleField");  // Ganon (FinalBattleField_* logic, Banc GanonCastle/)
    default:
        return std::nullopt;
    }
}

bool KeptScene(std::uint8_t room, std::string_view scene) {
    const std::optional<std::string_view> death = DeathScene(room);
    if (!death) {
        return true;  // no set: never given anywhere
    }
    if (room == 8 || room == 9) {
        return scene == "InsideGanonCastle" || scene == "CrumblingGanonCastle" || scene == "FinalBattleField";
    }
    return scene == *death;
}

bool Record::Add(std::uint8_t room) {
    if (ForRoom(room) == nullptr) {
        return false;
    }
    std::lock_guard lock(mutex_);
    if (room < 16) {
        seen_ |= static_cast<std::uint16_t>(1u << room);
    }
    if (Has(rooms_, room)) {
        return false;
    }
    rooms_.push_back(room);
    return true;
}

void Record::Remove(std::uint16_t mask) {
    std::lock_guard lock(mutex_);
    std::erase_if(rooms_, [&](std::uint8_t room) { return room < 16 && ((mask >> room) & 1u) != 0; });
}

void Record::Clear() {
    std::lock_guard lock(mutex_);
    rooms_.clear();
}

void Record::ForgetSession() {
    std::lock_guard lock(mutex_);
    rooms_.clear();
    seen_ = 0;
}

std::uint16_t Record::SeenMask() const {
    std::lock_guard lock(mutex_);
    return seen_;
}

std::vector<std::uint8_t> Record::Rooms() const {
    std::lock_guard lock(mutex_);
    return rooms_;
}

std::optional<std::uint8_t> ProvenDeath(const coop::protocol::Message& m) {
    namespace proto = coop::protocol;
    const auto in_its_dungeon = [](std::uint8_t room, const std::string& scene) -> std::optional<std::uint8_t> {
        const std::optional<std::string_view> where = DeathScene(room);
        if (!where || *where != scene) {
            return std::nullopt;
        }
        return room;
    };
    const auto* s = std::get_if<proto::BossScene>(&m);
    if (s == nullptr ||
        (s->kind != proto::BossSceneKind::outro_start && s->kind != proto::BossSceneKind::outro_end)) {
        return std::nullopt;  // never BOSS_ROOM defeated: a game also sends it on a guess
    }
    return in_its_dungeon(s->boss, s->scene);
}

Record& TheRecord() {
    static Record record;
    return record;
}

bool InDungeon(std::uint8_t room, std::string_view scene) {
    if (CoopBoss::RoomByIndex(room) == nullptr) {
        return false;
    }
    const std::optional<std::string_view> death = DeathScene(room);
    return death && *death == scene;
}

std::vector<Write> Apply::Check(const std::optional<std::string>& scene, bool place_known,
                                std::span<const std::uint8_t> beaten,
                                const std::function<std::optional<std::uint32_t>(std::uint32_t)>& read,
                                const std::function<bool(std::uint8_t)>& near,
                                const std::function<bool(std::uint8_t)>& death_here) {
    if (!scene) {
        scene_.reset();  // loading: the scene after it (the same one too) checks again
        loaded_.clear();
        checked_.clear();
        return {};
    }
    if (!scene_ || *scene_ != *scene) {
        scene_ = scene;
        loaded_.assign(beaten.begin(), beaten.end());  // a later record change waits for the next load
        checked_.clear();
    }
    if (!place_known) {
        return {};
    }
    // Elsewhere: the rooms the record held when this scene loaded. In a dungeon boss's own scene: live, the record
    // as it is now (rows 136/137).
    std::vector<std::uint8_t> due = loaded_;
    for (const std::uint8_t room : beaten) {
        if (InDungeon(room, *scene) && !Has(due, room)) {
            due.push_back(room);
        }
    }
    std::vector<Write> out;
    // Row 138 (4): the host's older save, a boss alive in it: its Bools back (HostReset).
    for (auto it = reverts_.begin(); it != reverts_.end();) {
        const std::uint8_t room = *it;
        const BeatenSet* set = ForRoom(room);
        if (set == nullptr || Has(beaten, room)) {
            if (set != nullptr) {
                Note(room, 200, std::string(set->boss) + ": the put-back after the host's save load ends: beaten "
                                                         "again since (the record has it)");
            }
            it = reverts_.erase(it);  // beaten again since the reset: Apply gives the set below
            continue;
        }
        if (InDungeon(room, *scene) && near(room) && (!death_here || death_here(room))) {
            // In (or before) the boss room while our own death of it may run (its event writes the set itself):
            // after it. Row 159: not until we walk out. The shared quest has the host's step at once, so the room's
            // logic shows the boss again live while our warp stays up (a mix); the Bools go back there too.
            Note(room, 1, std::string(set->boss) + ": the put-back after the host's save load waits: our own death "
                                                   "of it may run here (in its area)");
            ++it;
            continue;
        }
        const std::optional<std::uint32_t> step = read(set->quest);
        if (!step) {
            Note(room, 2, std::string(set->boss) + ": the put-back after the host's save load waits: its quest is "
                                                   "not read");
            ++it;  // unread: the next frame
            continue;
        }
        if (std::find(set->below.begin(), set->below.end(), *step) == set->below.end()) {
            // The host's save has the boss beaten (or the quest past it): ours stays. Row 159 (run 034533): kept,
            // not dropped: the host's step reaches our quest through the flag watcher's store, maybe a frame
            // after the reset is seen; a drop at that first read left the guest's warp up while the shared quest,
            // set a moment later, made the boss again. Only a later reset or the boss beaten again ends it.
            Note(room, 3, std::string(set->boss) + ": after the host's save load its quest reads beaten: our "
                                                   "flags stay (until it reads alive)");
            ++it;
            continue;
        }
        it = reverts_.erase(it);
        for (const BeatenBool& b : set->bools) {
            const std::optional<bool> alive = AliveValue(*set, b, *step);
            const std::optional<std::uint32_t> now = read(b.hash);
            if (alive && now && (*now != 0) != *alive) {
                out.push_back(Write{room, b.hash, false, *alive ? 1u : 0u, b.name, true});
            }
        }
    }
    // Row 159: a boss alive in the host's save whose quest is each game's own: quest and Bools back together.
    for (auto it = rewinds_.begin(); it != rewinds_.end();) {
        const std::uint8_t room = *it;
        const BeatenSet* set = ForRoom(room);
        if (set == nullptr || Has(beaten, room)) {
            it = rewinds_.erase(it);  // beaten again since (a proven death): Apply gives the set below
            continue;
        }
        if (room == 8 || room == 9) {
            if (KeptScene(room, *scene)) {
                ++it;  // Ganondorf's and Ganon's end sequence reads Defeat_Ganon live: the next scene
                continue;
            }
        } else if (InDungeon(room, *scene) && near(room) && (!death_here || death_here(room))) {
            ++it;  // our own death of it may run there: after it
            continue;
        }
        const std::optional<std::uint32_t> step = read(set->quest);
        if (!step) {
            ++it;
            continue;
        }
        it = rewinds_.erase(it);
        if (!InBeatenRange(*set, *step)) {
            continue;  // alive in ours too, or our quest went on past it (we took the warp on): ours stays
        }
        const std::uint32_t back = set->below.back();
        for (const BeatenBool& b : set->bools) {
            const std::optional<bool> alive_value = AliveValue(*set, b, back);
            const std::optional<std::uint32_t> now = read(b.hash);
            if (alive_value && now && (*now != 0) != *alive_value) {
                out.push_back(Write{room, b.hash, false, *alive_value ? 1u : 0u, b.name, true});
            }
        }
        // Two bosses of one quest (Ganondorf and Ganon, both alive in the host's save): one write, the earlier step.
        const auto same = std::find_if(out.begin(), out.end(),
                                       [&](const Write& w) { return w.is_enum && w.hash == set->quest; });
        if (same == out.end()) {
            out.push_back(Write{room, set->quest, true, back, set->quest_name, true});
        } else {
            const BeatenSet& last = LastOfQuest(*set);
            const auto pos = [&](std::uint32_t v) {
                return std::find(last.below.begin(), last.below.end(), v) - last.below.begin();
            };
            if (pos(back) < pos(same->value)) {
                *same = Write{room, set->quest, true, back, set->quest_name, true};
            }
        }
    }
    for (const std::uint8_t room : due) {
        if (!Has(beaten, room)) {
            continue;  // the record was cleared since (the host loaded a save): that boss fights again
        }
        const BeatenSet* set = ForRoom(room);
        if (set == nullptr || Has(checked_, room)) {
            continue;
        }
        if (InDungeon(room, *scene)) {
            if (near(room) && (!death_here || death_here(room))) {
                continue;  // in (or before) the boss room while our own death of it may run (D9): after it
            }
        } else if (KeptScene(room, *scene)) {
            continue;  // Ganondorf's and Ganon's end sequence: the next scene gives it
        }
        checked_.push_back(room);
        for (const BeatenBool& b : set->bools) {
            const std::optional<std::uint32_t> now = read(b.hash);
            if (now && (*now != 0) != b.value) {
                out.push_back(Write{room, b.hash, false, b.value ? 1u : 0u, b.name});
            }
        }
        const std::optional<std::uint32_t> step = read(set->quest);
        if (step && std::find(set->below.begin(), set->below.end(), *step) != set->below.end()) {
            // Two sets of one quest (Ganondorf's Step06, Ganon's Complete), both read before either is written: one
            // write, the furthest step (the one the other's step is below), whatever order the record holds them in.
            const auto same = std::find_if(out.begin(), out.end(),
                                           [&](const Write& w) { return w.is_enum && w.hash == set->quest; });
            if (same == out.end()) {
                out.push_back(Write{room, set->quest, true, set->step, set->quest_name});
            } else if (std::find(set->below.begin(), set->below.end(), same->value) != set->below.end()) {
                *same = Write{room, set->quest, true, set->step, set->quest_name};
            }
        }
    }
    return out;
}

AliveBits AliveInSave(const std::function<std::optional<std::uint32_t>(std::uint32_t)>& read) {
    AliveBits bits;
    for (const BeatenSet& s : kSets) {
        if (s.room >= 16) {
            continue;
        }
        const std::optional<std::uint32_t> step = read(s.quest);
        if (!step) {
            continue;
        }
        const auto bit = static_cast<std::uint16_t>(1u << s.room);
        bits.known |= bit;
        if (std::find(s.below.begin(), s.below.end(), *step) != s.below.end()) {
            bits.alive |= bit;
        }
    }
    bits.known &= coop::protocol::kBossAliveMask;
    bits.alive &= bits.known;
    return bits;
}

void HostAliveBox::Put(const coop::protocol::BossAlive& m) {
    std::lock_guard lock(mutex_);
    pending_ = m;
}

std::optional<coop::protocol::BossAlive> HostAliveBox::Take() {
    std::lock_guard lock(mutex_);
    return std::exchange(pending_, std::nullopt);
}

void HostAliveBox::Clear() {
    std::lock_guard lock(mutex_);
    pending_.reset();
}

HostAliveBox& TheHostAlive() {
    static HostAliveBox box;
    return box;
}

void Apply::HostAlive(std::uint16_t known, std::uint16_t alive, bool quests_shared) {
    rewinds_.clear();
    for (const BeatenSet& s : kSets) {
        if (s.room >= 16 || ((known >> s.room) & 1u) == 0 || ((alive >> s.room) & 1u) == 0) {
            continue;
        }
        if (quests_shared && CoopQuests::Find(s.quest) != nullptr) {
            continue;  // the host's FLAG_RESET set our quest: HostReset's put-back
        }
        rewinds_.push_back(s.room);
    }
}

void Apply::Note(std::uint8_t room, std::uint8_t reason, std::string line) {
    const auto key = static_cast<std::uint16_t>(room << 8 | reason);
    if (std::find(noted_.begin(), noted_.end(), key) == noted_.end()) {
        noted_.push_back(key);
        notes_.push_back(std::move(line));
    }
}

std::vector<std::string> Apply::TakeNotes() {
    return std::exchange(notes_, {});
}

void Apply::HostReset(bool quests_shared) {
    reverts_.clear();
    noted_.clear();
    if (!quests_shared) {
        return;  // LICHII_COOP_QUESTS=0: no quest follows the host, each game's boss stays its own
    }
    for (const BeatenSet& s : kSets) {
        if (CoopQuests::Find(s.quest) != nullptr) {
            reverts_.push_back(s.room);
        }
    }
}

void Apply::Reset() {
    reverts_.clear();
    rewinds_.clear();
    scene_.reset();
    loaded_.clear();
    checked_.clear();
}

bool OwnLoadClears(bool hosting, bool save_load, bool retry) {
    return hosting && save_load && !retry;
}

std::optional<std::uint32_t> Read(const GameIo& io, std::uint32_t hash) {
    if (io.read_bool) {
        if (const auto b = io.read_bool(hash)) {
            return b;
        }
    }
    return io.read_enum ? io.read_enum(hash) : std::nullopt;
}

bool WriteTo(const GameIo& io, const Write& w) {
    if (w.is_enum) {
        return io.write_enum && io.write_enum(w.hash, w.value);
    }
    return w.value <= 1 && io.write_bool && io.write_bool(w.hash, w.value != 0);
}

} // namespace CoopBossBeaten
