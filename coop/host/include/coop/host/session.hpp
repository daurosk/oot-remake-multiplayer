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

#include <atomic>
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
  // The steady clock the flag watcher counts its frames on (issue 106,
  // GameTicks). Empty: one frame() is one frame (tests).
  GameTicks::Clock clock;
  // Called on the network thread, under the network lock, for every client
  // event after the session has handled it (for logging). It may use the
  // client it is given but must not call into the Session.
  std::function<void(const net::Event&, net::Client&)> on_event;
  // jitter-b: how long ago (steady us) the game ran the frame whose state frame() reads (0: not
  // known). The player's samples are stamped with that frame's time, not this call's, so a
  // reading taken some time after its frame gives no uneven step on the other screens.
  // Empty (tests, the emulator): stamped now, as before.
  std::function<std::uint64_t()> game_frame_age_us;
};

// What one frame() did.
struct FrameReport {
  bool connected = false;               // welcomed and not dropped
  std::uint64_t server_now_us = 0;      // the client's estimate; 0 when not connected
  bool hosting = false;                 // the watcher's role is host
  bool holding = false;                 // a snapshot is coming in parts: hold the game
  bool race = false;                    // race mode: nothing of the world is shared (coop/protocol/race.hpp)
  // Rows 106/107: a held snapshot was dropped this frame (FlagWatcher::kHoldMaxFrames): the hook says so once.
  bool hold_timeout = false;
  // How long (1/60 s) the watcher's load window has been open; 0 when closed. Our own changes are not sent
  // while it is: the hook logs one that stays open for a minute (rows 106/107).
  std::uint64_t window_for = 0;
  std::size_t queued = 0;               // messages and flag lists queued to send
  // The host's save load reached this game this frame (FlagWatcher::
  // take_reset): drop session-wide state the save undoes (docs/flags.md, "Saves").
  bool host_reset = false;
  // Values other players' changes wrote into our game this frame (the chat
  // notices, coop/host/notify.hpp).
  std::vector<FlagWatcher::Received> received;
  // A save load was seen this frame (FlagWatcher::take_save_load): distinct
  // flags gone back, frames with no scene. 0 when not.
  bool save_load = false;  // (save_load_flags may be 0: a load through the title, #96)
  int save_load_flags = 0;
  int save_load_frames = 0;
  bool save_load_native = false;  // said by the game's own load nodes (patch 0024), not the old signs
  bool save_load_retry = false;   // Game Over's Try Again (row 159: the host sends no BOSS_ALIVE for it)
};

class Session {
 public:
  explicit Session(net::Client& client, SessionSettings settings = {});
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // Emulator thread, before frame(): whether a scene (Banc) is loaded now
  // (PlayerReader::read_scene). Its absence for a while in a load is the
  // title screen, the sign of a save load (FlagWatcher::set_scene).
  void set_scene_readable(bool readable) { scene_readable_ = readable; }
  // Emulator thread, before frame(): Hookshot's title menu is up now
  // (FlagWatcher::set_title; passed on also while not connected). Never set
  // on the emulator.
  void set_title_up(bool up) { title_up_ = up; }
  // Emulator thread, before frame(): Hookshot's save-load wraps (patch 0024; FlagWatcher::set_native_loads,
  // note_save_load_start, note_save_loaded). Passed on also while not connected: a load then is taken at the
  // connection's first frame. Never set on the emulator.
  void set_native_save_loads(bool on) {
    if (watcher_) watcher_->set_native_loads(on);
  }
  void note_save_load_start() {
    if (watcher_) watcher_->note_save_load_start();
  }
  void note_save_loaded(bool retry = false) {
    if (watcher_) watcher_->note_save_loaded(retry);
  }

  // Emulator thread, once per emulated frame. `reading` is this frame's
  // PlayerReader::read() (nothing when the game is not the right one);
  // `flags` the game's flags, or null when they cannot be reached this frame
  // (then incoming flag messages wait).
  FrameReport frame(const std::optional<PlayerReading>& reading, FlagStore* flags);
  FrameReport frame(GameMemory& memory, FlagStore* flags) {
    PlayerReader reader(memory, &drawn_state_);
    scene_readable_ = reader.read_scene().has_value();
    return frame(reader.read(), flags);
  }

