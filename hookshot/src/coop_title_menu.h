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
    bool map_players = true;       // LICHII_COOP_MAP (0: no other players on the minimap and the pause map)
    bool map_names = true;         // LICHII_COOP_MAP_NAMES (0: their icons without names)

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

// The Host dialog's two rows for friends who join by address: "Same network" (this PC's home-network address, the
// card with the router first) and "VPN" (a VPN program's card: Radmin VPN, Hamachi, ZeroTier, Tailscale, WireGuard,
// found by the card's name, so any address range it uses works).
struct LocalCard {
    std::string address;   // IPv4, dotted
    std::string adapter;   // the card's name and description as the system gives them ("" when it does not say)
    bool gateway = false;  // the card has a router (the default way out)
};
// The VPN program a card belongs to ("" when none): by its name ("Radmin VPN", "Hamachi Network Interface",
// "ZeroTier One Virtual Port", "Tailscale", "WireGuard Tunnel"; on Linux ham0, zt..., tailscale0, wg0).
std::string VpnLabel(std::string_view adapter);
struct LanSplit {
    std::vector<LanAddress> home;   // label "home network"; a card with a router first
    std::vector<LanAddress> vpn;    // label the VPN program's name
};
// A card without a name (the system did not say) counts as a VPN by its address range (LanLabel), as before.
LanSplit SplitLan(const std::vector<LocalCard>& cards);
// The lines each row shows (the first address's join text is on the clipboard when `copied`).
std::vector<std::string> HomeLines(const std::vector<LanAddress>& home, std::uint16_t port, std::string_view password,
                                   bool copied);
std::vector<std::string> VpnLines(const std::vector<LanAddress>& vpn, std::uint16_t port, std::string_view password,
                                  bool copied);
// The Host dialog's first line while hosting with an invite code: the code is the way to host, the rows the others.
std::string HostHeadline(bool copied);

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
    bool waiting() const { return phase_ == Phase::kWait; }   // (for the host's save; nothing pressed yet)
    // The player picked a file on the title while it waited: AutoLoad stops, with no line (their choice).
    void PlayerChose() {
        if (phase_ == Phase::kWait) phase_ = Phase::kDone;
    }
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
    // Whether an A that went in is the player picking a file (or a new game, or the title's options) on the title
    // itself: not AutoLoad's own press, a title button focused, and not one of the co-op rows (Host, Join, Settings:
    // they open our menu). A guest's pending switch to the host's save is dropped then (CoopSave).
    static bool PlayerPicksFile(bool title_up, bool autoload_pressed, int focused_button, bool on_coop_row) {
        return title_up && !autoload_pressed && focused_button >= 0 && !on_coop_row;
    }

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
