#pragma once

// Watches the shared flags in the running game, once per emulated frame, and
// moves them as docs/protocol.md ("Shared flags") asks:
//
// - A player's own changes go out as FLAG_SET (host: as FLAG_STATE).
// - FLAG_STATE and FLAG_SNAPSHOT from the server are written into the game,
//   and those writes are not seen as changes, so they are never sent back.
// - A FLAG_SNAPSHOT is held until its last part has come, then written all at
//   once; holding() says so, so the emulator can hold the game meanwhile.
// - A local change is kept until the host's answer for that flag comes: a
//   snapshot does not overwrite it, a FLAG_STATE does.
// - The host answers each FLAG_SET with FLAG_STATE of what came out, and sends
//   its whole table as FLAG_SNAPSHOT when asked.
// - Counts (FlagStore::is_count: small keys, Gold Skulltula tokens, heart
//   pieces) add up: a player sends its change as a count delta; the host's
//   game adds a delta (handed over in a FLAG_STATE event by its client, or
//   in a FLAG_SET) and sends the total as FLAG_STATE. A total is written on
//   top of any change the game made that is not sent yet, so two players
//   gaining at once both count (docs/protocol.md, "Counts").
//
// No sockets: messages come in already received (decoded or as bytes) and go
// out encoded. receive() only queues; every read and write of the game happens
// inside frame(), on the game's thread. Not thread-safe: call both from one
// thread, or lock around them.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <variant>
#include <utility>
#include <vector>

#include "coop/host/flags.hpp"
#include "coop/host/outgoing.hpp"
#include "coop/host/ticks.hpp"
#include "coop/protocol/messages.hpp"

namespace coop::host {

enum class FlagRole : std::uint8_t {
  player,  // sends FLAG_SET, applies FLAG_STATE and FLAG_SNAPSHOT
  host,    // its game is the truth: sends FLAG_STATE, answers FLAG_SET
};

struct FlagStats {
  std::uint64_t changes_sent = 0;      // entries in FLAG_SET or own FLAG_STATE
  std::uint64_t entries_in = 0;        // entries of every flag message that came (forest-temple-b log)
  std::uint64_t answers_sent = 0;      // host: entries answering a FLAG_SET
  std::uint64_t applied = 0;           // entries written into the game
  std::uint64_t deltas_added = 0;      // host: count deltas added to the game (in applied too)
  std::uint64_t kept_local = 0;        // snapshot entries skipped over a pending change
  std::uint64_t snapshot_exact = 0;    // player: join snapshot values written as they are (#96)
  std::uint64_t not_shared = 0;        // incoming entries not in the shared list
  std::uint64_t pickups_learned = 0;   // 64-bit keys taken as pickups from a message
  std::uint64_t pickup_resets = 0;     // a pickup's 0, ours or incoming: not passed on
  std::uint64_t max_drops_kept = 0;    // a max-rule value our game lowered: ours, not sent
  std::uint64_t max_caps_kept = 0;     // a rise of our own past cap_max's cap: put back, not sent
  std::uint64_t max_caps_left = 0;     // row 181: a capped rise left to the game after kMaxCapTries put-backs (not sent)
  std::uint64_t reset_holds_kept = 0;  // rows 136/137: the game's write-back of a value the host's save lowered
  std::uint64_t read_failures = 0;
  std::uint64_t write_failures = 0;    // each failed try; it is tried again next frame
  // A write the store refused because it holds that key for now (HoldFlagStore: a room gimmick flag held until
  // our game played its event or we leave the dungeon) or has no room for it in the game's queue now (busy, issue
  // 175). Waiting by design, so not a write failure: tried again
  // every frame until the hold lets it, which counted ~60 failures a second (events-next-a-11 probe: named
  // 0xb3a4fe84 = HavePlayedEvent.InsideTheDekuTree_Gimmick_FallingLadder, 5561 of 9235 in 90 s).
  std::uint64_t held_retries = 0;
  // The key with the most write failures so far, how many, and where (events-next-a-11: a log said ~60
  // failures a second after an event without saying which flag). Count 0 = none yet.
  FlagKey top_write_failure{};
  std::uint64_t top_write_failure_count = 0;
  const char* top_write_failure_site = "";
  std::uint64_t bad_snapshots = 0;     // parts out of order: the snapshot is dropped
  std::uint64_t ignored_messages = 0;  // not a flag message, or not for this role
  std::uint64_t load_reverts = 0;      // the game's load undid a group value: written back
  std::uint64_t title_loads = 0;       // player: a load through the title, fewer flags back than kSaveLoadMin (#96)
  std::uint64_t native_loads = 0;      // save loads the game's own load nodes said (set_native_loads, patch 0024)
  std::uint64_t old_sign_loads = 0;    // with native loads on: windows the old signs called a save load (cross-check)
  std::uint64_t native_timeouts = 0;   // a load node's start with no save in after kNativeLoadMaxFrames
  std::uint64_t retry_loads = 0;       // of native_loads: Game Over's Try Again (the group's values back, no reset)
  std::uint64_t offline_loads = 0;     // player: the title was up while not connected, our changes put back (#96)
  std::uint64_t one_way_restores = 0;  // a one-way Bool (map, compass) back to 0: written back
  std::uint64_t down_only_kept = 0;    // a group's 1 not written over our 0 (share_down_only)
  std::uint64_t up_only_kept = 0;      // a group's 0 not written over our 1 (share_up_only)
  std::uint64_t host_resets = 0;       // host: FLAG_RESETs sent after its save load
  std::uint64_t reset_applied = 0;     // player: values written from a FLAG_RESET
  std::uint64_t counts_guarded = 0;    // a guarded count's gain taken back (guard_count)
  std::uint64_t doubled_gains = 0;     // a chest opened again here: its gain taken back (expect_doubled_gain)
  std::uint64_t raced_gains = 0;       // host: a player's guarded gain for a source the group had: not added
  std::uint64_t entries_unsent = 0;    // entries a message kind cannot carry: left out, the rest sent
  std::uint64_t event_reads = 0;       // use_events: flags read because the game wrote them
  std::uint64_t event_misses = 0;      // use_events: a change the audit found that no event had named
  // Rows 106/107: the two states in which a player sends none of its own changes (dungeon-items-next).
  std::uint64_t holding_frames = 0;    // 1/60 s held for a join snapshot's last part: diff() off, FLAG_STATE held
  std::uint64_t window_frames = 0;     // 1/60 s in a load window: our own changes not sent (docs/flags.md "Saves")
  std::uint64_t hold_timeouts = 0;     // a snapshot whose last part never came (kHoldMaxFrames): dropped
  std::uint64_t pending_now = 0;       // player: own changes sent and not answered yet (now, not a count)
  std::uint64_t full_reads = 0;        // use_events: frames that read the whole list (a load window, the first)
  // Row 140: a one-way value (a dungeon's floors, a map, a compass) the game lowered again right after our put-back:
  // the game's own value from then on, until the next load window (kMaxQuickUndos).
  std::uint64_t one_way_left = 0;
  // Row 140: of one_way_restores, the values left to the game put back once after a room or scene load (owed_).
  std::uint64_t one_way_owed = 0;
  // New Event spam (world-fails-0115): the table is the host's save's, never its title screen's defaults.
  std::uint64_t table_waits = 0;       // host: a table asked for inside a load window (the title): sent when it ends
  std::uint64_t table_resets = 0;      // host: of those, sent as the whole table in FLAG_RESET after its save load
  std::uint64_t own_save_kept = 0;     // player: a save load with no group values known yet: our save's values kept
};

class FlagWatcher {
 public:
  FlagWatcher(SharedFlags shared, FlagRole role) : shared_(std::move(shared)), role_(role) {}

