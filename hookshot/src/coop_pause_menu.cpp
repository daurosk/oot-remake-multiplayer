// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_pause_menu.h"

#include <algorithm>
#include <utility>

#include "coop_i18n.h"

namespace CoopPauseMenu {

namespace {

using CoopTitleMenu::CodePoints;
using CoopTitleMenu::Trim;
using State = Session::State;

constexpr std::size_t kMaxJoinBytes = 300;

std::string Joined(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) {
        if (line.empty()) {
            continue;
        }
        out += out.empty() ? "" : " ";
        out += line;
    }
    return out;
}

// The same, each line in the player's language (coop_i18n.h): the lines are
// looked up one by one, since only a whole line is in the table.
std::string JoinedTr(const std::vector<std::string>& lines) {
    std::vector<std::string> translated;
    translated.reserve(lines.size());
    for (const std::string& line : lines) {
        translated.push_back(CoopI18n::Tr(line));
    }
    return Joined(translated);
}

Row Action(std::string name, std::string value = {}, bool fixed = false) {
    Row r;
    r.name = std::move(name);
    r.value = std::move(value);
    r.fixed = fixed;
    return r;
}

Row Choice(std::string name, std::string value) {
    Row r;
    r.name = std::move(name);
    r.value = std::move(value);
    r.kind = RowKind::kChoice;
    r.left = r.right = true;
    return r;
}

Row Toggle(std::string name, bool on) {
    Row r;
    r.name = std::move(name);
    r.kind = RowKind::kToggle;
    r.on = on;
    return r;
}

Row Note(std::string text) {
    Row r;
    r.name = std::move(text);
    r.kind = RowKind::kNote;
    return r;
}

const char* const kByLauncher =
    "Co-op started with the game (Host.bat, Join.bat or coop.ini). To host or join another game, quit and start the "
    "game again.";

}  // namespace

const std::vector<Command>& Commands() {
    static const std::vector<Command> list = {
        {"/teleport", "Lists the other players, with their numbers."},
        {"/teleport <player>",
         "Takes you to that player: the whole name, the start of it, or the player's number (/teleport 2). In another "
         "place, talk to someone or read a sign within a minute to travel. Not during a cutscene, a talk or a boss "
         "fight."},
        {"/tp <player>", "The same as /teleport."},
        {"/pvp", "Says whether players can hurt each other (PvP) in this game."},
        {"/pvp on, /pvp off", "The host only: lets players hurt each other, or stops it, for everyone."},
    };
    return list;
}

std::string ChatKeyWords(const std::string& chat_key) {
    std::string k = chat_key.empty() ? std::string("T") : chat_key;
    std::string low = k;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return low == "off" ? std::string("Off") : k;
}

// ---------------------------------------------------------------------------
// The pages' rows.

bool Menu::InSession() const {
    switch (session_.state) {
    case State::kStarting:
    case State::kHosting:
    case State::kJoining:
    case State::kWaitHost:
    case State::kJoined:
        return true;
    case State::kOff:
    case State::kFailed:
        break;
    }
    return session_.by_launcher;
}

struct Menu::Line {
    Row row;
    Act act = Act::kNone;
    std::string help;
};

