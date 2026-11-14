// SPDX-License-Identifier: GPL-3.0-or-later
// Local co-op online (docs/local-coop.md, step 4): each extra player on this
// PC is a player of the session, with its own number, name and colour, on
// this game's one connection (docs/protocol.md, "Local players").
//
// - Seats: which seats are in the session. While connected, a seat in play
//   that has no number asks for one (LOCAL_JOIN, again after a reconnect and
//   at most every kAskEveryMs); a seat that left play is taken out
//   (LOCAL_LEAVE). The server answers LOCAL_JOINED.
// - Each joined seat's own messages (its samples, status, equipment) go out
//   in envelopes: Wrap. The other games meet it as any player; the server
//   never sends this game its own seats' messages.
//
// No game or network code here (the hook passes in what it sends with), so
// the tests run it alone.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "coop/host/game_memory.hpp"
#include "coop/host/player_reader.hpp"
#include "coop/host/sampler.hpp"
#include "coop/net/client.hpp"
#include "coop/protocol/messages.hpp"
#include "coop_local_game.h"

namespace CoopLocalNet {

inline constexpr std::uint64_t kAskEveryMs = 3000;

class Seats {
public:
    using Send = std::function<bool(const coop::protocol::Message&)>;
    using Log = std::function<void(const std::string&)>;

    explicit Seats(Log log = {}) : log_(std::move(log)) {}
    void SetLog(Log log) {
        std::lock_guard lock(mutex_);
        log_ = std::move(log);
    }

    // The calc thread, each frame: `connected` (welcomed), the seats in play.
    void Frame(bool connected, const std::vector<CoopLocalGame::SeatNow>& seats, std::uint64_t now_ms,
               const Send& send);
    // The network thread: a new WELCOME (a new connection: every seat asks
    // again), LOCAL_JOINED.
    void Welcomed();
    void Answer(const coop::protocol::LocalJoined& answer);
    // The seat's number in the session; nothing while it is not in.
    std::optional<std::uint8_t> Number(int seat) const;
    // The seat's own message in its envelope; nothing when the seat is not
    // in, or no envelope carries that message.
    std::optional<coop::protocol::Message> Wrap(int seat, const coop::protocol::Message& own) const;

private:
    struct State {
        bool in_play = false;
        std::string name;
        std::optional<std::uint8_t> number;
        std::optional<std::uint64_t> asked_ms;
        bool refused = false;   // until the next connection
        bool leave_due = false; // a number came for a seat no longer in play
    };
    static bool Valid(int seat) { return seat >= 1 && seat <= coop::protocol::kMaxLocalSeats; }
    static std::size_t Index(int seat) { return static_cast<std::size_t>(seat - 1); }
    void Say(const std::string& line) const {
        if (log_) log_("local co-op: " + line);
    }

    Log log_;
    mutable std::mutex mutex_;
    std::array<State, coop::protocol::kMaxLocalSeats> seats_{};
};

// One seat's own samples: the game's own sampler over a reading of its
// Player (PlayerReader::set_actor), with the first Link's scene, age and
// event (one game, one scene) and the seat's colour.
class Sampling {
public:
    // The messages to wrap and send this frame (empty while the Player cannot be read).
    std::vector<coop::protocol::Message> Frame(coop::host::GameMemory& memory, std::uint64_t actor,
                                               const std::optional<coop::host::PlayerReading>& own,
                                               std::optional<std::uint32_t> color, std::uint64_t server_now_us);
    void Reset() { sampler_.reset(); }

private:
    coop::host::Sampler sampler_;
    coop::host::PlayerReader::DrawnState drawn_{};
};

// The hook's two calls (one game, the Seats and Samplings of its seats).
// The network thread, for each event: WELCOME and LOCAL_JOINED.
void OnEvent(const coop::net::Event& event);
// The calc thread, after the session's frame: seats in and out, and each
// joined seat's messages through `send`.
void Online(coop::host::GameMemory& memory, const std::optional<coop::host::PlayerReading>& own, bool connected,
            std::uint64_t server_now_us, const Seats::Send& send, const Seats::Log& log);

}  // namespace CoopLocalNet