  // A message from the server. Flag messages are queued for the next frame();
  // anything else, or a message this role does not take (a host takes no
  // FLAG_SNAPSHOT, a player no FLAG_SET), is ignored. Both take FLAG_STATE:
  // a host's client answers FLAG_SETs itself, and its game takes the answer. False when ignored or the
  // bytes do not decode.
  bool receive(const protocol::Message& message);
  bool receive(std::span<const std::uint8_t> bytes);

  // Host: send the whole table as FLAG_SNAPSHOT at the next frame (starting
  // a session, taking over as host). Only the game's own save is a table:
  // inside a load window (the title screen, file select, a save loading, a
  // door) the request waits until the window ends. When a save load ended
  // it, the whole table goes out as FLAG_RESET, so every player takes the
  // host's save exactly; else (a door) as FLAG_SNAPSHOT. A table sent from
  // the title held the game's defaults (quests at 'Ready', no compass): a
  // guest whose save loaded first had them written back over its own, and
  // the host's load raised them again, each quest step showing the game's
  // 'New Event' notice (world-fails-0115).
  void request_snapshot() { snapshot_requested_ = true; }

  // One emulated frame: writes what came in, then compares the shared flags
  // with last frame, and returns the messages to send, in order.
  std::vector<Outgoing> frame(FlagStore& store);

  // One-time pickups (docs/items.md, "One-time pickups"): a Gold Skulltula,
  // its token, a heart container or piece is a 64-bit-key flag keyed by the
  // actor's placement hash, and no list names those keys. The game's hook
  // calls this for each such actor it sees loaded; the key is shared from
  // then on, one way only: 0 to 1 is sent and written, a 1 never goes back
  // to 0 through the session (a key that the game resets on a scene change
  // must not put the pickup back for the others). False: already a pickup.
  bool share_pickup(FlagKey key);
  // A named Bool that the game itself sets back to 0 (a room's own switch
  // state, reset when the room is left: docs/dungeon.md "Live dungeon
  // actions"): shared one way like a pickup (a 1 is sent and written, a 0 is
  // never sent and never written over a 1), and never written back after a
  // load (the room must be able to reset). False: already one way.
  bool share_one_way(FlagKey key);
  // A named Bool the game only ever lowers (issue 84: Mido's blocker, 1 until
  // he lets Link by, never set back to 1 by the game): a 1 from the group
  // (a host's save load, a join snapshot, an answer, a load's write-back) is
  // never written over our game's 0; our 0 goes to the group instead.
  // False: already down-only.
  bool share_down_only(FlagKey key);
  // The same rule the other way: a named Bool the game only ever raises (a
  // gate a story scene opens: the Deku Tree's mouth, the royal tomb, the
  // drained well): a 0 from the group is never written over our game's 1;
  // our 1 goes to the group instead. False: already up-only (or down-only).
  bool share_up_only(FlagKey key);
  // The pickup rule for a named Bool (a chest's opened flag,
  // issue 87: the game records a chest opened in a named Bool of its own,
  // not in the placement's 64-bit key). It is also noted in
  // take_written_for_others when written 0 to 1 for another player. False:
  // already shared one way.
  // (chest-live's; a load's 0 is still written back, unlike share_one_way.)
  bool share_pickup_named(FlagKey key);
  // With learning on, an incoming 64-bit key set to 1 that this game does not
  // share yet is taken as a pickup too (the player who took it was in a scene
  // this game has not loaded). At most kMaxLearned keys. Off by default here;
  // coop_hook.cpp turns it on whenever flags are shared (LICHII_COOP_PICKUPS=0
  // turns it off).
  void set_learn_pickups(bool on) { learn_ = on; }
  static constexpr std::size_t kMaxLearned = 4096;
  std::size_t pickups() const { return pickups_.size(); }

