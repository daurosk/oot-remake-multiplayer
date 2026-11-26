// SPDX-License-Identifier: GPL-3.0-or-later
// The Lobby tab's logic (coop_lobby_tab.h).
#include "coop_lobby_tab.h"

#include <algorithm>
#include <utility>

#include "coop_i18n.h"
#include "coop_list_pages.h"

namespace CoopLobbyTab {

namespace {

namespace proto = coop::protocol;
using CoopPauseMenu::Row;
using CoopPauseMenu::RowKind;

constexpr std::uint8_t kVoice = proto::lobby_mute::voice;
constexpr std::uint8_t kChat = proto::lobby_mute::chat;
// The list shows an "Open place" row for each free place up to this many
// players; past it one line says how many more can join.
constexpr int kMostSlotRows = 4;

// "3.5", not "3 and a half": a number reads the same in every language's table
// (coop_i18n.h); the word after it ("hearts") is the table's.
std::string Hearts(std::uint16_t quarters) {
    const int whole = quarters / 4;
    switch (quarters % 4) {
    case 2: return std::to_string(whole) + ".5";
    case 1: return std::to_string(whole) + ".25";
    case 3: return std::to_string(whole) + ".75";
    default: return std::to_string(whole);
    }
}

std::string MutedWords(std::uint8_t bits) {
    if ((bits & kVoice) != 0 && (bits & kChat) != 0) {
        return "voice and chat";
    }
    return (bits & kVoice) != 0 ? "voice" : "chat";
}

std::string Shorten(const std::string& s, std::size_t most) {
    if (s.size() <= most) {
        return s;
    }
    std::size_t at = most;   // (back to the start of the character the cut would split)
    while (at > 0 && (static_cast<unsigned char>(s[at]) & 0xC0) == 0x80) {
        --at;
    }
    return s.substr(0, at) + "...";
}

}  // namespace

std::string Summary(const Player& p) {
    std::vector<std::string> parts;
    if (p.ms) {
        // The unit is the table's ("40 ms" / "40 毫秒"): the number is data.
        parts.push_back(CoopI18n::Tr(std::to_string(*p.ms) + " ms"));
    }
    if (!p.place.empty()) {
        parts.push_back(p.place);
    }
    switch (p.life) {
    case Player::Life::up: break;
    case Player::Life::downed: parts.push_back(CoopI18n::Tr("downed")); break;
    case Player::Life::out: parts.push_back(CoopI18n::Tr("out")); break;
    case Player::Life::dead: parts.push_back(CoopI18n::Tr("fainted")); break;
    }
    if (p.talking) {
        parts.push_back(CoopI18n::Tr("talking"));
    }
    if (p.host_mutes != 0 || p.own_mutes != 0) {
        parts.push_back(CoopI18n::Tr("muted"));
    }
    std::string out;
    for (const auto& s : parts) {
        out += out.empty() ? "" : "  ";
        out += s;
    }
    return out;
}

struct Tab::Line {
    Row row;
    Act act = Act::kNone;
    std::uint8_t player = 0;
    std::string help;
};

const Player* Tab::Shown() const {
    for (const auto& p : lobby_.players) {
        if (p.number == player_) {
            return &p;
        }
    }
    return nullptr;
}

std::vector<Tab::Line> Tab::Lines() const {
    std::vector<Line> out;
    // (`own`: the name is a player's or a room's own text, or already in the player's language: shown as it is, never
    // looked up in the table: a player called "Link" stays "Link")
    const auto heading = [&](std::string name, bool own = false) {
        Line l;
        l.row.name = std::move(name);
        l.row.own_name = own;
        l.row.kind = RowKind::kHeading;
        out.push_back(std::move(l));
    };
    const auto note = [&](std::string text, bool own = false) {
        Line l;
        l.row.name = std::move(text);
        l.row.own_name = own;
        l.row.kind = RowKind::kNote;
        out.push_back(std::move(l));
    };
    const auto action = [&](std::string name, std::string value, Act act, std::string help, std::uint8_t player = 0,
                            bool own = false) {
        Line l;
        l.row.name = std::move(name);
        l.row.value = std::move(value);
        l.row.own_name = l.row.own_value = own;   // (the name and the value: a room's, a player's; the value already ours)
        l.act = act;
        l.player = player;
        l.help = std::move(help);
        out.push_back(std::move(l));
    };
    const auto toggle = [&](std::string name, bool on, Act act, std::string help) {
        Line l;
        l.row.name = std::move(name);
        l.row.kind = RowKind::kToggle;
        l.row.on = on;
        l.act = act;
        l.help = std::move(help);
        out.push_back(std::move(l));
    };

    if (!lobby_.connected && !lobby_.waiting.empty()) {
        heading("Lobby");
        note(lobby_.waiting);
        note(lobby_.waiting_next.empty() ? "The players show here once this game is in the session."
                                         : lobby_.waiting_next);
        return out;
    }
    if (!lobby_.connected) {
        heading("Lobby");
        note("You are not in a game.");
        note("Host one or join a friend's on the Multiplayer tab; the players show here.");
        // The relay's public games (the same list and words as Browse games): Join with one press.
        heading("Public games");
        if (lobby_.games.empty()) {
            note(lobby_.games_state.empty() ? std::string("Looking for public games...") : lobby_.games_state);
        }
        // (more than a page: the Page row first, "Page  < 2/3 >", Left and Right or A turn it, round; then that page's
        // games. Row 264: the whole list ran on under the menu's scroll, with nothing saying there were more)
        const int pages = GamesPages(), page = GamesPage();
        if (pages > 1) {
            Line l;
            l.row.name = "Page";
            l.row.value = std::to_string(page + 1) + "/" + std::to_string(pages);
            l.row.own_value = true;   // (two numbers, as they are)
            l.row.kind = RowKind::kChoice;
            l.row.left = l.row.right = true;   // (round: both ways always)
            l.act = Act::kGamesPage;
            l.help = "Left and Right (or A) turn the page of games.";
            out.push_back(std::move(l));
        }
        const std::size_t first = static_cast<std::size_t>(page) * kGamesPerPage;
        for (std::size_t i = first; i < lobby_.games.size() && i < first + kGamesPerPage && i < 255; ++i) {
            const PublicGame& g = lobby_.games[i];
            action(g.name, g.joinable ? CoopI18n::Tr("Join") : g.detail, Act::kJoinGame,
                   g.detail + (g.joinable ? std::string(". ") +
                                                (g.is_public ? CoopI18n::Tr("Press to join: no password needed.")
                                                             : CoopI18n::Tr("Press, then type the host's password."))
                                          : std::string()),
                   static_cast<std::uint8_t>(i), true);
        }
        action("Refresh list", "Refresh", Act::kRefreshGames, "Asks the game list again now.");
        return out;
    }

    const Player* p = Shown();
    if (p == nullptr) {
        const int count = static_cast<int>(lobby_.players.size());
        heading(lobby_.max_players > 0
                    ? "Players  " + std::to_string(count) + " / " + std::to_string(lobby_.max_players)
                    : "Players  " + std::to_string(count));
        for (const auto& pl : lobby_.players) {
            std::string name = pl.host ? CoopI18n::Tr("Host: ") + pl.name : pl.name;
            if (pl.self) {
                name += CoopI18n::Tr(" (you)");
            }
            action(std::move(name), Summary(pl), Act::kPlayer,
                   CoopI18n::Tr("Player") + " " + std::to_string(pl.number) +
                       (pl.colour.empty() ? "" : CoopI18n::Tr(", ") + pl.colour) +
                       (pl.host ? CoopI18n::Tr(", the host") : std::string()) + CoopI18n::Tr(". Press to see ") +
                       CoopI18n::Tr(pl.self ? "yourself." : "what you can do."),
                   pl.number, true);
        }
        if (lobby_.max_players > count) {
            const int open = lobby_.max_players - count;
            if (lobby_.max_players <= kMostSlotRows) {
                for (int i = 0; i < open; ++i) {
                    note("Open place");
                }
            } else {
                // The count is data; the words after it are the table's (a shape: "3 more players can join").
                note(CoopI18n::Tr(std::to_string(open) +
                                  (open == 1 ? " more player can join" : " more players can join")));
            }
        }
        // Before a file is loaded (at the title or the file select) this game has no place yet.
        if (std::any_of(lobby_.players.begin(), lobby_.players.end(),
                        [](const Player& pl) { return pl.self && pl.place.empty(); })) {
            note("Load your file to play: the others see you once you do.");
        }
        heading("Invite");
        if (lobby_.hosting && !lobby_.invite.empty()) {
            action("Copy the invite code", Shorten(lobby_.invite, 24), Act::kCopy,
                   "Puts the invite code on the clipboard. Share it in DMs only (it contains your internet address); your friend pastes it in Join a game.");
        } else if (lobby_.hosting) {
            note("The invite code is not ready yet (see the Multiplayer tab).");
        } else {
            note("Only the host has the invite code: ask them for it.");
        }
        heading("Host's game");
        // The place comes already through the table (as each player's does: the module's
        // Place()); the counts are data, and "10 / 10 hearts" is a shape the table fills
        // ("10 / 10 颗心").
        std::string game = lobby_.host_place.empty() ? CoopI18n::Tr("Not known yet") : lobby_.host_place;
        if (lobby_.host_life && lobby_.host_max && *lobby_.host_max > 0) {
            game += "  " +
                    CoopI18n::Tr(Hearts(*lobby_.host_life) + " / " + Hearts(*lobby_.host_max) + " hearts");
        }
        note(std::move(game), true);   // (the place and the counts: already in the table's words)
        if (!lobby_.hosting && lobby_.can_resync) {
            action("Resync from host", "", Act::kResync,
                   "Gets the host's save again. Back on the title, it is switched to once the chat says Success; "
                   "while you play nothing changes until then. Your own saves are never touched.");
        }
        return out;
    }

    heading(p->host ? CoopI18n::Tr("Host: ") + p->name : p->name, true);
    // Each part in the table's words before they are joined: the composed line is
    // not a key the table has ("Player 2, red").
    note(CoopI18n::Tr("Player") + " " + std::to_string(p->number) +
         (p->colour.empty() ? "" : CoopI18n::Tr(", ") + p->colour) +
         (p->self ? CoopI18n::Tr(" (you)") : ""));
    if (const std::string s = Summary(*p); !s.empty()) {
        note(s, true);   // (the parts are the table's words and the place already)
    }
    if (p->host_mutes != 0) {
        note("The host muted their " + MutedWords(p->host_mutes) + " for everyone.");
    }
    if (p->self) {
        note("This is you. Pick another player to mute them or go to them.");
        return out;
    }
    toggle("Mute their voice (only you)", (p->own_mutes & kVoice) != 0, Act::kOwnVoice,
           "You stop hearing them. Only in your game: nobody is told.");
    toggle("Mute their chat (only you)", (p->own_mutes & kChat) != 0, Act::kOwnChat,
           "Their chat lines stop showing in your game. Nobody is told.");
    action("Teleport to them", "", Act::kTeleport,
           "Your Link goes to where they stand, as /teleport does (they must not be busy or in a boss fight).");
    if (lobby_.hosting) {
        heading("Host");
        toggle("Mute their voice for everyone", (p->host_mutes & kVoice) != 0, Act::kHostVoice,
               "The server stops passing their voice on to anyone.");
        toggle("Mute their chat for everyone", (p->host_mutes & kChat) != 0, Act::kHostChat,
               "The server drops their chat lines.");
        action("Kick", confirm_ == Act::kKick ? "Press again to kick" : "", Act::kKick,
               "Takes them out of the game. They can join again.");
        action("Ban for this session", confirm_ == Act::kBan ? "Press again to ban" : "", Act::kBan,
               "Takes them out; they cannot join again, under any name, until you stop hosting.");
    }
    return out;
}

namespace {

bool Selectable(RowKind kind) {
    return kind != RowKind::kHeading && kind != RowKind::kNote;
}

}  // namespace

int Tab::GamesPages() const {
    return CoopListPages::Pages(static_cast<int>(lobby_.games.size()), kGamesPerPage);
}

int Tab::GamesPage() const {
    return std::clamp(games_page_, 0, GamesPages() - 1);
}

void Tab::TurnGames(int dir) {
    games_page_ = CoopListPages::Step(GamesPage(), GamesPages(), dir);
}

void Tab::Show() {
    player_ = 0;
    selected_ = 0;
    games_page_ = 0;
    confirm_ = Act::kNone;
    note_.clear();
    Clamp();
}

void Tab::Hide() {
    confirm_ = Act::kNone;
}

void Tab::Set(Lobby lobby) {
    const bool was_connected = lobby_.connected;
    lobby_ = std::move(lobby);
    // In or out of a game, the page is another list (out of one: Public games): it starts at its top again.
    if (lobby_.connected != was_connected) {
        selected_ = 0;
        games_page_ = 0;
        confirm_ = Act::kNone;
    }
    if (player_ != 0 && (Shown() == nullptr || !lobby_.connected)) {
        player_ = 0;
        selected_ = 0;
        confirm_ = Act::kNone;
    }
    Clamp();
}

void Tab::Go(std::uint8_t player) {
    player_ = player;
    selected_ = 0;
    confirm_ = Act::kNone;
    Clamp();
}

void Tab::Clamp() {
    const auto lines = Lines();
    if (lines.empty()) {
        selected_ = 0;
        return;
    }
    if (selected_ >= lines.size()) {
        selected_ = lines.size() - 1;
    }
    if (Selectable(lines[selected_].row.kind)) {
        return;
    }
    for (std::size_t i = selected_; i < lines.size(); ++i) {
        if (Selectable(lines[i].row.kind)) {
            selected_ = i;
            return;
        }
    }
    for (std::size_t i = selected_; i-- > 0;) {
        if (Selectable(lines[i].row.kind)) {
            selected_ = i;
            return;
        }
    }
}

void Tab::Move(int step) {
    const auto lines = Lines();
    for (std::size_t i = selected_;;) {
        if (step < 0 ? i == 0 : i + 1 >= lines.size()) {
            return;  // no wrap
        }
        i = step < 0 ? i - 1 : i + 1;
        if (Selectable(lines[i].row.kind)) {
            selected_ = i;
            return;
        }
    }
}

void Tab::Change(const Line& line, int side) {
    const Player* p = Shown();
    if (p == nullptr) {
        return;
    }
    const bool on = side > 0 ? true : side < 0 ? false : !line.row.on;
    if (on == line.row.on) {
        return;
    }
    Request r;
    switch (line.act) {
    case Act::kOwnVoice:
    case Act::kOwnChat:
        r.kind = Request::Kind::kOwnMute;
        r.player = p->number;
        r.what = line.act == Act::kOwnVoice ? kVoice : kChat;
        r.on = on;
        break;
    case Act::kHostVoice:
        r.action = {on ? proto::LobbyActionKind::mute_voice : proto::LobbyActionKind::unmute_voice, p->number};
        break;
    case Act::kHostChat:
        r.action = {on ? proto::LobbyActionKind::mute_chat : proto::LobbyActionKind::unmute_chat, p->number};
        break;
    default: return;
    }
    request_ = std::move(r);
}

void Tab::Decide(const Line& line) {
    const Act was = confirm_;
    confirm_ = Act::kNone;
    switch (line.act) {
    case Act::kPlayer: Go(line.player); return;
    case Act::kCopy: {
        Request r;
        r.kind = Request::Kind::kCopyInvite;
        request_ = std::move(r);
        note_ = "The invite code is on the clipboard.";
        return;
    }
    case Act::kJoinGame: {
        if (line.player >= lobby_.games.size()) return;
        const PublicGame& g = lobby_.games[line.player];
        if (!g.joinable) {
            note_ = g.detail;
            return;
        }
        Request r;
        r.kind = Request::Kind::kJoinRoom;
        r.name = g.room;
        r.on = g.is_public;
        request_ = std::move(r);
        note_ = g.is_public ? "Joining " + g.room + "..." : "Type the host's password on the Multiplayer tab.";
        return;
    }
    case Act::kGamesPage: TurnGames(1); return;
    case Act::kRefreshGames: {
        Request r;
        r.kind = Request::Kind::kRefreshGames;
        request_ = std::move(r);
        note_ = "Asking the game list again...";
        return;
    }
    case Act::kResync: {
        Request r;
        r.kind = Request::Kind::kResync;
        request_ = std::move(r);
        note_ = "Asked the host for its save: the chat says how it goes.";
        return;
    }
    case Act::kOwnVoice:
    case Act::kOwnChat:
    case Act::kHostVoice:
    case Act::kHostChat: Change(line, 0); return;
    case Act::kTeleport: {
        if (const Player* p = Shown()) {
            Request r;
            r.kind = Request::Kind::kTeleport;
            r.name = p->name;
            request_ = std::move(r);
            note_ = "Teleporting to " + p->name + ": the chat says how it goes.";
        }
        return;
    }
    case Act::kKick:
    case Act::kBan: {
        const Player* p = Shown();
        if (p == nullptr) {
            return;
        }
        if (was != line.act) {
            confirm_ = line.act;  // the row says "Press again"
            return;
        }
        Request r;
        r.action = {line.act == Act::kKick ? proto::LobbyActionKind::kick : proto::LobbyActionKind::ban, p->number};
        request_ = std::move(r);
        note_ = (line.act == Act::kKick ? "Kicking " : "Banning ") + p->name + ".";
        Go(0);
        return;
    }
    case Act::kNone: return;
    }
}

bool Tab::Press(Input input) {
    if (input != Input::kDecide) {
        confirm_ = Act::kNone;
    }
    note_.clear();
    const auto lines = Lines();
    const Line* line = selected_ < lines.size() && Selectable(lines[selected_].row.kind) ? &lines[selected_] : nullptr;
    switch (input) {
    case Input::kUp: Move(-1); return true;
    case Input::kDown: Move(1); return true;
    case Input::kLeft:
        if (line != nullptr && line->act == Act::kGamesPage) {
            TurnGames(-1);
        } else if (line != nullptr && line->row.kind == RowKind::kToggle) {
            Change(*line, -1);
        }
        return true;
    case Input::kRight:
        if (line != nullptr && line->act == Act::kGamesPage) {
            TurnGames(1);
        } else if (line != nullptr && line->row.kind == RowKind::kToggle) {
            Change(*line, 1);
        }
        return true;
    case Input::kDecide:
        if (line != nullptr) {
            Decide(*line);
        } else {
            confirm_ = Act::kNone;
        }
        return true;
    case Input::kBack:
        if (player_ != 0) {
            const std::uint8_t was = player_;
            Go(0);
            // Back on the row of the player we came from.
            const auto list = Lines();
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (list[i].act == Act::kPlayer && list[i].player == was) {
                    selected_ = i;
                }
            }
            return true;
        }
        return false;
    case Input::kPaste:
    case Input::kErase: return false;
    }
    return false;
}

