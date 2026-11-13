// SPDX-License-Identifier: GPL-3.0-or-later
// Multiplayer saves: the saves co-op is played on, kept apart from the
// player's own (solo) saves, which a co-op session never writes.
//
//   <mod folder>\coop-saves\<name>\save\<account>\slot_NN\progress.sav ...
//                                  \save-tx, \save-backup   (the runtime's own)
//                          \<name>@<host>\...               a save received from a host
//                          \_deleted\<folder>\...           what the player deleted, kept once
//
// Each folder under coop-saves is a whole save folder the game can be started
// on (NX2NAVI_SAVE_DIR, or the plugin interface's set_save_dir): the runtime
// keeps the committed save in save\<account> of it. A save a host sent is kept
// under the host's save name and the host's player name ("Zelda@Saria"), so a
// player's own "Zelda" and a friend's are two folders; '@' cannot be typed
// into a name.
//
// Plain std::filesystem: no Windows, no game code, tested in
// tests/nx2navi_test.cpp. The title page (title_menu.cpp, menu_model.hpp)
// lists, makes, copies, renames and deletes them; coop_dll.cpp starts the game
// on one.
#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CoopNx2navi::MpSaves {

inline constexpr std::string_view kRootName = "coop-saves";
inline constexpr std::string_view kDeletedName = "_deleted";
inline constexpr std::size_t kMaxName = 16;  // code points

// The folder of the multiplayer saves of a mod folder.
std::filesystem::path Root(const std::filesystem::path& mod_folder);

// A name a player may give a save: 1 to 16 characters, none of \ / : * ? " < > | @,
// no control character, no space or dot at either end, not a name Windows
// keeps for itself (CON, NUL, COM1 ...) and not "_deleted".
bool ValidName(std::string_view name);
// The same text for two names that are one folder to Windows (ASCII letters
// lowered; enough for the comparison "is there one already").
std::string Folded(std::string_view name);

// The folder name of a save received from a host: "<the host's save name>@<the
// host's player name>", characters a folder name cannot hold replaced by '_'.
// A host that told no save name (an older build, or one not on a multiplayer
// save) gives "@<host>".
std::string ReceivedFolder(std::string_view host_save_name, std::string_view host_name);
// The parts of a folder name: {name, host}; host is "" for a save of the
// player's own.
struct NameParts {
    std::string name;
    std::string host;
    bool received = false;
};
NameParts PartsOf(std::string_view folder);

// The name of the multiplayer save a game save folder belongs to, when it is
// <anything>\coop-saves\<folder>\save\<account>: the part of <folder> before
// '@' ("" for a save folder that is not a multiplayer save's, a player's own
// profile for one). What a host tells its guests.
std::string NameOfSaveDir(const std::filesystem::path& save_dir);
// The same test, the folder's whole name ("" when it is not one).
std::string FolderOfSaveDir(const std::filesystem::path& save_dir);

struct Entry {
    std::string folder;        // the folder's name under the root, UTF-8
    std::string name;          // the save's name (the folder's, without "@host")
    std::string host;          // received: the host's player name
    bool received = false;
    bool empty = true;         // no slot with a progress.sav yet: a game to begin
    std::int64_t played = 0;   // the newest progress.sav, seconds since 1970 (0: never)
    std::string place;         // where that slot was saved (the game's scene name), "" unknown
    unsigned slots = 0;
    bool operator==(const Entry&) const = default;
};
// Every multiplayer save under `root`, the last played first, the never
// played ones (newest folder first) after them.
std::vector<Entry> List(const std::filesystem::path& root);
std::optional<Entry> Find(const std::filesystem::path& root, std::string_view folder);

// The scene a caption.sav names ("HyruleField_Adult" from
// "HyruleField/HyruleField_Adult"), "" when none is found. The file is the
// game's own; only this one text is looked for.
std::string PlaceOf(std::string_view caption_bytes);
// "2026-11-13 05:47", or "13/11/2026 05:47" for French.
std::string DateText(const std::tm& local, bool french);
std::string DateText(std::int64_t seconds, bool french);  // local time; "" for 0

// The first of "Coop", "Coop 2", "Coop 3" ... that no save of the player's own has.
std::string FreeName(const std::vector<Entry>& saves, std::string_view base);

// A new save with no slot: a game to begin. The player's own settings
// (account.sav, option.sav) are copied into it from `own_save_dir` when they
// are there (copied, never moved); `account` is the account folder's name.
// False with `error` when the name is not valid or taken.
bool Create(const std::filesystem::path& root, std::string_view name, std::string_view account,
            const std::filesystem::path& own_save_dir, std::string& error);
// A new save from the newest slot of the player's own saves in `own_save_dir`
// (<profile>\save\<account>): that slot's files and the player's settings are
// copied, under the slot's own name; nothing of `own_save_dir` is changed.
bool CopySolo(const std::filesystem::path& root, std::string_view name,
              const std::filesystem::path& own_save_dir, std::string& error);
// Another name for a save of the player's own (a received one keeps its
// host's). False when the new name is not valid or taken, or the game runs on
// that save (`in_use`: the game's save folder now, may be empty).
bool Rename(const std::filesystem::path& root, std::string_view folder, std::string_view name,
            const std::filesystem::path& in_use, std::string& error);
// Takes a save out of the list: its folder is moved to _deleted\<folder> (an
// earlier one of that name there is replaced), so a mistake can be undone by
// hand. Refused for anything that is not a save folder under `root`, and for
// the save the game runs on.
bool Remove(const std::filesystem::path& root, std::string_view folder,
            const std::filesystem::path& in_use, std::string& error);

}  // namespace CoopNx2navi::MpSaves
