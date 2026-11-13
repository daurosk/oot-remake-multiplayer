// SPDX-License-Identifier: GPL-3.0-or-later
// The other player's Link reads its own input (docs/native-player.md, step 2).
//
// The game's Player reads its pad in two small functions, each called from one
// AI node's update: the buttons (main+0x279ce80) and the sticks
// (main+0x279e2c0); a third (main+0x3025850) says whether input is off. On
// Hookshot all three are wrapped (patch 0010, wrap ids below). This file is
// what the wraps do, without any game or network code:
//   - for the local Link: after the game's own read, the bytes it read are
//     kept as one Frame for the other games;
//   - for another player's Link (a real Player actor, coop_native_player.h):
//     that player's next Frame is written where the game's read would have
//     put it, and the game's read is skipped.
// The Player code then runs on the same input in both games.
#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace CoopPlayerInput {

// Wrap ids (hookshot/src/hos_coop_player.h, nx2/coop_player_wraps.py).
inline constexpr std::uint32_t kWrapButtons = 16;
inline constexpr std::uint32_t kWrapSticks = 17;
inline constexpr std::uint32_t kWrapInputOff = 18;
inline constexpr std::uint32_t kWrapControllerGet = 19;

namespace addr {
// The buttons' out block (0x1d bytes, zeroed by the caller): the read ORs into
inline constexpr std::uint64_t kOutHold = 0x18;     // u16: buttons held
inline constexpr std::uint64_t kOutEdgeA = 0x1a;    // u16: from the controller's +0x114 word
inline constexpr std::uint64_t kOutEdgeB = 0x1c;    // u8: from the controller's +0xc word
// The sticks' out block: +0 the controller's +0x120 pair (the move stick), +8 its +0x128 pair.
inline constexpr std::uint64_t kOutSticks = 0x0;
// An AI node's actor: [[[node+8]+8]+0x1f0] (main+0x973030).
inline constexpr std::uint64_t kNodeOwner = 0x8;
inline constexpr std::uint64_t kOwnerHolder = 0x8;
inline constexpr std::uint64_t kHolderActor = 0x1f0;
// Where the input-off check returns to in the buttons' node (main+0x279cdf4 is its call).
inline constexpr std::uint64_t kButtonsOffReturn = 0x279cdf8;
}  // namespace addr

// One game frame of a Player's input, as the two reads give it.
struct Frame {
    std::uint16_t hold = 0;
    std::uint16_t edge_a = 0;  // true for one frame: must reach the other game exactly once
    std::uint8_t edge_b = 0;   // as edge_a
    std::array<float, 4> sticks{};  // move stick x, y; the other stick x, y

    bool operator==(const Frame& o) const {
        return hold == o.hold && edge_a == o.edge_a && edge_b == o.edge_b && sticks == o.sticks;
    }
    bool Level() const { return edge_a == 0 && edge_b == 0; }  // nothing that lasts one frame
};

// The move stick is read against the camera: the other player pushed it
// against THEIR camera. Turned by (their camera yaw - ours), the local game
// gets the same direction in the world. Radians, counter-clockwise.
inline void TurnMoveStick(Frame& f, float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    const float x = f.sticks[0], y = f.sticks[1];
    f.sticks[0] = x * c - y * s;
    f.sticks[1] = x * s + y * c;
}

// On the wire: 21 bytes a frame, little endian. A run of the same level frame
// is one frame and a count.
inline constexpr std::size_t kFrameBytes = 21;

inline void Encode(const Frame& f, std::uint8_t* out) {
    std::memcpy(out + 0, &f.hold, 2);
    std::memcpy(out + 2, &f.edge_a, 2);
    out[4] = f.edge_b;
    std::memcpy(out + 5, f.sticks.data(), 16);
}

inline Frame Decode(const std::uint8_t* in) {
    Frame f;
    std::memcpy(&f.hold, in + 0, 2);
    std::memcpy(&f.edge_a, in + 2, 2);
    f.edge_b = in[4];
    std::memcpy(f.sticks.data(), in + 5, 16);
    for (float& v : f.sticks) {
        if (!std::isfinite(v)) v = 0.0f;       // never a NaN into the game
        v = std::fmax(-1.0f, std::fmin(1.0f, v));
    }
    return f;
}

