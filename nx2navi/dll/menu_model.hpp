// SPDX-License-Identifier: GPL-3.0-or-later
// The "Multiplayer" menu the DLL shows from the game's title screen, as plain
// data: its language, its pages (the four ways to play, Host over Local
// Network, Join over Local Network, the multiplayer saves and one of them),
// the rules for a name, a port, a join
// password and an address (the launcher add-on's, mod/launcher_plugin.py), the
// text being typed, and the mod's settings file. title_menu.cpp shows it with
// NX2navi's plugin interface (the game's own settings screen). No Windows and
// no game code: tested on its own (tests/nx2navi_test.cpp).
//
// One shape for every page: five buttons, the game's ornament line, two
// buttons (kRows). The pages have the same number of rows of the same
// kinds for the game (a text row is a button to it), so going from one to
// another is an update of the open page's rows: at once, with no trip through
// the title menu, which opening another page costs (about a second of the
// title menu in between). A page holds only the rows it needs: the runtime
// hides the game's unused row templates since interface version 4's last
// build (no filler rows any more). Captions are two lines of at most some
// 46 characters each: the game's caption box does not wrap, and the runtime
// passes a line break on.
//
//   Multiplayer                Host over Local Network    Join over Local Network
//   0 Host over Steam          0 Name                     0 Name
//   1 Host over Local Network  1 Port                     1 Address
//   2 Join over Steam          2 Password                 2 Port
//   3 Join over Local Network  3 Address to give          3 Password
//   4 Multiplayer saves        4 Save                     4 Save (the host's)
//   5 ----                     5 ----                     5 ----
//   6 Status                   6 Start                    6 Join
//   7 Close                    7 Back                     7 Back
//
//   Multiplayer saves          <a save's name>
//   0 <save>                   0 Play on it
//   1 <save>                   1 Name
//   2 <save>                   2 Last played
//   3 More saves               3 Place
//   4 New save (a name)        4 Origin
//   5 ----                     5 ----
//   6 Copy my solo save        6 Delete (chosen twice)
//   7 Back                     7 Back
//
// Multiplayer saves (mp_saves.hpp) are the saves co-op is played on; the
// player's own saves are never written by a session. The model only knows
// their names and dates (State::saves, filled by its owner) and says what is
// to be done with the files (State::pending).
//
// The language is the game's (DetectLang), with no row: "language": "fr" or
// "en" in the mod's settings.json overrides it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace CoopNx2navi::Menu {

inline constexpr std::uint16_t kDefaultPort = 27510;
inline constexpr std::size_t kMaxName = 16;       // code points
inline constexpr std::size_t kMaxPassword = 128;  // bytes
inline constexpr std::size_t kMaxAddress = 253;

// --- Language ---

enum class Lang : std::uint8_t { en, fr };

// A Switch language code or a locale tag ("fr", "fr-CA", "fr_FR", "FR-be"): French
// for French, English for everything else.
Lang LangFromCode(std::string_view code);
// The value of `key` in an nx2navi.ini (case-insensitive, an optional NX2NAVI_
// prefix, quotes dropped, '#' and ';' lines skipped); "" when not set.
std::string IniValue(std::string_view ini_text, std::string_view key);
// The game's language as the runtime decides it (its ov_system.c): NX2NAVI_LANG
// from the environment, else `lang` of nx2navi.ini, else NX2NAVI_LOCALE (the
// environment, then the file), else the Windows user locale. English when
// nothing is known.
Lang DetectLang(std::string_view env_lang, std::string_view ini_text, std::string_view env_locale,
                std::string_view user_locale);

// --- The add-on's rules ---

bool ValidName(std::string_view name);          // 1 to 16 code points, no control characters
bool ValidPort(std::string_view port);          // 1 to 65535, digits only
bool ValidPassword(std::string_view password);
// A join password for a host who set none: every session has one since the server asks for it
// (it also makes the traffic keys). 8 characters a player can read out; `seed` from a random source.
std::string NewPassword(std::uint64_t seed);  // 1 to 128 bytes, no spaces or control characters
// "loopback", "link-local", "hamachi", "radmin", "tailscale", "lan", "public",
// or "" when it is not a usable IPv4 address.
std::string AddressKind(std::string_view address);
struct JoinAddress {
    std::string server;
    std::string port;  // "" when none was given
    bool operator==(const JoinAddress&) const = default;
};
// What a player typed or pasted for the host's address, made plain: spaces,
// quotes and "udp://" dropped, "localhost" is 127.0.0.1, "host:port" split.
std::optional<JoinAddress> ParseJoinAddress(std::string_view text);
// One IPv4 address of this PC with what Windows says about its network card.
struct LocalAddress {
    std::string address;
    bool gateway = false;   // the card has a default gateway: it leads somewhere
    bool virtual_card = false;  // Hyper-V, WSL, VMware, VirtualBox, a loopback card...
    std::string card;       // the card's name, for the log
    bool operator==(const LocalAddress&) const = default;
};
// Whether a network card's name or description is a virtual machine's or a
// container's switch (no friend's PC is behind it): Hyper-V and WSL
// ("vEthernet"), VMware, VirtualBox, Docker, loopback and tunnel cards.
bool VirtualCard(std::string_view name, std::string_view description);
// An address a host can give, and what it is.
struct GiveAddress {
    enum class Kind : std::uint8_t {
        lan,       // the home network: a card with a default gateway
        vpn,       // a gaming VPN (Hamachi, Radmin, Tailscale): friends on it reach it
        other,     // a private address on a card without a gateway
        virtual_card,  // a virtual card's: friends cannot reach it
        same_pc,   // 127.0.0.1: a second game on this PC
    };
    std::string address;
    Kind kind = Kind::lan;
    bool operator==(const GiveAddress&) const = default;
};
// The addresses a host gives to friends, best first: the home network's (cards
// with a default gateway), then VPN addresses, then other private ones, then
// those of virtual cards, then 127.0.0.1; no doubles, nothing link-local or
// public.
std::vector<GiveAddress> JoinAddresses(const std::vector<LocalAddress>& local);