void Tab::Select(std::size_t row) {
    const auto lines = Lines();
    if (row < lines.size() && Selectable(lines[row].row.kind) && row != selected_) {
        selected_ = row;
        confirm_ = Act::kNone;
    }
}

void Tab::Click(std::size_t row, int side) {
    const auto lines = Lines();
    if (row >= lines.size() || !Selectable(lines[row].row.kind)) {
        return;
    }
    Select(row);
    note_.clear();
    const Line& line = lines[row];
    if (line.act == Act::kGamesPage) {
        confirm_ = Act::kNone;
        TurnGames(side < 0 ? -1 : 1);   // (the value's left half: the page before; its right half or the name: after)
    } else if (line.row.kind == RowKind::kToggle && side != 0) {
        confirm_ = Act::kNone;
        Change(line, side);
    } else {
        Decide(line);
    }
}

std::optional<Request> Tab::TakeRequest() {
    return std::exchange(request_, std::nullopt);
}

View Tab::view() const {
    View v;
    const auto lines = Lines();
    for (const auto& l : lines) {
        v.rows.push_back(l.row);
    }
    if (selected_ < lines.size() && Selectable(lines[selected_].row.kind)) {
        v.selected = static_cast<int>(selected_);
        // (a player's or a room's name as it is; the help of such a row is built from words already in the table's
        // language and the name)
        const Line& picked = lines[selected_];
        v.help_title = picked.row.own_name ? picked.row.name : CoopI18n::Tr(picked.row.name);
        v.help = picked.row.own_name ? picked.help : CoopI18n::Tr(picked.help);
        if (lines[selected_].row.kind == RowKind::kToggle) {
            v.change = CoopI18n::Tr("Off / On");
        } else if (picked.act == Act::kGamesPage) {
            v.change = CoopI18n::Tr("Turn the page");   // (the prompt strip's Left / Right words)
        }
    } else {
        v.help_title = CoopI18n::Tr("Lobby");
        v.help = lobby_.connected ? "" : CoopI18n::Tr("The players of your game show here once you host or join.");
    }
    v.gold = note_;
    v.confirm = CoopI18n::Tr("Choose");
    v.back = player_ != 0 ? CoopI18n::Tr("Back") : CoopI18n::Tr("Close");
    return v;
}

