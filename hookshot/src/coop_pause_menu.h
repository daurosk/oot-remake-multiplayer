// SPDX-License-Identifier: GPL-3.0-or-later
// The Multiplayer tab of Hookshot's own menu (docs/hookshot.md, "The
// Multiplayer tab"): Host, Join, Local co-op and Settings, worked from the
// pad, the keyboard or the mouse, so nobody edits files or runs a .bat to
// play together. This file is the tab's logic only (no game, no files, no
// network), so the tests drive it: the module (coop_pause.cpp) hands it the
// player's presses and what the session is doing, carries out its requests
// (host, join, leave, save the settings) and gives Hookshot its View to draw
// (hos_coop_menu.h). Not thread-safe: the module guards it.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/difficulty.hpp"
#include "coop_look.h"
#include "coop_look_menu.h"
#include "coop_title_menu.h"

namespace CoopPauseMenu {

enum class Input { kUp, kDown, kLeft, kRight, kDecide, kBack, kPaste, kErase };
enum class Page { kMain, kHost, kJoin, kLocal, kSettings, kCommands, kCustomize, kFind };   // (kFind: the Join page's Find a game)

// The chat's commands, as the Commands page lists them (docs/setup.md "Chat
// commands"): what is typed, and what it does. A new command adds its line here.
struct Command {
    const char* typed;   // "/teleport <player>"
    const char* help;    // one short paragraph
};
const std::vector<Command>& Commands();
// The help of the main page's rows that open a page (Local co-op, Settings, Customize Link, Commands): the same words
// on the - page's Settings and look rows (CoopPauseTab::MenuTabHelp), one source.
inline constexpr const char* kLocalHelp = "Two players on this PC.";
inline constexpr const char* kSettingsHelp =
    "Your name, Link's look, how many players can join your game, voice chat, the chat key, which controller this "
    "game reads, and players on the map.";
inline constexpr const char* kCustomizeHelp =
    "A colour for each part of your tunics, shields, swords, boots and gauntlets, with a picture. Your Link wears it "
    "when you close the menu.";
inline constexpr const char* kCommandsHelp = "The chat's commands, and the key that opens the chat.";
// The word the play-test bridge's `menu` state member shows for a page (docs/playtest.md): "main", "host", "join",
// "local", "settings", "commands", "customize", "find".
const char* PageWord(Page page);
// The key that opens the chat box, in words, from LICHII_COOP_CHAT_KEY ("T"
// by default; "off": none).
std::string ChatKeyWords(const std::string& chat_key);
// The Settings page's Microphone row (shown while voice chat is on): its
// choices in order: Default ("": Longshot's Insert menu's microphone) first,
// then the microphones listed now (each name once), then the chosen one when
// it is not among them (kept by its name while it is unplugged).
std::vector<std::string> MicChoices(const std::vector<std::string>& listed, const std::string& chosen);
// What the row shows: "Default", or the name cut to kMicChars characters
// ("..." at its end), with " (not connected)" when it is not listed.
constexpr std::size_t kMicChars = 28;
std::string MicWords(const std::string& chosen, const std::vector<std::string>& listed);
// With the voice's state after it (SetVoiceStatus's words), unless the name already says "(not connected)".
std::string MicWords(const std::string& chosen, const std::vector<std::string>& listed, const std::string& status);
// The most bytes one of the title's own buttons shows (CoopTitleNative::RowText cuts a longer row, "..." at its end).
constexpr std::size_t kTitleRowBytes = 40;
// A game's name on a title row that has `rest` bytes after it ("1/4  Public", " asks for a password"), so the row's own
// words are never the part cut: "room (host)" when it fits, else without " (host)", else the name cut, "..." at its end.
std::string TitleGameName(const std::string& name, const std::string& host, std::size_t rest);
// The Host page's Difficulty row (docs/play-online.md "Difficulty"): Automatic, Easy, Normal, Hard, Custom, in
// that order (0-4), as the host's choice shows it, and the choice at one of them. Custom from another starts at the
// values in force, so the sliders open where the game is.
// (protocol types only here: coop_difficulty.h uses std::max, which a file with windows.h before this one breaks)
constexpr int kDifficultyRows = 5;
// The Find a game page (Hookshot's menu, F1) lists this many games a page, under a "Page  < 2/3 >" row that Left and
// Right (or A) turn, round (row 264, coop_list_pages.h: every game list pages; none runs on under the menu's scroll).
// The title's own Join list pages on its buttons instead (CoopTitleNative::LookFor).
inline constexpr int kFindPerPage = 8;
int DifficultyIndex(const coop::protocol::DifficultyChoice& choice);
coop::protocol::DifficultyChoice DifficultyAt(const coop::protocol::DifficultyChoice& now,
                                              const coop::protocol::DifficultyValues& in_force, int index);
// The session's difficulty as CoopDifficulty::Get says it (its State, field for field).
struct DifficultyState {
    bool connected = false;   // in a session, and the server has said its difficulty
    bool can_edit = false;    // we host
    coop::protocol::DifficultyChoice choice;
    coop::protocol::DifficultyValues values;
    // Not in a session: this player's next lobby's (CoopDifficulty::NextLobby), what the lobby-creation rows set.
    coop::protocol::DifficultyChoice next;
};

struct Request {
    // kHost: start the server and join it. kJoin: `text` is the code or the
    // address (CoopTitleMenu::ReadJoin says which). kLeave: stop hosting, or
    // leave the friend's game. kSave: write options() to the files. kLook:
    // `text` is the colour chosen for Link ("" the player number's); worn when
    // the menu closes. kPaste: hand the clipboard's text to Paste.
    // kCopyInvite: the session's invite code onto the clipboard. kPasteAnswer:
    // the friend's ANSWER CODE from the clipboard to the host. kCopyAnswer: a
    // guest's answer code (its host could not be reached) onto the clipboard.
    // kCopyLan / kCopyVpn: the line for friends on the same network / on a
    // VPN with the host onto the clipboard. kController / kKeyboard: the next choice of this game's
    // controller / keyboard (CoopInput::CycleController / CycleKeyboard),
    // applied and saved by the module, which then hands the new rows back
    // (SetInputRows). kLookParts: look() is the look chosen on the Customize
    // page (coop_look_menu.h); worn when the menu closes. kSetPassword: `text` is the host's new join password
    // (CoopHostMenu::Menu::SetPassword; never logged). kAllow / kDeny: the oldest player asking to join let in or
    // turned away (the chat's /allow, /deny); `text` is their name. kRefreshFound: the Find page's list asked again.
    enum class Kind { kHost, kJoin, kLeave, kSave, kLook, kPaste, kCopyInvite, kPasteAnswer, kCopyAnswer, kCopyLan,
                      kController, kKeyboard, kLookParts, kCopyVpn,
                      kNgPlus,     // (Navi) kNgPlus: `text` is the New Game+ level chosen (coop_ngplus.h)
                      kSaveSet,    // (Navi) kSaveSet: `text` "multiplayer" or "solo" (CoopSave::ChooseSaveSet)
                      kSetPassword, kAllow, kDeny, kRefreshFound,
                      // kChat: `text` is a chat command line ("/pvp on", "/dbno time 60"), sent as the chat box sends
                      // it (CoopHook::SendChat), so a host row and the command are one path and the chat line says
                      // what changed; the server takes it from the host only.
                      kChat,
                      kDifficulty,     // kDifficulty: `text` the host's new choice (CoopDifficulty::FormatChoice)
                      kCharAges,       // kCharAges: "on", "off", "child=N" or "adult=N" (coop_char_ages.h)
                      // kLocalStart: `text` is how many games this PC runs (kLocalFewest to kLocalMost), this one
                      // included: this game hosts them (when it does not yet) and the launcher starts the missing ones
                      kLocalStart };
    Kind kind = Kind::kHost;
    std::string text;
};

// What co-op is doing, as the module sees it each frame.
struct Session {
    enum class State {
        kOff,        // not in a game
        kStarting,   // Host was pressed: the server is starting
        kHosting,
        kJoining,    // Join was pressed: trying the host
        kWaitHost,   // the host must take this game's answer code first (`code`)
        kJoined,
        kFailed      // hosting or joining did not work (`lines` say why); as kOff otherwise
    };
    State state = State::kOff;
    bool hosting = false;      // what was tried last: Host (else Join); tells a failed Host from a failed Join
    bool public_game = false;  // hosting: the game was started as a public game (HosCoopHostOptions::public_game)
    bool by_launcher = false;  // co-op came with the game's start (Host.bat, Join.bat, coop.ini): not stopped here
    int players = 0;           // in the session, this one included (0: not known)
    std::string code;          // the invite code (hosting), or the answer code (kWaitHost)
    bool code_done = false;    // hosting: the code's making ended (an empty `code` then: none could be made)
    std::vector<std::string> lines;   // what it is doing or what went wrong, in plain words
    std::string password;      // the host's join password while it hosts ("" none): shown on A, never logged
    std::vector<std::string> asking;  // who asks to join this host now, oldest first (CoopLobby::Asking)
    // The session's rules in force (the server's, as CoopPvp::On and CoopDbno::Current hear them), for the host rows
    // of Co-op settings; `rules` false: not heard yet (not in a session), the rows show what the next Host uses.
    bool rules = false;
    bool pvp = false;
    bool party_wait = false;   // wait for the party in this session (CoopPartyWait::On)
    bool gold_own = false;   // Gold Skulltulas not shared in this session (CoopGoldMode::InForce)
    bool race = false;       // race mode in this session (CoopRaceMode::InForce)
    bool floor_switches = true;   // shared floor switches in this session (CoopSwitchStandMode::InForce)
    bool pause_world = false;     // the world runs in menus in this session (CoopPauseWorldMode::InForce)
    bool dbno = true;
    int dbno_bleed_s = 45, dbno_revive_tenths = 50, dbno_revive_life = 12;

