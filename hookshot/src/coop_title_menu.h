// SPDX-License-Identifier: GPL-3.0-or-later
// The Multiplayer menu on Hookshot's title screen (docs/hookshot.md, "The
// title menu"): Host, Join and the co-op settings, worked from the pad or the
// keyboard, so nobody edits files or runs a .bat to play together. This file
// is the menu's logic only (no game, no files, no processes), so the tests
// drive it: the module (coop_hookshot.cpp) feeds it the player's presses,
// plays the game's own UI sounds it names, carries out its requests (start
// the server, the join helper, write the settings) and draws its View.
// Not thread-safe: the module guards it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace CoopTitleMenu {

// ---------------------------------------------------------------------------
// Settings files: `key = value` lines (coop.ini beside hookshot.exe, the kit's
// coop-options.ini). Comments and everything else are kept as they are.

// The value of the last line `key = value` without a '#' or ';' in front (the
// key as written: coop-options.ini's are lower case, its LICHII_COOP_* upper
// case). A comment after the value (" # mine") is not part of it. Nothing
// when no such line is there.
std::optional<std::string> IniGet(std::string_view text, std::string_view key);
// `text` with `key` set: the line that has it now (the first; later ones are
// commented out), else the first commented-out line of that key ("# color =
// blue") made the line, else a new line at the end. `spaced`: "key = value"
// (coop-options.ini's own keys and coop.ini), else "KEY=value" (as the kit
// writes the LICHII_COOP_* switches). Keeps the file's line ends (CRLF when
// it has them).
std::string IniSet(std::string_view text, std::string_view key, std::string_view value, bool spaced);
// `text` with every line that sets `key` commented out ("# " in front): the
// default comes back.
std::string IniUnset(std::string_view text, std::string_view key);

// ---------------------------------------------------------------------------
// What the Settings page edits.

// The colours coop-options.ini's `color` takes (coop_colors.h names), after
// "" (none: the player number's colour).
const std::vector<std::string>& Colours();
// The chat box's keys to choose from (LICHII_COOP_CHAT_KEY): "T" (the
// default), a few free letters, F2 to F4 and "off".
const std::vector<std::string>& ChatKeys();

inline constexpr std::size_t kMaxNameChars = 16;   // coop_settings.h
inline constexpr std::size_t kMaxPasswordBytes = 128;
inline constexpr int kMinPlayers = 2, kMaxPlayers = 16;   // a session's size (protocol 11: up to 16)

// A name the server takes: 1 to 16 characters (code points), no control
// characters. Spaces at the ends do not count.
bool NameOk(std::string_view name);
// A join password the server takes: 1 to 128 bytes, no spaces or control
// characters.
bool PasswordOk(std::string_view password);

struct Options {
    std::string name = "Player";   // coop.ini name
    std::string colour;            // coop-options.ini color ("": the player number's); a tunic name, or the
                                   // file's own text when it is another colour (gray, #F82, rgb(...)): kept
    std::string colour_in_file;    // the color line's value as read: Write leaves the line alone while
                                   // colour still equals it (only a change of the colour row writes it)
    bool tunics = true;            // LICHII_COOP_TUNICS (0: everyone in green)
    bool hud = true;               // LICHII_COOP_HUD (0: the plain look)
    std::string chat_key = "T";    // LICHII_COOP_CHAT_KEY
    bool show_players = false;     // LICHII_COOP_SHOW_PLAYERS (the list always shown)
    bool pvp = false;              // pvp (the host's)
    bool upnp = true;              // upnp (the host's router forward)
    std::string password;          // server_password ("": a new one each session)
    bool debug_panel = false;      // LICHII_COOP_DEBUG_PANEL
    bool cheats = false;           // LICHII_COOP_CHEATS
    int max_players = 4;           // max_players (the host's: 2 to kMaxPlayers)
    bool voice = true;             // LICHII_COOP_VOICE (off: no voice chat)

    // From the two files' texts (a missing file: "").
    static Options Read(std::string_view coop_ini, std::string_view options_ini);
    // The two files with these settings in them.
    void Write(std::string& coop_ini, std::string& options_ini) const;
    // The LICHII_COOP_* variables these settings stand for, as the kit's
    // launcher sets them from coop-options.ini (name, "" = not set), so they
    // count for this run when co-op has not started yet.
    std::vector<std::pair<std::string, std::string>> Environment() const;
};

// coop.ini with what joining a server needs: server ("host:port"), name and
// the join password (an empty one: the line commented out).
std::string CoopIniFor(std::string_view coop_ini, std::string_view server, std::string_view name,
                       std::string_view password);

