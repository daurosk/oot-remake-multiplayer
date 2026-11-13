// The mod loader's library (docs/mods.md): what is in the mods folder, each
// mod's mod.json, the player's order and switches, and what Hookshot will
// index at the next start. Files and text only: no game, no co-op session, no
// threads, so the tests drive all of it in a temporary folder.
//
// The mods folder is Hookshot's own (<game>\mods, or HOS_MODS): a mod made for
// Hookshot or an emulator works as it is. The order and the switches go into
// <mods>\coop-mods.list, which Hookshot's host/mods.c reads at the start
// (patch 0011, hos_coop_mods.h); nothing is ever renamed or moved.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace CoopMods {

// The plugin interface this loader serves (coop_mod_api.h COOP_MOD_API_VERSION).
inline constexpr int kApiVersion = 1;

inline constexpr std::size_t kMaxId = 48;
inline constexpr std::size_t kMaxVersion = 32;
inline constexpr std::size_t kMaxSettings = 32;       // per mod
inline constexpr std::size_t kMaxSettingKey = 32;
inline constexpr std::size_t kMaxSettingValue = 512;
inline constexpr std::size_t kMaxManifestBytes = 256 * 1024;
inline constexpr const char* kManifestFile = "mod.json";
inline constexpr const char* kSettingsFile = "coop-mods.settings.json";

std::filesystem::path FromUtf8(std::string_view text);
std::string ToUtf8(const std::filesystem::path& path);  // '/' between the parts

// ---- mod.json ----

enum class SettingType { Toggle, Choice, Number, Text };

// One row of a mod's settings page. Values are text everywhere (the file, the
// plugin interface, the wire): "0"/"1" for a toggle, a choice's value, a
// number as written, the text itself.
struct Setting {
    std::string key;    // [a-z0-9_]{1,32}
    std::string name;   // the row's caption
    std::string help;   // one line under the row
    SettingType type = SettingType::Toggle;
    std::string initial;
    std::vector<std::pair<std::string, std::string>> choices;  // value, caption (Choice)
    double min = 0, max = 0, step = 1;                         // Number
    std::size_t max_length = 64;                               // Text
    bool restart = false;  // a change shows only after a restart
    bool shared = false;   // in co-op the host's value is everyone's
};

// Who needs the mod in a co-op game.
enum class Share {
    Everyone,  // every player needs the same mod (the default: it changes the game)
    Own,       // each player's own business (a HUD, a camera): not compared
};

struct Manifest {
    std::string id;           // [a-z0-9][a-z0-9._-]{0,47}; from the folder's name without one
    std::string name;         // shown; the folder's name without one
    std::string version = "0";
    std::string author;
    std::string description;
    std::string code;         // this system's library, a path inside the mod's folder; empty: none
    int api = 0;              // the plugin interface the code was written for (0: not said)
    Share share = Share::Everyone;
    std::vector<Setting> settings;
};

// An id made from any name: lower case, everything else '-', never empty.
std::string IdFrom(std::string_view name);
bool ValidId(std::string_view id);
// A path that stays inside its folder: relative, no "..", no drive; '/' or '\' between the parts.
bool SafeRelative(std::string_view path);
// Whether `value` is one the setting can have.
bool ValidValue(const Setting& setting, std::string_view value);

// A mod.json's text read strictly: nothing, with why in plain English, when it is not valid (the mod's game files
// still load, its code and settings do not). `folder_name` gives the id and the name when the file has none.
std::optional<Manifest> ParseManifest(std::string_view text, std::string_view folder_name, std::string* problem);
// The manifest of a mod without a mod.json.
Manifest PlainManifest(std::string_view folder_name);

// ---- the folder ----

struct Mod {
    std::string name;               // as Hookshot names it: the path under the mods folder with '/'
    std::filesystem::path folder;
    Manifest manifest;
    bool has_manifest = false;
    bool has_romfs = false;
    bool has_exefs = false;         // Hookshot leaves code patches out; shown so the player knows
    bool internal = false;          // co-op's own packs: always on, not listed, not compared
    bool folder_off = false;        // Hookshot's own switch: a folder name starting with '-' or '_'
    bool listable = false;          // its name can go into coop-mods.list (plain ASCII, short enough)
    std::filesystem::path code;     // the library to load, checked to be a file inside the folder; empty: none
    std::string problem;            // why its mod.json or its code is left out, in plain English; empty: fine
};