  // Emulator thread: the game process changed (a new game, or the same game
  // started again). The next connected frame() sends the status again and
  // starts velocity afresh, as after a WELCOME; the connection is kept.
  void reset() { reset_sampler_ = true; }
  // Emulator thread: the player's colour changed (Sampler::set_color).
  void set_color(std::optional<std::uint32_t> color) {
    std::lock_guard lock(sampler_mutex_);
    sampler_.set_color(color);
  }
  // Any thread: the game is held now (the play-test bridge's hold: no frame()
  // comes while it lasts). service() then sends the last sample again each
  // keep-alive time (Sampler::hold), so the others keep a still puppet of us.
  void set_held(bool held) { held_.store(held, std::memory_order_relaxed); }

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
  // FlagWatcher::keep_local. Emulator thread only, like share_pickup. 0 when flags are not shared.
  std::size_t keep_local(std::span<const FlagKey> keys) { return watcher_ ? watcher_->keep_local(keys) : 0; }
  // FlagWatcher::share_one_way. Emulator thread only, like share_pickup.
  bool share_one_way(FlagKey key) { return watcher_ && watcher_->share_one_way(key); }
  // FlagWatcher::share_down_only. Emulator thread only, like share_pickup.
  bool share_down_only(FlagKey key) { return watcher_ && watcher_->share_down_only(key); }
  // FlagWatcher::share_up_only. Emulator thread only, like share_pickup.
  bool share_up_only(FlagKey key) { return watcher_ && watcher_->share_up_only(key); }
  // The game's own flag writes as events (FlagWatcher::use_events,
  // game_wrote, game_wrote_all). Emulator thread only, before frame().
  void flag_events(bool on) {
    if (watcher_ && watcher_->uses_events() != on) watcher_->use_events(on);
  }
  void flag_wrote(FlagKey key) {
    if (watcher_) watcher_->game_wrote(key);
  }
  void flag_wrote_all() {
    if (watcher_) watcher_->game_wrote_all();
  }
  // The flag watcher's counters (all 0 when flags are not shared).
  FlagStats flag_stats() const { return watcher_ ? watcher_->stats() : FlagStats{}; }
  // FlagWatcher::share_pickup_named (a named Bool, the pickup rule).
  bool share_pickup_named(FlagKey key) { return watcher_ && watcher_->share_pickup_named(key); }
  // Emulator thread only: FlagWatcher::share_containers (row 218, heart containers add up).
  bool share_containers(FlagKey key) { return watcher_ && watcher_->share_containers(key); }
  // Emulator thread only: FlagWatcher::group_int (row 218: the group's maximum life as we know it).
  std::optional<std::int32_t> group_int(FlagKey key) const {
    return watcher_ ? watcher_->group_int(key) : std::nullopt;
  }
  // Emulator thread only: FlagWatcher::note_group_at_least (row 218, the boss's one container).
  void note_group_at_least(FlagKey key, std::int32_t value) {
    if (watcher_) watcher_->note_group_at_least(key, value);
  }
  // Emulator thread only: FlagWatcher::take_written_for_others (empty when
  // flags are not shared).
  std::vector<FlagWatcher::OneWayRestore> take_one_way_restores() {
    return watcher_ ? watcher_->take_one_way_restores() : std::vector<FlagWatcher::OneWayRestore>{};
  }
  // Row 140: FlagWatcher::take_one_way_left / take_keeps_failing (for the log; empty when flags are not shared).
  std::vector<FlagKey> take_one_way_left() {
    return watcher_ ? watcher_->take_one_way_left() : std::vector<FlagKey>{};
  }
  // Row 140: FlagWatcher::take_hud_refresh (false when flags are not shared).
  bool take_one_way_hud_refresh() { return watcher_ && watcher_->take_hud_refresh(); }
  std::vector<FlagWatcher::KeepsFailing> take_keeps_failing() {
    return watcher_ ? watcher_->take_keeps_failing() : std::vector<FlagWatcher::KeepsFailing>{};
  }
  // How many times a one-way Bool was put back this session (for the log).
  std::uint64_t one_way_restores() const {
    return watcher_ ? watcher_->stats().one_way_restores : 0;
  }
  std::vector<FlagKey> take_written_for_others() {
    return watcher_ ? watcher_->take_written_for_others() : std::vector<FlagKey>{};
  }
  // Emulator thread only: FlagWatcher::take_own_count_drops.
  std::vector<FlagKey> take_own_count_drops() {
    return watcher_ ? watcher_->take_own_count_drops() : std::vector<FlagKey>{};
  }
  // Emulator thread only: FlagWatcher::guard_count (issue 89, the Gold
  // Skulltula tokens). False when flags are not shared.
  bool guard_count(FlagKey count, std::span<const FlagKey> sources) {
    if (!watcher_) return false;
    watcher_->guard_count(count, sources);
    return true;
  }
  // Emulator thread only: FlagWatcher::set_tokens_anywhere (row 331: the seed places a token off a Skulltula).
  void set_tokens_anywhere(bool on) {
    if (watcher_) watcher_->set_tokens_anywhere(on);
  }
  // Emulator thread only: FlagWatcher::guard_reward (row 272, a once-per-save
  // reward got in two games at once). False when flags are not shared.
  bool guard_reward(std::span<const std::vector<FlagKey>> units, std::span<const FlagKey> sources) {
    if (!watcher_) return false;
    watcher_->guard_reward(units, sources);
    return true;
  }
  // Emulator thread only: FlagWatcher::set_reward_self (row 272).
  void set_reward_self(std::uint8_t number) {
    if (watcher_) watcher_->set_reward_self(number);
  }
  // Emulator thread only: FlagWatcher::take_rewards_guarded.
  std::vector<FlagWatcher::RewardGuarded> take_rewards_guarded() {
    return watcher_ ? watcher_->take_rewards_guarded() : std::vector<FlagWatcher::RewardGuarded>{};
  }
  // Emulator thread only: FlagWatcher::expect_doubled_gain (issue 195, a
  // chest opened again here). False when flags are not shared.
  bool expect_doubled_gain() {
    if (!watcher_) return false;
    watcher_->expect_doubled_gain();
    return true;
  }
  // Emulator thread only: FlagWatcher::end_doubled_gain.
  void end_doubled_gain() {
    if (watcher_) watcher_->end_doubled_gain();
  }
  // Emulator thread only: FlagWatcher::cap_max / clear_max_cap (a boss's one
  // heart container). False when flags are not shared.
  bool cap_max(FlagKey key, std::optional<std::int32_t> cap) {
    if (!watcher_) return false;
    if (cap) {
      watcher_->cap_max(key, *cap);
    } else {
      watcher_->clear_max_cap(key);
    }
    return true;
  }
  // Emulator thread only: FlagWatcher::reset_lowered (rows 136/137).
  std::optional<std::int32_t> reset_lowered(FlagKey key) const {
    return watcher_ ? watcher_->reset_lowered(key) : std::nullopt;
  }
  // Emulator thread only: FlagWatcher::lowered_done (row 238).
  void lowered_done(FlagKey key, std::int32_t seen) {
    if (watcher_) watcher_->lowered_done(key, seen);
  }
  // Emulator thread only: FlagWatcher::held_lower (rows 136/137 and 181).
  std::optional<std::int32_t> held_lower(FlagKey key) const {
    return watcher_ ? watcher_->held_lower(key) : std::nullopt;
  }
  // Emulator thread only: FlagWatcher::take_counts_guarded.
  std::vector<FlagWatcher::CountGuarded> take_counts_guarded() {
    return watcher_ ? watcher_->take_counts_guarded() : std::vector<FlagWatcher::CountGuarded>{};
  }