std::vector<Menu::Line> Menu::Lines() const {
    const bool mine = InSession() && !session_.by_launcher;
    const bool host = mine && session_.hosting, guest = mine && !session_.hosting;
    std::vector<Line> out;
    switch (page_) {
    case Page::kMain:
        out.push_back({Action("Host a game", host ? (session_.state == State::kHosting ? "Hosting" : "Starting...") : "",
                              session_.by_launcher || guest),
                       Act::kOpenHost,
                       session_.by_launcher ? kByLauncher
                       : guest ? "You are in a friend's game. To host your own, leave it first: Join a game, then Leave the game."
                       : host ? "Your game: the code for your friends, who joined, and Stop hosting."
                              : "Starts a game on this PC and makes a code. Send the code to your friends; they choose Join a "
                                "game and paste it. They play on your save."});
        out.push_back({Action("Join a game", guest ? (session_.state == State::kJoined ? "Joined" : "Joining...") : "",
                              session_.by_launcher || host),
                       Act::kOpenJoin,
                       session_.by_launcher ? kByLauncher
                       : host ? "You are hosting. To join a friend's game, stop yours first: Host a game, then Stop hosting."
                       : guest ? "The game you joined, and Leave the game."
                               : "Joins a friend's game with the code they sent you. You play on their save; your own saves "
                                 "are not touched."});
        out.push_back({Action("Local co-op"), Act::kOpenLocal, "Two players on this PC."});
        out.push_back({Action("Settings"), Act::kOpenSettings,
                       "Your name, Link's look, how many players can join your game, and voice chat."});
        out.push_back({Action("Commands"), Act::kOpenCommands, "The chat's commands, and the key that opens the chat."});
        break;
    case Page::kHost:
        if (host) {
            out.push_back({Action("Copy the code again", "Copy", session_.code.empty()), Act::kCopyInvite,
                           "Puts your code on the clipboard again, to paste into your chat with your friends."});
            out.push_back({Action("Paste friend's answer", "Paste"), Act::kPasteAnswer,
                           "Only when a friend's game shows an answer code (their way in was blocked): copy the code they "
                           "send you, then press this."});
            out.push_back({Action("Same network or VPN", "Copy the line"), Act::kCopyLan,
                           "For friends on your home network or on a VPN with you (Radmin VPN, Hamachi, Tailscale): copies "
                           "one line with this PC's address. They paste it in Join a game instead of the code."});
            out.push_back({Action("Stop hosting", confirm_leave_ ? "Press again to confirm" : "Stop"), Act::kLeave,
                           "Ends your game for everyone in it."});
        } else {
            out.push_back({Action("Start hosting", "Start", session_.by_launcher || guest), Act::kStartHost,
                           "Starts a game on this PC and makes a code for your friends."});
            out.push_back({Choice("Players", std::to_string(options_.max_players)), Act::kPlayers,
                           "The most players in your game, you included."});
        }
        break;
    case Page::kJoin:
        if (guest) {
            if (session_.state == State::kWaitHost) {
                out.push_back({Action("Copy your answer code", "Copy", session_.code.empty()), Act::kCopyAnswer,
                               "Your friend's router blocks the way in: send them this answer code (it is copied "
                               "already); they paste it on their Host page."});
            }
            out.push_back({Action("Leave the game", confirm_leave_ ? "Press again to confirm" : "Leave"), Act::kLeave,
                           "Leaves your friend's game. Their game goes on without you."});
        } else {
            const bool off = session_.by_launcher || host;
            if (!rejoin_code_.empty()) {
                out.push_back({Action("Join " + rejoin_host_ + " again", "Join", off), Act::kRejoin,
                               "Joins the last game you were in, with the same code. It works while your friend is still "
                               "hosting that game."});
            }
            const bool typing = typing_ && page_ == Page::kJoin;
            out.push_back({Action(rejoin_code_.empty() ? "Friend's code" : "Or a new code",
                                  typing ? join_ + "_" : join_.empty() ? std::string("Paste") : join_, off),
                           Act::kCode,
                           "Copy the code from your friend's message, then press this: it is pasted. On the same home "
                           "network or VPN as your friend? Paste the line from Same network or VPN on their Host page "
                           "instead (address and password). You can also type it."});
            out.push_back({Action("Join", "Join", off || Trim(join_).empty()), Act::kJoinNow,
                           Trim(join_).empty() ? "Paste your friend's code first." : "Joins your friend's game."});
        }
        break;
    case Page::kLocal:
        out.push_back({Note("Split screen: two players on this PC."), Act::kNone, {}});
        out.push_back({Note("Coming in the next test release."), Act::kNone, {}});
        break;
    case Page::kCommands: {
        const std::string key = ChatKeyWords(options_.chat_key);
        out.push_back({Action("Open the chat", key), Act::kCommand,
                       key == "Off" ? "The chat key is off (LICHII_COOP_CHAT_KEY=off in coop-options.ini): set a key "
                                      "there (a letter, a digit, or F1 to F12) to chat."
                                    : "Press " + key + " in the game to open the chat box; Enter sends, Esc closes. "
                                      "A line that starts with / is a command, not chat. Another key: "
                                      "LICHII_COOP_CHAT_KEY in coop-options.ini."});
        for (const Command& c : Commands()) {
            out.push_back({Action(c.typed), Act::kCommand, c.help});
        }
        break;
    }
    case Page::kSettings:
        out.push_back({Action("Your name", typing_ ? options_.name + "_" : options_.name), Act::kName,
                       "The name the other players see: 1 to 16 characters, typed with the keyboard."});
        out.push_back({Choice("Link's look", options_.colour.empty() ? std::string("By player number") : options_.colour),
                       Act::kLook,
                       "The colour of your Link's tunic and cap, as you and the other players see it. By player "
                       "number: each player's own colour."});
        out.push_back({Choice("Players when you host", std::to_string(options_.max_players)), Act::kPlayers,
                       "The most players in a game you host, you included."});
        out.push_back({Toggle("Proximity voice chat", options_.voice), Act::kVoice,
                       "Talk with players near your Link through your microphone; they sound quieter the farther away "
                       "they are. Off: you neither send nor hear voices."});
        break;
    }
    return out;
}

