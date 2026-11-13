// SPDX-License-Identifier: GPL-3.0-or-later
// Local co-op online (coop_local_net.h).
#include "coop_local_net.h"

#include "coop/protocol/codec.hpp"

namespace CoopLocalNet {

using coop::protocol::LocalJoin;
using coop::protocol::LocalJoined;
using coop::protocol::LocalLeave;
using coop::protocol::Message;

void Seats::Frame(bool connected, const std::vector<CoopLocalGame::SeatNow>& seats, std::uint64_t now_ms,
                  const Send& send) {
    std::vector<Message> out;
    {
        std::lock_guard lock(mutex_);
        for (int k = 1; k <= coop::protocol::kMaxLocalSeats; k++) {
            State& s = seats_[Index(k)];
            const CoopLocalGame::SeatNow* now = nullptr;
            for (const auto& seat : seats)
                if (seat.player == k) now = &seat;
            if (now == nullptr) {
                if ((s.number || s.leave_due) && connected) out.push_back(LocalLeave{static_cast<std::uint8_t>(k)});
                s = State{};
                continue;
            }
            if (!s.in_play) s.name = now->name;   // (the name a seat joins with is kept while it plays)
            s.in_play = true;
            if (!connected || s.number || s.refused) continue;
            if (s.asked_ms && now_ms - *s.asked_ms < kAskEveryMs) continue;
            s.asked_ms = now_ms;
            out.push_back(LocalJoin{static_cast<std::uint8_t>(k), s.name});
        }
    }
    for (const Message& m : out) send(m);   // (outside the lock: the client has its own)
}

void Seats::Welcomed() {
    std::lock_guard lock(mutex_);
    for (State& s : seats_) {
        s.number.reset();
        s.asked_ms.reset();
        s.refused = false;
        s.leave_due = false;   // (the server took them out with the old connection)
    }
}

void Seats::Answer(const LocalJoined& answer) {
    if (!Valid(answer.seat)) return;
    std::lock_guard lock(mutex_);
    State& s = seats_[Index(answer.seat)];
    if (!s.in_play) {   // (left play while it was asked: taken out again at the next frame)
        s.leave_due = answer.player_number != 0;
        return;
    }
    if (answer.player_number != 0) {
        if (s.number != answer.player_number)
            Say("player " + std::to_string(answer.seat + 1) + " (" + s.name + ") is player " +
                std::to_string(answer.player_number) + " of the session");
        s.number = answer.player_number;
        return;
    }
    s.refused = true;
    const char* why = answer.reason == coop::protocol::RefuseReason::name_taken ? "its name is taken"
                      : answer.reason == coop::protocol::RefuseReason::bad_name ? "its name is blank"
                      : answer.reason == coop::protocol::RefuseReason::server_full ? "the session is full"
                                                                                    : "refused";
    Say("player " + std::to_string(answer.seat + 1) + " (" + s.name + ") plays here only: " + why);
}

std::optional<std::uint8_t> Seats::Number(int seat) const {
    if (!Valid(seat)) return std::nullopt;
    std::lock_guard lock(mutex_);
    return seats_[Index(seat)].number;
}

std::optional<Message> Seats::Wrap(int seat, const Message& own) const {
    if (!Number(seat)) return std::nullopt;
    return coop::protocol::local_envelope(static_cast<std::uint8_t>(seat), own);
}

std::vector<Message> Sampling::Frame(coop::host::GameMemory& memory, std::uint64_t actor,
                                     const std::optional<coop::host::PlayerReading>& own,
                                     std::optional<std::uint32_t> color, std::uint64_t server_now_us) {
    std::optional<coop::host::PlayerReading> reading;
    if (actor != 0 && own) {
        coop::host::PlayerReader reader(memory, &drawn_);
        reader.set_actor(actor);
        reading = reader.read();
    }
    if (reading) {
        reading->scene = own->scene;
        reading->age = own->age;
        reading->event_running = own->event_running;
        if (reading->equipment && color) reading->equipment->color = color;
    }
    std::vector<Message> out;
    for (const coop::host::Outgoing& o : sampler_.frame(reading, server_now_us)) {
        Message m;
        if (coop::protocol::decode(o.bytes, m) == coop::protocol::Error::none) out.push_back(std::move(m));
    }
    return out;
}

namespace {
Seats& TheSeats() {
    static Seats seats;
    return seats;
}
std::array<Sampling, coop::protocol::kMaxLocalSeats>& TheSamplings() {
    static std::array<Sampling, coop::protocol::kMaxLocalSeats> samplings;
    return samplings;
}
}  // namespace

void OnEvent(const coop::net::Event& event) {
    if (event.kind == coop::net::EventKind::welcomed || event.kind == coop::net::EventKind::reconnected) {
        TheSeats().Welcomed();
    } else if (event.kind == coop::net::EventKind::message) {
        if (const auto* answer = std::get_if<LocalJoined>(&event.message)) TheSeats().Answer(*answer);
    }
}

void Online(coop::host::GameMemory& memory, const std::optional<coop::host::PlayerReading>& own, bool connected,
            std::uint64_t server_now_us, const Seats::Send& send, const Seats::Log& log) {
    static std::once_flag logged;
    std::call_once(logged, [&] { TheSeats().SetLog(log); });
    const std::vector<CoopLocalGame::SeatNow> seats = CoopLocalGame::Seats();
    const auto now_ms = static_cast<std::uint64_t>(server_now_us / 1000);
    TheSeats().Frame(connected, seats, now_ms, send);
    for (int k = 1; k <= coop::protocol::kMaxLocalSeats; k++) {
        Sampling& sampling = TheSamplings()[static_cast<std::size_t>(k - 1)];
        const CoopLocalGame::SeatNow* seat = nullptr;
        for (const auto& s : seats)
            if (s.player == k) seat = &s;
        if (!connected || seat == nullptr || !TheSeats().Number(k)) {
            sampling.Reset();   // (a seat that comes in starts with a full sample)
            continue;
        }
        for (const Message& m : sampling.Frame(memory, seat->actor, own, seat->color, server_now_us))
            if (auto wrapped = TheSeats().Wrap(k, m)) send(*wrapped);
    }
}

}  // namespace CoopLocalNet