  // Emulator thread only (the parts frame() uses), for tests and the overlay.
  // While held, service() also sends samples: read the sequence with
  // next_sample_sequence() then.
  const Sampler& sampler() const { return sampler_; }
  // Any thread: Sampler::next_sequence, under the sampler's lock.
  std::uint32_t next_sample_sequence() {
    std::lock_guard lock(sampler_mutex_);
    return sampler_.next_sequence();
  }
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

  // Emulator thread, and the network thread while held (set_held): the
  // sampler under sampler_mutex_ (taken after net_mutex_, never before).
  std::mutex sampler_mutex_;
  Sampler sampler_;
  std::atomic<bool> held_{false};
  // Emulator thread only.
  std::optional<FlagWatcher> watcher_;
  bool reset_sampler_ = false;
  bool scene_readable_ = true;  // set_scene_readable
  bool title_up_ = false;       // set_title_up
  PlayerReader::DrawnState drawn_state_{};  // frame(GameMemory&)'s readers share it

  // Guarded by net_mutex_.
  std::mutex net_mutex_;
  std::condition_variable wake_;
  bool woken_ = false;
  net::Client& client_;
  std::vector<Outbound> outbox_;
  std::vector<Inbound> inbox_;
  std::function<void(const net::Event&, net::Client&)> on_event_;
  std::function<std::uint64_t()> game_frame_age_us_;  // SessionSettings::game_frame_age_us
  bool sharing_ = false;  // set once, before any thread starts
  bool race_ = false;     // the game thread's: race mode seen in this session (frame())
  bool welcomed_held_ = false;  // a WELCOME came while held: service() sends the status again

  // Guarded by presence_mutex_.
  std::mutex presence_mutex_;
  presence::Presence presence_;
};

}  // namespace coop::host
