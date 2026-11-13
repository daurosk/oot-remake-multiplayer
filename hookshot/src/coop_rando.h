// The OoT Remake randomizer as the mod loader's first built-in mod
// (docs/mods.md "The randomizer"). The randomizer itself is OOTRR
// (github.com/REALAZK/ootrr, MIT): a program that makes a seed and builds a
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
//   player's own extracted game folder.
//
// The rules (what to build, which folders to switch, the command lines) are
// plain functions for the tests; Rando runs them in the game.
#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "coop_mods.h"

namespace CoopRando {

inline constexpr const char* kFolder = "oot-remake-randomizer";  // the switch and the settings
inline constexpr const char* kId = "oot-remake-randomizer";
inline constexpr const char* kSeedPrefix = "randomizer-";          // + the seed word: one built seed
inline constexpr const char* kSeedFile = "coop-seed.txt";          // OOTRR writes it into a Hookshot mod
inline constexpr const char* kPresetFile = "coop-rando.txt";       // we write the preset it was built with
inline constexpr const char* kLogFile = "last-build.txt";          // in kFolder: OOTRR's output
inline constexpr const char* kPresets[] = {"default", "easy", "open_world", "keysanity", "full_shuffle"};

// The control mod's mod.json.
std::string ManifestText();

// The seed as OOTRR writes it into coop-seed.txt and as the folder is named: ASCII letters, digits, '_' and '-',
// at most 63, "seed" when nothing is left.
std::string SeedWord(std::string_view seed);
// The seed's folder: "randomizer-<word>", with "-<preset>" for a preset other than "default".
std::string FolderFor(std::string_view seed, std::string_view preset);
// A new seed for a host who has none: 8 letters and digits from `entropy`.
std::string NewSeed(std::uint64_t entropy);

// A seed built before, as found in the mods folder.
struct Built {
    std::string name;    // the mod's folder name (randomizer-...)
    std::string seed;    // coop-seed.txt
    std::string preset;  // coop-rando.txt; empty when not ours
};
std::vector<Built> Scan(const std::filesystem::path& mods_root);

struct Want {
    bool on = false;
    std::string seed;    // empty: none yet
    std::string preset;
};

struct Plan {
    enum class Do { nothing, build, use } what = Do::nothing;  // use: only switch the folders (on / off)
    std::string folder;                 // the seed's folder (build / use)
    std::vector<std::string> on;        // folders the list turns on (first wins: the seed's)
    std::vector<std::string> off;       // folders the list turns off (every other seed; all when the randomizer is off)
    std::string why;                    // one plain line for the log and the tab
};
// What the game must do for `want`, given the seeds built and which mods are on now (Loader rows).
Plan Decide(const Want& want, const std::vector<Built>& built, const std::vector<CoopMods::Row>& rows);

// OOTRR's command lines (arguments after the program).
std::vector<std::string> GenerateArgs(const std::string& preset, const std::string& seed,
                                      const std::filesystem::path& seed_file);
std::vector<std::string> BuildArgs(const std::filesystem::path& seed_file, const std::filesystem::path& game_folder,
                                   const std::filesystem::path& mods_root, const std::string& folder);
// The Windows command line of a program and its arguments (each quoted as CommandLineToArgvW reads it back).
std::wstring CommandLine(const std::filesystem::path& program, const std::vector<std::string>& args);

// Runs a program with no window to its end, its output appended to `log_file`; its exit code, -1 when it could not
// start (Rando::Hooks::run in the game).
int RunProgram(const std::wstring& command_line, const std::filesystem::path& log_file);

// Where OOTRR is: the "ootrr" setting when set, else ootrr.exe in <game>\tools\ootrr or <mods>\oot-remake-randomizer.
// Where the game's extracted files are: the "game_folder" setting when set, else OOT_ROMFS.
std::optional<std::filesystem::path> FindOotrr(const std::string& setting, const std::filesystem::path& game_dir,
                                               const std::filesystem::path& mods_root);
std::optional<std::filesystem::path> FindGameFolder(const std::string& setting);

// In the game: watches the randomizer's settings and builds a seed in the background when they change.
class Rando {
public:
    struct Hooks {
        std::function<void(const std::string&)> log;
        std::filesystem::path game_dir;
        // Runs a program to its end (any thread); its output appended to `log_file`. The exit code, or -1 when it
        // could not start. Real processes in the game, fakes in the tests.
        std::function<int(const std::wstring& command_line, const std::filesystem::path& log_file)> run;
        std::function<std::uint64_t()> entropy;
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
    // Waits until no build runs (tests, exit).
    void Wait();

private:
    Want Wanted(bool* seed_locked) const;
    void Check();                         // settings -> Plan -> act (a setting's thread)
    void Switch(const Plan& plan);        // the list's switches for a plan that builds nothing
    void Work(Plan plan, Want want);      // the worker: builds until the settings stop changing
    void Build(const Plan& plan, const Want& want);
    void Say(std::string line);

    CoopMods::Loader& loader_;
    const Hooks hooks_;
    mutable std::mutex mutex_;
    std::thread worker_;
    bool building_ = false;    // a worker runs (mutex_)
    bool again_ = false;       // the settings changed while it ran (mutex_)
    std::string last_failed_;  // the folder whose build failed last (mutex_)
    std::string status_;
};

}  // namespace CoopRando
