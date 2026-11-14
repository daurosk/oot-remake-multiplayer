// The Mods tab's interface (docs/mods.md): the mods as rows, their switches,
// order and settings, and whether the game must be started again. The tab
// (the pause menu's second tab, right after Multiplayer) draws from these calls
// and nothing else; the plugin host and co-op's mod list use the same object.
//
// Every call may come from any thread. Nothing here touches the game: a change
// is written to <mods>\coop-mods.list and coop-mods.settings.json at once and
// shows in the game at its next start (RestartNeeded says so), except a mod's
// settings without "restart", which its code gets at once.
#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop_mod_library.h"

namespace CoopMods {

// One row of the tab. The first row wins a game file two mods have.
struct Row {
    std::string name;         // the mod's name in the folder: what the calls below take
    std::string id;
    std::string title;
    std::string version;
    std::string author;
    std::string description;
    bool on = false;          // wanted
    bool on_now = false;      // the running game uses it (its files indexed, or its code loaded)
    bool has_files = false;
    bool has_code = false;
    bool can_switch = false;  // SetOn works; when not, `note` says why
    bool can_move = false;    // Move works
    std::size_t settings = 0;
    std::string note;         // a problem or a hint in plain English; empty: none
};

struct SettingRow {
    Setting setting;
    std::string value;
    bool locked = false;  // in a co-op game the host's value: Set is refused for a guest
};

// A "shared" setting's value (mod.json "shared": in co-op the host's value is everyone's).
struct SharedValue {
    std::string id;  // the mod's id
    std::string key;
    std::string value;
    bool operator==(const SharedValue&) const = default;
};

struct Restart {
    bool needed = false;
    std::vector<std::string> why;  // plain English, one line each, at most 8
};

class Loader {
public:
    struct Hooks {
        // One line for the game's log (the caller adds its "coop: mods: " prefix).
        std::function<void(const std::string&)> log;
        // This process: coop-mods.applied of another process is a file from an earlier run.
        unsigned long pid = 0;
        // Whether that process runs now (a coop-mods.starting of a game that still starts is not a crash). Unset: no.
        std::function<bool(unsigned long)> running;
    };

    Loader(std::filesystem::path mods_root, Hooks hooks);
    Loader(const Loader&) = delete;
    Loader& operator=(const Loader&) = delete;

    // Once, when the module starts: reads the folder, the list and what this run of the game indexed, and logs one
    // line per mod. From then on "now" in Row::on_now and RestartNeeded means that moment.
    void Start();
    // The folder read again (the tab does it when it opens: a mod dropped in shows without a restart of the tab).
    void Refresh();

    std::filesystem::path Root() const;
    // Whether this hookshot.exe reads the list (patch 0011). Without it the mods load in Hookshot's own order and the
    // tab cannot switch or move them; code mods and settings work all the same.
    bool ExeReadsList() const;

    std::vector<Row> Rows() const;
    bool SetOn(std::string_view name, bool on, std::string* problem = nullptr);
    // Moves a row up (by < 0) or down (by > 0) that many places, as far as it goes.
    bool Move(std::string_view name, int by, std::string* problem = nullptr);

    std::vector<SettingRow> Settings(std::string_view name) const;
    bool Set(std::string_view name, std::string_view key, std::string_view value, std::string* problem = nullptr);
    // A setting's value by the mod's id (the plugin interface); empty when there is no such setting.
    std::string Value(std::string_view id, std::string_view key) const;
    // Called after every Set that changed a value, on the thread that made the change, with no lock held.
    using SettingFn = std::function<void(const std::string& id, const std::string& key, const std::string& value)>;
    void OnSetting(SettingFn fn);

    // Co-op (coop_mod_share.h). This game's own values of the shared settings of the mods that are on, for the host
    // to send. Then, in a guest's game, the host's values (nullopt: none, as when hosting or alone): a shared setting
    // the host has a valid value for shows it, locked, and the mods hear each value that changes by it (OnSetting).
    std::vector<SharedValue> SharedValues() const;
    void SetHostValues(std::optional<std::vector<SharedValue>> values);
    // Whether host values are set: this game is a guest in a co-op game.
    bool HostValuesSet() const;

    Restart RestartNeeded() const;
    // Something changed a mod's files under the running game (the randomizer made a new seed): a restart is needed
    // until the game is started again. One reason per mod, the last one given. An empty reason takes it back.
    void NoteChanged(std::string_view name, std::string reason);

    // The plugin host's part. The mods whose code is to be loaded now, first row first; then, per mod, what became of
    // it (an empty problem: loaded).
    std::vector<Mod> CodeToLoad() const;
    void NoteCode(std::string_view name, std::string problem);
    // Around each mod's library open and start: writes coop-mods.starting, then removes it. When the game dies or is
    // killed in between, the next Start skips that mod's code, switches the mod off and says so (log and tab).
    void CodeStarting(std::string_view name);
    void CodeStarted();

    // The mods as found, for co-op's list and the tests.
    std::vector<Mod> Mods() const;
    State Current() const;

private:
    const Mod* Find(std::string_view name) const;  // locked
    std::string Effective(const Mod& m, const Setting& s) const;  // locked: the host's value or the player's
    bool HostSets(const Mod& m, const Setting& s) const;          // locked
    bool SaveLocked(std::string* problem);
    void Say(const std::string& line) const;

    const std::filesystem::path root_;
    const Hooks hooks_;
    mutable std::mutex mutex_;
    std::vector<Mod> mods_;
    State state_;
    bool started_ = false;
    bool exe_reads_list_ = false;
    std::vector<std::string> files_now_;                 // what this run indexed, in its order
    std::map<std::string, std::string> code_now_;        // name -> problem ("" loaded), of the code tried at the start
    std::map<std::string, std::string> restart_values_;  // "id\nkey" -> the value at the start, "restart" settings
    std::map<std::string, std::string> changed_;         // name -> reason (NoteChanged)
    std::map<std::string, std::string> skipped_;         // name -> why: its code crashed or hung the last start
    std::vector<SettingFn> on_setting_;
    std::optional<std::map<std::string, std::string>> host_values_;  // id + newline + key -> the host's value
};

}  // namespace CoopMods