  // The 64-bit keys, and the named Bools shared by the pickup rule (share_pickup_named),
  // this watcher wrote 0 to 1 into the game for another
  // player (a chest opened, a pickup taken elsewhere), oldest first, since
  // the last call; at most kMaxWrittenForOthers are kept. A key the game
  // already had at 1 (opened here, the host's echo of it) is not in it. The
  // hook removes what is loaded of these (docs/items.md, "Chests").
  std::vector<FlagKey> take_written_for_others() { return std::exchange(written_for_others_, {}); }
  // One-way Bools (FlagStore::is_one_way) the game dropped to 0 and diff()
  // wrote back to 1 since the last call, for the log (issue 146). Each key
  // only the first time it is put back; stats().one_way_restores counts all.
  // dungeon-sync-b (#63/#76): with the values (a UInt's floors: before, the game's now, what was written), and
  // `kept` when the game's own value was kept instead: its dungeon's map (game_items map_of_floors) turned on
  // this frame or the one before, so the drop is the game's own map get writing its floors.
  struct OneWayRestore {
    FlagKey key;
    std::uint32_t before = 0;
    std::uint32_t now = 0;
    std::uint32_t put = 0;
    bool kept = false;
  };
  std::vector<OneWayRestore> take_one_way_restores() { return std::exchange(one_way_restored_, {}); }
  // Row 140 (flag-oneway-c): the game itself writes some one-way values from its own copy (the HUD screen's floors,
  // written back to EnteredFloorBit while Link is in the dungeon), so a put-back the game lowers again on the next
  // read is a fight we never win (49,223 put-backs in one host's session). A drop within kUndoPasses diff passes of
  // our last put-back counts as undone; after kMaxQuickUndos of those in a row the key is the game's own until the
  // next load window (set_loading): not put back, not sent (the group keeps its value). One-off drops (issue 146)
  // are still put back.
  static constexpr std::uint64_t kUndoPasses = 2;
  static constexpr int kMaxQuickUndos = 2;
  std::vector<FlagKey> take_one_way_left() { return std::exchange(one_way_left_, {}); }
  // Row 140 (review of flag-oneway-c): the game lowers these from the HUD screen's own copy, which reads the flag in
  // only at its init and refresh, so a put-back must reach that copy or it is lowered again. True when a one-way
  // dungeon value was put back since the last call (every put-back, not only a key's first): the hook asks the
  // game's own HUD refresh on the same frame (CoopMapRefresh WantNow). A value left to the game is put back once more
  // when the next room or scene load window ends (not after a save load: another save's floors are not this one's),
  // with that refresh, so the other player's floors reach the copy after a room load at the latest.
  bool take_hud_refresh() { return std::exchange(hud_refresh_, false); }
  // Row 140: a key whose write failed kFailureSayAt times this session (note_write_failure; once per key, for the log).
  struct KeepsFailing {
    FlagKey key;
    const char* site = "";
    std::uint64_t count = 0;
  };
  static constexpr std::uint64_t kFailureSayAt = 60;
  std::vector<KeepsFailing> take_keeps_failing() { return std::exchange(keeps_failing_, {}); }
  // Row 140: a max-rule rise past its cap whose put-back the game refused this often in a row is left (not sent,
  // not fought every frame) until the value changes again. Row 181: put-backs the game undid count too (cap_max).
  static constexpr int kMaxCapTries = 60;
  static constexpr std::size_t kMaxWrittenForOthers = 1024;

  // The counts (small keys, ...) our game itself lowered since the last call,
  // one entry per drop, oldest first: what the game did, never a total this
  // watcher wrote in (another player's spend). The hook gives a lock's second
  // key back only against one of these (docs/dungeon.md, "Locked doors").
  std::vector<FlagKey> take_own_count_drops() { return std::exchange(own_count_drops_, {}); }
  static constexpr std::size_t kMaxOwnCountDrops = 64;