std::string DiscordState(bool connected, std::size_t players) {
    if (!connected) {
        return "Solo";
    }
    return players == 1 ? "Co-op: 1 player" : "Co-op: " + std::to_string(players) + " players";
}

Waiting WaitingText(int state, const std::string& answer, bool held, std::uint64_t waited_ms) {
    // (the numbers of hos_coop_host.h's HosCoopState: this file has no C header of the host menu)
    constexpr int kIdle = 0, kJoining = 3, kWaitHost = 4, kConnected = 5, kFailed = 6;   // (1 starting, 2 hosting)
    Waiting w;
    if (state == kIdle || state == kFailed) return w;
    if (held) {
        w.line = "The host has to let you in.";
        w.next = "They answer with /allow in their chat. This page joins by itself once they do.";
        return w;
    }
    const bool joining = state == kJoining || state == kWaitHost || state == kConnected;
    if (!joining) {   // (our own hosting, starting or listening: as before)
        w.line = "Co-op is starting...";
        return w;
    }
    if (state == kWaitHost || waited_ms >= kNoAnswerMs) {
        w.line = "The host's game has not answered yet.";
        // (each line whole on one line: the table finds it by its text, i18n_test)
        w.next = answer.empty()
            ? "Ask the host to choose Host (Multiplayer tab, or the title screen). Keep this page open: it joins by itself."
            : "Ask the host to choose Host (Multiplayer tab, or the title screen). If they host already, send them this answer code: " + answer;
        return w;
    }
    w.line = state == kConnected ? "Joining the host's game..." : "Looking for the host's game...";
    return w;
}

}  // namespace CoopLobbyTab