// --- The mod's settings file (settings.json, the launcher add-on's) ---

// A JSON object of plain values, kept in order with every value as its JSON
// text, so keys this code does not know (the add-on's) survive a save.
class FlatJson {
public:
    // False when the text is not one JSON object (the object is then empty).
    bool Parse(std::string_view text);
    std::string Dump() const;  // two-space indent, as the launcher writes it
    std::optional<std::string> String(std::string_view key) const;
    std::optional<long long> Number(std::string_view key) const;
    std::optional<bool> Bool(std::string_view key) const;
    void SetString(std::string_view key, std::string_view value);
    void SetNumber(std::string_view key, long long value);
    void SetBool(std::string_view key, bool value);
    void Erase(std::string_view key);

private:
    std::vector<std::pair<std::string, std::string>> items_;  // key, the value's JSON text
    const std::string* Find(std::string_view key) const;
    void Set(std::string_view key, std::string value);
};

// --- The pages ---

enum class Page : std::uint8_t { main, host, join, saves, save };
enum class RowKind : std::uint8_t { action, separator, text };
struct Row {
    RowKind kind = RowKind::action;
    std::string label;
    std::string value;        // the text on the button
    std::string description;  // the caption under the list
    bool disabled = false;
    // text: above 0, the game takes the typing itself, up to this many bytes
    // (interface version 4, for a keyboard); 0: choosing the row starts our own
    // typing, which a gamepad alone can do too.
    std::uint32_t text_max = 0;
};
inline constexpr std::size_t kRows = 8;
// The rows every page has at the same place.
inline constexpr std::size_t kRowExtra = 4;      // the fifth button: the saves
inline constexpr std::size_t kRowSeparator = 5;
inline constexpr std::size_t kRowGo = 6;         // Status, Start, Join, Copy, Delete
inline constexpr std::size_t kRowBack = 7;       // Close, Back
inline constexpr std::size_t kSavesShown = 3;    // saves on the list page at a time

enum class Field : std::uint8_t {
    none, name, address, port, host_password, join_password,
    save_new,     // the name of a new multiplayer save
    save_rename,  // another name for the save shown
};

// A multiplayer save as the pages show it (MpSaves::Entry, with the date as text).
struct SaveInfo {
    std::string folder;  // what names it for its owner
    std::string name;
    std::string host;    // received from that player's game: "" for a save of our own
    bool received = false;
    bool empty = false;  // a game to begin
    std::string played;  // the date it was last played, "" never
    std::string place;   // the scene it was saved in, "" unknown
    bool operator==(const SaveInfo&) const = default;
};
// What the owner has to do with the files, after a button or a typed name.
enum class SaveOp : std::uint8_t { none, create, copy_solo, rename, remove };
struct PendingOp {
    SaveOp op = SaveOp::none;
    std::string folder;  // rename, remove: the save
    std::string name;    // create, copy_solo, rename: the new name
    bool operator==(const PendingOp&) const = default;
};

// Where the session is, for the Status row and the Start / Join buttons.
enum class Phase : std::uint8_t {
    idle,
    starting_server,
    connecting,       // to the host, for its save or for the session
    receiving_save,
    restarting,       // the game restarts into the co-op save
    hosting,          // our server runs and co-op is on
    joined,           // co-op is on, on someone's server
    failed,           // `message` says why
};

// What the menu asks its owner to do after a button was chosen.
enum class Action : std::uint8_t {
    none, redraw, begin_edit, start_host, start_join, close, copy_address,
    save_op,      // State::pending says what to do with a save's files
    save_chosen,  // State::save changed: to be kept for the next start
};