  // A count that only one-time pickups raise (issue 89: the Gold Skulltula
  // tokens, raised only by a Skulltula's token, whose taking sets that
  // Skulltula's 64-bit key in the same scan). `sources` are shared as pickups
  // (share_pickup). Then a gain of `count` in our own game is sent only
  // against one of `sources` going 0 to 1 in our own game within
  // kGuardHoldFrames before or after it. A gain with none, while one of
  // `sources` was written 0 to 1 here for another player within the last
  // kGuardRecentFrames, is our game counting again what another player took
  // (the token stayed here): it is put back and not sent. A gain with none and
  // no such take (a cheat, or a source we do not know) is sent as before, once
  // the hold is over. A drop of our own while a gain is held, or just after
  // one was put back, cancels against it: the world sync gave that same token
  // back (docs/world.md, "both at once"). docs/items.md, "Gold Skulltulas".
  // As host: a player's FLAG_SET with one of `sources` at 1 that our game
  // (the group) had at 1 already is a token taken twice (both games took it
  // before either heard of the other): that player's next gain of `count`
  // within kRaceFrames is not added (one per such source), and our total is
  // stated again, so every game counts the token once.
  void guard_count(FlagKey count, std::span<const FlagKey> sources);
  static constexpr std::uint64_t kGuardHoldFrames = 90;
  static constexpr std::uint64_t kRaceFrames = 2 * kGuardHoldFrames;
  static constexpr std::uint64_t kGuardRecentFrames = 5 * 60 * 60;
  struct CountGuarded {
    FlagKey count;
    std::int32_t amount = 0;  // the gain put back
  };
  // The gains guard_count put back since the last call, for the log.
  std::vector<CountGuarded> take_counts_guarded() { return std::exchange(counts_guarded_, {}); }

  // Issue 195: our Link opened a chest that another player had opened
  // already (the hook shows it open, but its AI still lets it open again).
  // Its content was given in the other game and counted for the group, so
  // the next gain of a count (a small key, a heart piece; never a guarded
  // count, guard_count: no chest holds a token) in our own game within
  // kDoubledGainFrames is put back and not sent (take_counts_guarded reports
  // it). One call drops one gain. There is no table of what a chest holds,
  // so the caller bounds it to that chest's own get scene: the hook calls
  // end_doubled_gain() when our Link's event ends (or never started), and
  // kDoubledGainFrames is only a cap. Inside the scene our Link is held, so
  // the only count gain there is the chest's own (none for a map, compass
  // or rupees, and then nothing is taken).
  void expect_doubled_gain();
  // The reopened chest's get scene is over: nothing more is taken back.
  void end_doubled_gain() { doubled_ = 0; }

  // Boss cutscenes (lichii/hook/coop_boss_scenes.h, one heart container for
  // all): while a cap is set for a max-rule int, a rise of our own game's
  // value past `cap` is put back (to the higher of the value before and the
  // cap) and never sent, so a second pickup of the boss's one container
  // cannot raise the group through the max rule. Values from the group are
  // written as before, and a rise up to the cap is sent as any change. One
  // cap per key; a new call replaces it.
  // Row 181: MaxLife has a live copy (Link's Life component writes its own
  // max back into MaxLife), so a put-back alone was undone by the game at
  // once, put back again, and so on for ever: the container's item get waited
  // on the hearts and the heart-increase sound played on every pass (a
  // soft-lock). A put-back now starts a cap hold: held_lower() names the
  // value, the object walk lowers the live copy to it, and the game keeps it.
  // Every put-back counts (also one the game undid): after kMaxCapTries in
  // one hold the rise is left to the game (still never sent). The hold ends
  // once the game has kept the value for kResetQuietFrames, when the value
  // moves with no put-back (the group's value came), when the cap goes or
  // rises past it, and at the end of a load window or the session.
  void cap_max(FlagKey key, std::int32_t cap) {
    max_caps_[key] = cap;
    if (const auto h = cap_holds_.find(key); h != cap_holds_.end() && cap > h->second.low) cap_holds_.erase(h);
  }
  void clear_max_cap(FlagKey key) {
    max_caps_.erase(key);
    cap_holds_.erase(key);
  }

  // Rows 136/137 (boss-away-0115): the host's save load (FLAG_RESET) lowered a
  // max-rule int in our game (PlayerStatus.MaxLife after a boss's heart
  // container, a magic meter, a bag...). Some of them the game keeps a live
  // copy of and writes back (Link's Life component writes its own max into
  // MaxLife while he is in the scene): that rise is the replaced save's, not
  // progress. While the hold lasts, a rise of our game's value up to what it
  // had before the reset is put back to the reset value and never sent (else
  // the max rule raised the whole group again while the container's own
  // Heart_Container_Obtained went back to 0: the container repeats). A rise
  // past that is real progress: sent, the hold ends. The hold ends once the
  // value has stayed at the reset value for kResetQuietFrames (the game took
  // it), or when a load window ends (the game makes Link from the flags), or
  // when the session ends. The reset value while a hold lasts, else nothing
  // (the object walk lowers Link's own max to it).
  std::optional<std::int32_t> reset_lowered(FlagKey key) const {
    const auto it = reset_holds_.find(key);
    return it == reset_holds_.end() ? std::nullopt : std::optional<std::int32_t>(it->second.low);
  }
  // What a max-rule int's live copy is to be lowered to now: a reset hold's
  // value (reset_lowered), else a cap hold's (cap_max, row 181), else nothing
  // (the usual case, and always with co-op idle: no session, no watcher).
  std::optional<std::int32_t> held_lower(FlagKey key) const {
    if (const auto r = reset_lowered(key)) return r;
    const auto it = cap_holds_.find(key);
    return it == cap_holds_.end() ? std::nullopt : std::optional<std::int32_t>(it->second.low);
  }
  static constexpr int kResetQuietFrames = 60;
  static constexpr std::uint64_t kDoubledGainFrames = 5 * 60;
  static constexpr std::int32_t kMaxDoubledGains = 4;

  // The host left and this player takes over, or the other way round. Keeps
  // what is known of the game; drops a snapshot being held.
  void set_role(FlagRole role);
  FlagRole role() const { return role_; }