// The variables the kit's launcher sets from coop-options.ini (play.ps1
// Read-KitOptions): each LICHII_COOP_* and EDEN_NX2_* line without a '#', and
// `color` as LICHII_COOP_COLOR (none when empty). The module sets the ones not
// set already when the game was started without the launcher. In file order.
std::vector<std::pair<std::string, std::string>> KitVariables(std::string_view options_ini);

// ---------------------------------------------------------------------------
// What the Join page's field may hold.

struct JoinInvite {   // an invite code (coop_server --invite): the helper window joins
    std::string host;   // "203.0.113.5:27510"
    std::string password;
};
struct JoinAddress {   // a server's address (VPN, home network, port forward), then its join password
    std::string server;     // "host" or "host:port"
    std::string password;   // "" when none was typed after it
};
// The field's text as an invite code or an address (with the join password
// after a space: "25.1.2.3:27510 PASSWORD"), or one line saying what is
// wrong. `explain_code` turns an invite code's letters into the invite or the
// reason it is refused (server/nat_codes.hpp read_invite, given by the module,
// so this file needs no network code).
struct CodeResult {
    std::optional<JoinInvite> invite;
    std::string error;   // when there is no invite
    bool looks_like_code = false;
};
using JoinTarget = std::variant<JoinInvite, JoinAddress, std::string>;
JoinTarget ReadJoin(std::string_view typed, const CodeResult& code);
// Whether `typed` is shaped as a server address ("host" or "host:port", as
// coop-ini.ps1 Test-CoopServer).
bool AddressOk(std::string_view typed);

// What Host or Join is doing, in plain words for the dialog (hos_coop_host.h's
// HOS_COOP_* state and HOS_COOP_PROBLEM_* problem): nothing about ports,
// addresses or passwords; what to do when it did not work.
struct HostView {
    bool host = false;      // hosting (else joining)
    int state = 0;          // HOS_COOP_*
    int problem = 0;        // HOS_COOP_PROBLEM_*
    int players = 0;
    bool invite_done = false;
    std::string invite;     // the code (host)
    std::string answer;     // the ANSWER CODE (guest, when the host could not be reached)
    bool copied = false;    // the code is on the clipboard
};
std::vector<std::string> PlainStatus(const HostView& v);

// Friends on the host's home network or on a VPN (Radmin VPN, Hamachi, Tailscale) join by address, not by the
// invite code (the code holds the host's internet address). The Host dialog's "Same network or VPN" row copies
// one line for them, "address:port password", which Join's code field takes as it is.
struct LanAddress {
    std::string address;   // IPv4, dotted
    std::string label;     // "Radmin VPN", "Hamachi", "Tailscale", "home network"
};
// "" for an address no friend can use (loopback, link-local, a public one).
std::string LanLabel(std::string_view ipv4);
// This PC's addresses a friend can use, VPN ones first; the rest dropped, no address twice.
std::vector<LanAddress> LanAddresses(const std::vector<std::string>& ipv4s);
std::string LanJoinText(std::string_view address, std::uint16_t port, std::string_view password);
// The Host dialog's lines for it: the first address's join text (copied: it is on the clipboard), the others.
std::vector<std::string> LanLines(const std::vector<LanAddress>& addresses, std::uint16_t port,
                                  std::string_view password, bool copied);

// After Join connects: the host's save (lichii/hook/coop_save.h, the co-op
// folder) is loaded in one step, with no Continue to find. Wait until the
// host's save is in this game's co-op folder, then show the title's Continue
// (a first-time joiner's title opened without it: the module sets the title's
// row count as the game's own dialogs set theirs), put the cursor on it (the
// co-op folder holds only the host's save) and press A every kPressEveryMs
// until Link is in the game (the file list and its question take the same A),
// but only while the cursor really is on Continue. Gives up after a while
// with a line saying what to do.
class AutoLoad {
public:
    static constexpr std::uint64_t kWaitSaveMs = 60000;   // for the host's save to arrive
    static constexpr std::uint64_t kLoadMs = 30000;       // for the presses to get Link into the game
    static constexpr std::uint64_t kPressEveryMs = 1200;
    static constexpr std::uint64_t kPressHoldMs = 120;

    void Start(std::uint64_t now_ms, std::string host);
    void Stop() { phase_ = Phase::kOff; }
    // The host has no save yet (a new game): stop waiting, say so (the title's own New Game / Continue stay).
    void NoSave() {
        if (phase_ == Phase::kWait) phase_ = Phase::kHostNone;
    }
    bool active() const { return phase_ == Phase::kWait || phase_ == Phase::kLoad; }
    // One look: whether A is held now, and whether the cursor should go to Continue (once).
    struct Tick {
        bool press_a = false;           // (the module sends it only while the cursor is on Continue)
        bool reveal_continue = false;   // once: the title shows its Continue row
        bool focus_continue = false;    // once, after it: the cursor to Continue
    };
    Tick Step(std::uint64_t now_ms, bool host_save_ready, bool title_up, bool in_game);
    // What the player sees (bottom left of the title); "" when nothing.
    std::string Line() const;