bool Menu::Selectable(const Line& line) const {
    return line.row.kind != RowKind::kHeading && line.row.kind != RowKind::kNote;
}

void Menu::Go(Page page, Act select) {
    page_ = page;
    typing_ = false;
    confirm_leave_ = false;
    selected_ = 0;
    const auto lines = Lines();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].act == select && Selectable(lines[i])) {
            selected_ = i;
            return;
        }
    }
    Clamp();
}

// The selection on a row that can have it, when the rows changed under it.
void Menu::Clamp() {
    const auto lines = Lines();
    if (lines.empty()) {
        selected_ = 0;
        return;
    }
    selected_ = std::min(selected_, lines.size() - 1);
    for (std::size_t n = 0; n < lines.size() && !Selectable(lines[selected_]); ++n) {
        selected_ = (selected_ + 1) % lines.size();
    }
}

void Menu::Move(int step) {
    const auto lines = Lines();
    const std::size_t n = lines.size();
    if (n == 0) {
        return;
    }
    std::size_t at = selected_;
    for (std::size_t tries = 0; tries < n; ++tries) {
        at = (at + n + static_cast<std::size_t>(step < 0 ? n - 1 : 1)) % n;
        if (Selectable(lines[at])) {
            selected_ = at;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// From the module.

void Menu::Show(const CoopTitleMenu::Options& options) {
    options_ = options;
    typing_ = false;
    confirm_leave_ = false;
    note_.clear();
    Clamp();
}

void Menu::Hide() {
    EndTyping(false);
    confirm_leave_ = false;
    note_.clear();
}

void Menu::Open(Page page) {
    Act act = Act::kNone;
    switch (page) {
    case Page::kMain: break;
    case Page::kHost: act = Act::kOpenHost; break;
    case Page::kJoin: act = Act::kOpenJoin; break;
    case Page::kLocal: act = Act::kOpenLocal; break;
    case Page::kSettings: act = Act::kOpenSettings; break;
    case Page::kCommands: act = Act::kOpenCommands; break;
    }
    Go(Page::kMain, act);
    note_.clear();
    if (act != Act::kNone) {
        Decide(act);
    }
}

void Menu::SetSession(Session session) {
    if (session == session_) {
        return;
    }
    const auto before = Lines();
    const Act act = selected_ < before.size() ? before[selected_].act : Act::kNone;
    session_ = std::move(session);
    const auto after = Lines();
    if (typing_ && page_ == Page::kJoin &&
        std::none_of(after.begin(), after.end(), [](const Line& l) { return l.act == Act::kCode && !l.row.fixed; })) {
        typing_ = false;   // (the code's field went away under the typing)
    }
    // The same row selected when it is still there (the rows of Host and Join change with the session).
    const auto same = std::find_if(after.begin(), after.end(), [act](const Line& l) { return l.act == act; });
    if (same != after.end()) {
        selected_ = static_cast<std::size_t>(same - after.begin());
    } else {
        selected_ = 0;
        confirm_leave_ = false;
    }
    Clamp();
}

void Menu::SetNote(std::vector<std::string> lines) {
    note_ = std::move(lines);
}

void Menu::SetRejoin(std::string host, std::string code) {
    const auto before = Lines();
    // A row that comes now is chosen (one press joins again); else the row chosen stays.
    const bool came = rejoin_code_.empty() && !Trim(code).empty() && !typing_;
    const Act act = came ? Act::kRejoin : selected_ < before.size() ? before[selected_].act : Act::kNone;
    rejoin_host_ = Trim(host).empty() ? std::string("your friend") : std::string(Trim(host));
    rejoin_code_ = std::string(Trim(code));
    const auto after = Lines();
    const auto same = std::find_if(after.begin(), after.end(), [act](const Line& l) { return l.act == act; });
    selected_ = same != after.end() ? static_cast<std::size_t>(same - after.begin()) : 0;
    Clamp();
}

std::optional<Request> Menu::TakeRequest() {
    auto r = std::move(request_);
    request_.reset();
    return r;
}

// ---------------------------------------------------------------------------
// The player's presses.

std::string* Menu::Typed() {
    if (!typing_) {
        return nullptr;
    }
    return page_ == Page::kSettings ? &options_.name : page_ == Page::kJoin ? &join_ : nullptr;
}

// Typing ends. keep: with what was typed (a name the server would refuse is
// not kept); else a name goes back to what it was.
void Menu::EndTyping(bool keep) {
    if (!typing_) {
        return;
    }
    typing_ = false;
    if (page_ == Page::kSettings) {
        if (keep && CoopTitleMenu::NameOk(options_.name)) {
            options_.name = std::string(Trim(options_.name));
            if (options_.name != name_before_) {
                request_ = Request{Request::Kind::kSave, {}};
            }
            return;
        }
        if (keep) {
            note_ = {"Your name: 1 to 16 characters. It was not changed."};
        }
        options_.name = name_before_;
    } else if (page_ == Page::kJoin && keep && !Trim(join_).empty()) {
        Go(Page::kJoin, Act::kJoinNow);
    }
}

bool Menu::Press(Input input) {
    note_.clear();
    const auto lines = Lines();
    const Line* line = selected_ < lines.size() && Selectable(lines[selected_]) ? &lines[selected_] : nullptr;
    if (!(input == Input::kDecide && line != nullptr && line->act == Act::kLeave)) {
        confirm_leave_ = false;
    }
    if (typing_) {
        switch (input) {
        case Input::kUp:
        case Input::kDown:
        {
            const bool to_join = page_ == Page::kJoin && !Trim(join_).empty();   // (EndTyping goes to Join)
            EndTyping(true);
            if (!to_join) {
                Move(input == Input::kUp ? -1 : 1);
            }
            return true;
        }
        case Input::kDecide:
            EndTyping(true);
            return true;
        case Input::kBack:
            EndTyping(false);
            return true;
        case Input::kPaste:
            request_ = Request{Request::Kind::kPaste, {}};
            return true;
        case Input::kErase:
            if (std::string* t = Typed(); t != nullptr && !t->empty()) {
                CoopTitleMenu::PopCodePoint(*t);
            }
            return true;
        case Input::kLeft:
        case Input::kRight:
            return true;
        }
        return true;
    }
    switch (input) {
    case Input::kUp:
        Move(-1);
        return true;
    case Input::kDown:
        Move(1);
        return true;
    case Input::kLeft:
    case Input::kRight:
        if (line != nullptr && !line->row.fixed && line->row.kind != RowKind::kAction) {
            Change(line->act, input == Input::kLeft ? -1 : 1);
        }
        return true;
    case Input::kDecide:
        if (line == nullptr) {
            return true;
        }
        if (line->row.fixed) {
            note_ = {line->help};   // (why not, said again where the eye is)
            return true;
        }
        Decide(line->act);
        return true;
    case Input::kBack:
        switch (page_) {
        case Page::kMain:
            return false;
        case Page::kHost:
            Go(Page::kMain, Act::kOpenHost);
            break;
        case Page::kJoin:
            Go(Page::kMain, Act::kOpenJoin);
            break;
        case Page::kLocal:
            Go(Page::kMain, Act::kOpenLocal);
            break;
        case Page::kSettings:
            Go(Page::kMain, Act::kOpenSettings);
            break;
        case Page::kCommands:
            Go(Page::kMain, Act::kOpenCommands);
            break;
        }
        return true;
    case Input::kPaste:
        if (page_ == Page::kJoin && std::any_of(lines.begin(), lines.end(), [](const Line& l) {
                return l.act == Act::kCode && !l.row.fixed;
            })) {
            request_ = Request{Request::Kind::kPaste, {}};
        }
        return true;
    case Input::kErase:
        return true;
    }
    return true;
}

void Menu::Decide(Act act) {
    switch (act) {
    case Act::kOpenHost:
        Go(Page::kHost);
        if (!InSession()) {   // one press hosts; the page then shows the code
            request_ = Request{Request::Kind::kHost, {}};
        }
        break;
    case Act::kStartHost:
        request_ = Request{Request::Kind::kHost, {}};
        break;
    case Act::kOpenJoin:
        Go(Page::kJoin);
        break;
    case Act::kOpenLocal:
        Go(Page::kLocal);
        break;
    case Act::kOpenSettings:
        Go(Page::kSettings);
        break;
    case Act::kOpenCommands:
        Go(Page::kCommands);
        break;
    case Act::kCommand:   // (a line to read: its help says it)
        break;
    case Act::kCopyInvite:
        request_ = Request{Request::Kind::kCopyInvite, {}};
        break;
    case Act::kPasteAnswer:
        request_ = Request{Request::Kind::kPasteAnswer, {}};
        break;
    case Act::kCopyAnswer:
        request_ = Request{Request::Kind::kCopyAnswer, {}};
        break;
    case Act::kCopyLan:
        request_ = Request{Request::Kind::kCopyLan, {}};
        break;
    case Act::kLeave:
        if (!confirm_leave_) {
            confirm_leave_ = true;
            break;
        }
        confirm_leave_ = false;
        request_ = Request{Request::Kind::kLeave, {}};
        break;
    case Act::kRejoin:
        request_ = Request{Request::Kind::kJoin, rejoin_code_};
        break;
    case Act::kCode:
        typing_ = true;
        if (join_.empty()) {   // the code is on the clipboard as a rule: pasted at once
            request_ = Request{Request::Kind::kPaste, {}};
        }
        break;
    case Act::kJoinNow:
        request_ = Request{Request::Kind::kJoin, join_};
        break;
    case Act::kName:
        name_before_ = options_.name;
        typing_ = true;
        break;
    case Act::kLook:
    case Act::kPlayers:
        Change(act, 1);
        break;
    case Act::kVoice:
        Change(act, 0);   // (a toggle: A flips it)
        break;
    case Act::kNone:
        break;
    }
}

void Menu::Change(Act act, int step) {
    switch (act) {
    case Act::kLook: {
        const auto& list = CoopTitleMenu::Colours();
        const auto at = std::find(list.begin(), list.end(), options_.colour);
        const std::size_t n = list.size(), i = at == list.end() ? 0 : static_cast<std::size_t>(at - list.begin());
        options_.colour = list[(i + n + static_cast<std::size_t>(step < 0 ? n - 1 : 1)) % n];
        request_ = Request{Request::Kind::kLook, options_.colour};
        break;
    }
    case Act::kPlayers: {
        const int span = CoopTitleMenu::kMaxPlayers - CoopTitleMenu::kMinPlayers + 1;
        const int at = std::clamp(options_.max_players, CoopTitleMenu::kMinPlayers, CoopTitleMenu::kMaxPlayers) -
                       CoopTitleMenu::kMinPlayers;
        options_.max_players = CoopTitleMenu::kMinPlayers + (at + (step < 0 ? span - 1 : 1)) % span;
        request_ = Request{Request::Kind::kSave, {}};
        break;
    }
    case Act::kVoice:
        options_.voice = step > 0 ? true : step < 0 ? false : !options_.voice;
        request_ = Request{Request::Kind::kSave, {}};
        break;
    default:
        break;
    }
}

void Menu::Select(std::size_t row) {
    const auto lines = Lines();
    if (typing_ || row >= lines.size() || !Selectable(lines[row]) || row == selected_) {
        return;
    }
    selected_ = row;
    confirm_leave_ = false;
}

void Menu::Click(std::size_t row, int side) {
    const auto lines = Lines();
    if (row >= lines.size() || !Selectable(lines[row])) {
        return;
    }
    if (typing_ && row != selected_) {
        EndTyping(true);
        return;   // (the click ended the typing; the next one chooses)
    }
    selected_ = row;
    const Line& line = lines[row];
    if (line.row.kind == RowKind::kAction) {
        Press(Input::kDecide);
    } else if (side != 0 && !line.row.fixed) {
        note_.clear();
        confirm_leave_ = false;
        Change(line.act, side);
    }
}

bool Menu::Char(std::uint32_t cp) {
    std::string* t = Typed();
    if (t == nullptr) {
        return false;
    }
    if (cp < 0x20 || cp == 0x7F) {
        return true;
    }
    const std::string add = CoopTitleMenu::Utf8(cp);
    if (add.empty()) {
        return true;
    }
    if (t == &options_.name ? CodePoints(*t) >= CoopTitleMenu::kMaxNameChars : t->size() + add.size() > kMaxJoinBytes) {
        return true;
    }
    *t += add;
    return true;
}

bool Menu::Paste(std::string_view text) {
    const std::string line = std::string(Trim(CoopTitleMenu::OneLine(text)));
    if (page_ == Page::kJoin) {
        const auto lines = Lines();
        if (std::none_of(lines.begin(), lines.end(), [](const Line& l) { return l.act == Act::kCode && !l.row.fixed; })) {
            return false;
        }
        if (line.empty()) {
            note_ = {"Nothing to paste: copy the code from your friend's message first. You can also type it."};
            return true;
        }
        join_ = line.substr(0, kMaxJoinBytes);
        typing_ = false;
        Go(Page::kJoin, Act::kJoinNow);
        return true;
    }
    std::string* t = Typed();
    if (t == nullptr) {
        return false;
    }
    for (std::size_t i = 0; i < line.size();) {   // character by character, so the name's limit holds
        const unsigned char c = static_cast<unsigned char>(line[i]);
        std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        n = std::min(n, line.size() - i);
        if (CodePoints(*t) >= CoopTitleMenu::kMaxNameChars) {
            break;
        }
        *t += line.substr(i, n);
        i += n;
    }
    return true;
}

// ---------------------------------------------------------------------------

View Menu::view() const {
    View v;
    const auto lines = Lines();
    for (const Line& l : lines) {
        v.rows.push_back(l.row);
    }
    const Line* line = selected_ < lines.size() && Selectable(lines[selected_]) ? &lines[selected_] : nullptr;
    v.selected = line != nullptr ? static_cast<int>(selected_) : -1;
    v.typing = typing_;
    const bool mine = InSession() && !session_.by_launcher;
    // What Host or Join is doing, or why it failed, on its own page: there the row's help gives way to it.
    const bool host_page = page_ == Page::kHost && session_.hosting, join_page = page_ == Page::kJoin && !session_.hosting;
    const bool said = !session_.by_launcher && (host_page || join_page) && !session_.lines.empty() &&
                      (mine || session_.state == State::kFailed);
    switch (page_) {
    case Page::kMain:
        v.help_title = "Multiplayer";
        break;
    case Page::kHost:
        v.help_title = "Host a game";
        break;
    case Page::kJoin:
        v.help_title = "Join a game";
        break;
    case Page::kLocal:
        v.help_title = "Local co-op";
        break;
    case Page::kSettings:
        v.help_title = "Settings";
        break;
    case Page::kCommands:
        v.help_title = "Commands";
        break;
    }
    if (!note_.empty()) {
        v.help = JoinedTr(note_);
    } else if (said && !(line != nullptr && confirm_leave_)) {
        v.help = JoinedTr(session_.lines);
    } else if (line != nullptr) {
        v.help = line->help;
    }
    if (mine && (host_page || join_page)) {
        v.hint = session_.code;
    }
    if (session_.by_launcher) {
        v.gold = session_.players > 1 ? "Co-op is on: " + std::to_string(session_.players) + " players" : "Co-op is on";
    } else if (mine && session_.hosting) {
        v.gold = session_.state != State::kHosting ? "Starting your game..."
                 : session_.players > 1 ? "Hosting: " + std::to_string(session_.players) + " players"
                                        : "Hosting: waiting for friends";
    } else if (mine) {
        v.gold = session_.state == State::kJoined ? "In a friend's game" : "Joining...";
    }
    if (typing_) {
        v.confirm = "Done";
        v.back = "Cancel";
        return v;
    }
    if (line != nullptr) {
        if (line->act == Act::kCommand) {
            // (nothing to press: the row is read)
        } else if (line->row.kind == RowKind::kAction) {
            v.confirm = line->row.value.empty() ? "Open" : line->act == Act::kName ? "Type" : "Choose";
        } else {
            v.change = "Change";
        }
    }
    v.back = page_ == Page::kMain ? "Close" : "Back";
    // The prompt strip's words in the player's language, translated here: Hookshot
    // draws the strip with its own table (its "Choose", "Back" are the graphics
    // page's words), and a word the tab wants said its own way -- "执行" on a row
    // that acts, beside the "选择" its arrows carry -- would be overwritten there.
    v.confirm = v.confirm.empty() ? std::string() : CoopI18n::Tr(v.confirm);
    v.change = v.change.empty() ? std::string() : CoopI18n::Tr(v.change);
    v.back = CoopI18n::Tr(v.back);
    return v;
}

}  // namespace CoopPauseMenu