  // The game is loading (no Link to read: the title screen, a save being
  // loaded, a scene change). A load writes the save's own values over the
  // flags, also over ones the group set just before (issue 129: a switch
  // shared by the snapshot was reset by the joiner's save load, sent back as
  // 0, and the group's later 1 was written while the load ran and lost).
  // Saves (Alex, 2026-11-11, docs/flags.md "Saves"): the host's save is the
  // truth. While there is no Link nothing is sent. A save was loaded when,
  // in one window, no scene was loaded for kTitleFrames (the title screen
  // or file select) and kSaveLoadMin named flags went back (a Bool to 0, an
  // Int lower) with no Link. A door, warp, cutscene, time travel or death
  // keeps a scene loaded, and its room "enter" flags do not count then. Then nothing is sent until the window ends, when
  // the host sends what its load changed as FLAG_RESET, which replaces the
  // group's values, and a player's game takes the group's values back for
  // what its load changed (the group is not changed). One-way room flags
  // the game resets itself are never part of it. Without that sign, the
  // rules below hold as before.
  // Also, for a player, while loading and kSettleFrames after, a shared flag that the game moves
  // away from the group's value backwards (a Bool from 1 to 0; while
  // loading only, not in the settle time, an Int back to exactly the value
  // it had before the group's value came; counts are left to their delta
  // rule) is not sent: the group's value is written back. A Bool going to 1,
  // or an Int to any other value, is the game's own change and is sent.
  void set_loading(bool loading);

  // A shared value another game's change wrote into ours (the chat notices,
  // coop/host/notify.hpp): the flag, its value before and after, who made
  // the change (the asker named by FLAG_ORIGIN, else the host that sent it;
  // 0 when not known) and how it came.
  enum class ReceivedKind { live, join, reset };
  struct Received {
    FlagKey key;
    protocol::FlagEntry before;
    protocol::FlagEntry after;
    std::uint8_t from = 0;
    ReceivedKind kind = ReceivedKind::live;
  };
  // What was received since the last call (at most kMaxReceived kept).
  std::vector<Received> take_received() { return std::exchange(received_, {}); }
  static constexpr std::size_t kMaxReceived = 4096;
  // A FLAG_ORIGIN names its asker for this many frames.
  static constexpr std::uint64_t kOriginFrames = 600;
  // Whether a scene (Banc) is loaded this frame. None for kTitleFrames in a
  // load window means the title screen or file select: the only way to a
  // save load (docs/flags.md, "Saves"). Default: loaded.
  void set_scene(bool readable) { scene_readable_ = readable; }
  // Hookshot's title menu is up (coop_title.cpp TitleUp), every frame, also
  // while not connected (frame() does not run then). A player's load window
  // with the title in it is a save load even with no flag back: a guest's own
  // richer save no longer sends its items into the host's file (player report
  // #96). A title while not connected puts back, at the next connection's
  // first frame with Link, what our game changed meanwhile. Never set on the
  // emulator (the old rule holds there). A host's load window with the title
  // in it is a save load too (forest-temple-a): its FLAG_RESET goes out.
  void set_title(bool up) {
    title_up_ = up;
    if (up) title_seen_ = true;
  }
  // Hookshot's save-load wraps (patch 0024, hos_coop_save_load.h): the game's own sequence nodes that load a save
  // (SeqExecuteLoadSaveData / SeqExecuteLoadTemporarySave's Enter) and say it is in
  // (SeqOneShotOnloadGameSaveDataWorldMgr): every save the game loads, from the title, the System menu's Load and
  // Game Over's Try Again. With them on, a save load is what they say and nothing else: from a start until the
  // save is in, the game is loading (Link read or not), and the save in is the save load (moved_: a host's
  // FLAG_RESET, a player's group values back). The old signs (no scene and flags back, the title menu) are only
  // counted then (stats().old_sign_loads), as a cross-check in the log. Off (the emulator, an older Hookshot): the
  // old signs decide, as before.
  void set_native_loads(bool on) { native_loads_ = on; }
  bool native_loads() const { return native_loads_; }
  void note_save_load_start() {
    native_loading_ = true;
    native_since_ = frames_;
  }
  // `retry`: Game Over's Try Again (patch 0024's Game Over mark, no title since). Not a save the player chose: as
  // host too, the group's values go back into our game and no FLAG_RESET goes out, so a host's death never takes
  // the group's unsaved progress (coordinator A's decision, 2026-11-16). A guest's load does that anyway.
  void note_save_loaded(bool retry = false) {
    native_loaded_ = true;
    native_retry_ = native_retry_ || retry;
  }
  // A start with no save in after this long (a load the game gave up): loading no more.
  static constexpr std::uint64_t kNativeLoadMaxFrames = 30 * 60;
  // A join snapshot whose last part has not come after this long is dropped as an out-of-order part is
  // (rows 106/107): held, a player sends none of its own changes and writes no FLAG_STATE. The server sends
  // every part at once on the reliable progress channel, so a whole snapshot takes well under a second.
  static constexpr std::uint64_t kHoldMaxFrames = 10 * 60;
  // FLAG_STATE messages kept while holding; one more drops the snapshot at once.
  static constexpr std::size_t kHeldStatesMax = 512;
  static constexpr int kTitleFrames = 30;
  static constexpr int kSaveLoadMin = 2;
  static constexpr int kSettleFrames = 120;
  bool settling() const { return loading_ || settle_left_ > 0; }
  // The frames above (and kOriginFrames) are 1/60 s of this clock, not
  // calls to frame() (issue 106: high refresh calls it up to 165 a second).
  // Without a clock each call is one frame.
  void set_clock(GameTicks::Clock clock) { ticks_.set_clock(std::move(clock)); }

