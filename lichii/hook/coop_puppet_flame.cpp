// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_puppet_flame.h"

#include <array>
#include <cstring>

#include "coop_puppet.h"
#include "coop_puppet_sound.h"

namespace CoopPuppetFlame {

static_assert(block::kText >= CoopPuppet::block::kHeadOwnerEu + 8);
static_assert(block::kEnd <= CoopPuppet::block::kSize);
static_assert(sizeof(kStartKey) <= block::kTextCapacity && sizeof(kFireKey) <= block::kTextCapacity);

namespace {

namespace pa = CoopPuppet::addr;
namespace sa = CoopPuppetSound::addr;

constexpr std::uint32_t kGoneFirst = 7;  // actor states 7 to 11: being deleted, gone
constexpr std::uint32_t kGoneLast = 11;
constexpr unsigned kMaxNotes = 6;

template <typename T>
bool Read(CoopPuppet::Guest& guest, std::uint64_t address, T& out) {
    return address != 0 && guest.Read(address, &out, sizeof(out));
}

std::uint64_t EffectUser(CoopPuppet::Guest& guest, std::uint64_t actor) {
    std::uint64_t parts = 0, xlink = 0, user = 0;
    if (!Read(guest, actor + sa::kActorParts, parts) || parts == 0 ||
        !Read(guest, parts + sa::kPartsXLink, xlink) || xlink == 0 ||
        !Read(guest, xlink + sa::kXLinkEffectUser, user)) {
        return 0;
    }
    return user;
}

// The lowest slot whose bit is in both, with a piece there; -1 none.
int WantedSlot(std::uint8_t lit, std::uint8_t drawn, CoopPuppet::Guest& guest, std::uint64_t puppet) {
    const unsigned both = lit & drawn;
    for (int i = 0; i < 8; ++i) {
        if ((both & (1u << i)) != 0 && PieceIn(guest, puppet, i) != 0) {
            return i;
        }
    }
    return -1;
}

bool PutKey(CoopPuppet::Guest& guest, std::uint64_t block, const char* key) {
    std::array<char, block::kTextCapacity> text{};
    std::strncpy(text.data(), key, text.size() - 1);
    return guest.Write(block + block::kText, text.data(), text.size());
}

bool FreshHandle(CoopPuppet::Guest& guest, std::uint64_t block) {
    // As the AI nodes start theirs: u8 0xff, u16 0xffff at +2, the rest 0.
    std::array<std::uint8_t, 0x10> h{};
    h[0] = 0xff;
    h[2] = 0xff;
    h[3] = 0xff;
    return guest.Write(block + block::kHandle, h.data(), h.size());
}

void Note(State& s, const std::function<void(const std::string&)>& say, const std::string& line) {
    if (s.notes < kMaxNotes) {
        ++s.notes;
        say(line);
    }
}

}  // namespace

std::uint64_t PieceIn(CoopPuppet::Guest& guest, std::uint64_t puppet, int slot) {
    if (slot < 0 || slot >= 8) {
        return 0;
    }
    std::uint64_t parts = 0, eu = 0, holder = 0, piece = 0;
    std::uint32_t id = 0, holder_id = 0, state = 0;
    if (!Read(guest, puppet + pa::kActorParts, parts) || parts == 0 ||
        !Read(guest, parts + pa::kPartsEquipmentUser, eu) || eu == 0) {
        return 0;
    }
    const std::uint64_t at = eu + pa::kEquipSlots + pa::kEquipSlotStride * static_cast<std::uint64_t>(slot);
    if (!Read(guest, at + pa::kEquipSlotId, id) || id == 0xFFFFFFFFu ||
        !Read(guest, at + pa::kEquipSlotHolder, holder) || holder == 0 ||
        !Read(guest, holder + pa::kHolderId, holder_id) || holder_id != id ||
        !Read(guest, holder + pa::kHolderActor, piece) || piece == 0 ||
        !Read(guest, piece + pa::kActorState, state) || (state >= kGoneFirst && state <= kGoneLast)) {
        return 0;
    }
    return piece;
}

std::optional<CoopPuppet::Call> Next(State& s, std::uint8_t lit, std::uint8_t drawn, bool shown,
                                     CoopPuppet::Guest& guest, std::uint64_t main_base,
                                     std::uint64_t puppet, std::uint64_t block, const std::string& who,
                                     const std::function<void(const std::string&)>& say) {
    if (s.pending != State::Step::none || puppet == 0 || block == 0) {
        return std::nullopt;
    }
    // The piece it burns on (or was starting on) still there? A piece made
    // again (an age change, the gear re-worn) or taken off took its effects
    // with it; the new one starts unlit.
    if ((s.on || s.next != State::Step::none) && PieceIn(guest, puppet, s.slot) != s.actor) {
        if (s.on) {
            say(who + " flame: the burning piece is gone or made again; its flame went with it");
        }
        s.on = false;
        s.next = State::Step::none;
        s.actor = 0;
        s.slot = -1;
    }
    const int want = shown ? WantedSlot(lit, drawn, guest, puppet) : -1;
    if (s.next == State::Step::none) {
        if (s.on && want != s.slot) {
            s.next = State::Step::find;  // put out: their stick went out, or is put away
        } else if (!s.on && want >= 0) {
            const std::uint64_t piece = PieceIn(guest, puppet, want);
            if (piece == s.refused) {
                return std::nullopt;
            }
            s.slot = want;
            s.actor = piece;
            s.next = State::Step::start_search;
        } else {
            return std::nullopt;  // burning as wanted, or nothing to burn
        }
    }
    const std::uint64_t user = EffectUser(guest, s.actor);
    std::uint64_t resource = 0, accessor = 0;
    if (user == 0 || !Read(guest, user + sa::kUserResource, resource) || resource == 0 ||
        !Read(guest, resource + sa::kResourceAccessor, accessor) || accessor == 0) {
        Note(s, say, who + " flame: the piece " + std::to_string(s.actor) + " has no effect user yet");
        if (!s.on) {
            s.next = State::Step::none;  // asked again next frame
        }
        return std::nullopt;
    }
    CoopPuppet::Call c;
    switch (s.next) {
    case State::Step::start_search:
    case State::Step::fire_search: {
        const char* key = s.next == State::Step::start_search ? kStartKey : kFireKey;
        const std::uint64_t zero = 0;
        if (!PutKey(guest, block, key) || !guest.Write(block + block::kAsset, &zero, sizeof zero)) {
            return std::nullopt;
        }
        c.function = main_base + sa::kSearchAsset;
        c.x[0] = accessor;
        c.x[1] = block + block::kAsset;
        c.x[2] = block + block::kText;
        break;
    }
    case State::Step::start_emit:
    case State::Step::fire_emit:
        if (!FreshHandle(guest, block)) {
            return std::nullopt;
        }
        c.function = main_base + sa::kEmit;
        c.x[0] = user;
        c.x[1] = block + block::kAsset;
        c.x[2] = block + block::kHandle;
        c.x[3] = 0;
        break;
    case State::Step::find:
        if (!PutKey(guest, block, kFireKey) || !FreshHandle(guest, block)) {
            return std::nullopt;
        }
        c.function = main_base + addr::kFindPlaying;
        c.x[0] = user;
        c.x[1] = block + block::kHandle;
        c.x[2] = block + block::kText;
        break;
    case State::Step::kill:
        c.function = main_base + addr::kKill;
        c.x[0] = block + block::kHandle;
        break;
    case State::Step::none:
        return std::nullopt;
    }
    s.pending = s.next;
    s.next = State::Step::none;
    return c;
}

void Finish(State& s, std::uint64_t result, const std::string& who,
            const std::function<void(const std::string&)>& say) {
    const bool found = (result & 1) != 0;
    const State::Step done = s.pending;
    s.pending = State::Step::none;
    switch (done) {
    case State::Step::start_search:
        // No flash key on this piece: the flame alone.
        s.next = found ? State::Step::start_emit : State::Step::fire_search;
        break;
    case State::Step::start_emit:
        s.next = State::Step::fire_search;
        break;
    case State::Step::fire_search:
        if (found) {
            s.next = State::Step::fire_emit;
        } else {
            Note(s, say, who + " flame: their piece is lit, but the puppet's has no '" + kFireKey +
                             "' effect; left unlit");
            s.refused = s.actor;
            s.actor = 0;
            s.slot = -1;
        }
        break;
    case State::Step::fire_emit:
        s.on = true;
        say(who + " flame: their stick burns; the puppet's (slot " + std::to_string(s.slot) +
            ") shows the game's '" + kFireKey + "' (a picture only: no chemistry, no fire in this world)");
        break;
    case State::Step::find:
        if (found) {
            s.next = State::Step::kill;
        } else {
            s.on = false;  // nothing of it plays any more (the game ended it)
            say(who + " flame: put out (it had already ended)");
            s.actor = 0;
            s.slot = -1;
        }
        break;
    case State::Step::kill:
        s.on = false;
        say(who + " flame: put out (the game's own kill, as OneShotXLinkFadeByKey)");
        s.actor = 0;
        s.slot = -1;
        break;
    case State::Step::none:
        break;
    }
}

void Dropped(State& s) {
    if (s.pending != State::Step::none) {
        s.next = s.pending;
        s.pending = State::Step::none;
    }
}

void Forget(State& s) {
    const unsigned notes = s.notes;
    s = State{};
    s.notes = notes;
}

}  // namespace CoopPuppetFlame
