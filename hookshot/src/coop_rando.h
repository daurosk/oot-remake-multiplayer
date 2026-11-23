// The OoT Remake randomizer as the mod loader's first built-in mod
// (docs/mods.md "The randomizer"). The randomizer itself is OOTRR
// (github.com/REALAZK/ootrr, GPL-3.0-or-later since 0.5.0): a program that makes a seed and builds a
// RomFS mod from it and the player's own game files. This file drives it:
//
// - mods\oot-remake-randomizer\mod.json (written here when missing) holds the
//   switch and the settings: the seed and the preset are "shared", so in co-op
//   the host's are everyone's (MOD_SETTINGS);
// - each seed is built once, by `ootrr generate` + `ootrr build --host
//   hookshot`, into its own mod folder mods\randomizer-<seed>; the Mods tab's
//   list turns that one on and every other seed off. Hookshot reads the mods
//   at its start, so a new seed plays after a restart of the game, never after
//   a game build;
// - nothing of the game is in this repository or the kit: OOTRR reads the
//   player's own game files, by default the ones the kit's build unpacked
//   beside the game (<game>\romfs). The kit brings OOTRR (<game>\tools\ootrr,
//   hookshot\OOTRR.txt), so a player sets nothing up.
//
// The rules (what to build, which folders to switch, the command lines) are
// plain functions for the tests; Rando runs them in the game.
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>
#include <string>
#include <string_view>
#include <thread>
#include <map>
#include <vector>

#include "coop_mods.h"