struct State {
    Lang lang = Lang::en;
    Page page = Page::main;
    bool dim_steam = false;  // show the Steam rows as disabled rows (see title_menu.cpp)
    // The game can take a text row's typing itself (interface version 4) and
    // the player's last press came from a keyboard: the name, address and port
    // rows are then the game's own text rows. With a gamepad they are ours.
    bool game_typing = false;
    // 1 + the row the cursor goes to with the next update of the page (interface
    // version 4), 0: it stays. Set when the page changes: a page's first row,
    // or back on the entry the player came through.
    std::uint32_t focus = 0;
    // The fields, as text.
    std::string name;
    std::string address;        // the host's, for Join
    std::string port = "27510";
    std::string host_password;  // "" none
    std::string join_password;
    std::vector<GiveAddress> addresses;  // JoinAddresses of this PC
    std::size_t address_shown = 0;
    bool address_copied = false;
    // The text being typed.
    Field editing = Field::none;
    std::string edit;
    // The session.
    Phase phase = Phase::idle;
    std::string message;  // a whole sentence in `lang`, shown under the list
    std::uint8_t player = 0;  // our number once welcomed
    // Multiplayer saves. `saves_on`: the game can be started again on a save
    // folder (interface version 4's restart): hosting then needs a save chosen
    // and never plays on the player's own. Off: the pages say so and a host
    // plays on its own saves, as before.
    bool saves_on = false;
    std::vector<SaveInfo> saves;   // the last played first (SetSaves)
    std::string save;              // the folder of the save chosen for hosting, "" none
    std::string running_save;      // the folder of the save this game runs on now, "" its own saves
    bool solo_save = false;        // the player has a save of their own to copy
    std::size_t save_first = 0;    // the first save the list page shows
    std::string save_shown;        // the save of Page::save
    bool delete_armed = false;     // Delete was chosen once
    std::string save_note;         // a sentence about the saves (a refused name, a failed copy)
    Page saves_from = Page::main;  // where the list page goes back to
    PendingOp pending;             // for the owner: TakePending
};

// The saves as they are on disk now. A chosen or shown save that is gone is
// let go of (the save page then is the list page).
void SetSaves(State& state, std::vector<SaveInfo> saves);
// What is to be done with the files, once: the owner does it and calls OpDone.
PendingOp TakePending(State& state);
// `folder`: the save as it is named now (made, renamed); `saves`: what is on
// disk after it; `ok` false: `error` is said on the page (a whole sentence,
// or "" for the usual one).
void OpDone(State& state, const PendingOp& op, bool ok, std::string_view folder,
            std::vector<SaveInfo> saves, std::string_view error = {});
// The cursor came to a row: a Delete waiting for its second choice is let go
// when it left that row. True when the page has to be drawn again.
bool Focused(State& state, std::size_t row);
// A save's name as a row shows it: "Zelda", or "Zelda (Saria)" for one
// received from Saria's game.
std::string SaveLabel(const SaveInfo& save);
const SaveInfo* FindSave(const State& state, std::string_view folder);

std::string Title(const State& state);
// Always kRows rows, of the skeleton's kinds.
std::vector<Row> Rows(const State& state);
// The field a row of the current page edits.
Field FieldAt(Page page, std::size_t row);
// A button was chosen. Changes the page or the state and says what else is
// needed. `begin_edit`: BeginEdit was done for FieldAt(page, row).
Action Activate(State& state, std::size_t row);
// The game's own typing of a text row ended with `text` (interface version
// 4): taken when the field's rule holds (true); else `state.message` says the
// rule and the field keeps what it had.
bool TakeText(State& state, std::size_t row, std::string_view text);
// The page is gone (back, or closed): the next opening starts at the first page.
void Closed(State& state);

// --- Typing ---

void BeginEdit(State& state, Field field);
// One typed character; refused ones (control characters, a letter in a port,
// one too many) are dropped.
void EditText(State& state, std::uint32_t codepoint);
void EditBackspace(State& state);
// For a player without a keyboard: up and down change the last character
// through the field's alphabet, right adds one, left takes one off.
enum class PadKey : std::uint8_t { up, down, left, right };
void EditPad(State& state, PadKey key);
// Takes the text when the field's rule holds (true); else `state.message`
// says the rule and the field keeps what it had. Either way typing ends.
bool CommitEdit(State& state);
void CancelEdit(State& state);

// Whether Start / Join can go: every field's rule holds. Else the reason, in
// `lang`, is in `why`.
bool ReadyToHost(const State& state, std::string& why);
bool ReadyToJoin(const State& state, std::string& why);

// A few texts the owner needs in the player's language.
enum class Text : std::uint8_t {
    title_entry,            // the title screen's entry
    server_failed,          // our server stopped at once
    server_missing,         // no server program in the mod folder
    connecting,
    receiving_save,
    restarting,
    no_host_save,           // the host sent no save: playing with our own
    no_answer,              // nobody answers at that address
    refused,                // + ": <reason>"
    hosting_done,           // the session runs: press Continue
    joined_done,
    already_running,
    save_failed,            // the host's save could not be written
    restart_failed,         // the save is in, but the game cannot start itself again
    host_restarting,        // the game starts again on the chosen multiplayer save: then Continue
    host_restarting_new,    // the same, for a save with nothing in it yet: then a new game
    host_restart_failed,    // the game cannot start itself again on the save
    save_op_failed,         // a save could not be made, copied, renamed or deleted
    save_in_use,            // not while the game runs on that save
    restarting_new,         // a joiner: the host is beginning a new game; the game restarts for one too
    last = restarting_new,
};
const char* T(Lang lang, Text text);

}  // namespace CoopNx2navi::Menu
