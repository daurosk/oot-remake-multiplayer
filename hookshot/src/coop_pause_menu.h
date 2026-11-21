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

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
// The most bytes one of the title's own buttons shows (CoopTitleNative::RowText cuts a longer row, "..." at its end).
constexpr std::size_t kTitleRowBytes = 40;
// A game's name on a title row that has `rest` bytes after it ("1/4  Public", " asks for a password"), so the row's own
// words are never the part cut: "room (host)" when it fits, else without " (host)", else the name cut, "..." at its end.
std::string TitleGameName(const std::string& name, const std::string& host, std::size_t rest);

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
                      kSetPassword, kAllow, kDeny, kRefreshFound };
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
    std::vector<std::string> lines;   // what it is doing or what went wrong, in plain words
    std::string password;      // the host's join password while it hosts ("" none): shown on A, never logged
    std::vector<std::string> asking;  // who asks to join this host now, oldest first (CoopLobby::Asking)

    bool operator==(const Session&) const = default;
};

// A row as Hookshot draws it (the numbers are hos_coop_menu.h's HOS_COOP_MENU_*).
enum class RowKind : std::uint8_t { kAction = 0, kChoice = 1, kToggle = 2, kHeading = 3, kNote = 4 };
struct Row {
    std::string name;
    std::string value;
    RowKind kind = RowKind::kAction;
    bool on = false;                  // kToggle
    bool left = false, right = false; // kChoice: it can go that way
    bool fixed = false;               // it cannot be used now
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
    // The look shown now (the one waiting for the menu to close, else the one
    // worn): the Customize page opens on it.
    void SetLook(const CoopLook::Look& look);
    // (Navi) New Game+ (coop_ngplus.h): the level chosen, the highest unlocked (0: Ganon not beaten yet), the
    // hearts carried (quarters). The Settings page's New Game+ row.
    void SetNgPlus(int level, int unlocked, int hearts);
    // (Navi) The Settings page's Saves row: solo or multiplayer saves (CoopSave::MultiplayerSaves).
    void SetSaveSet(bool multiplayer);
    // Character Select (coop_char_colours.h ShownCharacter): this game shows a character, not Link, so the
    // Customize page says Link's colours wait for Link (#308's live display; the character keeps its own).
    void SetCharacter(bool picked) { character_ = picked; }
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

private:
    enum class Act {
        kNone,
        kOpenHost, kOpenJoin, kOpenLocal, kOpenSettings, kOpenCommands, kCommand,
        kStartHost, kHostPort, kCopyInvite, kPasteAnswer, kCopyAnswer, kCopyLan, kCopyVpn, kLeave,
        kRejoin, kCode, kClearCode, kJoinIp, kJoinPort, kJoinPassword, kJoinNow, kRelay, kRoom,
        kName, kLook, kPlayers, kVoice, kMic, kVoiceMode, kChatKey, kController, kKeyboard, kMapPlayers, kMapNames,
        kSameNetwork, kGameKind,
        kOpenCustomize, kNgPlus, kSaveSet,
        kOpenFind, kFindList, kFindSearch, kFindGame, kFindFilter, kFindSort, kFindRefresh,
        kShowPassword, kNewPassword, kLetIn, kRefuse, kListRoom, kAutoRejoin, kOriginal
    };
    struct Line;   // a row, what it does and its help
    std::vector<Line> Lines() const;
    void LauncherLines(std::vector<Line>& out) const;
    void TitleListLines(std::vector<Line>& out, bool off) const;
    void TitleCodeLines(std::vector<Line>& out, bool off) const;
    void TitleHostLines(std::vector<Line>& out, bool host, bool guest) const;
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
    int ngplus_level_ = 0, ngplus_unlocked_ = 0, ngplus_hearts_ = 0;   // (Navi) New Game+ (SetNgPlus)
    bool mp_saves_ = false;   // (Navi) the Saves row (SetSaveSet)
    bool character_ = false;  // SetCharacter
    bool title_ = false;      // SetTitle
    std::string picked_;      // the title: the game picked on the list that asks for a password ("": none)
    std::string picked_host_; // (its host, so a long name gives way on the page's note: TitleGameName)
    bool find_online_ = true;                 // the Find a game page lists the relay's rooms (else this network's)
    std::string find_search_, search_before_; // its search, as typed
    int find_filter_ = 0, find_sort_ = 0;      // its filter and order (CoopBrowse)
    std::vector<Found> found_;                // what it lists now (SetFound), the search kept
    std::string found_state_;                 // why it lists none ("" looking)
    Session session_;
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
