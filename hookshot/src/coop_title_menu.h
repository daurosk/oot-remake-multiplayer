// SPDX-License-Identifier: GPL-3.0-or-later
// What the Multiplayer tab of Hookshot's menu works with (coop_pause_menu.h,
// docs/hookshot.md "The Multiplayer tab"): the settings files, what the Join
// field may hold, Host's and Join's status in plain words, the addresses for
// friends on the same network, and the host's save loaded in one step at the
// title after a join (SaveWait). Logic only (no game, no files, no
// processes), so the tests drive it. The name is from the title screen's
// menu, where all of it began.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
inline constexpr std::uint16_t kDefaultPort = 27510;
// The relay a kit uses when coop-options.ini names none, or an empty one (relay = ...): the co-op project's own, run by
// its maintainer (given 2026-11-20). It cannot be removed: clearing the Relay row (Co-op Settings) goes back to it. A
// player may use another (a relay line of their own, or the row typed).
inline constexpr const char* kDefaultRelay = "159.54.182.85:27530";
inline constexpr const char* kDefaultRelayName = "Squadala relay";   // (what the Relay row shows for it)
// A room name the relay takes: 3 to 24 letters, digits, - or _ ("": none).
bool RoomNameOk(std::string_view room);
// A relay line: host[:port], no spaces, at most 128 bytes ("": the kit's default).
bool RelayOk(std::string_view relay);

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
    bool approve_joins = false;    // approve_joins (the host's: each new player waits for /allow or /deny); read only
    bool same_network_password = false;   // same_network_password (the host's: on, players on this network need the
                                          // join password too; off, the default, they join without it)
    bool auto_rejoin = false;      // auto_rejoin (join the last friend's game again by itself: coop_auto_rejoin.h)
    bool original_game = false;    // original_game (from the next start: the game as it came, coop_original.h)
    bool list_room = false;        // list_room (hosting with a relay room: the room on the relay's public list)
    bool upnp = true;              // upnp (the host's router forward)
    std::string password;          // server_password ("": a new one each session)
    bool debug_panel = false;      // LICHII_COOP_DEBUG_PANEL
    bool cheats = false;           // LICHII_COOP_CHEATS
    int max_players = 4;           // max_players (the host's: 2 to kMaxPlayers)
    std::uint16_t host_port = kDefaultPort;  // server_port (the host's: 1 to 65535)
    bool voice = false;            // LICHII_COOP_VOICE (open or ptt: proximity voice chat; unset, the default: off)
    bool voice_ptt = false;        // push to talk: LICHII_COOP_VOICE=ptt (or an older on, 1); open: open mic, the menu's default
    std::string voice_key = "V";   // LICHII_COOP_VOICE_KEY as the Voice mode row names it (read only)
    std::string voice_mic;         // LICHII_COOP_VOICE_MIC: the voice's microphone by its name ("": the game's)
    bool map_players = true;       // LICHII_COOP_MAP (0: no other players on the minimap and the pause map)
    std::string hud_size = "auto"; // LICHII_COOP_HUD_SIZE: the player list on the left (auto, full, compact, off)
    bool map_names = true;         // LICHII_COOP_MAP_NAMES (0: their icons without names)
    std::string relay = kDefaultRelay;   // relay (host and join rooms through it, Host.bat's too; kDefaultRelay without a
                                   // line; Co-op Settings' Relay row writes it)
    std::string relay_key;         // relay_key (that relay's host key; read only)
    std::string relay_room;        // relay_room (a room name on the relay, issue 261; Co-op Settings' Room name)
    std::string list_name;         // list_name (the name list_room shows the room as; "": the room name; read only)

    // From the two files' texts (a missing file: "").
    static Options Read(std::string_view coop_ini, std::string_view options_ini);
    // The two files with these settings in them.
    void Write(std::string& coop_ini, std::string& options_ini) const;
    // The LICHII_COOP_* variables these settings stand for, as the kit's
    // launcher sets them from coop-options.ini (name, "" = not set), so they
    // count for this run when co-op has not started yet.
    std::vector<std::pair<std::string, std::string>> Environment() const;
    // LICHII_COOP_VOICE's value while voice is on: "ptt" for push to talk, else "open" (open mic).
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
// Valid UDP port in 1..65535 (after trimming ends), or empty.
std::optional<std::uint16_t> ParsePort(std::string_view text);

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
    // HOS_COOP_CONNECTED only says this game was told to connect: in_session is whether the session took it in
    // (WELCOME came), as the Lobby tab counts it; refused is why the session turned it away ("" it did not).
    bool in_session = true;
    std::string refused;
    bool banned = false;    // refused by the host's ban: joining again cannot work
};
std::vector<std::string> PlainStatus(const HostView& v);
// The HOS_COOP_* state the menus show: a join told to connect is still joining until the session took it in, so
// the Multiplayer tab and the Lobby tab agree.
int ShownState(int state, bool host, bool in_session);
// A refusal reason for one menu row: the part before its first ':' (the full text is in the chat and the log).
std::string ShortReason(const std::string& reason);
// The menus' line for a refusal: "Your friend's game turned this one away: <ShortReason>."
std::string RefusedLine(const std::string& reason);

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

