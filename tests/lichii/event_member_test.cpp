// Tests for the game's own event member test (lichii/hook/coop_event_member.h, row 97, G1).
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "coop_event_member.h"

namespace {

// Guest memory as 8-byte words; anything not put cannot be read.
struct Memory {
    std::map<std::uint64_t, std::uint64_t> words;
    void Put(std::uint64_t at, std::uint64_t v) { words[at] = v; }
    void PutText(std::uint64_t at, const std::string& text) {
        std::string padded = text;
        padded.resize((text.size() / 8 + 1) * 8, '\0');
        for (std::size_t i = 0; i < padded.size(); i += 8) {
            std::uint64_t w = 0;
            std::memcpy(&w, padded.data() + i, 8);
            Put(at + i, w);
        }
    }
    CoopEventMember::ReadWord Reader() const {
        return [this](std::uint64_t at) -> std::optional<std::uint64_t> {
            const auto it = words.find(at);
            if (it == words.end()) {
                return std::nullopt;
            }
            return it->second;
        };
    }
};

constexpr std::uint64_t kActor = 0x10'0000;
constexpr std::uint64_t kParts = 0x20'0000;
constexpr std::uint64_t kPerformer = 0x30'0000;

} // namespace

TEST_CASE("event member: our running event's kind and the names it points at, for the log") {
    using namespace CoopEventMember;
    constexpr std::uint64_t kMain = 0x7100000000;
    constexpr std::uint64_t kManager = 0x50'0000, kSession = 0x60'0000, kEvents = 0x70'0000;
    constexpr std::uint64_t kEvent = 0x80'0000, kName = 0x90'0000, kJunk = 0xa0'0000;
    Memory m;
    CHECK(ReadRunning(m.Reader(), kMain).event == 0);  // nothing readable
    m.Put(kMain + kEventManagerPtr, kManager);
    m.Put(kManager + kEventSession, 0);
    CHECK(ReadRunning(m.Reader(), kMain).event == 0);  // no event runs
    m.Put(kManager + kEventSession, kSession);
    m.Put(kSession + kSessionEvents, kEvents);
    m.Put(kSession + kSessionCount, 2);
    m.Put(kSession + kSessionCurrent, 1);
    m.Put(kEvents + 8, kEvent);
    m.Put(kEvent + kEventKind, 0);
    m.Put(kEvent + 0x10, kName);
    m.PutText(kName, "InsideTheDekuTree_Scrub_Talk");
    m.Put(kEvent + 0x18, kJunk);
    m.PutText(kJunk, "\x01\x02zz");          // not a name
    m.Put(kEvent + 0x20, kName);             // the same name again: once
    const Running r = ReadRunning(m.Reader(), kMain);
    CHECK(r.event == kEvent);
    CHECK(r.count == 2);
    REQUIRE(r.kind);
    CHECK(*r.kind == 0);
    REQUIRE(r.names.size() == 1);
    CHECK(r.names[0] == "InsideTheDekuTree_Scrub_Talk");
    m.Put(kSession + kSessionCurrent, 2);  // past the count: none
    CHECK(ReadRunning(m.Reader(), kMain).event == 0);
}