// A batch: [u8 count] then count x ([u8 repeat][frame]); repeat = how many
// game frames in a row had this frame (1..255).
inline std::vector<std::uint8_t> EncodeBatch(const std::vector<Frame>& frames) {
    std::vector<std::uint8_t> out(1, 0);
    std::size_t i = 0;
    while (i < frames.size() && out[0] < 255) {
        std::size_t run = 1;
        while (frames[i].Level() && i + run < frames.size() && run < 255 && frames[i + run] == frames[i]) {
            ++run;
        }
        const std::size_t at = out.size();
        out.resize(at + 1 + kFrameBytes);
        out[at] = static_cast<std::uint8_t>(run);
        Encode(frames[i], out.data() + at + 1);
        ++out[0];
        i += run;
    }
    return out;
}

// std::nullopt: not a batch (too short, a run of 0).
inline std::optional<std::vector<Frame>> DecodeBatch(const std::uint8_t* in, std::size_t size) {
    if (size < 1 || size != 1 + std::size_t{in[0]} * (1 + kFrameBytes)) {
        return std::nullopt;
    }
    std::vector<Frame> frames;
    for (std::size_t n = 0; n < in[0]; ++n) {
        const std::uint8_t* at = in + 1 + n * (1 + kFrameBytes);
        if (at[0] == 0) {
            return std::nullopt;
        }
        frames.insert(frames.end(), at[0], Decode(at + 1));
    }
    return frames;
}

// One remote player's frames, from the network thread to the game thread that
// runs that player's Link. The two games do not run at the same rate: with
// nothing waiting the last frame's level part is given again (what is held
// stays held, nothing is pressed anew); with too many waiting two frames
// become one (edges kept), so the Link never lags further than kMaxWaiting.
class Queue {
public:
    static constexpr std::size_t kMaxWaiting = 4;

    void Push(const Frame& f) {
        std::lock_guard lock(mutex_);
        frames_.push_back(f);
        while (frames_.size() > kMaxWaiting) {
            Frame a = frames_.front();
            frames_.pop_front();
            Frame& b = frames_.front();  // the newer one keeps its level part
            b.edge_a |= a.edge_a;
            b.edge_b |= a.edge_b;
            ++merged_;
        }
    }
    // The frame for this game frame of the Link (called once a frame, at the buttons' read).
    Frame Next() {
        std::lock_guard lock(mutex_);
        if (!frames_.empty()) {
            current_ = frames_.front();
            frames_.pop_front();
        } else {
            current_.edge_a = 0;
            current_.edge_b = 0;
            ++repeated_;
        }
        return current_;
    }
    Frame Current() const {  // the frame Next() gave last (the sticks' read of the same game frame)
        std::lock_guard lock(mutex_);
        return current_;
    }
    void Clear() {
        std::lock_guard lock(mutex_);
        frames_.clear();
        current_ = Frame{};
    }
    std::size_t Waiting() const {
        std::lock_guard lock(mutex_);
        return frames_.size();
    }
    std::uint64_t merged() const { return merged_.load(std::memory_order_relaxed); }
    std::uint64_t repeated() const { return repeated_.load(std::memory_order_relaxed); }

private:
    mutable std::mutex mutex_;
    std::deque<Frame> frames_;
    Frame current_;
    std::atomic<std::uint64_t> merged_{0};
    std::atomic<std::uint64_t> repeated_{0};
};

// Guest memory for the wraps.
struct Guest {
    std::function<bool(std::uint64_t, void*, std::size_t)> read;
    std::function<bool(std::uint64_t, const void*, std::size_t)> write;
};

// What the three wraps do. One object for the game; any game thread.
class Wraps {
public:
    static constexpr std::size_t kMaxPlayers = 16;  // player numbers 1..16

    void SetMainBase(std::uint64_t main_base) { main_base_ = main_base; }  // before the first wrap
    // Which actor is player `number`'s Link here (0: none). The calc thread.
    void SetRemote(std::uint8_t number, std::uint64_t actor) {
        if (number >= 1 && number <= kMaxPlayers) {
            if (actors_[number].exchange(actor, std::memory_order_relaxed) != actor) {
                queues_[number].Clear();
            }
        }
    }
    // That player's frames, from the network thread.
    void Receive(std::uint8_t number, const std::vector<Frame>& frames, float turn_radians = 0.0f) {
        if (number < 1 || number > kMaxPlayers) return;
        for (Frame f : frames) {
            if (turn_radians != 0.0f) TurnMoveStick(f, turn_radians);
            queues_[number].Push(f);
        }
    }
    // The local Link's frames since the last call, for the network.
    std::vector<Frame> TakeLocal() {
        std::lock_guard lock(local_mutex_);
        std::vector<Frame> out;
        out.swap(local_);
        return out;
    }
    const Queue& QueueOf(std::uint8_t number) const { return queues_[number]; }

