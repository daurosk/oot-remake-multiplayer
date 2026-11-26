// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_switch_share.h"

#include <algorithm>
#include <cstdio>

#include "coop/protocol/codec.hpp"
#include "coop_fairy.h"
#include "coop_logic_set.h"

namespace CoopSwitchShare {

namespace {
std::string Describe(const CoopRoomObjects::SwitchPlug& l) {
    char text[200];
    std::snprintf(text, sizeof text, "%.*s %.*s at %.1f %.1f %.1f",
                  static_cast<int>(l.name.size()), l.name.data(), static_cast<int>(l.plug.size()), l.plug.data(),
                  l.place[0], l.place[1], l.place[2]);
    return text;
}
}  // namespace

std::uint32_t PlugKey(std::string_view plug) { return coop::protocol::flag_hash(plug); }

void Sharer::Receive(const coop::protocol::SwitchState& m, std::uint64_t now_ms) {
    std::lock_guard lock(mutex_);
    for (const CoopRoomObjects::SwitchPlug* line : CoopRoomObjects::SwitchPlugsOf(m.scene, m.name, m.place)) {
        if (PlugKey(line->plug) != m.plug) continue;
        State& s = states_[line];
        if (m.on) {
            s.remote[m.player_number] = Remote{true, now_ms + kRemoteHoldMs};
        } else {
            s.remote.erase(m.player_number);
        }
    }
}

void Sharer::Forget(std::uint8_t player) {
    std::lock_guard lock(mutex_);
    for (auto& kv : states_) kv.second.remote.erase(player);
}

bool Sharer::RemoteOn(State& s, std::uint64_t now_ms, const std::set<std::uint8_t>& away) {
    std::erase_if(s.remote, [now_ms](const auto& r) { return !r.second.on || now_ms > r.second.until_ms; });
    // Row 164 follow-up: a presser whose world stands still is kept (its press comes back with it) but not held: its
    // switch's own timer stands still, so holding ours would keep the door or mover open the whole pause.
    return std::any_of(s.remote.begin(), s.remote.end(), [&away](const auto& r) { return !away.contains(r.first); });
}

std::vector<Hold> Sharer::Tick(const std::string& scene, const std::vector<Seen>& seen, bool others_here,
                               std::uint8_t self, std::uint64_t now_ms, std::vector<coop::protocol::SwitchState>& send,
                               std::vector<std::string>& notes, const std::set<std::uint8_t>& away) {
    std::lock_guard lock(mutex_);
    if (scene != scene_) {
        // A new scene: nothing of the old one is held or owed. A press of this scene received before our first walk
        // here is kept.
        std::erase_if(states_, [&scene](const auto& kv) { return kv.first->scene != scene; });
        scene_ = scene;
    }
    std::vector<Hold> holds;
    ++tick_;
    for (const Seen& v : seen) {
        if (v.line == nullptr || v.line->scene != scene) continue;
        State& s = states_[v.line];
        // A load guard (Seen::load_on) counts as it was at the room's load: the first sight of this switch actor, or
        // the first after a walk without it (a room reload or a save load makes a new one, maybe at the same address).
        if (!s.load_on || v.address != s.load_address) {
            s.load_on = v.load_on;
            s.load_address = v.address;
            if (v.line->guard.find(CoopRoomObjects::kLoadGuard) != std::string_view::npos) {
                notes.push_back("switch share: " + Describe(*v.line) +
                                (v.load_on ? ": room loaded with its load guard on: our asker is gone"
                                           : ": room loaded with its load guard off: our asker stays, not held this "
                                             "visit"));
            }
        }
        s.seen_tick = tick_;
        const bool armed = v.armed && *s.load_on;
        const bool remote_on = RemoteOn(s, now_ms, away);
        // g3-objects: a plug that also raises our event asker is held only once that asker cannot fire (armed);
        // until then another player's press is not ours to hold, and our own plug is still our own.
        const bool remote = remote_on && armed;
        if (remote_on && !armed && !s.told_waiting) {
            s.told_waiting = true;
            notes.push_back("switch share: " + Describe(*v.line) + ": another player's is on: waiting for " +
                            std::string(v.line->guard) + " here (holding it would raise our asker's trigger: a replay; the result is our asker's)");
        }
        if (armed || !remote_on) s.told_waiting = false;  // review-a38-3: a press that ends unarmed logs the next wait
        if (s.held && !remote) {
            s.held = false;
            s.told_held = false;
            s.told_lead = false;
            s.echo_until_ms = now_ms + kEchoMs;  // the switch's AI puts its own 0 back
            char text[64];
            std::snprintf(text, sizeof text, ": another player's is off: let go after %.1f s held",
                          static_cast<double>(now_ms - s.held_since_ms) / 1000.0);
            notes.push_back("switch share: " + Describe(*v.line) + text);
        }
        if (remote) {
            if (!s.held) s.held_since_ms = now_ms;
            s.held = true;
            if (v.on != std::optional<bool>(true)) holds.push_back(Hold{v.address, PlugKey(v.line->plug)});
            if (!s.told_held) {
                s.told_held = true;
                notes.push_back("switch share: " + Describe(*v.line) +
                                ": another player's is on: ours held on, its own logic runs");
            }
            // Row 244: our press, sent before this hold, is resent while we hold when ours is the lowest number of
            // the pressers, so two players on one switch never both go quiet (header).
            // (Row 164 follow-up: a presser whose world stands still sends nothing, so it is no lower number here.)
            std::uint8_t lowest = 0;
            for (const auto& r : s.remote) {
                if (!away.contains(r.first)) {
                    lowest = r.first;
                    break;
                }
            }
            const bool lead = self != 0 && lowest > self;
            if (s.sent_on && lead && others_here && now_ms >= s.last_send_ms + kResendMs) {
                coop::protocol::SwitchState m;
                m.scene = scene;
                m.name = std::string(v.line->name);
                m.place = v.line->place;
                m.plug = PlugKey(v.line->plug);
                m.on = true;
                send.push_back(std::move(m));
                s.last_send_ms = now_ms;
                if (!s.told_lead) {
                    s.told_lead = true;
                    notes.push_back("switch share: " + Describe(*v.line) +
                                    ": another player presses it too: ours is resent while held (the lowest number "
                                    "of the pressers sends)");
                }
            }
            continue;  // what our plug says now is our hold, not a press of ours
        }
        if (now_ms < s.echo_until_ms || !v.on) continue;
        const auto message = [&](bool on) {
            coop::protocol::SwitchState m;
            m.scene = scene;
            m.name = std::string(v.line->name);
            m.place = v.line->place;
            m.plug = PlugKey(v.line->plug);
            m.on = on;
            send.push_back(std::move(m));
            s.last_send_ms = now_ms;
        };
        if (*v.on) {
            if (!others_here) continue;  // solo: nobody to tell
            if (!s.sent_on || now_ms >= s.last_send_ms + kResendMs) {
                message(true);
                if (!s.sent_on) s.pressed_since_ms = now_ms;
                if (!s.sent_on && !s.told_sent) {
                    s.told_sent = true;
                    notes.push_back("switch share: " + Describe(*v.line) + ": our player's press: sent");
                }
                s.sent_on = true;
            }
        } else if (s.sent_on) {
            message(false);
            s.sent_on = false;
            s.told_sent = false;
            char text[64];
            std::snprintf(text, sizeof text, ": our player's press ended after %.1f s: sent",
                          static_cast<double>(now_ms - s.pressed_since_ms) / 1000.0);
            notes.push_back("switch share: " + Describe(*v.line) + text);
        }
    }
    for (auto& kv : states_) {
        if (kv.second.seen_tick != tick_) kv.second.load_on.reset();  // this walk had no such switch: its room went
    }
    return holds;
}

void Sharer::Clear() {
    std::lock_guard lock(mutex_);
    states_.clear();
    scene_.clear();
}

void Setter::Want(const std::vector<Hold>& holds, std::uint64_t main, std::uint64_t now_ms) {
    std::lock_guard lock(mutex_);
    main_ = main;
    for (const Hold& h : holds) {
        auto it = std::find_if(wanted_.begin(), wanted_.end(), [&h](const auto& w) { return w.first == h; });
        if (it == wanted_.end()) {
            wanted_.emplace_back(h, now_ms + kSetHoldMs);
        } else {
            it->second = now_ms + kSetHoldMs;
        }
    }
}

void Setter::Clear() {
    std::lock_guard lock(mutex_);
    wanted_.clear();
}

std::vector<std::string> Setter::TakeNotes() {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    out.swap(notes_);
    return out;
}

std::optional<CoopPuppet::Call> Setter::Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                             std::uint64_t now_ms) {
    std::uint64_t main = 0;
    {
        std::lock_guard lock(mutex_);
        main = main_;
        if (after_call && asked_malloc_) {
            asked_malloc_ = false;
            if (result == 0) {
                failed_ = true;
                notes_.push_back("switch share: the game's malloc gave nothing; switch holds off");
                return std::nullopt;
            }
            block_ = result;
        }
        if (!after_call) {
            // A new frame: every plug still wanted, once.
            std::erase_if(wanted_, [now_ms](const auto& w) { return w.second < now_ms; });
            frame_.clear();
            for (const auto& w : wanted_) frame_.push_back(w.first);
        }
    }
    if (failed_ || main == 0 || frame_.empty()) return std::nullopt;
    if (block_ == 0) {
        if (after_call) return std::nullopt;  // the malloc is asked at the start of a frame only
        std::lock_guard lock(mutex_);
        asked_malloc_ = true;
        return CoopLogicSet::Malloc(main);
    }
    return Next(guest, main);
}

std::optional<CoopPuppet::Call> Setter::Next(CoopPuppet::Guest& guest, std::uint64_t main) {
    while (!frame_.empty()) {
        const Hold h = frame_.front();
        frame_.pop_front();
        if (!CoopFairy::ActorPresent(guest, h.address)) continue;
        // The execute reads [actor+0x270] without a check: only a switch whose logic has a live bool plug of that key.
        const auto plug = CoopLogicSet::BoolPlug(guest, h.address, h.key);
        if (!plug) continue;
        if (plug->value == std::optional<std::uint8_t>(std::uint8_t{1})) continue;  // on already
        if (!CoopLogicSet::WriteNode(guest, block_, main, h.address, h.key)) continue;
        return CoopLogicSet::Execute(main, block_);
    }
    return std::nullopt;
}

Sharer& TheSharer() {
    static Sharer sharer;
    return sharer;
}

Setter& TheSetter() {
    static Setter setter;
    return setter;
}

}  // namespace CoopSwitchShare