// After Join connects at the title: what the title's bottom-left line says
// while the host's save (lichii/hook/coop_save.h, the co-op folder) comes.
// Nothing is pressed for the player: they load a save themselves when the
// chat says Success (save::GuestNotices). The states:
// - wait: the save is not in yet. Only time on the title's menu with nothing
//   coming counts (not a menu over it, a file list, a transfer under way, a
//   hitch); after kWaitSaveMs of it: late.
// - late: still not here; the line says Continue plays the player's own save.
//   A save that comes now is still written and announced.
// - chose: the player picked a title button themselves (PlayerChose) while it
//   waited or was late: no line. Link in the game: done. Back on the title's
//   menu for kBackMs after it had gone (they backed out of their own file or a
//   new game): wait again (Tick::rearm: the host's save is offered again).
// - ready: the save is in and the game plays from the co-op folder: "load any
//   save" until Link is in the game (done).
// - host none: the host has no save yet. host gone: the session ended while it
//   waited. done: Link is in the game.
class SaveWait {
public:
    static constexpr std::uint64_t kWaitSaveMs = 60000;    // title time with nothing coming, before "late"
    static constexpr std::uint64_t kStepGapMs = 1000;      // a longer gap between two looks counts as this much
    static constexpr std::uint64_t kBackMs = 1500;         // chose: back on the title this long, then wait again

    // What the module sees at one look (each pad poll).
    struct Input {
        bool host_save_ready = false;   // written whole, and the game plays from the co-op folder
        bool save_coming = false;       // parts coming in, or in and waiting for the title's menu
        bool title_up = false;          // the title's menu is up (TitleUp)
        bool in_game = false;           // Link is in the game
        bool session_live = true;       // the co-op session is on (CoopSave::SessionLive)
    };

    void Start(std::uint64_t now_ms, std::string host);
    void Stop() { phase_ = Phase::kOff; }
    // The host has no save yet (a new game): stop waiting, say so.
    void NoSave() {
        if (waiting()) phase_ = Phase::kHostNone;
    }
    // Waiting with nothing late yet (the title's co-op rows wait).
    bool active() const { return phase_ == Phase::kWait; }
    // Step is called: wait, late, chose or ready.
    bool watching() const { return waiting() || phase_ == Phase::kChose || phase_ == Phase::kReady; }
    // For the host's save: wait or late.
    bool waiting() const { return phase_ == Phase::kWait || phase_ == Phase::kLate; }
    // The player picked a file on the title while it waited: no line (their choice), until Link is in the game or
    // they are back on the title.
    void PlayerChose() {
        if (waiting()) {
            phase_ = Phase::kChose;
            chose_gone_ = false;
            back_ms_ = 0;
        }
    }
    bool chose() const { return phase_ == Phase::kChose; }
    // One look: what the module should do (never a button press).
    struct Tick {
        bool rearm = false;   // chose -> wait: the module offers the host's save again (CoopSave)
    };
    Tick Step(std::uint64_t now_ms, const Input& in);
    // What the player sees (bottom left of the title); "" when nothing.
    std::string Line() const;