namespace CoopRando {

inline constexpr const char* kFolder = "oot-remake-randomizer";  // the switch and the settings
inline constexpr const char* kId = "oot-remake-randomizer";
inline constexpr const char* kSeedPrefix = "randomizer-";          // + the seed word: one built seed
inline constexpr const char* kSeedFile = "coop-seed.txt";          // OOTRR writes it into a Hookshot mod
inline constexpr const char* kPresetFile = "coop-rando.txt";       // we write the preset it was built with
inline constexpr const char* kLogFile = "last-build.txt";          // in kFolder: OOTRR's output
// In kFolder: the seed folders a host's world switched off in this guest's game, one per line, switched on again
// when it plays alone (Decide: Want::restore, Plan::remember / forget).
inline constexpr const char* kHostOffFile = "host-switched-off.txt";
// In kFolder: the host's hand-put seed folder a host's world switched ON in this guest's game (it had it off), switched
// off again when it plays alone (Want::host_on, Plan::remember_on / forget).
inline constexpr const char* kHostOnFile = "host-switched-on.txt";
inline constexpr const char* kTablesFile = "coop-tables.txt";      // in a seed: the size tables it was built beside
// In a seed, for the players to share (player report: no spoiler log to post): OOTRR's own spoiler log (`generate
// --spoiler`, kept as <seed folder>.spoiler.txt beside the seed file in kFolder) and the seed's OOTRR1- settings string
// (the seed file's "settings"). Every build writes both, host and guest alike (each builds the same seed).
inline constexpr const char* kSpoilerFile = "spoiler.txt";
inline constexpr const char* kSettingsFile = "settings.txt";
inline constexpr const char* kPresets[] = {"default", "easy", "open_world", "keysanity", "full_shuffle", "open", "open_fast"};
// The kit's own presets (rows 250/255): OOTRR's easy and open_world presets ask for settings its build cannot apply
// (small keys removed, adult start, open bridge: "This build cannot apply these settings yet"), so these two are
// OOTRR1- settings strings made of only the openness settings it does apply (forest, Kakariko, Zora, Door of Time,
// Gerudo Fortress open or fast, Ganon's trials skipped, boss keys removed). Each is given to `generate` in place of
// its name; the folder and the Settings row use the name. Empty: not one of ours (OOTRR's own preset id).
inline const char* OwnPresetString(std::string_view name) {
    if (name == "open") return "OOTRR1-AMAQABABAADACAQHAEBA6AIACAAQCEQBAEPACAJGAEASOAIBGMAQCTABAFHACATJAEAG2AIDQ4AQCADGXY";
    if (name == "open_fast") return "OOTRR1-AMAQABABAADACAQHAEAQ6AIACAAQCEQBAEPACAJGAEASOAIBGMAQCTABAFHACATJAEAIOAIBACCKS";
    return "";
}
inline constexpr const char* kPresetsFile = "presets-ok.txt";   // in kFolder: which presets the installed OOTRR builds

// The control mod's mod.json.
std::string ManifestText();

// The seed as OOTRR writes it into coop-seed.txt and as the folder is named: ASCII letters, digits, '_' and '-',
// at most 63, "seed" when nothing is left.
std::string SeedWord(std::string_view seed);
// The seed's folder: "randomizer-<word>", with "-<preset>" for a preset other than "default", or "-s<8 hex digits>"
// (a hash) for a settings string (IsSettingsString: too long for a folder name).
std::string FolderFor(std::string_view seed, std::string_view preset);
// OOTRR's settings string, as its own page shows it and as `ootrr generate` takes in place of a preset (the .ootrr
// file's "settings", "OOTRR1-EYAQCTABAHIHE" for default with OOTRR 0.5.0; player report #48: a seed made on OOTRR's
// page for its extra settings). The "settings" setting holds one: when set and valid it is used in place of the
// preset (shared, so a guest builds the host's). Valid: "OOTRR" then letters, digits, '-' and '_', at most
// kSettingsMax, spaces around it left out (SettingsString gives it trimmed; empty when not valid).
inline constexpr std::size_t kSettingsMax = 400;
bool IsSettingsString(std::string_view text);
std::string SettingsString(std::string_view text);
// The settings as the tab and the log name them: a preset, or "settings OOTRR1-EYAQCT..." (cut).
std::string SettingsName(std::string_view preset);
// A new seed for a host who has none: 8 letters and digits from `entropy`.
std::string NewSeed(std::uint64_t entropy);

// A seed built before, as found in the mods folder.
struct Built {
    std::string name;    // the mod's folder name (randomizer-...)
    std::string seed;    // coop-seed.txt
    std::string preset;  // coop-rando.txt; empty when not ours
    std::string tables;  // coop-tables.txt (TablesStamp); empty when built before it
    bool shared_files = true;  // settings.txt is there (kSettingsFile; false: built before it)
};
std::vector<Built> Scan(const std::filesystem::path& mods_root);

struct Want {
    Want() = default;
    Want(bool on_, std::string seed_, std::string preset_)
        : on(on_), seed(std::move(seed_)), preset(std::move(preset_)) {}
    bool on = false;
    std::string seed;    // empty: none yet
    std::string preset;  // a preset, or a settings string (IsSettingsString) in its place
    // Row 145: a seed folder is only switched off behind the player's back when the player asked for it.
    bool guest = false;       // the host's values decide `on` (a guest in the host's world)
    bool turned_off = false;  // the player turned our switch from on to off in this run
    std::vector<std::string> restore;  // kHostOffFile: what a host's world switched off here
    std::vector<std::string> host_on;  // kHostOnFile: what a host's world switched on here
    // A guest whose host plays a seed folder put in by hand (kHandSeedKey): that folder, and `keep` the same when this
    // game has it with the same files (SeedFilesHash): kept on in the host's world. `hand_pending`: this game has the
    // folder, its hash is not known yet (Decide waits).
    std::string host_hand;
    std::string keep;
    bool hand_pending = false;
};

struct Plan {
    // use: only switch the folders (on / off). refresh: the seed is built, but beside other size tables than now
    // (a puppet pack made or made again after it): it is built again and only its size table is taken.
    enum class Do { nothing, build, refresh, use } what = Do::nothing;
    std::string folder;                 // the seed's folder (build / use)
    std::vector<std::string> on;        // folders the list turns on (first wins: the seed's)
    std::vector<std::string> off;       // folders the list turns off (every other seed; all when the randomizer is off)
    std::string why;                    // one plain line for the log and the tab
    bool for_tables = true;             // refresh: for the size tables (false: only for spoiler.txt / settings.txt)
    std::vector<std::string> remember;  // a guest: `off` goes into kHostOffFile, to come back on alone
    std::vector<std::string> remember_on;  // a guest: the host's hand-put seed switched on, into kHostOnFile
    bool forget = false;                // kHostOffFile and kHostOnFile are done with (removed)
};
// What the game must do for `want`, given the seeds built and which mods are on now (Loader rows). `tables`: the
// TablesStamp of the other mods' size tables now; a built seed with another one is refreshed (none: not compared).
Plan Decide(const Want& want, const std::vector<Built>& built, const std::vector<CoopMods::Row>& rows,
            const std::optional<std::string>& tables = std::nullopt);

// A seed put in by hand (Nax 2026-11-20: built with an OOTRR from source, or made on OOTRR's page, its folder copied to
// every player): a seed folder without kPresetFile. The host tells the guests which one this run plays, as one more
// MOD_SETTINGS value of the control mod (key kHandSeedKey, "<SeedFilesHash> <folder>"; a key no manifest has, so an
// older guest keeps it unread). A guest with the same folder and the same files keeps it on; any other guest switches
// its seeds off for the host's world, as before, and both chats say why.
inline constexpr const char* kHandSeedKey = "hand_seed";
// The seed folder this run plays, when it is one put in by hand: the first row (the winner) of the seed folders the
// game indexed at its start; empty when none, or when it is ours (built by the kit: kPresetFile).
std::string HandPlaying(const std::vector<Built>& built, const std::vector<CoopMods::Row>& rows);
std::string HandSeedValue(std::string_view hash, std::string_view folder);  // empty when too long to send
bool ParseHandSeed(std::string_view value, std::string* hash, std::string* folder);
// What makes two copies of a seed the same world: every file under <folder>/romfs, by path (lower case, '/') and
// bytes, in path order, as FNV-1a 64 (16 lower-case hex). The files the kit's start rewrites in a winning mod are read
// from the copies it keeps of the mod's own (merge_tables.py: romfs/System/Resource/<f> from _coop-size-table/<f>,
// romfs/RSDB/<f> from _coop-rows/<f>), so two players match after their own merges. Nothing outside romfs counts
// (coop-seed.txt, the kit's marks). Empty: no files, one could not be read, or `stop` turned true.
std::string SeedFilesHash(const std::filesystem::path& folder, const std::function<bool()>* stop = nullptr);
// The chat lines about the host's hand-put seed: this game's (a guest) and the one the host gets (TellLine's limit).
enum class HandMatch { same, missing, different };
std::string HandLine(HandMatch match, std::string_view folder, bool restart);
std::string HandTellLine(HandMatch match, std::string_view folder);

// OOTRR's command lines (arguments after the program).
// `spoiler`: where OOTRR writes its spoiler log (--spoiler, in every OOTRR release); empty: none.
std::vector<std::string> GenerateArgs(const std::string& preset, const std::string& seed,
                                      const std::filesystem::path& seed_file,
                                      const std::filesystem::path& spoiler = {});
// The settings string a seed file (.ootrr, JSON) was made with: its "settings" (IsSettingsString), empty when none.
std::string SeedSettings(std::string_view seed_file_text);
// Whether a seed file (.ootrr, JSON) places shop items (shopsanity: a top-level "shops" object with an entry). Such a seed
// sells its one-time shelves once (lichii/hook/coop_shop_sale.h: co-op shares their sold counters).
bool SeedShops(std::string_view seed_file_text);
std::vector<std::string> BuildArgs(const std::filesystem::path& seed_file, const std::filesystem::path& game_folder,
                                   const std::filesystem::path& mods_root, const std::string& folder);
// The game uses one resource size table, the one of the mod that wins (the seed), so the seed's must hold every
// other mod's entries. OOTRR 0.5.0 merges those only from mods/<name>/romfs, and the puppet pack is deeper
// (mods/contents/<title>/coop-puppet): without its entries the game aborts while it loads; 0.6.0 reads deeper
// folders too, but not the Mods tab's switches (a row switched off would still count). So the seed is built in
// `stage`, where this copies the size tables (romfs/System/Resource/*.rsizetable.zs) of every other mod under
// `mods_root` the game loads next time, each into <stage>/<n>/romfs/System/Resource. Left out: seeds
// (randomizer-*), the control folder, a folder whose name starts with '-' or '_' (Longshot skips those) and a row
// switched off. The count of tables copied; -1 when one could not be copied.
int StageSizeTables(const std::filesystem::path& mods_root, const std::vector<CoopMods::Row>& rows,
                    const std::filesystem::path& stage);
// Those size tables (the same choice as StageSizeTables), in the order found.
std::vector<std::filesystem::path> OtherSizeTables(const std::filesystem::path& mods_root,
                                                   const std::vector<CoopMods::Row>& rows);
// What a seed was built beside, as kept in its coop-tables.txt: each table's path under `mods_root`, length and a
// hash of its bytes, sorted. A seed is only as good as its size table: the game reads that one table (the seed sorts
// first), so a puppet pack made after the seed has no sizes there and its actors cannot load (the other players
// invisible with the randomizer on).
std::string TablesStamp(const std::filesystem::path& mods_root, const std::vector<std::filesystem::path>& tables);
// The mod whose resource size table this run of the game reads, when it is none of co-op's own: any mod with its
// own size table above the puppet pack (player report #48: a seed made on OOTRR's own page, put into mods by hand).
// Longshot serves one size table, the last indexed mod's. With the Mods list (patch 0011) `indexed` is what the game
// indexed, in its order (Loader::Indexed): the list's rows ranked first row last, under them, at rank 0, the mods the
// list does not name (one dropped in since the list was written; the pack, never listed) by name, so such a mod loses
// to the pack when its name sorts under contents/0400486024DEC000/coop-puppet. Null: Hookshot's own order, from
// `rows` (by name, the last wins; only a row the game indexed, Row::on_now). Only a mod holding
// romfs/System/Resource/*.rsizetable.zs counts. That mod's table was made without the pack's entries, so the puppets
// cannot load (invisible, or a crash when the map loads). Empty when the winner is one of our seeds, the control
// folder or the pack, and when the kit merged the pack's entries into the winner's table (MergedByKit; the module
// reads no zstd, so it trusts that mark; `merged`, when given, gets that winner's name and `rule` the mark's rule). A
// seed of ours is built beside every other mod's table, StageSizeTables.
std::optional<std::string> OutsideSizeTable(const std::filesystem::path& mods_root,
                                            const std::vector<CoopMods::Row>& rows,
                                            const std::vector<std::string>* indexed,
                                            std::string* merged = nullptr, std::string* rule = nullptr);
// The kit's size-table merge (#48 step 2: a seed folder from OOTRR's site just works): the kit start writes ONE table,
// the mod's entries and the puppet pack's (and any other mod's), into the winning mod, and beside its romfs the mark
// kMergedFile: "coop-merged 1", "table <file in romfs/System/Resource> <FNV-1a 64 of it, 16 lower-case hex>", "pack
// <FNV-1a 64 of the pack's table of that name>". True only while both files are still those bytes: a new table
// dropped in or a rebuilt pack makes the mark stale (held and said again until the next kit start merges). Since
// 0.11.3 a fourth line "rule merged|contains" says which rule let the table through: "merged" (the kit wrote it) or
// "contains" (it held every pack entry at no smaller a size already, e.g. a skins mod whose maker merged them in by
// hand; left as it was). No rule line reads as "merged" (an older kit); any other rule is not trusted. `rule`, when
// given, gets it.
inline constexpr const char* kMergedFile = "coop-merged.txt";
inline constexpr const char* kPackFolder = "contents/0400486024DEC000/coop-puppet";
std::string Fnv64Hex(std::string_view bytes);
bool MergedByKit(const std::filesystem::path& mods_root, std::string_view mod, std::string* rule = nullptr);
// The RSDB tables the puppet pack adds rows to (ActorInfo, GameActorInfo: CoopPuppet_*, ...), file names in its
// romfs/RSDB, sorted.
std::vector<std::string> PackActorTables(const std::filesystem::path& mods_root);
// Player report 144/145: Longshot serves each RSDB file from the winning mod (OutsideSizeTable's order), so a mod
// with its own copy of a pack table (an OOTRR seed whose settings change actors) hides the pack's rows and the game
// refuses every puppet. The kit's start merges the pack's rows into the winner (tools/puppet_pack/merge_tables.py
// merge_rows) and writes kRowsFile beside its romfs: "coop-rows 1", then per table "table <file in romfs/RSDB>
// <FNV-1a 64 of it> <FNV-1a 64 of the pack's copy>". True only while both files are still those bytes.
inline constexpr const char* kRowsFile = "coop-rows.txt";
bool RowsMergedByKit(const std::filesystem::path& mods_root, std::string_view mod, std::string_view table);
// The first pack table whose winning copy (not the pack's) lacks the kit's mark: {mod, table}. Seeds count too.
std::optional<std::pair<std::string, std::string>> OutsideActorRows(const std::filesystem::path& mods_root,
                                                                    const std::vector<CoopMods::Row>& rows,
                                                                    const std::vector<std::string>* indexed);
// A refresh replaced a seed's size table (Plan::Do::refresh): the actor tables the kit merged (kept as they were in
// <folder>/_coop-rows) go back and kRowsFile goes. How many went back; -1 when one could not be copied.
int PutBackKeptRows(const std::filesystem::path& folder);
// Its chat line (repeats when joining).
std::string RowsLine(std::string_view mod, std::string_view table);
// This game's chat line for it (it repeats when joining), and the line the other players get (at most kTellChars).
std::string OutsideLine(std::string_view mod);
std::string OutsideTellLine(std::string_view mod);

// The Windows command line of a program and its arguments (each quoted as CommandLineToArgvW reads it back).
std::wstring CommandLine(const std::filesystem::path& program, const std::vector<std::string>& args);

// Runs a program with no window to its end, its output appended to `log_file`; its exit code, -1 when it could not
// start, kStopped when it ran longer than `limit` or `stop` turned true (it is then ended) (Rando::Hooks::run).
// Windows and Linux.
inline constexpr int kStopped = -2;
int RunProgram(const std::filesystem::path& program, const std::vector<std::string>& args,
               const std::filesystem::path& log_file, std::chrono::milliseconds limit, const std::atomic<bool>* stop);

// OOTRR's program name on this system: ootrr.exe (Windows) or ootrr.
const char* OotrrName();
// Where OOTRR is: the "ootrr" setting when set, else <game>\tools\ootrr (where the kit puts it, hookshot\OOTRR.txt)
// or <mods>\oot-remake-randomizer.
// Where the game's extracted files are: the "game_folder" setting when set, else OOT_ROMFS, else <game>\romfs (the
// files the kit's build unpacked: OOTRR only reads them). A folder counts when it has Pack and System.
std::optional<std::filesystem::path> FindOotrr(const std::string& setting, const std::filesystem::path& game_dir,
                                               const std::filesystem::path& mods_root);
std::optional<std::filesystem::path> FindGameFolder(const std::string& setting, const std::filesystem::path& game_dir);
// The kit's last OOTRR setup result, beside the game: <game>\tools\ootrr-setup.txt ("ok <release>" or
// "failed <release>: <why>", kit-paths.ps1 Install-KitOotrr / build.sh).
inline constexpr const char* kOotrrStatusFile = "ootrr-setup.txt";
// What the tab and the chat say when the kit's OOTRR is not beside the game: why the kit's last setup failed when it
// wrote that down (a player who never saw the start window's line, Velvet 2026-11-19), and what to do: close the game,
// run Update.bat (it repairs the game's co-op parts), or start the kit.
std::string OotrrMissingText(const std::filesystem::path& game_dir);

// What to do about a failed `ootrr build`, from its output: game files of another version than 1.1.1 (OOTRR says
// "the game folder has no ...") say so plainly; anything else is empty (the caller gives the exit code).
std::string BuildFailureHint(std::string_view ootrr_output);
// Which presets an OOTRR builds (player report #16: OOTRR 0.5.0's own full_shuffle preset exits 1, "This build cannot
// apply these settings yet"). Tried once per OOTRR (its size and write time: `key`), kept in kPresetsFile as
// "ootrr <key>" then "<preset> ok" or "<preset> bad" lines.
struct PresetCache {
    std::string key;
    std::map<std::string, bool> ok;
};
PresetCache ReadPresetCache(std::string_view text);
std::string PresetCacheText(const PresetCache& cache);
std::string OotrrKey(const std::filesystem::path& ootrr);
// The control mod's manifest offering only `presets` (default always, first).
std::string ManifestText(const std::vector<std::string>& presets);
// The last `n` non-empty lines of OOTRR's output (for longshot_log.txt when it fails), oldest first, without carriage returns.
std::vector<std::string> LastLines(std::string_view text, std::size_t n);
// The tab's line for OOTRR's own failure: `head` and where its output is, by file name (player report: a full path
// was cut off in the tab). The full path goes to the chat and the log.
std::string SeeLogLine(const std::string& head);
// A line for the co-op chat (others may see a screenshot): the user's profile folder (Users<name>, /home/<name>)
// masked as <user>, as collect-logs does, and cut to at most `max_chars` bytes (its "..." included) at a whole UTF-8
// character.
std::string ChatSafe(std::string line, std::size_t max_chars = 100);
// Whether a probe's run means OOTRR refused the preset: a real exit code above 0 with something said. A stop, a
// timeout, a program that did not start (kStopped, -1) or a silent exit are not known: never cached as refused.
bool PresetRefused(int code, std::string_view output);
// At most the last `max_bytes` of a file (OOTRR's output can be long): empty when it cannot be read.
inline constexpr std::size_t kTailBytes = 64 * 1024;
std::string ReadTail(const std::filesystem::path& file, std::size_t max_bytes);
// OOTRR's own error lines (player report #16: "This build cannot apply these settings yet: ..."): the first `n` lines
// that say error, cannot, can't or failed; none: the last `n` lines.
std::vector<std::string> ErrorLines(std::string_view text, std::size_t n);
// The chat line a guest sends when it cannot build the host's seed (Hooks::tell_others): at most kTellChars bytes
// (a chat line's limit), cut at a whole UTF-8 character.
inline constexpr std::size_t kTellChars = 200;
std::string TellLine(std::string_view why);
// The guest's own loud chat line when the host's seed could not be made (#59: guests played the base game without
// knowing it): no seed, the base game, why, and what to do. For this game's chat only (`why` may name a local file).
std::string NoSeedLine(std::string_view seed, std::string_view why);

// What the tab and the chat say when the randomizer is switched (by the player, or the host's switch reaching a guest).
// A seed plays only after a restart, and only from a new save (a save from before has the items of the plain game).
std::string SwitchLine(bool on);
// The chat line once the seed is ready to play (built, or switched on): restart now. Said again when this game joins
// a host and every kRepeat until the restart.
std::string ReadyLine(std::string_view seed);
// The chat line when a seed in use was made again for the size tables of now (the co-op puppets'): restart.
std::string RefreshLine(std::string_view seed);
inline constexpr std::chrono::seconds kRepeat{180};

// In the game: watches the randomizer's settings and builds a seed in the background when they change.
class Rando {
public:
    struct Hooks {
        std::function<void(const std::string&)> log;
        std::filesystem::path game_dir;
        // Runs a program to its end (any thread); its output appended to `log_file`. The exit code, or -1 when it
        // could not start. Real processes in the game, fakes in the tests.
        std::function<int(const std::filesystem::path& program, const std::vector<std::string>& args,
                          const std::filesystem::path& log_file)>
            run;
        std::function<std::uint64_t()> entropy;
        // A guest whose game could not build the host's seed tells the others, so the host's chat names the player
        // and why: one chat line (any thread). False when it could not be sent (not connected): tried again at the
        // next Recheck. Empty: said only in this game.
        std::function<bool(const std::string& line)> tell_others;
        // The clock for repeating the restart line (tests); empty: steady_clock.
        std::function<std::chrono::steady_clock::time_point()> now;
        // Try every preset once per OOTRR before its first build (#16): the Settings row then offers only the ones it
        // builds. (Off in tests that count OOTRR's runs.)
        bool probe_presets = true;
        // The probe's runs (a shorter time limit than a build's: ~90 s each); empty: `run`.
        std::function<int(const std::filesystem::path& program, const std::vector<std::string>& args,
                          const std::filesystem::path& log_file)>
            probe_run;
    };
    Rando(CoopMods::Loader& loader, Hooks hooks);
    ~Rando();  // waits for a build that runs
    Rando(const Rando&) = delete;
    Rando& operator=(const Rando&) = delete;