TEST_CASE("event member: the game's own test, a slot pointer with its flag bit (G1)") {
    using namespace CoopEventMember;
    constexpr std::uint64_t kRecord = 0x40'0000, kEventA = 0x41'0000, kEventB = 0x42'0000;
    const auto slot = [&](std::size_t i) { return kRecord + kRecordSlots + kMemberSlotSize * i; };
    Memory m;
    CHECK_FALSE(IsMember(m.Reader(), kActor));  // nothing readable
    CHECK_FALSE(IsMember(m.Reader(), 0));
    CHECK(ReadMember(m.Reader(), kActor).member == 0);
    CHECK(MemberText(ReadMember(m.Reader(), kActor)) == "no member");
    m.Put(kActor + kActorParts, kParts);
    m.Put(kParts + kMemberSlot, kPerformer);
    m.Put(kPerformer + kMemberState, 0xffffffff'00000003ull);  // u32: the high half is another field
    m.Put(kPerformer + kMemberRecord, 0);
    {
        const MemberRead r = ReadMember(m.Reader(), kActor);
        CHECK(r.member == kPerformer);
        CHECK(r.state == 3);
        CHECK(r.record == 0);  // no record: in no event
        CHECK_FALSE(r.Member());
        CHECK_FALSE(r.SomeReason());
        CHECK(MemberText(r) == "state 3 record 0x0 slots none");
        CHECK(MemberDump(m.Reader(), r) == "no slot used");
    }
    // A record whose slots are all free: not a member.
    m.Put(kPerformer + kMemberRecord, kRecord);
    for (std::size_t i = 0; i < kMemberSlots; ++i) {
        m.Put(slot(i), 0);
        m.Put(slot(i) + kSlotFlags, 0);
    }
    CHECK_FALSE(IsMember(m.Reader(), kActor));
    // A flag without a pointer: free. A pointer without the flag: "some reason" only.
    m.Put(slot(0) + kSlotFlags, 1);
    CHECK_FALSE(IsMember(m.Reader(), kActor));
    CHECK_FALSE(ReadMember(m.Reader(), kActor).SomeReason());
    m.Put(slot(1), kEventA);
    m.Put(slot(1) + kSlotFlags, 0xffffffff'ffffff02ull);  // u8, bit 0 clear
    CHECK_FALSE(IsMember(m.Reader(), kActor));
    CHECK(ReadMember(m.Reader(), kActor).SomeReason());
    CHECK(MemberText(ReadMember(m.Reader(), kActor)) == "state 3 record 0x400000 slots 1:0x410000/0x2");
    // Pointer and flag in any of the four slots: a member.
    for (std::size_t i = 0; i < kMemberSlots; ++i) {
        CAPTURE(i);
        m.Put(slot(i), kEventB);
        m.Put(slot(i) + kSlotFlags, 0x101);  // only the low byte is the flag
        CHECK(IsMember(m.Reader(), kActor));
        CHECK(ReadMember(m.Reader(), kActor).slots[i].flags == 1);
        m.Put(slot(i), i == 1 ? kEventA : 0);
        m.Put(slot(i) + kSlotFlags, i == 1 ? 2 : 0);
        CHECK_FALSE(IsMember(m.Reader(), kActor));
    }
    // A slot that cannot be read is free, never a member.
    m.Put(slot(3), kEventB);
    m.words.erase(slot(3) + kSlotFlags);
    CHECK_FALSE(IsMember(m.Reader(), kActor));
    CHECK(ReadMember(m.Reader(), kActor).slots[3].pointer == 0);
    // Past the fourth slot nothing is read.
    m.Put(slot(3), 0);
    m.Put(slot(3) + kSlotFlags, 0);
    m.Put(slot(4), kEventB);
    m.Put(slot(4) + kSlotFlags, 1);
    CHECK_FALSE(IsMember(m.Reader(), kActor));
    // The dump: the slot's seven words, then four words its pointer leads to ('?' unread).
    m.Put(kEventA, 0x1234);
    const std::string dump = MemberDump(m.Reader(), ReadMember(m.Reader(), kActor));
    CHECK(dump == "slot 1: 0x410000 0x2 ? ? ? ? ? -> 0x1234 ? ? ?");
}

TEST_CASE("event member: the EventPerformer bits are the u16 at +0x8c of the 0x90 component (G1)") {
    using namespace CoopEventMember;
    Memory m;
    CHECK_FALSE(PerformerBits(m.Reader(), kActor));
    CHECK_FALSE(PerformerBits(m.Reader(), 0));
    m.Put(kActor + kActorParts, kParts);
    m.Put(kParts + kEventPerformerSlot, 0);
    CHECK_FALSE(PerformerBits(m.Reader(), kActor));  // no component there
    m.Put(kParts + kEventPerformerSlot, kPerformer);
    CHECK_FALSE(PerformerBits(m.Reader(), kActor));  // unreadable
    m.Put(kPerformer + 0x88, 0xaaaa'0003'5555'5555ull);  // +0x88 u32, +0x8c u16, +0x8e u16
    REQUIRE(PerformerBits(m.Reader(), kActor));
    CHECK(*PerformerBits(m.Reader(), kActor) == 3);
    // The member component (0x88) is not read for it.
    m.Put(kParts + kMemberSlot, 0x99'0000);
    CHECK(*PerformerBits(m.Reader(), kActor) == 3);
}

TEST_CASE("event member: the ring of live events and hex, for the log (G1)") {
    using namespace CoopEventMember;
    constexpr std::uint64_t kMain = 0x7100000000;
    Memory m;
    CHECK(Hex(0) == "0x0");
    CHECK(Hex(0x106d7f9dd0) == "0x106d7f9dd0");
    CHECK(EventRingText(m.Reader(), kMain) == "? ? ? ?");
    m.Put(kMain + kEventRing, 0x80'0000);
    m.Put(kMain + kEventRing + 8, 0);
    m.Put(kMain + kEventRing + 24, 0x81'0000);
    CHECK(EventRingText(m.Reader(), kMain) == "0x800000 0x0 ? 0x810000");
}

namespace {

// One actor with an EventMember M and a record L, as the game keeps them.
struct MemberActor {
    std::uint64_t actor, parts, member, record;
    std::uint64_t Slot(std::size_t i) const {
        return record + CoopEventMember::kRecordSlots + CoopEventMember::kMemberSlotSize * i;
    }
    void Make(Memory& m) const {
        m.Put(actor + CoopEventMember::kActorParts, parts);
        m.Put(parts + CoopEventMember::kMemberSlot, member);
        m.Put(member + CoopEventMember::kMemberState, 0);
        m.Put(member + CoopEventMember::kMemberRecord, record);
        for (std::size_t i = 0; i < CoopEventMember::kMemberSlots; ++i) {
            m.Put(Slot(i), 0);
            m.Put(Slot(i) + CoopEventMember::kSlotFlags, 0);
        }
    }
    void Set(Memory& m, std::size_t i, std::uint64_t event, std::uint64_t flags) const {
        m.Put(Slot(i), event);
        m.Put(Slot(i) + CoopEventMember::kSlotFlags, flags);
    }
};

} // namespace

TEST_CASE("event member: in our Link's event when a member slot of each holds the same event (G1, row 97)") {
    using namespace CoopEventMember;
    // Pointers and records as the offline run fable-g1-events read them.
    constexpr std::uint64_t kTalk = 0x106d85b750, kNested = 0x106d85c970, kOther = 0x106d862270;
    const MemberActor link{0x50'0000, 0x51'0000, 0x52'0000, 0x106d7f9dd0};
    const MemberActor navi{0x60'0000, 0x61'0000, 0x62'0000, 0x106d7f9f98};
    Memory m;
    link.Make(m);
    navi.Make(m);
    const auto in = [&] { return InSameEvent(m.Reader(), navi.actor, ReadMember(m.Reader(), link.actor)); };

    // Our Link in no event: nothing is ours, even an actor that is a member of some event.
    navi.Set(m, 0, kTalk, 1);
    REQUIRE(in());
    CHECK_FALSE(*in());

    // The same event in both: ours.
    link.Set(m, 0, kTalk, 1);
    CHECK(*in());

    // Different events: not ours (another player's replay, or an event of the actor's own).
    navi.Set(m, 0, kOther, 1);
    CHECK_FALSE(*in());

    // An always-there actor holds our event's pointer without the member flag (a butterfly 0x6,
    // Link's sword 0x2, a scene area 0x4): not a member, never ours.
    for (const std::uint64_t flags : {std::uint64_t{0x6}, std::uint64_t{0x2}, std::uint64_t{0x4}}) {
        CAPTURE(flags);
        navi.Set(m, 0, kTalk, flags);
        CHECK_FALSE(*in());
    }
    // Our Link's slot without the flag: our Link is not a member, so nothing is ours.
    navi.Set(m, 0, kTalk, 1);
    link.Set(m, 0, kTalk, 2);
    CHECK_FALSE(*in());
    link.Set(m, 0, kTalk, 1);

    // Two live events in different slots: an event nested in ours (the revival fairy's in A's
    // death) sits in our Link's slot 1 and in the actor's slot 0; still the same event.
    link.Set(m, 1, kNested, 1);
    navi.Set(m, 0, kNested, 1);
    CHECK(*in());
    navi.Set(m, 0, 0, 0);
    navi.Set(m, 2, kTalk, 1);
    CHECK(*in());
    navi.Set(m, 2, 0, 0);
    CHECK_FALSE(*in());  // in no event at all

    // Unreadable memory fails closed: the actor's record or slots gone -> not ours.
    navi.Set(m, 0, kTalk, 1);
    CHECK(*in());
    m.words.erase(navi.Slot(0) + kSlotFlags);
    CHECK_FALSE(*in());
    m.Put(navi.Slot(0) + kSlotFlags, 1);
    m.words.erase(navi.member + kMemberRecord);
    REQUIRE(in());
    CHECK_FALSE(*in());
    m.Put(navi.member + kMemberRecord, navi.record);
    // ... our Link's unreadable: not ours either.
    m.words.erase(link.Slot(0) + kSlotFlags);
    m.words.erase(link.Slot(1) + kSlotFlags);
    CHECK_FALSE(*in());
    m.Put(link.Slot(0) + kSlotFlags, 1);

    // No EventMember component at all: nothing to ask (nullopt), never a claim.
    m.Put(navi.parts + kMemberSlot, 0);
    CHECK_FALSE(in());
    m.words.erase(navi.actor + kActorParts);
    CHECK_FALSE(in());
    CHECK_FALSE(InSameEvent(m.Reader(), 0, ReadMember(m.Reader(), link.actor)));

    // Records are per actor: our Link's and the actor's are never equal, and do not need to be.
    CHECK(link.record != navi.record);
}