    // Whether an A that went in is the player picking a file (or a new game, or the title's options) on the title
    // itself: a title button focused, and not one of the co-op rows (Host, Join, Settings: they open our menu). A
    // guest's pending switch to the host's save is dropped then (CoopSave).
    static bool PlayerPicksFile(bool title_up, int focused_button, bool on_coop_row) {
        return title_up && focused_button >= 0 && !on_coop_row;
    }

private:
    enum class Phase { kOff, kWait, kLate, kChose, kReady, kDone, kHostNone, kHostGone };
    Phase phase_ = Phase::kOff;
    std::string host_;
    std::uint64_t last_ms_ = 0;          // the last look
    std::uint64_t waited_ms_ = 0;        // wait: title time with nothing coming
    std::uint64_t back_ms_ = 0;          // chose: title time since it came back
    bool chose_gone_ = false;            // chose: the title went after the pick
    bool live_seen_ = false;             // the session was seen on since Start (a join still connecting is not off)
};

// The player's own A on the title, seen on player 1's buttons BEFORE a code mod's hold takes them
// (CoopTitle::NotePlayerPad, coop_pad_order.h step 2). Save Profiles' file select holds that A, so the title's watch on
// the game's buttons (step 3) never sees it: this one does, for both things the A means.
// - picks_file: the player picks their own file (or a new game, or the title's options) on a title button that is not
//   a co-op row: a guest's pending switch to the host's save is dropped (CoopSave::CancelPendingSwitch), else the
//   switch could change the save folder under the load a mod's Continue starts.
// - the window: an A on the title's first button (Continue, or Start in the title's own menu) makes a code mod's
//   button choice the player's for kWindowMs (a file picked with the mouse brings no A after the first), co-op busy or
//   not (coop_hookshot.cpp, TitleHoldWhy). It ends once Link is in the game (the load came, or a later return to the
//   title is a new choice) and on Clear: a join begun after the A (StartSaveWait), or the host's save offered again
//   because the player backed out of their pick (SaveWait's rearm).
class PlayerChoice {
public:
    static constexpr std::uint64_t kA = 1ull << 0;          // (nn::hid::NpadButton A)
    static constexpr std::uint64_t kWindowMs = 120000;      // naming a file in a mod's file select takes a while
    struct Pad {
        std::uint64_t buttons = 0;   // player 1's, before the mods' holds
        bool title_up = false;
        int focused = -1;            // the title's focused button (-1: none)
        bool on_coop_row = false;    // a row the module answers (Host, Join, Co-op Settings, Multiplayer, Back, ...)
        bool in_game = false;        // Link is in the game
    };
    struct Seen {
        bool picks_file = false;     // drop the pending switch
        bool window_opened = false;  // a new window (for the log)
    };
    Seen Step(std::uint64_t now_ms, const Pad& in) {
        const std::uint64_t down = in.buttons & ~last_;
        last_ = in.buttons;
        if (in.in_game) {
            at_ms_ = 0;
        }
        Seen s;
        if ((down & kA) == 0) {
            return s;
        }
        s.picks_file = SaveWait::PlayerPicksFile(in.title_up, in.focused, in.on_coop_row);
        if (s.picks_file && in.focused == 0) {
            s.window_opened = !Lately(now_ms);
            at_ms_ = now_ms + 1;   // (+1: 0 is "none")
        }
        return s;
    }
    bool Lately(std::uint64_t now_ms) const { return at_ms_ != 0 && now_ms + 1 - at_ms_ < kWindowMs; }
    void Clear() { at_ms_ = 0; }

private:
    std::uint64_t last_ = 0;    // the buttons last seen (A edges only)
    std::uint64_t at_ms_ = 0;   // the A that opened the window, + 1 (0: none)
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

// ---------------------------------------------------------------------------
// Player report 124: after the save folder is switched (CoopSave, a join's host's save, Resync from host, a new
// session back on the player's own folder) the game's save list still shows the old folder: no Continue until a
// restart. The game keeps that list in its caption manager (the title's SeqQueryIsExistSave reads it). Its state
// byte: 0 cleared, 1-3 reading, 4 read (done once every slot entry says so: the game's own idle test). A new read
// starts only from 0, so the game's order is: its idle test, its list reset (back to 0), then its read
// (SeqOneshotRequestLoadAndSerializeCaption's body). SaveListRefresh says when the title's step does that, and when
// the game is done: idle again with state 4. Only on the title (its step), once per switch.
class SaveListRefresh {
public:
    struct Input {
        bool stale = false;          // CoopSave::TakeSaveListStale() this step
        bool title = false;          // the title's step runs (on its menu)
        bool manager = false;        // the caption manager was found
        bool busy = true;            // the game's own idle test says it is reading (or it could not be asked)
        std::uint8_t state = 0;      // its state byte
        std::uint64_t now_ms = 0;
    };
    enum class Action { none, ask, done, gave_up };
    // Why nothing is asked yet (for a log line once per wait): nothing pending, no title, no manager, the game busy.
    enum class Wait { none, title, manager, busy };
    static constexpr std::uint8_t kRead = 4;            // the state byte once the game's read is done
    static constexpr std::uint64_t kGiveUpMs = 10000;   // not read after this: said once, not asked again

