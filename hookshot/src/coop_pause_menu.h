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

#include "coop_title_menu.h"

namespace CoopPauseMenu {

enum class Input { kUp, kDown, kLeft, kRight, kDecide, kBack, kPaste, kErase };
enum class Page { kMain, kHost, kJoin, kLocal, kSettings };

struct Request {
    // kHost: start the server and join it. kJoin: `text` is the code or the
    // address (CoopTitleMenu::ReadJoin says which). kLeave: stop hosting, or
    // leave the friend's game. kSave: write options() to the files. kLook:
    // `text` is the colour chosen for Link ("" the player number's); worn when
    // the menu closes. kPaste: hand the clipboard's text to Paste.
    // kCopyInvite: the session's invite code onto the clipboard. kPasteAnswer:
    // the friend's ANSWER CODE from the clipboard to the host. kCopyLan: the
    // line for friends on the same network or VPN onto the clipboard.
    enum class Kind { kHost, kJoin, kLeave, kSave, kLook, kPaste, kCopyInvite, kPasteAnswer, kCopyLan };
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
    bool by_launcher = false;  // co-op came with the game's start (Host.bat, Join.bat, coop.ini): not stopped here
    int players = 0;           // in the session, this one included (0: not known)
    std::string code;          // the invite code (hosting), or the answer code (kWaitHost)
    std::vector<std::string> lines;   // what it is doing or what went wrong, in plain words

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
    // The last code this game joined with, offered first on the Join page as
    // "Join <host> again" (one press). An empty code: no such row.
    void SetRejoin(std::string host, std::string code);

    Page page() const { return page_; }
    std::size_t selected() const { return selected_; }
    bool typing() const { return typing_; }
    const CoopTitleMenu::Options& options() const { return options_; }
    const std::string& join_text() const { return join_; }
    const Session& session() const { return session_; }
    View view() const;

private:
    enum class Act {
        kNone,
        kOpenHost, kOpenJoin, kOpenLocal, kOpenSettings,
        kStartHost, kCopyInvite, kPasteAnswer, kCopyLan, kLeave,
        kRejoin, kCode, kJoinNow,
        kName, kLook, kPlayers, kVoice
    };
    struct Line;   // a row, what it does and its help
    std::vector<Line> Lines() const;
    bool Selectable(const Line& line) const;
    void Go(Page page, Act select = Act::kNone);
    void Clamp();
    void Move(int step);
    void Decide(Act act);
    void Change(Act act, int step);
    void EndTyping(bool keep);
    std::string* Typed();
    bool InSession() const;

    Page page_ = Page::kMain;
    std::size_t selected_ = 0;
    bool typing_ = false;
    bool confirm_leave_ = false;   // Stop hosting / Leave was pressed once: the next press does it
    CoopTitleMenu::Options options_;
    std::string name_before_;      // the name as it was when its typing began
    std::string join_;
    std::string rejoin_host_, rejoin_code_;
    Session session_;
    std::vector<std::string> note_;
    std::optional<Request> request_;
};

}  // namespace CoopPauseMenu
