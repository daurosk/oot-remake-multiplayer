// SPDX-License-Identifier: GPL-3.0-or-later
// What the Multiplayer tab of Hookshot's menu works with (coop_pause_menu.h,
// docs/hookshot.md "The Multiplayer tab"): the settings files, what the Join
// field may hold, Host's and Join's status in plain words, the addresses for
// friends on the same network, and the host's save loaded in one step at the
// title after a join (AutoLoad). Logic only (no game, no files, no
// processes), so the tests drive it. The name is from the title screen's
// menu, where all of it began.
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
// Settings files: `key = value` lines (coop.ini beside longshot.exe, the kit's
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
    bool voice = false;            // LICHII_COOP_VOICE (ptt or open: proximity voice chat; unset, the default: off)
    std::string voice_in_file;     // that line's value as read ("open" written by hand stays open while voice is on)

    // From the two files' texts (a missing file: "").
    static Options Read(std::string_view coop_ini, std::string_view options_ini);
    // The two files with these settings in them.
    void Write(std::string& coop_ini, std::string& options_ini) const;
    // The LICHII_COOP_* variables these settings stand for, as the kit's
    // launcher sets them from coop-options.ini (name, "" = not set), so they
    // count for this run when co-op has not started yet.
    std::vector<std::pair<std::string, std::string>> Environment() const;
    // LICHII_COOP_VOICE's value while voice is on: "open" when the file said so, else "ptt" (V to talk).
    std::string VoiceMode() const;
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
// folder) is loaded in one step, with no Continue to find. The states:
// - wait: the save is not in yet. Nothing pressed. The player's A on Continue
//   is held back (the module drops it: Continue now would load their own
//   file); the host's save loads by itself once it is here. Only time on the
//   title's menu with nothing coming counts (not a menu over it, a file list,
//   a transfer under way, a hitch); after kWaitSaveMs of it: late.
// - late: still waiting, but the line says Continue plays the player's own
//   save. A save that comes now still loads by itself.
// - chose: the player picked a title button themselves (PlayerChose) while
//   it waited or was late: nothing pressed, no line. Link in the game: done.
//   Back on the title's menu for kBackMs after it had gone (they backed out
//   of their own file or a new game): wait again (Tick::rearm).
// - load: the save is in and the game plays from the co-op folder. Continue
//   shown (a first-time joiner's title opened without it), the cursor put on
//   it (again every kFocusEveryMs while it is elsewhere), and A pressed every
//   kPressEveryMs: on the title only with the cursor on Continue; once that
//   press went in, also off the title (the file list, its question) for
//   kAfterTitleMs. kLoadMs on the title's menu without getting in: stuck.
// - stuck: "Press Continue to play <host>'s save." (the folder is the host's).
// - host none: the host has no save yet. done: Link is in the game.
class AutoLoad {
public:
    static constexpr std::uint64_t kWaitSaveMs = 60000;    // title time with nothing coming, before "late"
    static constexpr std::uint64_t kLoadMs = 30000;        // title time with the save in, before "stuck"
    static constexpr std::uint64_t kAfterTitleMs = 15000;  // presses after the title went
    static constexpr std::uint64_t kFocusEveryMs = 2000;
    static constexpr std::uint64_t kPressEveryMs = 1200;
    static constexpr std::uint64_t kPressHoldMs = 120;
    static constexpr std::uint64_t kStepGapMs = 1000;      // a longer gap between two looks counts as this much
    static constexpr std::uint64_t kBackMs = 1500;         // chose: back on the title this long, then wait again

    // What the module sees at one look (each pad poll).
    struct Input {
        bool host_save_ready = false;   // written whole, and the game plays from the co-op folder
        bool save_coming = false;       // parts coming in, or in and waiting for the title's menu
        bool title_up = false;          // the title's menu is up (TitleUp)
        bool in_game = false;           // Link is in the game
        int focused = -1;               // the title's focused button (-1: none, or no title)
    };