// Every mod under the folder, sorted by name as Hookshot sorts them. As host/mods.c finds them (a folder holding a
// romfs or exefs folder, at most four levels down, names starting with '.' skipped), and also a folder holding a
// mod.json (a code-only mod), and the folders Hookshot's own '-' / '_' rule turns off. Two mods with one id: the
// later one gets a problem and "<id>-2". A folder that cannot be read is skipped.
std::vector<Mod> Scan(const std::filesystem::path& mods_root);

// Hookshot's order of two names (ASCII letters without case).
bool NameLess(std::string_view a, std::string_view b);
bool NameSame(std::string_view a, std::string_view b);

// ---- the player's order, switches and settings ----

struct State {
    std::vector<std::string> order;  // mod names, the first wins a file two mods have
    std::vector<std::string> off;    // mod names turned off
    std::map<std::string, std::map<std::string, std::string>> settings;  // by mod id, then key

    bool IsOff(std::string_view name) const;
};

// The state for the mods found: names of mods that are gone dropped, mods the order does not name yet added under
// the named ones in Hookshot's own order (so adding them changes nothing), internal mods never named.
void Normalize(State& state, const std::vector<Mod>& mods);

// coop-mods.list's text for the state (hos_coop_mods.h), and the state a list's text gives (order and off only).
std::string ListText(const State& state, const std::vector<Mod>& mods);
bool ReadListText(std::string_view text, State& state);

// Reads coop-mods.list and coop-mods.settings.json from the mods folder; a missing file is an empty state, a broken
// one is said in `problems` and counts as missing.
State Load(const std::filesystem::path& mods_root, std::vector<std::string>* problems);
// Writes both (each to a temporary file first, then renamed over the old one). False with `problem` on failure.
bool Save(const std::filesystem::path& mods_root, const State& state, const std::vector<Mod>& mods,
          std::string* problem);

// A setting's value now: the player's when valid, else the mod's initial one.
std::string ValueOf(const Mod& mod, const Setting& setting, const State& state);

// ---- what the game uses ----

// The mods Hookshot indexes at the next start with this state, in its order (the last wins): exactly
// hos_coop_mods_rank over ListText, then the name.
std::vector<std::string> IndexOrder(const std::vector<Mod>& mods, const State& state);

// coop-mods.applied (written by a hookshot.exe with patch 0011 at each start).
struct Applied {
    bool valid = false;
    unsigned long pid = 0;
    std::vector<std::pair<std::string, int>> on;  // name, RomFS files, in the order indexed
    std::vector<std::string> off;
};
Applied ParseApplied(std::string_view text);
Applied LoadApplied(const std::filesystem::path& mods_root);

// coop-mods.starting: the code mod whose library is being opened and started now. Written before, removed after; a
// file still there at the next start means that mod crashed or hung the game in its start, so it is skipped.
inline constexpr const char* kStartingFile = "coop-mods.starting";
struct Starting {
    unsigned long pid = 0;  // the game that wrote it
    std::string name;       // the mod's folder name
};
std::string StartingText(const Starting& s);
std::optional<Starting> ParseStarting(std::string_view text);
bool WriteStarting(const std::filesystem::path& mods_root, const Starting& s);
std::optional<Starting> LoadStarting(const std::filesystem::path& mods_root);
void ClearStarting(const std::filesystem::path& mods_root);

// ---- what a mod is, for comparing two players' ----

using Hash = std::array<std::uint8_t, 16>;
inline constexpr Hash kNoHash{};

struct Hashes {
    Hash game_files{};  // romfs: every file's path (lower case, '/') and bytes, in path order; zero without files
    Hash code{};        // the library's bytes; zero without one
    std::uint64_t files = 0, bytes = 0;
};
// Reads the whole mod (slow for a big one: call it off the game's threads). `stop`, when given, is asked now and
// then; true ends the work early with nothing.
std::optional<Hashes> HashMod(const Mod& mod, const std::function<bool()>* stop = nullptr);
std::string Hex(const Hash& hash);

}  // namespace CoopMods