    bool operator==(const Session&) const = default;
};

// Local co-op started from the game (the Local co-op page; owner 2026-11-24: two to four games on one PC, one click):
// this PC's games as the module sees them (SetLocal; coop_local_start.h starts them).
struct LocalGames {
    bool offered = false;   // this game can start the others (a Windows kit with the launcher; not an extra game)
    int player = 0;         // this game is player N (2 to 4) of this PC's games, an extra game; 0 not one
    int asked = 0;          // the local session's games (0: no local session in this game)
    int running = 0;        // this PC's games that run, this one included (0: not counted)
    int joined = 0;         // the players in this game's session, this one included
    bool starting = false;  // the launcher is starting games now
    std::string note;       // what the last start did ("" nothing yet)
    std::string problem;    // the launcher's one problem line ("" none): the Start row's help says it first
    bool operator==(const LocalGames&) const = default;
};
// The most games of one PC: a controller each and a quarter of the screen (the kit's scripts\local-coop.ps1).
inline constexpr int kLocalFewest = 2, kLocalMost = 4;

// A row as Hookshot draws it (the numbers are hos_coop_menu.h's HOS_COOP_MENU_*).
enum class RowKind : std::uint8_t { kAction = 0, kChoice = 1, kToggle = 2, kHeading = 3, kNote = 4 };
struct Row {
    std::string name;
    std::string value;
    RowKind kind = RowKind::kAction;
    bool on = false;                  // kToggle
    bool left = false, right = false; // kChoice: it can go that way
    bool fixed = false;               // it cannot be used now
    // Words that are not the tab's own: a player's, a room's or a mod's text (a name, a room, a port, a password, a
    // search, a mod's title or setting), or words already in the player's language. They are shown as they are and
    // never looked up in the translation table (a player called "Link" must not read 林克, a name typed "On" must not
    // become 开启). The tab's own words stay as they are: the row's name beside a text field is looked up, its value is not.
    bool own_name = false, own_value = false;
    // A choice of two shown as the game's own Options toggle where the page can (the title's Host page: its two cells,
    // the chosen one `cell`, 0 or 1; -1 none): the value's words then are cells[cell].
    std::array<std::string, 2> cells{};
    int cell = -1;
};
struct View {
    std::vector<Row> rows;
    int selected = -1;         // -1: the page has no row to select
    bool typing = false;       // the keyboard types into the selected row
    std::string help_title;
    std::string help;          // a paragraph
    std::string hint;          // set off under it: a code
    std::string gold;          // one short line at the foot
    std::string change;        // the prompts' words; "": no such prompt
    std::string confirm;
    std::string back;
};

class Menu {
public:
    // The tab came to show: the settings as the files have them. The page and
    // the selection stay where the player left them.
    void Show(const CoopTitleMenu::Options& options);
    // It shows no more: a field's typing ends (a name half typed is dropped).
    void Hide();
    // Straight to a page, as its row on the first page would go (the title's
    // Host, Join and Co-op Settings rows): Host hosts at once when no session
    // runs. kMain: the first page.
    void Open(Page page);