    void Start(std::uint64_t now_ms, std::string host);
    void Stop() { phase_ = Phase::kOff; }
    // The host has no save yet (a new game): stop waiting, say so (the title's own New Game / Continue stay).
    void NoSave() {
        if (waiting()) phase_ = Phase::kHostNone;
    }
    // Busy: wait or load (the title's co-op rows wait).
    bool active() const { return phase_ == Phase::kWait || phase_ == Phase::kLoad; }
    // Step is called: busy, late or chose (still watching; the title's rows work).
    bool watching() const { return active() || phase_ == Phase::kLate || phase_ == Phase::kChose; }
    // For the host's save (nothing pressed yet): wait or late.
    bool waiting() const { return phase_ == Phase::kWait || phase_ == Phase::kLate; }
    // The player picked a file on the title while it waited: AutoLoad presses nothing, with no line (their
    // choice), until Link is in the game or they are back on the title.
    void PlayerChose() {
        if (waiting()) {
            phase_ = Phase::kChose;
            chose_gone_ = false;
            back_ms_ = 0;
        }
    }
    bool chose() const { return phase_ == Phase::kChose; }
    // One look: whether A is held now, and what the title should do (made at the title's next step).
    struct Tick {
        bool press_a = false;           // AutoLoad's A
        bool reveal_continue = false;   // once: the title shows its Continue row
        bool focus_continue = false;    // the cursor to Continue
        bool rearm = false;             // chose -> wait: the module offers the host's save again (CoopSave)
    };
    Tick Step(std::uint64_t now_ms, const Input& in);
    // What the player sees (bottom left of the title); "" when nothing.
    std::string Line() const;

    // The title's Continue (Pa_Button_02: OpenTitle.module.ainb gives it label 0001).
    static constexpr int kContinue = 2;
    // Whether AutoLoad's A may go in: on the title only with the cursor on Continue (never New Game, never a row
    // the title hides); off the title only once an A went in on Continue (the file list, its question), never
    // while the title only seems gone for a moment (a hitch longer than TitleUp's 300 ms).
    static bool PressAllowed(bool title_up, int focused_button, bool pressed_continue) {
        return title_up ? focused_button == kContinue : pressed_continue;
    }
    // Whether an A that went in is the player picking a file (or a new game, or the title's options) on the title
    // itself: not AutoLoad's own press, a title button focused, and not one of the co-op rows (Host, Join, Settings:
    // they open our menu). A guest's pending switch to the host's save is dropped then (CoopSave).
    static bool PlayerPicksFile(bool title_up, bool autoload_pressed, int focused_button, bool on_coop_row) {
        return title_up && !autoload_pressed && focused_button >= 0 && !on_coop_row;
    }
    // Whether the player's own A on the title is held back from the game: on Continue while waiting (not late),
    // where Continue would load their own file; the host's save loads by itself when it is here.
    bool HoldsPlayerA(bool title_up, int focused_button) const {
        return phase_ == Phase::kWait && title_up && focused_button == kContinue;
    }

private:
    enum class Phase { kOff, kWait, kLate, kChose, kLoad, kDone, kHostNone, kStuck };
    void Load(std::uint64_t now_ms);
    Phase phase_ = Phase::kOff;
    std::string host_;
    std::uint64_t last_ms_ = 0;          // the last look
    std::uint64_t waited_ms_ = 0;        // wait: title time with nothing coming
    std::uint64_t on_title_ms_ = 0;      // load: title time
    std::uint64_t after_title_ms_ = 0;   // load: time off the title after an A went in on Continue
    std::uint64_t next_press_ = 0;
    std::uint64_t next_focus_ = 0;
    std::uint64_t back_ms_ = 0;          // chose: title time since it came back
    bool chose_gone_ = false;            // chose: the title went after the pick
    bool revealed_ = false;
    bool pressed_continue_ = false;
};

// ---------------------------------------------------------------------------
// Text, for the menu's fields (coop_pause_menu.h).

// Without the spaces, tabs and line ends at its two ends.
std::string_view Trim(std::string_view s);
// Its characters (UTF-8 code points).
std::size_t CodePoints(std::string_view s);
// Without its last character.
void PopCodePoint(std::string& s);
// A character as UTF-8; "" for what is not one.
std::string Utf8(std::uint32_t cp);
// On one line: line ends and tabs become spaces.
std::string OneLine(std::string_view text);

}  // namespace CoopTitleMenu