  // After a reconnect: drops anything queued or held, and the group values
  // of set_loading (the next snapshot brings them again). Local changes still
  // waiting for an answer are sent again at the next frame, since the answer
  // may have been lost with the connection.
  void reset();

  // True once after the host's save load reached this game: as host, its
  // FLAG_RESET went out; as player, one was applied. The game then drops its
  // own session-wide state the save undoes (docs/flags.md, "Saves").
  bool take_reset() { return std::exchange(reset_seen_, false); }

  // Once, when a save load was seen in a load window: how many distinct
  // flags went back, and how many frames had no scene (for the log line).
  struct SaveLoadSeen {
    int flags = 0;
    int frames = 0;
    bool native = false;  // the game's own load nodes said it (set_native_loads), not the old signs
    bool retry = false;   // Game Over's Try Again (the game's own load nodes said so): not a save chosen
  };
  std::optional<SaveLoadSeen> take_save_load() {
    if (!seen_) return std::nullopt;
    seen_ = false;
    SaveLoadSeen s{seen_flags_, seen_frames_, native_loads_, window_retry_};
    seen_flags_ = 0;
    seen_frames_ = 0;
    return s;
  }

  // The game's own flag writes come as events (game_flag_events.hpp: the
  // game's write queues, read as its applier is about to take them). Once
  // on, a frame while playing reads only the flags the game wrote
  // (game_wrote) and those a rule here asks for, not the whole list. The
  // whole list is still read on the first frame, in every load window (a
  // save load changes most of it) and after game_wrote_all(). As an audit, a
  // small slice of the list is read each frame too (the whole list about
  // every kAuditFrames frames): a change found there that no event named
  // is handled as any other and counted in stats().event_misses, so a
  // missing event shows and costs a few seconds, never the change.
  void use_events(bool on) {
    if (on && !events_) baselined_ = false;  // the whole list first
    events_ = on;
  }
  bool uses_events() const { return events_; }
  // The game wrote this flag (any value; a flag not in the list is ignored).
  void game_wrote(FlagKey key) {
    if (!events_) return;
    if (wrote_.size() >= kMaxWrote) wrote_all_ = true; else wrote_.insert(key);
  }
  // Events were lost: the next frame reads the whole list.
  void game_wrote_all() { wrote_all_ = true; }
  static constexpr std::size_t kAuditFrames = 240;
  static constexpr std::size_t kMaxWrote = 1u << 16;

  // A FLAG_SNAPSHOT has started and its last part has not come yet.
  bool holding() const { return holding_; }
  // How long (1/60 s) the snapshot has been held, and the load window open; 0 when not.
  std::uint64_t holding_for() const { return holding_ ? frames_ - holding_since_ : 0; }
  std::uint64_t window_for() const { return window_ ? frames_ - window_since_ : 0; }
  // A held snapshot was dropped by kHoldMaxFrames or kHeldStatesMax since the last call (the hook's notice).
  bool take_hold_timeout() { return std::exchange(hold_timed_out_, false); }
  // Local changes sent and not answered yet.
  std::size_t pending() const { return pending_.size(); }
  const FlagStats& stats() const { return stats_; }
  const SharedFlags& shared() const { return shared_; }

 private:
  using Incoming = std::variant<protocol::FlagSet, protocol::FlagState, protocol::FlagSnapshot,
                                protocol::FlagReset, protocol::FlagOrigin>;
  using Table = std::map<FlagKey, protocol::FlagEntry>;

  void handle(FlagStore& store, Incoming& message, Table& answers);
  void apply_state(FlagStore& store, const std::vector<protocol::FlagEntry>& flags);
  void apply_snapshot(FlagStore& store);
  // A snapshot that cannot finish: dropped, the game no longer held, what came meanwhile applied.
  void drop_snapshot(FlagStore& store);
  // The host's save load (FLAG_RESET): every value written exactly.
  void apply_reset(FlagStore& store, const std::vector<protocol::FlagEntry>& flags);
  // The group's value written as it is (no OR, max or mirror rule), except a
  // one-way flag that keeps our own value (keeps_own). True when written.
  bool write_exact(FlagStore& store, const protocol::FlagEntry& entry);
  // A load window (settling()) ends. Host: when its save load moved named
  // flags, the flags that moved go out as FLAG_RESET. Player: when its own
  // save load moved them, they go back to what the group had before.
  void end_window(FlagStore& store, std::vector<Outgoing>& out);
  // Takes the save-load sign when both parts are in (kSaveLoadMin distinct
  // flags back, kTitleFrames with no scene); from frame() and from diff().
  void see_save_load();
  void answer_set(FlagStore& store, const protocol::FlagSet& set, Table& answers);
  bool write(FlagStore& store, const protocol::FlagEntry& entry);
  void add(FlagStore& store, const protocol::FlagEntry& delta);
  void diff(FlagStore& store, Table& changes);
  void send_snapshot(FlagStore& store, std::vector<Outgoing>& out);
  // Host: the whole table as FLAG_RESET at the end of a save load a table was asked for in (request_snapshot).
  void send_table_reset(FlagStore& store, std::vector<Outgoing>& out);