    // One press; false when nothing took it (a Back on the first page: the
    // caller closes the whole menu). A request may be waiting after it.
    bool Press(Input input);
    // Whether a Back now would be taken (Press(kBack) true): typing ends, or a page under the first goes back. Hookshot's
    // menu is answered from it at once while the press waits for the game's thread (coop_menu_handoff.h, row 279).
    bool BackTaken() const { return typing_ || page_ != Page::kMain; }
    // The mouse: over a row, and a click on its name (side 0) or on the left
    // (-1) or right (1) half of its value.
    void Select(std::size_t row);
    void Click(std::size_t row, int side);
    // A typed character (Unicode) into the field being typed into; false when
    // there is none.
    bool Char(std::uint32_t cp);
    // The clipboard's text: the Join page's code (the whole field), or added
    // to a name being typed. Line ends and tabs become spaces. False when no
    // field takes it.
    bool Paste(std::string_view text);
    std::optional<Request> TakeRequest();

    // What co-op is doing now; the pages follow it.
    void SetSession(Session session);
    // This PC's games (the Local co-op page's rows follow them).
    void SetLocal(LocalGames local);
    const LocalGames& local() const { return local_; }
    int local_count() const { return local_count_; }
    // The answer to the last request, shown until the next press ("Settings
    // saved.", "Copy your friend's answer code first").
    void SetNote(std::vector<std::string> lines);
    const std::vector<std::string>& NoteLines() const { return note_; }
    // The last code this game joined with, offered first on the Join page as
    // "Join <host> again" (one press). An empty code: no such row.
    void SetRejoin(std::string host, std::string code);
    // The Settings page's Controller and Keyboard rows, as CoopInput's
    // ControllerRow / KeyboardRow say them ("Controller: 2 - Xbox Wireless
    // Controller"). Empty: this Hookshot cannot choose (no patch 0015), so
    // the rows are not shown.
    void SetInputRows(std::string controller, std::string keyboard);
    // The Microphone row's list, the microphones there now by their names
    // (CoopVoice::Microphones). None: this game cannot choose one for the
    // voice (an older patch 0009), so the row is not shown.
    void SetMicrophones(std::optional<std::vector<std::string>> listed);
    // Row 229: what the voice's microphone gives now (CoopVoice::MicStatus): the Microphone row's words after its
    // name ("(Off in Longshot)", MicStatusWords) and one sentence after the Proximity voice chat row's help
    // (MicStatusNote); both "" when it reaches the others or is not known yet.
    void SetVoiceStatus(std::string words, std::string note);
    // The look shown now (the one waiting for the menu to close, else the one
    // worn): the Customize page opens on it.
    void SetLook(const CoopLook::Look& look);
    // (Navi) New Game+ (coop_ngplus.h): the level chosen, the highest unlocked (0: Ganon not beaten yet), the
    // hearts carried (quarters). The Settings page's New Game+ row.
    void SetNgPlus(int level, int unlocked, int hearts);
    // A Character Select pick per age (coop_char_ages.h): the Settings page's "Character by age" rows, shown only
    // when `shown` (the mod and its NPC models are there). `characters`: the pickable ones (number, name), Link first.
    void SetCharacterAges(bool shown, bool on, int child, int adult,
                          std::vector<std::pair<int, std::string>> characters);
    // (Navi) The Settings page's Saves row: solo or multiplayer saves (CoopSave::MultiplayerSaves).
    void SetSaveSet(bool multiplayer);
    // The session's difficulty (CoopDifficulty::Get) and its players: the Settings page's Difficulty rows, among
    // the host's rows (the host changes it there; everyone else sees it, dimmed).
    void SetDifficulty(const DifficultyState& state, int players);
    // Character Select (coop_char_colours.h ShownCharacter): this game shows a character, not Link, so the
    // Customize page says Link's colours wait for Link (#308's live display; the character keeps its own).
    void SetCharacter(bool picked) { character_ = picked; }
    // Character Select's code is in the game (coop_pause_tab.h SnailCodeBuiltIn): his Customize screen, with co-op's
    // rows (coop_snail_custom.h), is the one home of Link's look (the owner, 0.11.9: "one home per thing"). The Customize
    // page keeps Your name and says where the colours are; the main page's Customize Link row says it too.
    void SetSnailHome(bool on) { snail_home_ = on; }
    bool SnailHome() const { return snail_home_; }
    static constexpr const char* kSnailHomeNote = "Link's colours are not on this page: press L until the Characters tab (the first tab of the - menu), pick Link, then Customize.";
    static constexpr const char* kCharacterNote =
        "A Character Select character is picked: it keeps its own colours (X on its Characters page); these colours show on Link when you pick Link again.";
    // The Find a game page's list (the Join page's first row): the games on this network or the relay's public
    // rooms, as the module hears them while the page shows (find_online() says which). A game picked fills the Join
    // page: `join` its room name or its address, `port` its port (0: the default), then Join.
    struct Found {
        std::string name;      // "Saria's game: kokiri", "kokiri (Saria)"
        std::string players;   // "1/4", "Full", "Other version"
        std::string join;      // a room name, or an address
        int port = 0;
        bool joinable = true;
        std::string host;          // its host's name (the search finds it too)
        int count = 0, max = 0;    // its players and the most it takes (the sort, the filter)
        bool same_version = true;
        bool is_public = false;    // a public game (CoopRoomList::Room::is_public): joined at once, no password
        bool online = false;       // the title's list (both kinds): a relay room (else a game on this network)
        bool operator==(const Found&) const = default;
    };
    void SetFound(std::vector<Found> games, std::string state);
    // A private game picked on a public list (Browse games, the Lobby tab): the Join page with `room` typed and its
    // Password row chosen, saying the game needs the host's password. Nothing is tried.
    void PrefillJoin(const std::string& room);
    bool find_online() const { return find_online_; }
    // The title's own pages (CoopPause::OpenNative; docs/hookshot.md "The title menu"): Join opens straight on the
    // list of games, this network's and the relay's public ones in one (the module hands both, Found::online), with
    // "Enter a code or address" after the games and no filter rows; that row's page (the Join page) has Paste a code,
    // Type an address, its Password once an address is in, and Join; a picked game that needs a password shows a
    // page of its password and Join. Host opens on its choices (Who can join first) and hosts on Start hosting only.
    // B goes back one page. Off: the overlay's and the pause tab's pages, unchanged.
    void SetTitle(bool on);
    bool title() const { return title_; }
    // B here leaves the pages (the caller's Multiplayer rows again): Press(kBack) would go to the first page.
    bool BackLeaves() const;
    // The Find page's list asked again (its Refresh row; the title's Y).
    void RefreshFound();
    const std::string& find_search() const { return find_search_; }
    // The Customize page's picture of Link (hos_coop_menu_picture.h); empty
    // on the other pages.
    std::vector<CoopLookMenu::Shape> Picture() const;
    const CoopLook::Look& look() const { return look_; }

