#pragma once

// One co-op session in the emulator: ties the client, the sampler, the shared
// flags and presence together, so the emulator calls two things, one on each
// of its threads (docs/host.md, "Session").
//
// Threads, the same shape as lichii/hook/coop_hook.cpp:
// - the emulator thread calls frame() once per emulated frame. It is the only
//   thread that reads or writes the game: the sampler reads the player, the
//   flag watcher reads and writes flags. What it builds is queued.
// - the network thread calls service() (and wait() between rounds). It is the
//   only thread that uses the client: it sends what frame() queued, runs the
//   client, and hands what came in to presence (at once) and to the flag
//   watcher (queued for the next frame()).
// - any thread may call with_client() and with_presence() to look in.
// Lock order: the network lock, then the presence lock. frame() never holds
// both at once and never holds either while it touches the game.
//
// The client is borrowed: once it is given to a Session, use it only through
// service() and with_client(), and keep it alive as long as the Session.
//
// How the flags move:
// - the game's own changes (the watcher's FLAG_SET or FLAG_STATE) go out
//   through Client::set_flags, which sends FLAG_SET, or FLAG_STATE when the
//   client hosts, and sends unanswered ones again when the host changes;
// - every `flag_state` event (the host's answer, or as host the client's own
//   answer to a FLAG_SET and the server's value on a clash) and every
//   FLAG_SNAPSHOT part goes to the watcher, which writes it into the game;
// - `host_changed`: the watcher takes the new role, and when we became host
//   it reads the whole list from the game and sends it with
//   Client::send_flag_snapshot;
// - WELCOME (first or again): the sampler and the watcher reset, so the
//   status goes again and unanswered changes are sent again.
// While not connected frame() reads nothing and sends nothing; changes the
// game made meanwhile are found at the first connected frame.

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <variant>
#include <vector>

#include "coop/host/flag_watcher.hpp"
#include "coop/host/flags.hpp"
#include "coop/host/game_memory.hpp"
#include "coop/host/player_reader.hpp"
#include "coop/host/sampler.hpp"
#include "coop/net/client.hpp"
#include "coop/presence/presence.hpp"
#include "coop/protocol/messages.hpp"

namespace coop::host {

struct SessionSettings {
  SamplerSettings sampler;
  presence::Settings presence;
  // The flags this session shares (SharedFlags::load). Nothing: flags are
  // not shared, and flag messages are ignored.
  std::optional<SharedFlags> shared;
  // FlagWatcher::set_learn_pickups: take another game's 64-bit key set to 1
  // as a one-time pickup even when this game has not seen its actor.
  bool learn_pickups = false;
  // Called on the network thread, under the network lock, for every client
  // event after the session has handled it (for logging). It may use the
  // client it is given but must not call into the Session.
  std::function<void(const net::Event&, net::Client&)> on_event;
};

// What one frame() did.
struct FrameReport {
  bool connected = false;               // welcomed and not dropped
  std::uint64_t server_now_us = 0;      // the client's estimate; 0 when not connected
  bool hosting = false;                 // the watcher's role is host
  bool holding = false;                 // a snapshot is coming in parts: hold the game
  std::size_t queued = 0;               // messages and flag lists queued to send
};

class Session {
 public:
  explicit Session(net::Client& client, SessionSettings settings = {});
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // Emulator thread, once per emulated frame. `reading` is this frame's
  // PlayerReader::read() (nothing when the game is not the right one);
  // `flags` the game's flags, or null when they cannot be reached this frame
  // (then incoming flag messages wait).
  FrameReport frame(const std::optional<PlayerReading>& reading, FlagStore* flags);
  FrameReport frame(GameMemory& memory, FlagStore* flags) {
    PlayerReader reader(memory, &drawn_state_);
    return frame(reader.read(), flags);
  }

  // Emulator thread: the game process changed (a new game, or the same game
  // started again). The next connected frame() sends the status again and
  // starts velocity afresh, as after a WELCOME; the connection is kept.
  void reset() { reset_sampler_ = true; }

  // Network thread: sends what frame() queued, runs the client once without
  // waiting, and handles every event that came.
  void service();
  // Network thread, between rounds: waits up to `max_ms` or until frame()
  // queued something or wake() was called.
  void wait(std::uint32_t max_ms);
  // Ends a wait() early (to stop the network thread).
  void wake();

  // Runs `f(client)` under the network lock. Any thread.
  template <typename F>
  decltype(auto) with_client(F&& f) {
    std::lock_guard lock(net_mutex_);
    return f(client_);
  }
  // Runs `f(presence)` under the presence lock. Any thread; frame() does not
  // take it, so the emulator thread can use this to draw (Presence::shown
  // and coop::overlay::collect change it, so it is not const).
  template <typename F>
  decltype(auto) with_presence(F&& f) {
    std::lock_guard lock(presence_mutex_);
    return f(presence_);
  }

  // Emulator thread only, like frame(): a one-time pickup's 64-bit key seen
  // loaded in the game (FlagWatcher::share_pickup). False when flags are not
  // shared or it was known.
  bool share_pickup(FlagKey key) { return watcher_ && watcher_->share_pickup(key); }
  // Emulator thread only: FlagWatcher::take_written_for_others (empty when
  // flags are not shared).
  std::vector<FlagKey> take_written_for_others() {
    return watcher_ ? watcher_->take_written_for_others() : std::vector<FlagKey>{};
  }
  // Emulator thread only: FlagWatcher::take_own_count_drops.
  std::vector<FlagKey> take_own_count_drops() {
    return watcher_ ? watcher_->take_own_count_drops() : std::vector<FlagKey>{};
  }

  // Emulator thread only (the parts frame() uses), for tests and the overlay.
  const Sampler& sampler() const { return sampler_; }
  // Null when no flags are shared.
  const FlagWatcher* flags() const { return watcher_ ? &*watcher_ : nullptr; }

 private:
  // From the network thread to the next frame().
  enum class Control : std::uint8_t { reset, become_host, become_player };
  using Inbound = std::variant<protocol::Message, Control>;
  // From frame() to the network thread.
  struct FlagChanges {
    std::vector<protocol::FlagEntry> flags;
  };
  struct FlagTable {
    std::vector<protocol::FlagEntry> flags;
  };
  using Outbound = std::variant<protocol::Message, FlagChanges, FlagTable>;

  void handle(const net::Event& event);  // network thread, under net_mutex_
  void send(Outbound& out);              // network thread, under net_mutex_
  void queue_flags(const std::vector<Outgoing>& out, std::vector<Outbound>& to);

  // Emulator thread only.
  Sampler sampler_;
  std::optional<FlagWatcher> watcher_;
  bool reset_sampler_ = false;
  PlayerReader::DrawnState drawn_state_{};  // frame(GameMemory&)'s readers share it

  // Guarded by net_mutex_.
  std::mutex net_mutex_;
  std::condition_variable wake_;
  bool woken_ = false;
  net::Client& client_;
  std::vector<Outbound> outbox_;
  std::vector<Inbound> inbox_;
  std::function<void(const net::Event&, net::Client&)> on_event_;
  bool sharing_ = false;  // set once, before any thread starts

  // Guarded by presence_mutex_.
  std::mutex presence_mutex_;
  presence::Presence presence_;
};

}  // namespace coop::host
