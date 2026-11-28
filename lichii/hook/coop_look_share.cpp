// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_look_share.h"

#include <algorithm>
#include <utility>

#include "coop/host/session.hpp"
#include "coop/net/client.hpp"

#ifdef LICHII_COOP
#include "common/logging.h"
#endif

namespace CoopLookShare {

using coop::net::EventKind;

bool HasGear(const Parts& parts) {
    return std::any_of(parts.begin(), parts.end(), [](const coop::protocol::LookPart& p) { return p.part != 0; });
}

void Share::Set(Parts parts) {
    ours_ = std::move(parts);
    due_ = true;
}

void Share::SetCharacter(std::uint16_t number) {
    if (number > kCharacterMax) {
        number = 0;
    }
    if (character_ == number) {
        return;
    }
    character_ = number;
    character_said_ = character_said_ || number != 0;
    due_ = true;
}

void Share::SetCustom(std::uint32_t options) {
    options &= 0xFFFFFFu;
    if (custom_ == options) {
        return;
    }
    custom_ = options;
    due_ = due_ || character_ != 0;   // (with Link nothing of it is sent)
}

std::vector<std::size_t> OptionsToHold(const std::array<std::optional<std::int32_t>, 6>& values) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (values[i] && *values[i] != 0) out.push_back(i);
    }
    return out;
}

std::uint32_t Share::CustomOf(std::uint8_t number) const {
    const auto it = their_custom_.find(number);
    return it == their_custom_.end() ? std::uint32_t{0} : it->second;
}

std::uint16_t Share::CharacterOf(std::uint8_t number) const {
    const auto it = their_character_.find(number);
    return it == their_character_.end() ? std::uint16_t{0} : it->second;
}

bool Share::OnEvent(const coop::net::Event& event) {
    switch (event.kind) {
    case EventKind::welcomed: {
        // A new session (or a server that lost us): the others' looks of the
        // last one are not theirs any more; they send them again.
        const bool had = !theirs_.empty();
        theirs_.clear();
        their_character_.clear();
        their_custom_.clear();
        slots_.fill(0);
        for (std::size_t i = 0; i < kSlots; ++i) {
            Forget(i);
        }
        due_ = ours_.has_value() || character_said_;
        return had;
    }
    case EventKind::reconnected:
    case EventKind::player_joined:
        due_ = ours_.has_value() || character_said_;   // the newcomer (or a server that lost us) has not got it
        return false;
    case EventKind::player_left:
        Free(event.player);
        their_character_.erase(event.player);
        their_custom_.erase(event.player);
        return theirs_.erase(event.player) != 0;
    case EventKind::message:
        if (const auto* look = std::get_if<coop::protocol::PlayerLook>(&event.message);
            look != nullptr && look->player_number != coop::protocol::kPlayerSelf) {
            // The picked character rides along as a part of its own (kCharacterPart): out of the parts.
            Parts parts = look->parts;
            std::uint16_t character = 0;
            std::uint32_t custom = 0;
            for (auto it = parts.begin(); it != parts.end();) {
                if (it->part != kCharacterPart && it->part != kCustomPart) {
                    ++it;
                    continue;
                }
                if (it->part == kCustomPart) {
                    // (0.11.9, one customizer: a 0.11.8 game still sends its Character Select options; they are taken
                    // out of the parts and not kept, so no puppet is dressed by the mod's routine with them)
                    custom = 0;
                } else if ((it->colour & 0xFF0000u) == kCharacterMark && (it->colour & 0xFFFFu) <= kCharacterMax) {
                    character = static_cast<std::uint16_t>(it->colour & 0xFFFFu);
                }
                it = parts.erase(it);
            }
            if (character == 0) {
                custom = 0;   // (the options are a character's)
            }
            auto& theirs = their_character_[look->player_number];
            auto& their_custom = their_custom_[look->player_number];
            auto& worn = theirs_[look->player_number];
            const bool changed = worn != parts || theirs != character || their_custom != custom;
            worn = std::move(parts);
            theirs = character;
            their_custom = custom;
            // Every player gets a slot, gear colours or not (held-sword): a puppet
            // that wears the game's own item shares its model's blade switch with
            // our own Link's, so its sword hid whenever ours was put away. Their
            // own copy, painted in no colours, is the game's item with its own.
            Assign(look->player_number);
            return changed;
        }
        return false;
    default:
        return false;
    }
}

std::optional<coop::protocol::PlayerLook> Share::TakeSend() {
    if (!due_ || (!ours_ && !character_said_)) {
        return std::nullopt;
    }
    due_ = false;
    Parts parts = ours_ ? *ours_ : Parts{};
    if (character_ != 0) {
        parts.push_back({kCharacterPart, kCharacterMark | character_});
        if (custom_ != 0) {
            parts.push_back({kCustomPart, custom_});
        }
    }
    return coop::protocol::PlayerLook{coop::protocol::kPlayerSelf, std::move(parts)};
}