    Page page() const { return page_; }
    std::size_t selected() const { return selected_; }
    bool typing() const { return typing_; }
    const CoopTitleMenu::Options& options() const { return options_; }
    const std::string& join_text() const { return join_; }
    const std::string& join_ip() const { return join_ip_; }
    const std::string& join_port() const { return join_port_; }
    const Session& session() const { return session_; }
    View view() const;
    // Co-op settings' first host row (Game): where the pause tab's Host settings row opens the page (0 elsewhere).
    std::size_t HostRulesRow() const;

private:
    enum class Act {
        kNone,
        kOpenHost, kOpenJoin, kOpenLocal, kOpenSettings, kOpenCommands, kCommand,
        kStartHost, kHostPort, kCopyInvite, kPasteAnswer, kCopyAnswer, kCopyLan, kCopyVpn, kLeave,
        kRejoin, kCode, kClearCode, kJoinIp, kJoinPort, kJoinPassword, kJoinNow, kRelay, kRoom,
        kName, kLook, kPlayers, kVoice, kMic, kVoiceMode, kChatKey, kController, kKeyboard, kMapPlayers, kMapNames,
        kSameNetwork, kGameKind,
        kOpenCustomize, kNgPlus, kSaveSet,
        kOpenFind, kFindList, kFindSearch, kFindGame, kFindFilter, kFindSort, kFindRefresh, kFindPage,
        kShowPassword, kNewPassword, kLetIn, kRefuse, kListRoom, kAutoRejoin, kOriginal,
        kApproveJoins, kGoldSkulltulas, kRace, kFloorSwitches, kPauseWorld, kPvp, kPartyWait, kDbno, kDbnoTime, kDbnoHold, kDbnoHearts,
        kDifficulty, kDifficultyLife, kDifficultyDamage, kDifficultyCount,
        kCharAges, kCharChild, kCharAdult,
        kLocalGames, kLocalStart
    };
    struct Line;   // a row, what it does and its help
    std::vector<Line> Lines() const;
    void LauncherLines(std::vector<Line>& out) const;
    void TitleListLines(std::vector<Line>& out, bool off) const;
    void TitleCodeLines(std::vector<Line>& out, bool off) const;
    void TitleHostLines(std::vector<Line>& out, bool host, bool guest) const;
    // The Host page's Code row's value, on the title and the - page alike: the code (ShownCode), "Starting..." until
    // there is one, "None" when none could be made (the page's help says why).
    std::string CodeValue() const;
    void HostRuleLines(std::vector<Line>& out, bool guest) const;
    bool Selectable(const Line& line) const;
    void Go(Page page, Act select = Act::kNone);
    void Clamp();
    void Move(int step);
    void Decide(Act act);
    void Change(Act act, int step);
    void EndTyping(bool keep);
    std::string* Typed();
    void SyncManualJoinFromText();
    std::vector<Found> FoundShown() const;   // the Find page's games the search keeps, in its order
    int FindPage(std::size_t games) const;   // find_page_ within the pages `games` take now
    void UpdateJoinFromIpPort();
    bool InSession() const;