    // The title's Continue (Pa_Button_02: OpenTitle.module.ainb gives it label 0001).
    static constexpr int kContinue = 2;
    // Whether AutoLoad's A may go in: on the title only with the cursor on Continue (never New Game, never a row
    // the title hides); once the title has gone (the file list, its question) every press.
    static bool PressAllowed(bool title_up, int focused_button) { return !title_up || focused_button == kContinue; }

private:
    enum class Phase { kOff, kWait, kLoad, kDone, kNoSave, kHostNone, kStuck };
    Phase phase_ = Phase::kOff;
    std::string host_;
    std::uint64_t since_ = 0;
    std::uint64_t next_press_ = 0;
    bool focused_ = false;
    bool revealed_ = false;
};

// ---------------------------------------------------------------------------
// The menu.

enum class Input { kUp, kDown, kLeft, kRight, kDecide, kBack, kPaste, kErase };
// The game's UI sounds the module plays by name (docs/hookshot.md): the cursor,
// a choice, opening, going back, starting to play, a refusal.
enum class Sound { kNone, kMove, kDecide, kOpen, kBack, kStart, kRefuse };
enum class Page { kClosed, kMain, kJoin, kSettings, kHosting, kJoining };

struct Request {
    // kHost: start the server and join it. kJoin: `text` is the Join page's
    // field (ReadJoin says what it is). kSave: write options() to the files.
    // kPaste: hand the clipboard's text to Paste. kCopyInvite: put the
    // session's invite code on the clipboard.
    // kPasteAnswer: the host hands the module's host the friend's ANSWER CODE from
    // the clipboard (when the friend's game could not reach this one directly).
    enum class Kind { kHost, kJoin, kSave, kPaste, kCopyInvite, kPasteAnswer, kCopyLan };
    Kind kind = Kind::kHost;
    std::string text;
};

struct Row {
    std::string label;
    std::string value;      // "" for a plain button
    bool selected = false;
    bool editing = false;   // the keyboard types into it (a caret shows)
    bool arrows = false;    // Left and Right change it
};
struct View {
    std::string title;
    std::vector<Row> rows;
    std::vector<std::string> status;   // lines under the rows (what Host or Join is doing, an error)
    std::string hint;                  // the controls, at the bottom
};

class Menu {
public:
    bool open() const { return page_ != Page::kClosed; }
    Page page() const { return page_; }
    // Opens with these settings to edit, on the main page or another (the title's
    // own Join and Co-op Settings rows open theirs).
    void Open(const Options& options, Page page = Page::kMain);
    void Close();
    // One press; the sound to play for it. A request may be waiting after it.
    Sound Press(Input input);
    // A typed character (Unicode) into the field being edited, or the Join
    // page's field; false when no field takes it.
    bool Char(std::uint32_t cp);
    // Text from the clipboard into the field (the Join page's code: the whole
    // field; a field being edited: added at its end). Line ends and tabs
    // become spaces; false when no field takes it.
    bool Paste(std::string_view text);
    std::optional<Request> TakeRequest();
    // What Host or Join is doing, shown under the rows; the module sets it.
    void SetStatus(std::vector<std::string> lines);
    // The module moves to the Hosting or Joining page once it started that.
    void ShowPage(Page page);
    // The last code this game joined with, offered first on the Join page as
    // "Join <host> again" (one press). An empty code: no such row.
    void SetRejoin(std::string host, std::string code);
    const Options& options() const { return options_; }
    const std::string& join_text() const { return join_; }
    std::size_t selected() const { return selected_; }
    bool editing() const { return editing_; }
    View view() const;

private:
    std::size_t RowCount() const;
    Sound Decide();
    Sound Change(int step);
    Sound Leave(std::size_t main_row);
    void Go(Page page);
    std::string* EditedText();

    Page page_ = Page::kClosed;
    std::size_t selected_ = 0;
    bool editing_ = false;
    bool direct_ = false;   // opened on a page by the title's own row: Back and Cancel close it
    std::string rejoin_host_, rejoin_code_;
    std::size_t JoinOffset() const { return rejoin_code_.empty() ? 0 : 1; }
    Options options_;
    Options saved_;   // what the files hold: Back on Settings goes back to it
    std::string join_;
    std::vector<std::string> status_;
    std::optional<Request> request_;
};

}  // namespace CoopTitleMenu
