// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_look_share.h"

#include <utility>

#include "coop/host/session.hpp"
#include "coop/net/client.hpp"

#ifdef LICHII_COOP
#include "common/logging.h"
#endif

namespace CoopLookShare {

using coop::net::EventKind;

void Share::Set(Parts parts) {
    ours_ = std::move(parts);
    due_ = true;
}

bool Share::OnEvent(const coop::net::Event& event) {
    switch (event.kind) {
    case EventKind::welcomed:
    case EventKind::reconnected:
    case EventKind::player_joined:
        due_ = ours_.has_value();   // the newcomer (or a server that lost us) has not got it
        return false;
    case EventKind::player_left:
        return theirs_.erase(event.player) != 0;
    case EventKind::message:
        if (const auto* look = std::get_if<coop::protocol::PlayerLook>(&event.message);
            look != nullptr && look->player_number != coop::protocol::kPlayerSelf) {
            auto& worn = theirs_[look->player_number];
            const bool changed = worn != look->parts;
            worn = look->parts;
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

}  // namespace CoopLookShare
