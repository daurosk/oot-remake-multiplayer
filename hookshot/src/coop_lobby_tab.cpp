// SPDX-License-Identifier: GPL-3.0-or-later
// The Lobby tab's logic (coop_lobby_tab.h).
#include "coop_lobby_tab.h"

#include <algorithm>
#include <utility>

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

std::string Hearts(std::uint16_t quarters) {
    const int whole = quarters / 4;
    const int rest = quarters % 4;
    std::string s = std::to_string(whole);
    if (rest == 2) {
        s += " and a half";
    } else if (rest != 0) {
        s += " and " + std::to_string(rest) + "/4";
    }
    return s;
}

std::string MutedWords(std::uint8_t bits) {
    if ((bits & kVoice) != 0 && (bits & kChat) != 0) {
        return "voice and chat";
    }
    return (bits & kVoice) != 0 ? "voice" : "chat";
}

std::string Shorten(const std::string& s, std::size_t most) {
    return s.size() <= most ? s : s.substr(0, most) + "...";
}

}  // namespace

std::string Summary(const Player& p) {
    std::vector<std::string> parts;
    if (p.ms) {
        parts.push_back(std::to_string(*p.ms) + " ms");
    }
    if (!p.place.empty()) {
        parts.push_back(p.place);
    }
    switch (p.life) {
    case Player::Life::up: break;
    case Player::Life::downed: parts.push_back("downed"); break;
    case Player::Life::out: parts.push_back("out"); break;
    case Player::Life::dead: parts.push_back("fainted"); break;
    }
    if (p.talking) {
        parts.push_back("talking");
    }
    if (p.host_mutes != 0 || p.own_mutes != 0) {
        parts.push_back("muted");
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
    const auto heading = [&](std::string name) {
        Line l;
        l.row.name = std::move(name);
        l.row.kind = RowKind::kHeading;
        out.push_back(std::move(l));
    };
    const auto note = [&](std::string text) {
        Line l;
        l.row.name = std::move(text);
        l.row.kind = RowKind::kNote;
        out.push_back(std::move(l));
    };
    const auto action = [&](std::string name, std::string value, Act act, std::string help, std::uint8_t player = 0) {
        Line l;
        l.row.name = std::move(name);
        l.row.value = std::move(value);
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

    if (!lobby_.connected) {
        heading("Lobby");
        note("You are not in a game.");
        note("Host one or join a friend's on the Multiplayer tab; the players show here.");
        return out;
    }

    const Player* p = Shown();
    if (p == nullptr) {
        const int count = static_cast<int>(lobby_.players.size());
        heading(lobby_.max_players > 0
                    ? "Players  " + std::to_string(count) + " / " + std::to_string(lobby_.max_players)
                    : "Players  " + std::to_string(count));
        for (const auto& pl : lobby_.players) {
            std::string name = pl.host ? "Host: " + pl.name : pl.name;
            if (pl.self) {
                name += " (you)";
            }
            action(std::move(name), Summary(pl), Act::kPlayer,
                   "Player " + std::to_string(pl.number) + (pl.colour.empty() ? "" : ", " + pl.colour) +
                       (pl.host ? ", the host" : "") + ". Press to see " + (pl.self ? "yourself." : "what you can do."),
                   pl.number);
        }
        if (lobby_.max_players > count) {
            const int open = lobby_.max_players - count;
            if (lobby_.max_players <= kMostSlotRows) {
                for (int i = 0; i < open; ++i) {
                    note("Open place");
                }
            } else {
                note(std::to_string(open) + (open == 1 ? " more player can join" : " more players can join"));
            }
        }
        heading("Invite");
        if (lobby_.hosting && !lobby_.invite.empty()) {
            action("Copy the invite code", Shorten(lobby_.invite, 24), Act::kCopy,
                   "Puts the invite code on the clipboard: send it to a friend, who pastes it in Join a game.");
        } else if (lobby_.hosting) {
            note("The invite code is not ready yet (see the Multiplayer tab).");
        } else {
            note("Only the host has the invite code: ask them for it.");
        }
        heading("Host's game");
        std::string game = lobby_.host_place.empty() ? std::string("Not known yet") : lobby_.host_place;
        if (lobby_.host_life && lobby_.host_max && *lobby_.host_max > 0) {
            game += "  " + Hearts(*lobby_.host_life) + " of " + Hearts(*lobby_.host_max) + " hearts";
        }
        note(std::move(game));
        return out;
    }

    heading(p->host ? "Host: " + p->name : p->name);
    note("Player " + std::to_string(p->number) + (p->colour.empty() ? "" : ", " + p->colour) +
         (p->self ? " (you)" : ""));
    if (const std::string s = Summary(*p); !s.empty()) {
        note(s);
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
               "Takes them out; their name and address cannot join again until you stop hosting.");
    }
    return out;
}

namespace {

bool Selectable(RowKind kind) {
    return kind != RowKind::kHeading && kind != RowKind::kNote;
}

}  // namespace

void Tab::Show() {
    player_ = 0;
    selected_ = 0;
    confirm_ = Act::kNone;
    note_.clear();
    Clamp();
}

void Tab::Hide() {
    confirm_ = Act::kNone;
}

void Tab::Set(Lobby lobby) {
    lobby_ = std::move(lobby);
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
        if (line != nullptr && line->row.kind == RowKind::kToggle) {
            Change(*line, -1);
        }
        return true;
    case Input::kRight:
        if (line != nullptr && line->row.kind == RowKind::kToggle) {
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
    if (line.row.kind == RowKind::kToggle && side != 0) {
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
        v.help_title = lines[selected_].row.name;
        v.help = lines[selected_].help;
        if (lines[selected_].row.kind == RowKind::kToggle) {
            v.change = "Off / On";
        }
    } else {
        v.help_title = "Lobby";
        v.help = lobby_.connected ? "" : "The players of your game show here once you host or join.";
    }
    v.gold = note_;
    v.confirm = "Choose";
    v.back = player_ != 0 ? "Back" : "Close";
    return v;
}

}  // namespace CoopLobbyTab