    Action Step(const Input& in);
    // The ask could not be made (the game's reset said no: a save screen is busy): asked again at a later step.
    void Refused() { asked_ = false; pending_ = true; }
    bool pending() const { return pending_; }
    bool asked() const { return asked_; }
    Wait waiting() const { return wait_; }

private:
    bool pending_ = false;   // a switch happened, the game has not re-read its list since
    bool asked_ = false;
    Wait wait_ = Wait::none;
    std::uint64_t asked_ms_ = 0;
};

// Player report 124, its second half: the re-read list is not what the open title shows. The title's buttons are
// arranged once, by the game's Sequence graph OpenTitle.module when the title opens: from SeqQueryIsExistSave's
// IsExist (the caption manager's +0x8e) it stops the Title_00 layout's `BtnNum` animation at frame 3 (three buttons:
// Continue, New Game, Options) or 2 (two: New Game, Options), sets the buttons' texts from LayoutMsg/Title_00 and the
// title's sound-link arrangement (SeqOneShotSetSLinkPropertyTitleBtnArrangementType: 1 with a save, 0 without).
// TitleArrangement is that branch's values as the graph has them (read from the game's OpenTitle.module.ainb: nodes
// 6-8 and 20 without a save, 11-14 and 21 with one). After a re-read (SaveListRefresh's done) the title's step makes
// the same calls the graph's nodes end in, so the open title shows the list the game has now. What a press does is the
// game's NormalTitle.module, which asks SeqQueryIsExistSave again at the press: it already follows the new list.
struct TitleButtonText {
    const char* pane = nullptr;    // the button's text pane in Title_00
    const char* label = nullptr;   // its message in LayoutMsg/Title_00 (0000 New Game, 0001 Continue, 0002 Options)
};
struct TitleArrangement {
    float btn_num_frame = 0;                 // the frame BtnNum stops at: the buttons shown
    std::int32_t sound_type = 0;             // the title's sound-link property (its arrangement type)
    std::array<TitleButtonText, 3> texts{};  // the first `count` are set
    std::size_t count = 0;
};
TitleArrangement ArrangementFor(bool any_save);

// The title's own menu (a pack whose Title_00 has the hidden marker pane kMenuMarker, tools/puppet_pack/title_menu.py):
// three rows on the game's own buttons, Start, Multiplayer and Options; Multiplayer turns them into Host, Join, Co-op
// Settings and Back, the game's hidden fourth slot shown (BtnNum's frame 4). With a save-screen mod's file select,
// Continue and New Game led to the same place, so one Start row stands for both (one_start): the game's first button,
// Continue with a save, New Game without. Without such a mod (one_start false: no running code mod whose mod.json says
// "file_select": true, CoopModsRuntime::FileSelectRuns; another menu-screen mod does not count) and with a save the rows are Continue, New Game, Multiplayer and Options, the fourth
// slot shown for Options (the game's third button decided in its place), so New Game stays reachable; without a save
// New Game, Multiplayer, Options. Options is the game's own (its third button with a save; without one the game's
// second is Options, decided by the module when the third row is chosen). The other rows are the module's: it takes
// their A (the game never sees it) and plays the decide sound itself.
inline constexpr const char* kMenuMarker = "Pa_CoopMenu_00";
enum class TitleDo {
    kGame,         // the game's own press (the A goes through)
    kDecide,       // the game's button `decide` chosen in its place (Button::Decide)
    kMultiplayer,  // the submenu
    kHost,
    kJoin,
    kSettings,
    kBack,         // the three rows again (also B in the submenu)
};
struct TitleMenuRow {
    const char* label = nullptr;   // LayoutMsg/Title_00 (the pack's 0100..0106)
    TitleDo action = TitleDo::kGame;
    int decide = -1;               // kDecide: which of the game's buttons
};
struct TitleMenuLook {
    float btn_num_frame = 3;   // the game's buttons shown (BtnNum's frame)
    std::array<TitleMenuRow, 4> rows{};
    std::size_t count = 0;
    int focus = -1;            // where the cursor goes when this look comes in (-1: where it is)
};
TitleMenuLook MenuFor(bool any_save, bool multiplayer, bool one_start);
// Whether an A on that row is the player's own choice on the title (the game's press, or Options decided in its
// place): it drops a guest's pending switch to the host's save. The module's rows (Multiplayer, Host, Join, Co-op
// Settings, Back) and no row are not.
inline bool PlayersOwn(const TitleMenuRow* row) {
    return row != nullptr && (row->action == TitleDo::kGame || row->action == TitleDo::kDecide);
}
// The first row of `look` that does `action` (-1: none).
int RowOf(const TitleMenuLook& look, TitleDo action);
// The pack's labels: 0100 Host, 0101 Join, 0102 Co-op Settings (the older packs' rows too), 0103 Start,
// 0104 Multiplayer, 0105 Options, 0106 Back. (Continue and New Game rows use the game's own 0001 and 0000.)
inline constexpr std::array<std::pair<const char*, const char*>, 7> kMenuLabels{{{"0100", "Host"},
                                                                               {"0101", "Join"},
                                                                               {"0102", "Co-op Settings"},
                                                                               {"0103", "Start"},
                                                                               {"0104", "Multiplayer"},
                                                                               {"0105", "Options"},
                                                                               {"0106", "Back"}}};

// ---------------------------------------------------------------------------
// Player report 133: Longshot's Skip title screen (its Insert menu row, off by default) starts a launch with the newest
// save. Patch 0028 makes it choose the title's own buttons natively (the game's Button::Decide on the focused one, no
// pressed buttons), once per launch, and first asks the module (hos_coop_title_hold): while co-op is doing something
// at the title, the choice is the player's. SkipTitleHold is that answer: why it must not choose now, or nullptr (free:
// solo, co-op idle). Any state but idle holds, a failed try too (the player was choosing co-op).
struct SkipTitleInput {
    int host_state = 0;          // HosCoopStatus.state (hos_coop_host.h HOS_COOP_*): 0 idle
    bool session_live = false;   // CoopSave::SessionLive(): a session chose its save folder and is connected
    bool save_coming = false;    // CoopSave::HostSaveComing(): the host's save is on its way or waits for the title
    bool save_wait = false;      // the title waits for the host's save (CoopTitle's SaveWait)
};
const char* SkipTitleHold(const SkipTitleInput& in);

}  // namespace CoopTitleMenu
