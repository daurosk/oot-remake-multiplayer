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

bool Share::OnEvent(const coop::net::Event& event) {
    switch (event.kind) {
    case EventKind::welcomed: {
        // A new session (or a server that lost us): the others' looks of the
        // last one are not theirs any more; they send them again.
        const bool had = !theirs_.empty();
        theirs_.clear();
        slots_.fill(0);
        for (std::size_t i = 0; i < kSlots; ++i) {
            Forget(i);
        }
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
    if (r.copy == 0 || r.player != number) {
        return std::nullopt;
    }
    return r.copy;
}

void Share::EnableSlots(bool on) {
    slots_on_ = on;
    slots_.fill(0);
    ready_.fill({});
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

void OnEvent(const coop::net::Event& event, coop::net::Client& client) {
    std::optional<coop::protocol::PlayerLook> send;
    bool changed = false;
    {
        std::lock_guard lock(g_mutex);
        changed = g_share.OnEvent(event);
        send = g_share.TakeSend();
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