    Page page_ = Page::kMain;
    std::size_t selected_ = 0;
    bool typing_ = false;
    Act typing_act_ = Act::kNone;
    bool typing_fresh_ = false;
    bool paste_on_open_ = false;   // Room or address was just opened with A: the clipboard is asked, used only when
                                   // it holds an invite code or an address line (a pad has no Ctrl+V)
    bool drop_open_paste_ = false;  // a key was typed before that clipboard came (on Linux it can come late): unused
    bool replace_typed_ = false;   // the Join field was opened with a code in it: a typed character starts a new one
    bool confirm_leave_ = false;   // Stop hosting / Leave was pressed once: the next press does it
    bool show_password_ = false;   // the Host page shows the join password (A on its row; hidden on another page)
    std::string new_password_;     // Change join password, while typed (handed to the host on Enter, then cleared)
    CoopTitleMenu::Options options_;
    std::string name_before_;      // the name as it was when its typing began
    std::string port_text_;        // port buffer while typing Host / Join Port
    std::string join_;
    std::string join_ip_;
    std::string join_port_;
    std::string join_password_;
    std::string ip_before_;        // join_ip_ as it was when its typing began
    std::string password_before_;  // join_password_ as it was when its typing began
    std::string rejoin_host_, rejoin_code_;
    std::string controller_row_, keyboard_row_;
    std::optional<std::vector<std::string>> microphones_;
    std::string voice_words_, voice_note_;   // SetVoiceStatus
    int ngplus_level_ = 0, ngplus_unlocked_ = 0, ngplus_hearts_ = 0;   // (Navi) New Game+ (SetNgPlus)
    struct CharAges {   // SetCharacterAges
        bool shown = false, on = false;
        int child = -1, adult = -1;
        std::vector<std::pair<int, std::string>> characters;
    } char_ages_;
    bool mp_saves_ = false;   // (Navi) the Saves row (SetSaveSet)
    DifficultyState difficulty_;   // SetDifficulty
    int difficulty_players_ = 0;
    std::optional<coop::protocol::DifficultyChoice> difficulty_wish_;   // the host's change, until the server says it
    coop::protocol::DifficultyChoice DifficultyShown() const;
    coop::protocol::DifficultyValues DifficultyValuesShown() const;
    void DifficultyLines(std::vector<Line>& out) const;
    // Whether this player's Difficulty rows change something: the host's in the session, or the next lobby's while
    // in none (a guest's and a joining game's are read only).
    bool DifficultyEditable() const;
    bool character_ = false;  // SetCharacter
    bool snail_home_ = false;  // SetSnailHome
    bool title_ = false;      // SetTitle
    std::string picked_;      // the title: the game picked on the list that asks for a password ("": none)
    std::string picked_host_; // (its host, so a long name gives way on the page's note: TitleGameName)
    bool find_online_ = true;                 // the Find a game page lists the relay's rooms (else this network's)
    std::string find_search_, search_before_; // its search, as typed
    int find_filter_ = 0, find_sort_ = 0;      // its filter and order (CoopBrowse)
    int find_page_ = 0;                        // its page of games (kFindPerPage a page; FindPage keeps it in range)
    std::vector<Found> found_;                // what it lists now (SetFound), the search kept
    std::string found_state_;                 // why it lists none ("" looking)
    Session session_;
    LocalGames local_;        // SetLocal
    int local_count_ = kLocalFewest;   // the Games on this PC row (kLocalFewest to kLocalMost)
    void LocalLines(std::vector<Line>& out) const;
    std::vector<std::string> note_;
    std::optional<Request> request_;
    // The Customize page (coop_look_menu.h): it has its own rows and presses.
    CoopLookMenu::Page customize_;
    // Customize Link's first row, above the parts: Your name (the Settings page's own field; a tester asked for the
    // name and the look in one place). Shown while the parts' list is (not in a colour's mixer); true while selected.
    bool customize_name_ = false;
    std::size_t CustomizeFirst() const;   // the parts' list's first row that can be selected
    CoopLook::Look look_;
    void TakeCustomize();
};

}  // namespace CoopPauseMenu
