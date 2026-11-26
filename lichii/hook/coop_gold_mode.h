// SPDX-License-Identifier: GPL-3.0-or-later
//
// Gold Skulltulas, a host setting (docs/world.md "Gold Skulltulas: shared or
// not"; sync-coverage row 63):
//
// - Shared (the default): a Gold Skulltula is a shared enemy. Its token drops
//   in every game (CoopWorld::WorldSync makes it in a game whose own copy
//   died with no token), one pickup counts once for everyone, and the
//   placement keys, the count and IsGet go through the flag sync.
// - Not shared (race mode): each game keeps its own Skulltulas, tokens and
//   count. Nothing about them crosses the wire: the enemy is not on the
//   objects sync, a token's taking is no WORLD_BREAK, and the flag watcher
//   keeps the keys, the count and IsGet in this game (KeepLocal).
//
// The host's choice is saved in coop-options.ini (`gold_skulltulas = own`;
// no line: shared). It is read when this game starts hosting: changing it in
// a session takes effect at the next Host. The mode goes to the guests in the
// host's MOD_SETTINGS (491) as mod id "coop", key "gold_skulltula", value
// "shared" or "own" (docs/protocol.md "Mods"); the server keeps it for late
// joiners, and a host after a migration sends the mode in force, not its own
// choice. A guest that hears no value (a host from before this) plays Shared.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coop/host/flags.hpp"
#include "coop/protocol/messages.hpp"

