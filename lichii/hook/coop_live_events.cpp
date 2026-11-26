// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_live_events.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "coop_fairy.h"

namespace CoopLiveEvents {

namespace {

// Near one of the event's places in this scene; nullopt when it has none here.
std::optional<bool> NearPlace(std::string_view event, std::string_view scene,
                              const std::optional<std::array<float, 3>>& link, const RoomOf* room_of) {
    bool any = false;
    std::optional<int> link_room;
    if (link && room_of != nullptr && *room_of) {
        link_room = (*room_of)(*link);
    }
    for (const Place& p : Places()) {
        if (p.event != event || p.scene != scene) {
            continue;
        }
        any = true;
        if (!link) {
            continue;
        }
        const float dx = (*link)[0] - p.x;
        const float dy = (*link)[1] - p.y;
        const float dz = (*link)[2] - p.z;
        if (dx * dx + dy * dy + dz * dz > kNearM * kNearM) {
            continue;
        }
        if (link_room) {
            // Near, but through a wall: that room is not loaded here.
            const auto place_room = p.room >= 0 ? std::optional<int>(p.room)
                                                : (*room_of)(std::array<float, 3>{p.x, p.y, p.z});
            if (place_room && *place_room != *link_room) {
                continue;
            }
        }
        return true;
    }
    return any ? std::optional<bool>(false) : std::nullopt;
}

} // namespace

bool SpansRooms(std::string_view event, std::string_view scene) {
    // Two places in two rooms: by their rooms when both are known (round 5: a room-22 event's two doors are one
    // room), else by distance (kOneRoomM).
    std::vector<const Place*> mine;
    for (const Place& p : Places()) {
        if (p.event == event && p.scene == scene) {
            mine.push_back(&p);
        }
    }
    for (std::size_t i = 0; i < mine.size(); ++i) {
        for (std::size_t j = i + 1; j < mine.size(); ++j) {
            const Place& a = *mine[i];
            const Place& b = *mine[j];
            if (a.room >= 0 && b.room >= 0) {
                if (a.room != b.room) {
                    return true;
                }
                continue;
            }
            const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            if (dx * dx + dy * dy + dz * dz > kOneRoomM * kOneRoomM) {
                return true;
            }
        }
    }
    return false;
}

std::optional<std::string> Decider::Next(const std::vector<std::string>& held, const std::string& scene,
                                         bool event_running, std::optional<std::array<float, 3>> link,
                                         const RoomOf* room_of) {
    if (scene.empty()) {
        return std::nullopt;  // no scene read (a load): the same visit, nothing asked
    }
    Visit(scene);
    if (event_running) {
        return std::nullopt;
    }
    std::vector<std::string> wanted = held;
    for (const std::string& name : early_) {
        if (std::find(wanted.begin(), wanted.end(), name) == wanted.end()) {
            wanted.push_back(name);
        }
    }
    for (const std::string& name : wanted) {
        const bool named = allowed_.contains(name);
        if (!allowed_.empty() && !named) {
            continue;
        }
        if (asked_.contains(name)) {
            continue;
        }
        if (SpansRooms(name, scene)) {
            continue;  // deku-ladder: one played flag for two rooms; which one played is not known here
        }
        const auto near = NearPlace(name, scene, link, room_of);
        if (near ? !*near : !named) {
            continue;  // not near it yet (it waits), or no place known and not asked by name
        }
        asked_.insert(name);
        early_.erase(name);
        return name;
    }
    return std::nullopt;
}

void Decider::Visit(const std::string& scene) {
    if (scene != scene_) {
        scene_ = scene;
        asked_.clear();
        early_.clear();
    }
}

void Decider::Started(const std::string& name, const std::string& scene) {
    if (scene.empty() || name.empty()) {
        return;
    }
    Visit(scene);
    if (!asked_.contains(name)) {
        early_.insert(name);
    }
}

void Decider::Ours(const std::string& name, const std::string& scene) {
    if (scene.empty() || name.empty()) {
        return;
    }
    Visit(scene);
    asked_.insert(name);
    early_.erase(name);
}

std::vector<std::string_view> EventsIn(std::string_view scene) {
    std::vector<std::string_view> out;
    for (const Place& p : Places()) {
        if (p.scene == scene && std::find(out.begin(), out.end(), p.event) == out.end()) {
            out.push_back(p.event);
        }
    }
    return out;
}

namespace {

// The name of `names` that `bytes` hold at `at` as a whole C string (the
// start or a NUL before it, a NUL after it), or nullptr.
const std::string_view* NameAt(const std::vector<std::uint8_t>& bytes, std::size_t at,
                               const std::vector<std::string_view>& names) {
    if (at > 0 && bytes[at - 1] != 0) {
        return nullptr;
    }
    for (const std::string_view& n : names) {
        if (at + n.size() < bytes.size() && bytes[at + n.size()] == 0 &&
            std::memcmp(bytes.data() + at, n.data(), n.size()) == 0) {
            return &n;
        }
    }
    return nullptr;
}

// `size` bytes at `address`, read in 0x100-byte pieces; a piece that cannot
// be read stays zero (a mapping may end inside the block).
std::vector<std::uint8_t> ReadSome(Reader read, void* context, std::uint64_t address, std::size_t size) {
    std::vector<std::uint8_t> out(size, 0);
    for (std::size_t at = 0; at < size; at += 0x100) {
        const std::size_t n = std::min<std::size_t>(0x100, size - at);
        if (!read(context, address + at, out.data() + at, n)) {
            std::fill(out.begin() + static_cast<std::ptrdiff_t>(at),
                      out.begin() + static_cast<std::ptrdiff_t>(at + n), std::uint8_t{0});
        }
    }
    return out;
}

// A word that may point into the game's memory (not 0, not a small number).
bool MayPoint(std::uint64_t w) {
    return w >= 0x10000 && w < (std::uint64_t{1} << 48);
}

} // namespace

std::optional<std::string> FindRunningEvent(Reader read, void* context, std::uint64_t main,
                                            const std::vector<std::string_view>& names) {
    if (names.empty()) {
        return std::nullopt;
    }
    const auto u64 = [&](std::uint64_t a) -> std::optional<std::uint64_t> {
        std::uint64_t v = 0;
        if (!read(context, a, &v, 8)) {
            return std::nullopt;
        }
        return v;
    };
    const auto manager = u64(main + addr::kEventStatePtr);
    if (!manager || *manager == 0) {
        return std::nullopt;
    }
    const auto session = u64(*manager + addr::kEventSession);
    if (!session || *session == 0) {
        return std::nullopt;
    }
    std::uint64_t event = 0;
    std::uint32_t count = 0;
    std::uint32_t current = 0;
    const auto events = u64(*session + addr::kSessionEvents);
    if (events && *events != 0 && read(context, *session + addr::kSessionCount, &count, 4) &&
        read(context, *session + addr::kSessionCurrent, &current, 4) && current < count &&
        count < 0x100) {
        event = u64(*events + 8ull * current).value_or(0);
    }
    std::size_t longest = 0;
    for (const std::string_view& n : names) {
        longest = std::max(longest, n.size());
    }
    // The string a word points to: one read of the longest name's length;
    // when that fails, each name's own length (a string may end right before
    // unmapped memory).
    const auto pointed = [&](std::uint64_t w) -> std::optional<std::string> {
        std::vector<std::uint8_t> all(longest + 1, 0xff);
        if (read(context, w, all.data(), all.size())) {
            const auto* n = NameAt(all, 0, names);
            return n != nullptr ? std::optional<std::string>(std::string(*n)) : std::nullopt;
        }
        for (const std::string_view& n : names) {
            std::vector<std::uint8_t> s(n.size() + 1, 0xff);
            if (read(context, w, s.data(), s.size()) && s.back() == 0 &&
                std::memcmp(s.data(), n.data(), n.size()) == 0) {
                return std::string(n);
            }
        }
        return std::nullopt;
    };
    const auto search = [&](const std::vector<std::uint8_t>& block,
                            std::vector<std::uint64_t>* words) -> std::optional<std::string> {
        for (std::size_t at = 0; at < block.size(); ++at) {
            if (const auto* n = NameAt(block, at, names)) {
                return std::string(*n);
            }
        }
        for (std::size_t at = 0; at + 8 <= block.size(); at += 8) {
            std::uint64_t w = 0;
            std::memcpy(&w, block.data() + at, 8);
            if (!MayPoint(w)) {
                continue;
            }
            if (auto n = pointed(w)) {
                return n;
            }
            if (words != nullptr) {
                words->push_back(w);
            }
        }
        return std::nullopt;
    };
    std::vector<std::uint64_t> inner;
    if (event != 0) {
        if (auto n = search(ReadSome(read, context, event, addr::kScanEvent), &inner)) {
            return n;
        }
    }
    if (auto n = search(ReadSome(read, context, *session, addr::kScanSession), nullptr)) {
        return n;
    }
    for (const std::uint64_t w : inner) {
        if (auto n = search(ReadSome(read, context, w, addr::kScanInner), nullptr)) {
            return n;
        }
    }
    return std::nullopt;
}

std::optional<std::set<std::string>> ParseNames(std::string_view text) {
    std::set<std::string> out;
    const auto trim = [](std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
        return s;
    };
    if (trim(text) == "all") {
        return out;
    }
    while (!text.empty()) {
        const auto comma = text.find(',');
        const std::string_view part = trim(text.substr(0, comma));
        if (!part.empty()) {
            out.emplace(part);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    if (out.empty()) {
        return std::nullopt;  // no names: not "all"
    }
    return out;
}

void Requester::Want(std::string name, std::string scene, std::uint64_t at_us, std::uint64_t max_age_us,
                     std::optional<int> room) {
    if (name.empty()) {
        notes_.push_back("live event with no name not asked");
        return;
    }
    if (name.size() >= 0x80 || wanted_.size() >= 8) {
        notes_.push_back("live event " + name + " not asked (name too long or too many waiting)");
        return;
    }
    const bool waiting =
        std::any_of(wanted_.begin(), wanted_.end(), [&](const Wanted& w) { return w.name == name; });
    if (!waiting && name != current_) {
        wanted_.push_back(Wanted{std::move(name), std::move(scene), at_us, max_age_us, {}, 0, false, room});
    }
}

void Requester::WantFromRoom(std::string name, std::string scene, std::optional<int> room) {
    const std::string key = name;
    Want(std::move(name), std::move(scene), 0, 0, room);  // no age limit: started when near, as before
    for (Wanted& w : wanted_) {
        if (w.name == key) {
            w.room_asker = true;
        }
    }
}

std::uint64_t NearestActor(CoopPuppet::Guest& guest, std::uint64_t main, std::span<const std::string_view> names,
                           const std::array<float, 3>& at, float max_m) {
    std::uint64_t best = 0;
    float best_d2 = max_m * max_m;
    CoopFairy::WalkActors(guest, main, [&](std::uint64_t actor) {
        std::array<float, 3> p{};
        if (CoopFairy::ActorPresent(guest, actor) &&
            std::any_of(names.begin(), names.end(),
                        [&](std::string_view n) { return CoopFairy::ActorNamed(guest, actor, n); }) &&
            guest.Read(actor + CoopPuppet::addr::kActorPosition, p.data(), sizeof p)) {
            const float dx = p[0] - at[0], dy = p[1] - at[1], dz = p[2] - at[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 <= best_d2) {
                best_d2 = d2;
                best = actor;
            }
        }
        return false;  // the whole list: the nearest wins
    });
    return best;
}

std::uint64_t Requester::PerformerOf(CoopPuppet::Guest& guest, std::uint64_t actor) {
    std::uint64_t parts = 0;
    if (actor == 0 || !guest.Read(actor + CoopPuppet::addr::kActorParts, &parts, 8) || parts == 0) {
        return 0;
    }
    for (const std::uint64_t slot : {addr::kPerformerSlot, addr::kPerformerSlotAlt}) {
        std::uint64_t performer = 0, owner = 0;
        if (guest.Read(parts + slot, &performer, 8) && performer != 0 &&
            guest.Read(performer + addr::kPerformerActor, &owner, 8) && owner == actor) {
            return performer;  // its own: the game links exactly that actor
        }
    }
    return 0;
}

std::uint64_t Requester::FindAsker(CoopPuppet::Guest& guest, std::uint64_t main, const std::string& name,
                                   const std::string& scene, std::string& where) {
    for (const Place& p : Places()) {
        if (p.event != name || (!scene.empty() && p.scene != scene)) {
            continue;
        }
        const std::array<float, 3> at{p.x, p.y, p.z};
        if (const std::uint64_t actor = NearestActor(guest, main, kAskerNames, at, kAskerNearM); actor != 0) {
            char text[64];
            std::snprintf(text, sizeof text, "%.1f %.1f %.1f", p.x, p.y, p.z);
            where = text;
            return actor;
        }
    }
    return 0;
}

void Requester::WantWith(std::string name, std::vector<Arg> args, std::uint64_t starter) {
    const std::size_t before = wanted_.size();
    Want(name);
    if (wanted_.size() == before) {
        // Already waiting (or refused, with its note): the newest arguments are the ones to use.
        for (Wanted& w : wanted_) {
            if (w.name == name) {
                w.args = std::move(args);
                w.starter = starter;
                return;
            }
        }
        return;
    }
    wanted_.back().args = std::move(args);
    wanted_.back().starter = starter;
}

bool Requester::WriteArgs(CoopPuppet::Guest& guest) {
    std::uint32_t capacity = 0, count = 0;
    std::uint64_t entries = 0;
    if (!guest.Read(block_ + addr::kArgCapacity, &capacity, 4) || !guest.Read(block_ + addr::kArgEntries, &entries, 8) ||
        !guest.Read(block_ + addr::kArgCount, &count, 4) || entries == 0) {
        return false;
    }
    std::uint64_t used = 0;  // of our strings' room
    for (const Arg& arg : current_args_) {
        if (count >= capacity) {
            return false;
        }
        const std::uint64_t entry = entries + addr::kArgEntrySize * count;
        std::uint64_t name_buffer = 0;
        std::uint32_t name_cap = 0;
        if (!guest.Read(entry + addr::kEntryName, &name_buffer, 8) || !guest.Read(entry + addr::kEntryNameCap, &name_cap, 4) ||
            name_buffer == 0 || arg.name.size() + 1 > name_cap ||
            !guest.Write(name_buffer, arg.name.c_str(), arg.name.size() + 1)) {
            return false;
        }
        std::uint32_t type = addr::kTypeBool;
        if (arg.text) {
            if (used + arg.text->size() + 1 > addr::kStringsSize) {
                return false;
            }
            const std::uint64_t text = block_ + addr::kArgStrings + used;
            if (!guest.Write(text, arg.text->c_str(), arg.text->size() + 1) ||
                !guest.Write(entry + addr::kEntryValue, &text, 8)) {
                return false;
            }
            used += arg.text->size() + 1;
            type = addr::kTypeString;
        } else {
            const std::uint8_t value = arg.flag ? 1 : 0;
            if (!guest.Write(entry + addr::kEntryValue, &value, 1)) {
                return false;
            }
        }
        ++count;
        if (!guest.Write(entry + addr::kEntryType, &type, 4) || !guest.Write(block_ + addr::kArgCount, &count, 4)) {
            return false;
        }
    }
    return true;
}

std::optional<CoopPuppet::Call> Requester::Ask(CoopPuppet::Guest& guest, std::uint64_t main) {
    std::uint64_t holder = 0;
    std::uint64_t manager = 0;
    if (!guest.Read(main + addr::kManagerPtr, &holder, 8) || holder == 0 || !guest.Read(holder, &manager, 8) ||
        manager == 0) {
        notes_.push_back("live event " + current_ + ": no event manager; not asked");
        return FreeFirst(guest, main);  // still free what the build made
    }
    CoopPuppet::Call c;
    c.function = main + addr::kRequest;
    c.x[0] = manager;
    c.x[1] = block_;
    next_ = Next::request;
    return c;
}

std::vector<std::string> Requester::TakeNotes() {
    std::vector<std::string> out;
    out.swap(notes_);
    return out;
}

std::optional<CoopPuppet::Call> Requester::FreeFirst(CoopPuppet::Guest& guest, std::uint64_t main) {
    const std::uint64_t vtable = main + addr::kStringVtable;
    guest.Write(block_ + addr::kArgMember1, &vtable, 8);
    CoopPuppet::Call c;
    c.function = main + addr::kStringFree;
    c.x[0] = block_ + addr::kArgMember1;
    next_ = Next::free1;
    return c;
}

bool Requester::EventRunning(CoopPuppet::Guest& guest, std::uint64_t main) {
    std::uint64_t manager = 0;
    std::uint64_t session = 0;
    // docs/dungeons-boss.md: an event runs while [[main + 0x4e94840] + 0x68] is set.
    if (!guest.Read(main + addr::kEventStatePtr, &manager, 8) || manager == 0) {
        return false;
    }
    return guest.Read(manager + addr::kEventSession, &session, 8) && session != 0;
}

std::optional<CoopPuppet::Call> Requester::Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                                std::uint64_t main) {
    CoopPuppet::Call c;
    if (after_call) {
        const Next done = next_;
        next_ = Next::none;
        switch (done) {
        case Next::malloc:
            if (result == 0) {
                malloc_failed_ = true;
                notes_.push_back("live events: the game's malloc gave nothing; live events off");
                return std::nullopt;
            }
            block_ = result;
            break;  // then build, below
        case Next::build: {
            // The name into the arg's own buffer, as the node copies it.
            std::uint64_t buffer = 0;
            std::uint32_t cap = 0;
            if (!guest.Read(block_ + addr::kArgName, &buffer, 8) || !guest.Read(block_ + addr::kArgNameCap, &cap, 4) ||
                buffer == 0 || cap == 0) {
                notes_.push_back("live event " + current_ + ": the request has no name buffer; not asked");
                return FreeFirst(guest, main);  // the build made its members: free them
            }
            const std::size_t n = std::min<std::size_t>(current_.size(), cap - 1);
            std::string text = current_.substr(0, n);
            text.push_back('\0');
            if (!guest.Write(buffer, text.data(), text.size())) {
                notes_.push_back("live event " + current_ + ": name not written; not asked");
                return FreeFirst(guest, main);
            }
            if (!current_args_.empty() && !WriteArgs(guest)) {
                notes_.push_back("live event " + current_ + ": its arguments do not fit the request; not asked");
                return FreeFirst(guest, main);
            }
            if (current_starter_ != 0 && current_room_asker_ &&
                !(CoopFairy::ActorPresent(guest, current_starter_) &&
                  std::any_of(std::begin(kAskerNames), std::end(kAskerNames), [&](std::string_view n) {
                      return CoopFairy::ActorNamed(guest, current_starter_, n);
                  }))) {
                // Found when the event was picked, a frame ago: deleted since (its Logic_Delete, the room unloaded)?
                // Never hand the game's link a freed actor. cutscene-bleed (#251): and never ask it by name alone
                // either (the rule of the no-asker case in the pick below): without its asker the event only plays
                // its camera for a room that is gone, holds our game's event system and Link with it, and nothing
                // in our room moves. Not asked; its played flag stays held until we leave or load.
                notes_.push_back("live event " + current_ + ": our room's asker went away before the link: not asked "
                                 "(by name alone it would only hold our game; it stays held until we leave or load)");
                return FreeFirst(guest, main);  // the build made its members: free them
            }
            if (current_starter_ != 0) {
                c.function = main + addr::kSetLink;
                c.x[0] = block_ + addr::kArgMember2;
                c.x[1] = current_starter_;
                c.x[2] = 0;
                next_ = Next::link;
                return c;
            }
            return Ask(guest, main);
        }
        case Next::link:
            return Ask(guest, main);
        case Next::own_request:
            notes_.push_back("live event " + current_ + " asked by our room's asker itself (result " +
                             std::to_string(result & 0xff) + ")");
            if ((result & 0xff) != 0 && on_taken_) {
                on_taken_(current_);
            }
            current_.clear();
            current_args_.clear();
            current_starter_ = 0;
            current_room_asker_ = false;
            return std::nullopt;  // its own call built and freed its arg; one event a frame
        case Next::request: {
            notes_.push_back("live event " + current_ + " asked of the game (result " + std::to_string(result & 0xff) +
                             ")");
            if ((result & 0xff) != 0 && on_taken_) {
                on_taken_(current_);
            }
            return FreeFirst(guest, main);
        }
        case Next::free1: {
            const std::uint64_t vtable = main + addr::kStringVtable;
            guest.Write(block_ + addr::kArgMember2, &vtable, 8);
            c.function = main + addr::kStringFree;
            c.x[0] = block_ + addr::kArgMember2;
            next_ = Next::free2;
            return c;
        }
        case Next::free2:
            current_.clear();
            current_args_.clear();
            current_starter_ = 0;
            current_room_asker_ = false;
            return std::nullopt;  // one event a frame
        case Next::none:
            return std::nullopt;
        }
    }
    if (next_ != Next::none || malloc_failed_ || main == 0) {
        return std::nullopt;
    }
    if (block_ == 0) {
        if (wanted_.empty()) {
            return std::nullopt;
        }
        c.function = main + addr::kMalloc;
        c.x[0] = addr::kArgSize + addr::kStringsSize;  // the arg, then room for our string arguments
        next_ = Next::malloc;
        return c;
    }
    // cutscene-bleed (#251, #197): the first waiting start that may be asked now: none while our teleport is under
    // way, and not one held because our Link is in another room than the one it was decided in (Expire).
    auto pick = wanted_.end();
    if (current_.empty()) {
        if (!paused_) {
            pick = std::find_if(wanted_.begin(), wanted_.end(), [](const Wanted& w) { return !w.held; });
        }
        if (pick == wanted_.end()) {
            return std::nullopt;
        }
    }
    if (EventRunning(guest, main)) {
        return std::nullopt;  // an event began since it was asked: wait, it stays queued
    }
    if (current_.empty()) {
        current_ = std::move(pick->name);
        current_args_ = std::move(pick->args);
        current_starter_ = pick->starter;
        current_room_asker_ = false;
        if (pick->room_asker && current_starter_ == 0) {
            std::string where;
            current_starter_ = FindAsker(guest, main, current_, pick->scene, where);
            current_room_asker_ = current_starter_ != 0;
            if (current_starter_ == 0) {
                // dungeon-sync-b (#62, #72): no asker here. Asked by name alone the event only plays its
                // camera (the room moves on its asker's outputs), and where the asker went because the
                // room's clear flag deleted it (the Dodongo's Cavern Lizalfos rooms'
                // Gimmick_TreasureBoxAppear module: Logic_Delete <- Clear) the room has its result already.
                // That camera kept our event system busy just as the shared clear flag unlocked the room's
                // shutters, and the door stayed shut. Not asked: its played flag stays held until we leave
                // or load, as before live events.
                notes_.push_back("live event " + current_ +
                                 ": no asker actor within 1 m of its places here: not asked (the room moves only "
                                 "on its asker; it stays held until we leave or load)");
                wanted_.erase(pick);
                current_.clear();
                current_args_.clear();
                return std::nullopt;
            }
            notes_.push_back("live event " + current_ + ": started by our room's asker at " + where +
                             " (camera only: the room moves on its asker's EventBool, which the asker's own AI "
                             "keeps 0 until its own trigger rises)");
        }
        wanted_.erase(pick);
        // deku-sync-fix: the asker asks the event itself, as its room graph's node does, so the event is its own.
        if (current_room_asker_ && current_args_.empty()) {
            if (const std::uint64_t performer = PerformerOf(guest, current_starter_); performer != 0) {
                std::uint32_t state = 0;
                guest.Read(performer + addr::kPerformerState, &state, 4);
                const std::uint64_t at = block_ + addr::kArgStrings;  // [char*][the name], in our own block
                const std::size_t n = std::min<std::size_t>(current_.size(), addr::kStringsSize - 9);
                std::string text = current_.substr(0, n);
                text.push_back('\0');
                const std::uint64_t text_at = at + 8;
                if (state != 3 && guest.Write(at, &text_at, 8) && guest.Write(text_at, text.data(), text.size())) {
                    c.function = main + addr::kPerformerRequest;
                    c.x[0] = performer;
                    c.x[1] = at;
                    next_ = Next::own_request;
                    return c;
                }
                notes_.push_back("live event " + current_ + ": our room's asker is busy (state " +
                                 std::to_string(state) + ") or the name did not fit; linked instead");
            } else {
                notes_.push_back("live event " + current_ + ": our room's asker has no event performer we know; "
                                 "linked instead");
            }
        }
    }
    c.function = main + addr::kArgBuild;
    c.x[0] = block_;
    next_ = Next::build;
    return c;
}

} // namespace CoopLiveEvents