    // The module's hos_coop_wrap for ids 16-18: `x` the guest registers (x[19]
    // the caller's node, x[0] the out block at phase 0). Returns 1 when the
    // game's function must not run (phase 0 only).
    int Wrap(std::uint32_t id, int phase, std::uint64_t* x, const Guest& guest) {
        if (id != kWrapButtons && id != kWrapSticks && id != kWrapInputOff) {
            return 0;
        }
        const std::uint64_t actor = ActorOf(x[19], guest);
        const int remote = actor != 0 ? RemoteOf(actor) : 0;
        if (phase == 0) {
            if (id != kWrapInputOff) Out(id) = x[0];
            if (remote == 0) {
                return 0;  // the local Link (or not known): the game's own read
            }
            Queue& q = queues_[static_cast<std::size_t>(remote)];
            if (id == kWrapInputOff) {
                x[0] = 0;  // the local game's menu or event does not stop another player's Link
            } else if (id == kWrapButtons) {
                const Frame f = q.Next();
                guest.write(x[0] + addr::kOutHold, &f.hold, 2);
                guest.write(x[0] + addr::kOutEdgeA, &f.edge_a, 2);
                guest.write(x[0] + addr::kOutEdgeB, &f.edge_b, 1);
            } else {
                const Frame f = q.Current();
                guest.write(x[0] + addr::kOutSticks, f.sticks.data(), 16);
            }
            return 1;
        }
        if (remote != 0 || actor == 0) {
            return 0;
        }
        // Phase 1, the local Link: keep what the game read.
        if (id == kWrapInputOff) {
            // Input off (a menu, an event): the node skips its read, the Link gets nothing, and so
            // do its copies. Both nodes ask; the buttons' node (by its return address) makes the frame.
            if ((x[0] & 1) != 0) {
                {
                    std::lock_guard lock(local_mutex_);
                    local_sticks_ = {};
                }
                if (main_base_ != 0 && x[30] == main_base_ + addr::kButtonsOffReturn) {
                    Local(Frame{});
                }
            }
            return 0;
        }
        std::uint64_t& out = Out(id);
        if (out == 0) {
            return 0;  // a phase 1 without its phase 0 (the module was loaded in between)
        }
        if (id == kWrapSticks) {
            std::array<float, 4> sticks{};
            if (guest.read(out + addr::kOutSticks, sticks.data(), 16)) {
                std::lock_guard lock(local_mutex_);
                local_sticks_ = sticks;
            }
        } else {
            Frame f;
            if (guest.read(out + addr::kOutHold, &f.hold, 2) && guest.read(out + addr::kOutEdgeA, &f.edge_a, 2) &&
                guest.read(out + addr::kOutEdgeB, &f.edge_b, 1)) {
                Local(f);
            }
        }
        out = 0;
        return 0;
    }

    static std::uint64_t ActorOf(std::uint64_t node, const Guest& guest) {
        std::uint64_t owner = 0, holder = 0, actor = 0;
        if (node == 0 || !guest.read(node + addr::kNodeOwner, &owner, 8) || owner == 0 ||
            !guest.read(owner + addr::kOwnerHolder, &holder, 8) || holder == 0 ||
            !guest.read(holder + addr::kHolderActor, &actor, 8)) {
            return 0;
        }
        return actor;
    }

private:
    int RemoteOf(std::uint64_t actor) const {
        for (std::size_t n = 1; n <= kMaxPlayers; ++n) {
            if (actors_[n].load(std::memory_order_relaxed) == actor) return static_cast<int>(n);
        }
        return 0;
    }
    static std::uint64_t& Out(std::uint32_t id) {
        thread_local std::uint64_t out[2] = {0, 0};
        return out[id == kWrapSticks ? 1 : 0];
    }
    // One frame for each buttons' read (or input-off) of the local Link, with the newest sticks.
    void Local(Frame f) {
        std::lock_guard lock(local_mutex_);
        f.sticks = local_sticks_;
        if (local_.size() < kLocalMax) {
            local_.push_back(f);
        }
    }

    static constexpr std::size_t kLocalMax = 512;  // nobody takes them (not connected): no growth
    std::array<std::atomic<std::uint64_t>, kMaxPlayers + 1> actors_{};
    std::array<Queue, kMaxPlayers + 1> queues_;
    std::mutex local_mutex_;
    std::vector<Frame> local_;
    std::array<float, 4> local_sticks_{};
    std::uint64_t main_base_ = 0;
};

}  // namespace CoopPlayerInput