namespace CoopGoldMode {

enum class Mode : std::uint8_t { shared, own };

inline constexpr std::string_view kModId = "coop";
inline constexpr std::string_view kKey = "gold_skulltula";

std::string_view Value(Mode mode);                         // "shared", "own"
std::optional<Mode> Parse(std::string_view value);         // nullopt: not one of them
// The mode a list of shared settings carries (the coop entry), nullopt without one.
std::optional<Mode> FromSettings(const std::vector<coop::protocol::ModSetting>& settings);
coop::protocol::ModSetting AsSetting(Mode mode);

// This player's own choice for its next Host (the menu, coop-options.ini). Any thread.
void SetChoice(Mode mode);
Mode Choice();

// The session's mode, as the connection events give it (CoopModShare, the network thread):
// a new connection (and a new session, CoopHook's Begin, before its client starts) plays Shared
// until told; the first time this game hosts in a connection
// that nobody told a mode, it takes its own choice; a host's MOD_SETTINGS set it.
void OnConnection();
void OnHosting();
// A new session's welcome: OnConnection, then OnHosting when this game is the host from the start (the first
// player in gets no host change, so OnHosting alone never ran for it: its Not shared choice was lost).
void OnWelcomed(bool hosting);
void OnHostSettings(const std::vector<coop::protocol::ModSetting>& settings);

// The mode in force. Any thread, cheap (one atomic load).
Mode InForce();
inline bool NotShared() { return InForce() == Mode::own; }
// The connection has been told a mode (a host's settings, or our own Host).
bool Told();
// The hook keeps the Gold Skulltula flags in this game (FlagWatcher::keep_local, no way back)
// only once the session's mode is known to be Not shared: never in the frames before the
// welcome, where a mode left from an earlier session could still be in force.
inline bool KeepLocalNow() { return Told() && NotShared(); }

// Not shared, a guest: the host's save (at join, and at Resync from host) is written with this game's own race with
// that host (review of f36a8852; KeepRaceIn below). Each placement key in `skulltula_keys` and key + 2 take the race's
// state in the save's 64-bit key set, and the Int `count_hash` and the Bool `get_hash` take the race's value (0
// without one). The save layout: tools/autotest/save_flags.py. A host's save that does not read as one is left as it
// is.
struct GoldKept {
    bool done = false;        // the host's save read and changed as said
    int host_keys = 0;        // gold keys the host's save had
    int own_keys = 0;         // gold keys it has now (ours)
    std::int32_t host_count = 0;
    std::int32_t own_count = 0;
    int flags = 0;            // race flags (flag_hashes) the host's save has, each now the race's value
    bool keys_left = false;   // ours could not be added: the host's save has no key set
    std::string why;          // not done: why
};

// Not shared: one player's race with one host's game (DECISIONS 2026-11-23 18:49): the Skulltulas it took (each
// placement key and key + 2), its token count and IsGet. Kept per player and per host, beside coop.ini
// (`gold-race/<race id>.txt`), so it is remembered between sessions and put back after every save load. Never taken
// from the host's save or from the player's own (single-player) saves: a race with a host starts from 0 tokens.
struct Race {
    std::set<std::uint64_t> keys;
    std::int32_t count = 0;
    bool get = false;
    // The named flags each racer keeps too (coop::host::gold_race_flags: the Skulltula House's rewards, the wallet
    // tiers and the Stone of Agony; review-01112-h), by hash: the value, never 0 (a flag at 0 is not listed).
    std::map<std::uint32_t, std::uint32_t> flags;
    bool empty() const { return keys.empty() && count == 0 && !get && flags.empty(); }
    // Adds `other` (its keys, the higher count, IsGet if either, each flag's higher value): true when this race grew.
    // Tokens are never spent and a reward is never given back, so a race only grows: a save load that shows fewer (an
    // older save) never takes any away.
    bool Join(const Race& other);
    bool operator==(const Race&) const = default;
};

// The race as text (one `count N`, `get 0|1`, `key <hex>` and `flag <8 hex> <value>` per line) and back; nullopt
// for a text that is not one.
std::string RaceText(const Race& race);
std::optional<Race> ParseRace(std::string_view text);

// The race's file for `race_id` in `dir`, read (nullopt: none, or not a race) and written whole (through a
// temporary file). An id that is not 16 hex digits has no file.
std::filesystem::path RaceFile(const std::filesystem::path& dir, std::string_view race_id);
std::optional<Race> LoadRace(const std::filesystem::path& dir, std::string_view race_id);
bool SaveRace(const std::filesystem::path& dir, std::string_view race_id, const Race& race);

// The host's save (a progress.sav, `host`, changed in place) written with `race` (nullptr: none, a race from 0).
// Each Bool or Int of `flag_hashes` the save has takes the race's value (0 without one), as the count does.
GoldKept KeepRaceIn(std::vector<std::uint8_t>& host, const Race* race,
                    std::span<const std::uint64_t> skulltula_keys, std::uint32_t count_hash,
                    std::uint32_t get_hash, std::span<const std::uint32_t> flag_hashes = {});

// The race id: the host's game, 16 hex digits. Each game's own id is a tag of its player key
// (coop::protocol::player_key_tag); a Not shared host sends the session's id in its MOD_SETTINGS coop entry
// (key kRaceKey). A guest takes the host's; a host takes its own, unless a host before it told the session one (a
// migration keeps the race of the game the session plays). Empty: not known (a host from before this), and the race
// is then kept for this session only.
inline constexpr std::string_view kRaceKey = "gold_race";
std::string RaceIdOf(const std::array<std::uint8_t, 16>& player_key);
void SetOwnRaceId(std::string id);   // at start (the hook)
std::string RaceId();                // the session's; "" until known
std::optional<coop::protocol::ModSetting> RaceSetting();   // the host's entry: Not shared with a known id

// Each playthrough of the host's has its own race (coordinator #11): a save of the host's own, or a new game, gets a
// random playthrough id (16 hex digits), and the race id is a tag of the player key and that id. With no playthrough
// (the slot the host plays is not known on this runtime) the id is the key's alone, as before.
std::string RaceIdOf(const std::array<std::uint8_t, 16>& player_key, std::string_view playthrough);
std::string NewPlaythroughId();
// The host's playthrough ids per slot folder (its name), kept in `dir`/playthroughs.txt, and the one in force.
// OnNewGame: the game plays a new game not saved yet: a fresh id, the same until the title. OnSlot, a load
// (`by_write` false): the slot's own id (a fresh one the first time) is in force. OnSlot, a save (`by_write`): the game
// saves what it plays, so the slot takes the id in force (a new game saved over an old one, a save into another or a
// rotated autosave slot); with none in force yet (hosting began mid-game), the slot's own. OnSend: the host sends
// `slot`. The slot its game plays not known (`played` "", at the title): the guests get that slot, so its id is in
// force, as at a load, and the host's load of that slot keeps the id the guests already have. A known played slot
// that could not be sent (another went out): nothing changes, the host plays its own playthrough. OnTitle: a new pick
// follows.
class Playthroughs {
public:
    explicit Playthroughs(std::filesystem::path dir = {});
    std::string OnNewGame();
    std::string OnSlot(const std::string& slot, bool by_write);
    std::string OnSend(const std::string& slot, const std::string& played) {
        return played.empty() ? OnSlot(slot, false) : std::string();
    }
    void OnTitle() { new_game_ = false; }
    const std::string& Current() const { return current_; }
    const std::map<std::string, std::string>& Slots() const { return slots_; }

private:
    void Save() const;
    std::filesystem::path dir_;
    std::map<std::string, std::string> slots_;
    std::string current_;   // the playthrough in force ("" : none known yet)
    bool new_game_ = false;   // a new game's id taken since the title
};
// The host side: this game's player key (at start) and the playthrough its game plays now ("" : not known). While this
// game hosts the session it started (not a host after a migration, which keeps the told id) the session's race id
// follows the playthrough.
void SetOwnKey(const std::array<std::uint8_t, 16>& player_key);
void SetPlaythrough(std::string playthrough);
// The race id the host's settings carried last (MarkRaceSent, by coop_mod_share once sent). RaceSendDue: a Not shared
// host whose race id is not sent yet; its save waits, so a guest has the id before the save it keeps its race in (both
// on the session channel, reliable and in order).
void MarkRaceSent(std::string id);
bool RaceSendDue();

// This player's race with the session's host (RaceId), in memory and in `dir` (SetRaceDir: `gold-race` beside
// coop.ini). Any thread. TheRace: the race so far (read from its file the first time an id is asked; none: a race from
// 0). GrowRace: `seen` added (Race::Join) and, when it grew, written to its file at once; true when it grew. With no
// race id the race lives for this session only. A new connection (OnConnection) forgets the one in memory.
void SetRaceDir(std::filesystem::path dir);
std::filesystem::path RaceDir();
Race TheRace();
bool GrowRace(const Race& seen);

// The game's Gold Skulltula state through `game` (read), and the race put back into it (write): each key of the race
// not set in the game is set, the count raised to the race's, IsGet set, each of the race's flags raised to its
// value. Nothing lowers the game's. ReadRace reads each of `flag_hashes` the game has (not 0) into the race's flags.
std::optional<Race> ReadRace(coop::host::FlagStore& game, std::span<const std::uint64_t> skulltula_keys,
                             std::uint32_t count_hash, std::uint32_t get_hash,
                             std::span<const std::uint32_t> flag_hashes = {});
struct PutBack {
    int written = 0;   // flags written
    int failed = 0;    // writes the game refused (try again)
    int unread = 0;    // flags that could not be read (the save not in yet)
    bool done() const { return failed == 0 && unread == 0; }
};
PutBack PutRaceBack(coop::host::FlagStore& game, const Race& race, std::uint32_t count_hash, std::uint32_t get_hash);

// Test only: back to a fresh start (Shared, choice Shared).
void ResetForTest();

}  // namespace CoopGoldMode