  // True when the entry is for a shared flag and may be applied: a pickup's
  // 0 is not, and an unknown 64-bit key set to 1 is learned first.
  bool takes(const protocol::FlagEntry& entry);
  // Records `value` as the group's, with the game's value before it.
  void note_group(const protocol::FlagEntry& value, const protocol::FlagEntry* before);

  SharedFlags shared_;
  FlagRole role_;
  std::set<FlagKey> pickups_;  // one-way 64-bit keys
  std::set<FlagKey> one_way_;  // one-way named flags, no load write-back (share_one_way)
  std::set<FlagKey> down_only_;  // share_down_only
  std::set<FlagKey> up_only_;    // share_up_only
  // True (and counted) when `entry` would raise a down-only Bool our game
  // holds at 0, or lower an up-only Bool it holds at 1; the group's value
  // here becomes ours.
  bool keeps_own(FlagStore& store, const protocol::FlagEntry& entry);
  std::vector<FlagKey> written_for_others_;  // take_written_for_others
  std::vector<OneWayRestore> one_way_restored_;  // take_one_way_restores
  std::map<FlagKey, std::uint64_t> map_on_pass_;  // a one-way map Bool's 0 -> 1, by diff pass
  std::uint64_t diff_pass_ = 0;
  std::set<FlagKey> one_way_told_;           // keys take_one_way_restores gave once
  struct PutBack {
    std::uint64_t pass = 0;  // diff pass of our last put-back
    int undone = 0;          // drops right after it, in a row
  };
  std::map<FlagKey, PutBack> put_back_;   // row 140: one-way put-backs
  std::set<FlagKey> game_owned_;          // row 140: left to the game until the next load window
  std::vector<FlagKey> one_way_left_;     // take_one_way_left
  std::map<FlagKey, protocol::FlagEntry> owed_;     // row 140: left to the game: the group's value, put back after a room load
  bool owed_due_ = false;                 // a room or scene load window ended (no save load): put owed_ back
  bool hud_refresh_ = false;              // take_hud_refresh
  // Row 140: puts owed_ back (ORed with the game's floors), each once, after a room or scene load window.
  void put_owed(FlagStore& store);
  std::vector<KeepsFailing> keeps_failing_;  // row 140: note_write_failure, kFailureSayAt
  std::vector<FlagKey> own_count_drops_;     // take_own_count_drops
  struct Guard {
    std::int32_t held = 0;          // own gain not sent yet
    std::uint64_t held_since = 0;   // frame the oldest held gain was seen
    std::int32_t credit = 0;        // own source 0 to 1 not matched yet
    std::uint64_t credit_since = 0;
    std::uint64_t others_at = 0;    // frame a source was last written 0 to 1 for another player
    std::int32_t put_back = 0;      // gain just put back: an own drop cancels against it
    std::uint64_t put_back_at = 0;
    std::int32_t own = 0;           // that change, ours only
  };
  std::map<FlagKey, Guard> guards_;          // guard_count, by count
  struct Raced {
    std::int32_t gains = 0;  // a player's gains of the count still to drop
    std::uint64_t at = 0;    // the frame the last raced source came
  };
  std::map<std::pair<FlagKey, std::uint8_t>, Raced> raced_;  // host: by count and player
  // Host: the sources in `set` the group had already, as gains to drop.
  void note_raced(FlagStore& store, const protocol::FlagSet& set);
  // Host: what of a player's delta for a guarded count is added (a raced gain
  // dropped); the rest of `entry`'s value.
  std::int32_t unraced(const protocol::FlagEntry& entry);
  std::map<FlagKey, std::int32_t> max_caps_;  // cap_max
  struct CapHold {
    std::int32_t low = 0;      // the value put back (the cap, or our value before when higher)
    int tries = 0;             // put-backs in this hold (refused or undone by the game)
    int quiet = 0;             // frames the game has kept `low`
    std::uint64_t put_at = 0;  // the frame of the last put-back
  };
  std::map<FlagKey, CapHold> cap_holds_;  // cap_max, held_lower (row 181)
  struct ResetHold {
    std::int32_t low = 0;    // the host's save's value (FLAG_RESET)
    std::int32_t high = 0;   // our game's value before it
    int quiet = 0;           // frames the game has kept `low`
    int tries = 0;           // put-backs the game refused or undid
    std::uint64_t put_at = 0;  // the frame of the last put-back
  };
  std::map<FlagKey, ResetHold> reset_holds_;  // reset_lowered
  // Rows 136/137: a rise of a held key (diff); true when handled (not sent).
  bool hold_reset(FlagStore& store, FlagKey key, const protocol::FlagEntry& before, const protocol::FlagEntry& now,
                  protocol::FlagEntry& seen);
  void age_reset_holds(FlagStore& store, std::uint32_t step);
  void age_cap_holds(FlagStore& store, std::uint32_t step);
  std::map<FlagKey, FlagKey> guard_source_;  // a source: its count
  Table added_;                              // count deltas add() put in this frame
  std::vector<CountGuarded> counts_guarded_;  // take_counts_guarded
  std::int32_t doubled_ = 0;                  // expect_doubled_gain: gains still to drop
  std::uint64_t doubled_until_ = 0;           // the frame they lapse
  void settle_guards(FlagStore& store, Table& changes);
  // Notes a 64-bit key about to be written to 1 when the game has it at 0.
  void note_for_others(FlagStore& store, const protocol::FlagEntry& entry);
  std::size_t learned_ = 0;
  bool learn_ = false;
  std::vector<Incoming> queue_;
  // The chat notices (take_received).
  void note_received(const FlagKey& key, const protocol::FlagEntry& before, const protocol::FlagEntry& after);
  std::uint8_t from_ = 0;  // who sent the message being applied
  ReceivedKind received_kind_ = ReceivedKind::live;
  bool recording_ = false;  // applying another game's message
  std::vector<Received> received_;
  std::map<FlagKey, std::pair<std::uint8_t, std::uint64_t>> origin_;  // asker, frame
  // from_ becomes the asker FLAG_ORIGIN named for key (if fresh); used once.
  void take_origin(const FlagKey& key);
  std::uint64_t frames_ = 0;
  GameTicks ticks_;   // set_clock
  Table last_;        // each shared flag as the game last had it
  Table pending_;     // player: own changes waiting for the host's answer
  Table unwritten_;   // incoming values whose write failed; tried again
  // Row 159: the unwritten_ keys from an exact write (the host's save load, the join snapshot), tried again exact;
  // through write() the max rule kept our higher value, and the lower one was taken as written.
  std::set<FlagKey> unwritten_exact_;
  Table unadded_;     // host: count deltas whose write failed; tried again
  Table group_;         // each shared flag as the group last had it (set_loading)
  Table group_before_;  // an Int's value in our game just before group_ took it
  bool loading_ = false;
  int settle_left_ = 0;
  // A load window (docs/flags.md, "Saves"): from the first loading frame to
  // the end of the settle frames. Nothing is sent from inside it.
  bool window_ = false;
  bool moved_ = false;  // a save was loaded in it (the title seen, kSaveLoadMin flags back)
  std::set<FlagKey> back_keys_;  // distinct named flags gone back with no Link in this window
  bool seen_ = false;            // take_save_load: a save load was seen, not taken yet
  int seen_flags_ = 0;           // take_save_load: the sign was taken with these
  int seen_frames_ = 0;
  int no_scene_frames_ = 0;  // frames in this window with no scene loaded
  bool native_loads_ = false;    // set_native_loads
  bool native_loading_ = false;  // note_save_load_start, until the save is in (or kNativeLoadMaxFrames)
  std::uint64_t native_since_ = 0;
  bool native_loaded_ = false;   // note_save_loaded, taken by the next frame()
  bool native_forced_ = false;   // loading_ held true by a load node's load (frame())
  bool native_retry_ = false;    // note_save_loaded(true), taken with it
  bool window_retry_ = false;    // this window's save load is Game Over's Try Again
  bool link_loading_ = false;    // set_loading's last value (no Link), for when that ends
  bool old_counted_ = false;     // this window's old sign counted in old_sign_loads
  bool scene_readable_ = true;  // set_scene
  bool title_up_ = false;       // set_title: up now
  bool title_seen_ = false;     // set_title: up since the last frame() (also while not connected)
  bool window_title_ = false;   // the title was up in this load window
  bool check_offline_ = false;  // reset(): a connection's first frame with Link looks at title_seen_
  bool reset_seen_ = false;  // take_reset
  // Player: what last_ holds is worth putting back after our save load: our game played (Link, no load window)
  // since this watcher started, or the group's values came (a snapshot or a reset with flags). Neither: last_ is
  // what our title screen read (the game's defaults), and our save's own values are kept (world-fails-0115).
  bool played_ = false;
  bool group_known_ = false;
  bool table_waiting_ = false;  // host: request_snapshot is waiting for a load window to end (counted once)
  std::set<FlagKey> restate_;  // host: counts a delta was added to this frame
  bool resend_pending_ = false;
  bool snapshot_requested_ = false;
  bool holding_ = false;
  std::uint64_t holding_since_ = 0;  // frames_ at part 0
  bool hold_timed_out_ = false;      // take_hold_timeout
  std::uint64_t window_since_ = 0;   // frames_ when window_ opened
  std::uint16_t snapshot_next_part_ = 0;
  std::uint16_t snapshot_part_count_ = 0;
  Table snapshot_;                        // parts so far
  std::vector<protocol::FlagState> held_states_;  // came while holding
  FlagStats stats_;
  // write_failures per key, for FlagStats::top_write_failure; at most kWriteFailureKeys keys (a new key past
  // that is counted in write_failures only).
  static constexpr std::size_t kWriteFailureKeys = 256;
  std::map<FlagKey, std::uint64_t> write_failure_keys_;
  // A refused write: held_retries when the store holds the key (and the hold refuses this write: hold_applies) or
  // is busy, else a write failure named by key and site.
  void note_write_failure(FlagStore& store, const FlagKey& key, const char* site, bool hold_applies = true);
  // use_events
  const std::vector<FlagKey>& diff_keys();
  bool events_ = false;
  bool baselined_ = false;       // a whole-list read was made since events came on
  bool wrote_all_ = false;       // game_wrote_all, or too many game_wrote
  std::set<FlagKey> wrote_;      // game_wrote since the last diff
  std::vector<FlagKey> visit_;   // this frame's flags: written, restated, then the audit's
  std::size_t audit_from_ = 0;   // where the audit's begin in visit_ (past the end: none)
  std::size_t audit_next_ = 0;   // the audit's place in the list
  std::size_t listed_ = 0;       // the list's size at the last whole read
};

}  // namespace coop::host