std::optional<Parts> Share::Of(std::uint8_t number) const {
    const auto it = theirs_.find(number);
    if (it == theirs_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<std::size_t> Share::SlotOf(std::uint8_t number) const {
    if (number == 0) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < kSlots; ++i) {
        if (slots_[i] == number) {
            return i + 1;
        }
    }
    return std::nullopt;
}

std::vector<Share::Job> Share::Jobs(std::uint64_t now_ms) const {
    std::vector<Job> out;
    for (std::size_t i = 0; i < kSlots; ++i) {
        const std::uint8_t player = slots_[i];
        const auto parts = player != 0 ? Of(player) : std::nullopt;
        if (!parts) {
            continue;
        }
        const Ready& r = ready_[i];
        if (r.copy != 0 && r.player == player && r.parts == *parts) {
            continue;   // painted already
        }
        if (r.copy != 0 && now_ms < r.at_ms + kCopyDwellMs) {
            continue;   // the puppet may still wear the other copy: later
        }
        const std::size_t first = kCopiesPerSlot * i + 1;
        out.push_back({i + 1, r.copy == first ? first + 1 : first, player, *parts});
    }
    return out;
}

void Share::Done(const Job& job, std::uint64_t now_ms) {
    if (job.slot < 1 || job.slot > kSlots) {
        return;
    }
    Ready& r = ready_[job.slot - 1];
    if (slots_[job.slot - 1] != job.player) {
        // The slot changed hands meanwhile: its next job paints it, but not
        // this copy (a puppet may have been given it already).
        r.copy = job.copy;
        r.at_ms = now_ms;
        return;
    }
    r = {job.copy, job.player, job.parts, now_ms};
}

void Share::Forget(std::size_t i) {
    ready_[i].player = 0;   // nobody wears it now; the copy is kept, so the next job paints the other one
    ready_[i].parts.clear();
}

std::optional<std::size_t> Share::CopyOf(std::uint8_t number) const {
    const auto slot = SlotOf(number);
    if (!slot) {
        return std::nullopt;
    }
    const Ready& r = ready_[*slot - 1];
    if (r.copy != 0 && r.player == number) {
        return r.copy;
    }
    // (row 198) Not painted for them yet: the copy their first job paints (Jobs: never the one painted last), in
    // the game's colours or an older look until then, but marked as a copy. Never nothing: the puppet would wear the
    // game's own item, the very texture your own Link's colour is drawn over.
    const std::size_t first = kCopiesPerSlot * (*slot - 1) + 1;
    return r.copy == first ? first + 1 : first;
}

void Share::EnableSlots(bool on) {
    slots_on_ = on;
    slots_.fill(0);
    ready_.fill({});
    if (on) {
        for (const auto& [number, parts] : theirs_) {   // in player order: the same slots every time
            Assign(number);   // every player, gear colours or not (OnEvent)
        }
    }
}

void Share::Assign(std::uint8_t number) {
    if (!slots_on_ || number == 0 || SlotOf(number)) {
        return;
    }
    for (auto& s : slots_) {
        if (s == 0) {
            s = number;
            return;
        }
    }
}

void Share::Free(std::uint8_t number) {
    for (std::size_t i = 0; i < kSlots; ++i) {
        if (slots_[i] == number) {
            slots_[i] = 0;
            Forget(i);
        }
    }
}

namespace {
std::mutex g_mutex;
Share g_share;   // guarded
}  // namespace

void Set(Parts parts) {
    std::lock_guard lock(g_mutex);
    g_share.Set(std::move(parts));
}

void SetCharacter(std::uint16_t number) {
    std::lock_guard lock(g_mutex);
    g_share.SetCharacter(number);
}

std::uint16_t CharacterOf(std::uint8_t number) {
    std::lock_guard lock(g_mutex);
    return g_share.CharacterOf(number);
}

void SetCustom(std::uint32_t options) {
    std::lock_guard lock(g_mutex);
    g_share.SetCustom(options);
}

std::uint32_t CustomOf(std::uint8_t number) {
    std::lock_guard lock(g_mutex);
    return g_share.CustomOf(number);
}

void OnEvent(const coop::net::Event& event, coop::net::Client& client) {
    std::optional<coop::protocol::PlayerLook> send;
    bool changed = false;
    std::uint16_t character = 0;
    {
        std::lock_guard lock(g_mutex);
        changed = g_share.OnEvent(event);
        send = g_share.TakeSend();
        if (const auto* look = std::get_if<coop::protocol::PlayerLook>(&event.message);
            event.kind == EventKind::message && look != nullptr) {
            character = g_share.CharacterOf(look->player_number);
        }
    }
    if (send) {
        client.send(*send);   // the network thread: its own client, no lock taken twice
    }
#ifdef LICHII_COOP
    if (const auto* look = std::get_if<coop::protocol::PlayerLook>(&event.message);
        changed && event.kind == EventKind::message && look != nullptr) {
        LOG_INFO(Core, "coop: look: P{} wears {} part colour(s), character {} (PLAYER_LOOK)", look->player_number,
                 look->parts.size() - (character != 0 ? 1 : 0), character);
    }
#else
    (void)changed;
    (void)character;
#endif
}

void Frame(coop::host::Session* session) {
    if (session == nullptr) {
        return;
    }
    std::optional<coop::protocol::PlayerLook> send;
    {
        std::lock_guard lock(g_mutex);
        send = g_share.TakeSend();
    }
    if (send) {
        session->with_client([&](coop::net::Client& c) { c.send(*send); });
#ifdef LICHII_COOP
        LOG_INFO(Core, "coop: look: {} part colour(s) sent (PLAYER_LOOK)", send->parts.size());
#endif
    }
}

std::optional<Parts> Of(std::uint8_t number) {
    std::lock_guard lock(g_mutex);
    return g_share.Of(number);
}

std::optional<std::size_t> SlotOf(std::uint8_t number) {
    std::lock_guard lock(g_mutex);
    return g_share.SlotOf(number);
}

std::vector<Share::Job> Jobs(std::uint64_t now_ms) {
    std::lock_guard lock(g_mutex);
    return g_share.Jobs(now_ms);
}

void Done(const Share::Job& job, std::uint64_t now_ms) {
    std::lock_guard lock(g_mutex);
    g_share.Done(job, now_ms);
}

std::optional<std::size_t> CopyOf(std::uint8_t number) {
    std::lock_guard lock(g_mutex);
    return g_share.CopyOf(number);
}

void EnableSlots(bool on) {
    std::lock_guard lock(g_mutex);
    g_share.EnableSlots(on);
}

}  // namespace CoopLookShare
