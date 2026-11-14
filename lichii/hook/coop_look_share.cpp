// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_look_share.h"

#include <algorithm>
#include <atomic>
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

bool Share::OnEvent(const coop::net::Event& event) {
    switch (event.kind) {
    case EventKind::welcomed: {
        // A new session (or a server that lost us): the others' looks of the
        // last one are not theirs any more; they send them again.
        const bool had = !theirs_.empty();
        theirs_.clear();
        slots_.fill(0);
        due_ = ours_.has_value();
        return had;
    }
    case EventKind::reconnected:
    case EventKind::player_joined:
        due_ = ours_.has_value();   // the newcomer (or a server that lost us) has not got it
        return false;
    case EventKind::player_left:
        Free(event.player);
        return theirs_.erase(event.player) != 0;
    case EventKind::message:
        if (const auto* look = std::get_if<coop::protocol::PlayerLook>(&event.message);
            look != nullptr && look->player_number != coop::protocol::kPlayerSelf) {
            auto& worn = theirs_[look->player_number];
            const bool changed = worn != look->parts;
            worn = look->parts;
            if (HasGear(worn)) {
                Assign(look->player_number);
            } else {
                Free(look->player_number);
            }
            return changed;
        }
        return false;
    default:
        return false;
    }
}

std::optional<coop::protocol::PlayerLook> Share::TakeSend() {
    if (!due_ || !ours_) {
        return std::nullopt;
    }
    due_ = false;
    return coop::protocol::PlayerLook{coop::protocol::kPlayerSelf, *ours_};
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

std::array<std::optional<Parts>, kSlots> Share::SlotLooks() const {
    std::array<std::optional<Parts>, kSlots> out;
    for (std::size_t i = 0; i < kSlots; ++i) {
        if (slots_[i] != 0) {
            out[i] = Of(slots_[i]);
        }
    }
    return out;
}

void Share::EnableSlots(bool on) {
    slots_on_ = on;
    slots_.fill(0);
    if (on) {
        for (const auto& [number, parts] : theirs_) {   // in player order: the same slots every time
            if (HasGear(parts)) {
                Assign(number);
            }
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
    for (auto& s : slots_) {
        if (s == number) {
            s = 0;
        }
    }
}

namespace {
std::mutex g_mutex;
Share g_share;   // guarded
std::atomic<std::uint64_t> g_serial{0};
}  // namespace

void Set(Parts parts) {
    std::lock_guard lock(g_mutex);
    g_share.Set(std::move(parts));
}

void OnEvent(const coop::net::Event& event, coop::net::Client& client) {
    std::optional<coop::protocol::PlayerLook> send;
    bool changed = false;
    {
        std::lock_guard lock(g_mutex);
        changed = g_share.OnEvent(event);
        send = g_share.TakeSend();
    }
    if (changed) {
        g_serial.fetch_add(1);
    }
    if (send) {
        client.send(*send);   // the network thread: its own client, no lock taken twice
    }
#ifdef LICHII_COOP
    if (const auto* look = std::get_if<coop::protocol::PlayerLook>(&event.message);
        changed && event.kind == EventKind::message && look != nullptr) {
        LOG_INFO(Core, "coop: look: P{} wears {} part colour(s) (PLAYER_LOOK)", look->player_number,
                 look->parts.size());
    }
#else
    (void)changed;
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

std::array<std::optional<Parts>, kSlots> SlotLooks() {
    std::lock_guard lock(g_mutex);
    return g_share.SlotLooks();
}

void EnableSlots(bool on) {
    {
        std::lock_guard lock(g_mutex);
        g_share.EnableSlots(on);
    }
    g_serial.fetch_add(1);
}

std::uint64_t Serial() {
    return g_serial.load();
}

void NoteWorn() {
    g_serial.fetch_add(1);
}

}  // namespace CoopLookShare
