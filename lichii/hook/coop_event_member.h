// SPDX-License-Identifier: GPL-3.0-or-later
//
// Row 97 / G1 (event ownership): which actors are in the same event as our
// Link, read with the game's own event member test (ReadMember, InSameEvent
// below): no distance, no clock, no event names.
//
// No emulator code: coop_objects.cpp reads it in its walk, tests in
// tests/lichii/event_member_test.cpp.
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace CoopEventMember {

inline constexpr std::uint64_t kActorParts = 0x270;      // the actor's component table

// One 8-byte word of guest memory; nullopt when it cannot be read.
using ReadWord = std::function<std::optional<std::uint64_t>(std::uint64_t address)>;

// For the log (row 97: the stuck event had no name in the log). Our running
// event as the event manager has it (docs player-state-map, "The event
// object"): [main + 0x4e94840] +0x68 the session S; S +0xeb8 its events,
// +0xeb0 their count, +0xf18 the current one; an event's kind at +0x254
// (0 flowchart, 1 timeline, 2 movie). Its name is not mapped: the words of
// the event's first kScanWords that point at a C string that looks like a
// name (a letter, then letters, digits, '_', '-', '.' or '/', kMinName to
// kMaxName long) are given as candidates, at most kMaxNames.
inline constexpr std::uint64_t kEventManagerPtr = 0x4e94840;
inline constexpr std::uint64_t kEventSession = 0x68;
inline constexpr std::uint64_t kSessionCount = 0xeb0;
inline constexpr std::uint64_t kSessionEvents = 0xeb8;
inline constexpr std::uint64_t kSessionCurrent = 0xf18;
inline constexpr std::uint64_t kEventKind = 0x254;
inline constexpr std::uint64_t kScanWords = 0x80;
inline constexpr std::size_t kMinName = 4;
inline constexpr std::size_t kMaxName = 63;
inline constexpr std::size_t kMaxNames = 4;

struct Running {
    std::uint64_t event = 0;           // 0: none read
    std::optional<std::uint32_t> kind;
    std::uint32_t count = 0;           // events in the session
    std::vector<std::string> names;    // name-like strings the event points at
};

// The C string at `at` when it looks like a name; empty otherwise.
inline std::string NameAt(const ReadWord& read, std::uint64_t at) {
    std::string out;
    for (std::size_t i = 0; i <= kMaxName; i += 8) {
        const auto word = read(at + i);
        if (!word) {
            return {};
        }
        for (int b = 0; b < 8; ++b) {
            const char c = static_cast<char>((*word >> (8 * b)) & 0xff);
            if (c == '\0') {
                return out.size() >= kMinName ? out : std::string{};
            }
            const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
            const bool other = (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/';
            if (!(letter || (!out.empty() && other)) || out.size() >= kMaxName) {
                return {};
            }
            out.push_back(c);
        }
    }
    return {};
}

inline Running ReadRunning(const ReadWord& read, std::uint64_t main) {
    Running out;
    const auto manager = read(main + kEventManagerPtr);
    const auto session = manager && *manager ? read(*manager + kEventSession) : std::nullopt;
    if (!session || *session == 0) {
        return out;
    }
    const auto events = read(*session + kSessionEvents);
    const auto count = read(*session + kSessionCount);    // u32 in the low half
    const auto current = read(*session + kSessionCurrent);  // u32 in the low half
    if (!events || *events == 0 || !count || !current) {
        return out;
    }
    out.count = static_cast<std::uint32_t>(*count);
    const auto index = static_cast<std::uint32_t>(*current);
    if (index >= out.count) {
        return out;
    }
    const auto event = read(*events + 8ull * index);
    if (!event || *event == 0) {
        return out;
    }
    out.event = *event;
    if (const auto kind = read(*event + kEventKind)) {
        out.kind = static_cast<std::uint32_t>(*kind);
    }
    for (std::uint64_t w = 0; w < kScanWords && out.names.size() < kMaxNames; ++w) {
        const auto p = read(*event + 8 * w);
        if (!p || *p < 0x10000 || (*p & 7) != 0) {
            continue;
        }
        std::string name = NameAt(read, *p);
        if (!name.empty() && std::find(out.names.begin(), out.names.end(), name) == out.names.end()) {
            out.names.push_back(std::move(name));
        }
    }
    return out;
}

// G1 (event ownership): the game's own membership test, read in main (build e0f3ccfaf50065ae).
// The component at [[actor + 0x270] + 0x88] is the game's EventMember M (older notes called it
// the performer); the one at + 0x90 is a separate EventPerformer P. The game's AI queries:
// - QueryEventMemberIsEventMemver (0x29f3770, and the same test inlined in the actors' own code,
//   for example 0x4b7690): L = [M + 0x18], false when 0. L holds kMemberSlots slots of
//   kMemberSlotSize bytes from L + 0x10; the actor is an event member when any slot has a pointer
//   at +0 that is not 0 AND bit 0 of the byte at +8.
// - QueryEventMemberIsEventMemberSomeReason (0x29f3580): any slot pointer that is not 0.
// - QueryEventMemberGetEventRequestState (0x29f3410): u32 [M + 0x28] (0 none, 1 asked, 3 busy).
// - QueryEventMemberIsEventPerformer (0x29f3980): bit 0 of u16 [P + 0x8c].
// So [M + 0x18] is the member's own record (one per actor while it is in events, one slot per
// live event), not "the event": two actors never share it at the same time.
// What a slot's pointer is, read in the offline run fable-g1-events (2026-11-17, Deku Tree, both
// games, offline/fable-g1-events.members.txt): the running event itself. In every event of the run
// (the ~1 s event after each scene load, A's death with the revival fairy and the event nested in
// it, the Ocarina take-out) our Link's slot pointer equalled the event manager's running event,
// and every other member (Navi, Saria's voice, the event/system actors) held that same pointer in
// its own slot. Flag bit 0 marks the members; the rest of the scene holds the pointer without it
// (scene areas, cameras, foliage 0x4; Link's sword, shield and Ocarina 0x2; butterflies 0x6), so
// "some reason" alone is not membership. Two actors are in the same event when a member slot of
// each holds the same pointer (InSameEvent).
inline constexpr std::uint64_t kMemberSlot = 0x88;          // M: the game's EventMember
inline constexpr std::uint64_t kEventPerformerSlot = 0x90;  // P: the game's EventPerformer
inline constexpr std::uint64_t kMemberRecord = 0x18;        // M: its record L; 0 when it has none
inline constexpr std::uint64_t kMemberState = 0x28;         // M: u32 request state
inline constexpr std::uint64_t kRecordSlots = 0x10;         // L: the first slot
inline constexpr std::uint64_t kMemberSlotSize = 0x38;
inline constexpr std::size_t kMemberSlots = 4;
inline constexpr std::uint64_t kSlotFlags = 0x8;            // slot: u8, bit 0 = a member of that event
inline constexpr std::uint64_t kPerformerBits = 0x8c;       // P: u16, bit 0 = an event performer now
// For the log only: the event base class keeps its live events in a ring of four here.
inline constexpr std::uint64_t kEventRing = 0x4e93080;
inline constexpr std::size_t kEventRingSize = 4;

struct MemberSlotRead {
    std::uint64_t pointer = 0;  // 0: the slot is free
    std::uint8_t flags = 0;
    bool Member() const { return pointer != 0 && (flags & 1) != 0; }
};

struct MemberRead {
    std::uint64_t member = 0;  // M; 0: the actor has none (or it could not be read)
    std::uint64_t record = 0;  // L; 0: in no event
    std::uint32_t state = 0;
    MemberSlotRead slots[kMemberSlots]{};
    // The game's IsEventMemver.
    bool Member() const {
        return std::any_of(std::begin(slots), std::end(slots), [](const MemberSlotRead& s) { return s.Member(); });
    }
    // The game's IsEventMemberSomeReason.
    bool SomeReason() const {
        return std::any_of(std::begin(slots), std::end(slots), [](const MemberSlotRead& s) { return s.pointer != 0; });
    }
};

// The actor's EventMember, read the game's way (no back-pointer test: the game makes none).
// A slot that cannot be read counts as free, so a bad pointer never makes a member.
inline MemberRead ReadMember(const ReadWord& read, std::uint64_t actor) {
    MemberRead out;
    if (actor == 0) {
        return out;
    }
    const auto parts = read(actor + kActorParts);
    const auto member = parts && *parts ? read(*parts + kMemberSlot) : std::nullopt;
    if (!member || *member == 0) {
        return out;
    }
    out.member = *member;
    if (const auto state = read(*member + kMemberState)) {
        out.state = static_cast<std::uint32_t>(*state);
    }
    const auto record = read(*member + kMemberRecord);
    if (!record || *record == 0) {
        return out;
    }
    out.record = *record;
    for (std::size_t i = 0; i < kMemberSlots; ++i) {
        const std::uint64_t at = *record + kRecordSlots + kMemberSlotSize * i;
        const auto pointer = read(at);
        const auto flags = read(at + kSlotFlags);
        if (!pointer || !flags) {
            continue;
        }
        out.slots[i].pointer = *pointer;
        out.slots[i].flags = static_cast<std::uint8_t>(*flags & 0xff);
    }
    return out;
}

// The game's IsEventMemver for one actor.
inline bool IsMember(const ReadWord& read, std::uint64_t actor) {
    return ReadMember(read, actor).Member();
}

// Whether two members are in the same event: a member slot (pointer AND flag bit 0) of each holds
// the same event pointer, in any of the four slots (an event nested in another uses the next one).
inline bool SameEvent(const MemberRead& a, const MemberRead& b) {
    for (const MemberSlotRead& x : a.slots) {
        if (!x.Member()) {
            continue;
        }
        for (const MemberSlotRead& y : b.slots) {
            if (y.Member() && y.pointer == x.pointer) {
                return true;
            }
        }
    }
    return false;
}

// Whether the actor is in our Link's event (link: our Link's ReadMember). nullopt: the actor has
// no EventMember at all (nothing to ask). Anything unreadable, or our Link in no event, is false:
// no claim.
inline std::optional<bool> InSameEvent(const ReadWord& read, std::uint64_t actor, const MemberRead& link) {
    const MemberRead m = ReadMember(read, actor);
    if (m.member == 0) {
        return std::nullopt;
    }
    return SameEvent(m, link);
}

// The actor's EventPerformer bits (u16 [P + 0x8c]; bit 0 = the game's IsEventPerformer);
// nullopt when the actor has no component there or it cannot be read. The game checks P's
// class first; we do not, so this is for the log, never for a decision.
inline std::optional<std::uint16_t> PerformerBits(const ReadWord& read, std::uint64_t actor) {
    if (actor == 0) {
        return std::nullopt;
    }
    const auto parts = read(actor + kActorParts);
    const auto performer = parts && *parts ? read(*parts + kEventPerformerSlot) : std::nullopt;
    if (!performer || *performer == 0) {
        return std::nullopt;
    }
    const auto word = read(*performer + kPerformerBits - 4);  // the aligned word that holds +0x8c
    if (!word) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>((*word >> 32) & 0xffff);
}

inline std::string Hex(std::uint64_t v) {
    static const char* const digits = "0123456789abcdef";
    std::string out;
    do {
        out.insert(out.begin(), digits[v & 0xf]);
        v >>= 4;
    } while (v != 0);
    return "0x" + out;
}

// For the log, one line that changes exactly when the actor's membership changes: its state,
// L and each used slot as "index:pointer/flags"; "no member" when the actor has no M.
inline std::string MemberText(const MemberRead& m) {
    if (m.member == 0) {
        return "no member";
    }
    std::string out = "state " + std::to_string(m.state) + " record " + Hex(m.record) + " slots";
    bool any = false;
    for (std::size_t i = 0; i < kMemberSlots; ++i) {
        if (m.slots[i].pointer == 0) {
            continue;
        }
        any = true;
        out += " " + std::to_string(i) + ":" + Hex(m.slots[i].pointer) + "/" + Hex(m.slots[i].flags);
    }
    return any ? out : out + " none";
}

// For the log, once per change (never read for a decision): the seven words of each used slot
// and the first four words its pointer leads to, so one run shows whether a slot's pointer is
// the event itself, or leads to it, and what else a slot keeps.
inline std::string MemberDump(const ReadWord& read, const MemberRead& m) {
    std::string out;
    for (std::size_t i = 0; i < kMemberSlots; ++i) {
        if (m.record == 0 || m.slots[i].pointer == 0) {
            continue;
        }
        const std::uint64_t at = m.record + kRecordSlots + kMemberSlotSize * i;
        out += (out.empty() ? "slot " : "; slot ") + std::to_string(i) + ":";
        for (std::uint64_t w = 0; w < kMemberSlotSize; w += 8) {
            const auto v = read(at + w);
            out += " " + (v ? Hex(*v) : std::string("?"));
        }
        out += " ->";
        for (std::uint64_t w = 0; w < 0x20; w += 8) {
            const auto v = read(m.slots[i].pointer + w);
            out += " " + (v ? Hex(*v) : std::string("?"));
        }
    }
    return out.empty() ? std::string("no slot used") : out;
}

// For the log: the event base class's ring of live events.
inline std::string EventRingText(const ReadWord& read, std::uint64_t main) {
    std::string out;
    for (std::size_t i = 0; i < kEventRingSize; ++i) {
        const auto v = read(main + kEventRing + 8 * i);
        out += (out.empty() ? "" : " ") + (v ? Hex(*v) : std::string("?"));
    }
    return out;
}

} // namespace CoopEventMember