    // Once, after the Loader started: writes the control mod when missing, switches folders, and listens.
    void Start();
    // Whether a build runs now, and the last line said (for the tab).
    bool Building() const;
    std::string Status() const;
    // Looks at the settings again: after the host's values came or went (a host without the randomizer changes no
    // value of ours, so no setting call comes).
    void Recheck() {
        Check();  // first: drops a line no longer true (the host left, another seed)
        SayOutside();  // joined: the others hear about an outside size table too
        TellOthers();
        RepeatRestart();  // joined: a restart still due is said again, in the chat the player now looks at
    }
    // Whether the puppet pack's actors must not be made in this run: the seed it plays was built beside other size
    // tables than there are now, so the size table the game read at its start may lack the pack's entries, and making
    // a puppet then crashes the game when the map loads (lane1 2026-11-15: a tunic model's buffer too small). Set at
    // Start until the seed is checked (a refresh); cleared when its table was right after all, kept until the restart
    // when it was made again (RefreshLine) or could not be. The other players show as their fairies meanwhile.
    // Also kept for the whole run when another mod's size table wins (OutsideSizeTable).
    // Also when a pack actor table wins without the pack's rows (OutsideActorRows, RowsMissing).
    // This run plays one of our seeds whose seed file places shop items (SeedShops; set at Start).
    bool SeedHasShops() const { return seed_shops_.load(); }
    bool HoldPack() const { return hold_pack_.load() || outside_.load() || rows_out_.load(); }
    // A mod's RSDB table wins without the pack's rows this run (OutsideActorRows): the game would refuse the puppets.
    bool RowsMissing() const { return rows_out_.load(); }
    // The host's MOD_SETTINGS value for the hand-put seed this run plays (kHandSeedKey), once its files are hashed (the
    // first call starts that, off the caller's thread); nullopt meanwhile and when none. Cheap: called every frame.
    std::optional<std::string> HandValue();
    // The chat lines since the last call (SwitchLine when the switch changed).
    std::vector<std::string> TakeLines();
    // Waits until no build runs (tests, exit).
    void Wait();

private:
    Want Wanted(bool* seed_locked) const;
    void Check();                         // settings -> Plan -> act (a setting's thread)
    void Switch(const Plan& plan);        // the list's switches for a plan that builds nothing
    void Work(Plan plan, Want want);      // the worker: builds until the settings stop changing
    void Build(const Plan& plan, const Want& want);
    void Say(std::string line);
    void TellOthers();                    // sends tell_ and outside_tell_ (Hooks::tell_others)
    void SayOutside();                    // outside_line_ into the chat again, outside_tell_ armed (mutex_ not held)
    void SayRestart(std::string line);    // a restart line for the chat, kept to repeat (mutex_ not held)
    void RepeatRestart();                 // restart_ into the chat now (mutex_ not held)
    std::chrono::steady_clock::time_point Now() const;
    std::string CurrentTables() const;    // TablesStamp of the other mods' size tables now
    // The presets the OOTRR at `ootrr` builds, tried and kept in kPresetsFile when it is new (#16); the manifest
    // offers only those.
    std::map<std::string, bool> ProbePresets(const std::filesystem::path& ootrr, const std::filesystem::path& control);

    CoopMods::Loader& loader_;
    const Hooks hooks_;
    mutable std::mutex mutex_;
    std::thread worker_;
    bool building_ = false;    // a worker runs (mutex_)
    bool again_ = false;       // the settings changed while it ran (mutex_)
    std::string last_failed_;  // the folder whose build failed last (mutex_)
    std::string status_;
    bool on_now_ = false;      // the switch as the game started (mutex_)
    bool on_said_ = false;     // the switch as last said (mutex_)
    mutable std::mutex own_mutex_;              // own_on_ / turned_off_ (Wanted runs on the worker too)
    mutable std::optional<bool> own_on_;        // our own switch as last read (not the host's)
    mutable bool turned_off_ = false;           // the player turned it from on to off in this run
    std::vector<std::string> lines_;  // mutex_
    std::string probed_key_;               // the OOTRR the presets were tried for in this run (mutex_)
    std::map<std::string, bool> probed_;  // what that found (mutex_)
    bool log_in_chat_ = false;        // OOTRR's log path said in the chat this run (mutex_)
    std::string tell_;                // a line for the others not sent yet (mutex_): never a path
    std::string tell_seed_;           // the host's seed it is about (mutex_)
    std::atomic<bool> hold_pack_{false};  // HoldPack
    std::atomic<bool> seed_shops_{false};  // SeedHasShops
    std::atomic<bool> outside_{false};    // another mod's size table wins this run (HoldPack)
    std::string outside_mod_;         // that mod (mutex_): empty when none
    std::atomic<bool> rows_out_{false};  // a pack actor table wins without the pack's rows (RowsMissing)
    std::string rows_mod_, rows_table_;  // that mod and table (mutex_): empty when none
    bool outside_tell_ = false;       // OutsideTellLine not sent yet (mutex_)
    std::string bad_settings_;        // the "settings" value said to be no settings string (mutex_)
    std::string restart_;             // the restart line until this game restarts (mutex_): empty when none
    std::chrono::steady_clock::time_point restart_said_{};  // when it was last put in the chat (mutex_)
    // Hand-put seeds (HandValue, Want::keep): each folder's SeedFilesHash this run ("" none), hashed one at a time on
    // hasher_ (a guest's Check runs again when it ends).
    void HashLater(const std::string& folder);  // hand_mutex_ not held
    void HashWork();                            // hasher_: hand_job_, then hand_next_
    // A guest's chat line about the host's hand-put seed (mutex_ not held); `restart_said`: Switch said one already.
    void SayHand(const Want& want, bool restart_said);
    mutable std::mutex hand_mutex_;
    std::map<std::string, std::string> hand_hash_;  // hand_mutex_
    std::string hand_job_;                          // the folder hashed now (hand_mutex_)
    std::string hand_next_;                         // the folder asked for meanwhile (hand_mutex_)
    bool hasher_running_ = false;                   // until hasher_'s last step (hand_mutex_)
    std::thread hasher_;                            // hand_mutex_
    std::atomic<bool> stop_hash_{false};
    std::string hand_playing_;  // set at Start: HandPlaying
    std::string hand_said_;     // the host's folder the guest's chat line was said for, with its match (mutex_)
    std::string hand_tell_;     // a HandTellLine not sent yet (mutex_)
};

}  // namespace CoopRando
