// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_pause_menu.h"

#include "coop_browse.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <utility>

#include "coop/protocol/join_password.hpp"
#include "coop_i18n.h"

namespace CoopPauseMenu {

namespace {

using CoopTitleMenu::CodePoints;
using CoopTitleMenu::Trim;
using State = Session::State;

constexpr std::size_t kMaxJoinBytes = 300;
// A pasted invite code on the Room or address row: its start, its end and its length (codes start alike).
constexpr std::size_t kShownCodeEnds = 8;
std::size_t Utf8Start(const std::string& s, std::size_t at) {   // back to the start of the character at `at`
    while (at > 0 && at < s.size() && (static_cast<unsigned char>(s[at]) & 0xC0) == 0x80) {
        --at;
    }
    return at;
}

std::string ShownCode(const std::string& code) {
    if (code.size() <= 2 * kShownCodeEnds + 3) {
        return code;
    }
    return code.substr(0, Utf8Start(code, kShownCodeEnds)) + "..." +
           code.substr(Utf8Start(code, code.size() - kShownCodeEnds)) + " (" + std::to_string(code.size()) + ")";
}

// An invite (or answer) code's shape: at least 30 letters and digits, in groups with '-' (server/nat_codes.cpp
// looks_like_code reads it for real when Join is pressed).
bool CodeShaped(std::string_view line) {
    std::size_t letters = 0;
    for (const char c : line) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
            ++letters;
        } else if (c != '-') {
            return false;
        }
    }
    return letters >= 30;
}

// What an A on Room or address takes from the clipboard: an invite code, an address line ("host[:port][ password]",
// "room@relay[ password]") or a bare room or PC name; not any other text that happens to be there.
bool JoinLineShaped(std::string_view line) {
    if (CodeShaped(line)) {
        return true;
    }
    const auto target = CoopTitleMenu::ReadJoin(line, {});
    const auto* addr = std::get_if<CoopTitleMenu::JoinAddress>(&target);
    if (addr == nullptr) {
        return false;
    }
    return addr->server.find_first_of(".:@") != std::string::npos ||
           (addr->password.empty() && CoopTitleMenu::BareJoinName(line).has_value());
}

constexpr std::size_t kMaxIpBytes = 259;   // host (up to 253) + ':' + port (up to 5)
constexpr std::size_t kMaxPortBytes = 5;

const char* const kHostPortHelp =
    "The UDP port other players reach you on (default 27510). Type a number from 1 to 65535; changing it while "
    "hosting restarts hosting: friends join again with the new code.";
// The Host page's Game row, its two choices' notes (docs/play-online.md "Public games").
const char* const kPrivateHelp =
    "Private game (the default): friends join with your code or room name and the password. Nobody else can join.";
// (each one literal: coop_i18n's table finds it whole)
const char* const kPublicHelp =
    "Public game: shows in Public games and anyone can join, no password. Only through the game list's server, so players never see your internet address. Kick or ban players from the Lobby tab. Players can change the shared progress of the save you host from.";

// A join password is never drawn: one star per character.
std::string Masked(const std::string& password) {
    return std::string(CodePoints(password), '*');
}

// The host's own join password as its row shows it on A: ten characters a line in two groups of five, at most three
// lines (a longer one cut with "..." on its third), so each line fits a row's value.
std::vector<std::string> PasswordLines(const std::string& password) {
    std::vector<std::string> chars;
    for (std::size_t i = 0; i < password.size();) {
        std::size_t n = 1;
        while (i + n < password.size() && (static_cast<unsigned char>(password[i + n]) & 0xC0) == 0x80) ++n;
        chars.push_back(password.substr(i, n));
        i += n;
    }
    std::vector<std::string> out;
    for (std::size_t c = 0; c < chars.size() && out.size() < 3; c += 10) {
        std::string line;
        for (std::size_t k = c; k < chars.size() && k < c + 10; ++k) {
            if (k > c && (k - c) % 5 == 0) line += ' ';
            line += chars[k];
        }
        if (out.size() == 2 && chars.size() > 30) line += "...";
        out.push_back(std::move(line));
    }
    return out;
}

const char* const kJoinPasswordHelp =
    "Your join password: friends type it with your room name or address. A shows it in groups of five, A again "
    "hides it. Share it in DMs only.";

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

// A row that opens a page: its value cell says where it stands and the arrow after it (right) says it goes deeper.
Row PageRow(std::string name, std::string value, bool fixed = false) {
    Row r = Action(std::move(name), std::move(value), fixed);
    r.right = true;
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

// A CoopInput label ("Controller: 2 - Xbox Wireless Controller") as a choice row: the words after ": " are the value.
Row InputChoice(const char* name, const std::string& label) {
    const auto colon = label.find(": ");
    return Choice(name, colon == std::string::npos ? label : label.substr(colon + 2));
}

Row Note(std::string text) {
    Row r;
    r.name = std::move(text);
    r.kind = RowKind::kNote;
    return r;
}

// The first words when a dim row is pressed, before why (its help).
const char* const kNotNow = "Not now.";

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
        // The Lobby (docs/play-online.md "Lobby"): the same as the Lobby tab's buttons.
        {"/kick <player>", "The host only: takes that player out of the game. They can join again."},
        {"/ban <player>",
         "The host only: takes that player out, and they cannot join again, under any name, until the host stops "
         "hosting."},
        {"/mute <player>",
         "The host: nobody hears that player's voice or sees their chat (the server stops passing them on); add "
         "voice or chat for only that (/mute 2 voice). Anyone else: you stop hearing and seeing them yourself, and "
         "nobody is told."},
        {"/unmute <player>", "Undoes /mute (add voice or chat for only that)."},
        // Host approval (docs/play-online.md "Lobby"): with approve_joins = on, each new player waits for these.
        {"/allow, /allow <player>",
         "The host only, when a player asks to join (approve_joins = on in coop-options.ini): lets them in (the one "
         "who asked first, or by name). Add always to let them in without asking until you stop hosting."},
        {"/deny, /deny <player>",
         "The host only: turns away the player who asks to join. Add always to keep them out until you stop "
         "hosting."},
    };
    return list;
}

std::vector<std::string> MicChoices(const std::vector<std::string>& listed, const std::string& chosen) {
    std::vector<std::string> out{std::string()};
    for (const std::string& n : listed) {
        if (!n.empty() && std::find(out.begin(), out.end(), n) == out.end()) {
            out.push_back(n);
        }
    }
    if (std::find(out.begin(), out.end(), chosen) == out.end()) {
        out.push_back(chosen);
    }
    return out;
}

std::string TitleGameName(const std::string& name, const std::string& host, std::size_t rest) {
    const std::size_t room = rest < kTitleRowBytes ? kTitleRowBytes - rest : 0;
    if (name.size() <= room) return name;
    const std::string with = host.empty() ? std::string() : " (" + host + ")";
    std::string t = name;
    if (!with.empty() && t.size() > with.size() && t.compare(t.size() - with.size(), with.size(), with) == 0) {
        t.resize(t.size() - with.size());   // (the host's name goes first: the room says which game)
        if (t.size() <= room) return t;
    }
    if (room <= 3) return std::string();
    std::size_t k = room - 3;
    while (k > 0 && (static_cast<unsigned char>(t[k]) & 0xC0) == 0x80) --k;   // (never inside a letter)
    return t.substr(0, k) + "...";
}

std::string MicWords(const std::string& chosen, const std::vector<std::string>& listed) {
    if (chosen.empty()) {
        return "Default";
    }
    std::string words = chosen;
    if (CodePoints(words) > kMicChars) {
        std::size_t at = 0;
        for (std::size_t points = 0; at < words.size(); ++at) {   // (the byte where character kMicChars - 3 starts)
            if ((static_cast<unsigned char>(words[at]) & 0xc0) != 0x80 && points++ == kMicChars - 3) {
                break;
            }
        }
        words = words.substr(0, at) + "...";
    }
    const bool there = std::find(listed.begin(), listed.end(), chosen) != listed.end();
    return there ? words : words + " (not connected)";
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
        // Co-op that came with the game's start: both rows still open their page, which says so (a press must never
        // do nothing; the owner, 2026-11-19).
        // (every row says where it stands in its value cell, and the arrow after it says it opens a page: the cell is
        // never left empty, owner photo 2026-11-19)
        out.push_back({PageRow("Host a game", session_.by_launcher ? "Co-op is on"
                                              : host ? (session_.state == State::kHosting ? "Hosting" : "Starting...") : "Off",
                               guest),
                       Act::kOpenHost,
                       session_.by_launcher ? kByLauncher
                       : guest ? "You are in a friend's game. To host your own, leave it first: Join a game, then Leave the game."
                       : host ? "Your game: the code for your friends, who joined, and Stop hosting."
                              : "Starts a game on this PC and makes a code. Send the code to your friends; they choose Join a "
                                "game and paste it. They play on your save."});
        out.push_back({PageRow("Join a game", session_.by_launcher ? "Co-op is on"
                                              : guest ? (session_.state == State::kJoined ? "Joined" : "Joining...") : "Off",
                               host),
                       Act::kOpenJoin,
                       session_.by_launcher ? kByLauncher
                       : host ? "You are hosting. To join a friend's game, stop yours first: Host a game, then Stop hosting."
                       : guest ? "The game you joined, and Leave the game."
                               : "Joins a friend's game with the code they sent you. You play on their save; your own saves "
                                 "are not touched."});
        out.push_back({PageRow("Local co-op", "2 players"), Act::kOpenLocal, "Two players on this PC."});
        out.push_back({PageRow("Settings", options_.name.size() > 12 ? options_.name.substr(0, Utf8Start(options_.name, 9)) + "..." : options_.name), Act::kOpenSettings,
                       "Your name, Link's look, how many players can join your game, voice chat, the chat key, "
                       "which controller this game reads, and players on the map."});
        out.push_back({PageRow("Customize Link", "Colours"), Act::kOpenCustomize,
                       "A colour for each part of your tunics, shields, swords, boots and gauntlets, with a picture of "
                       "the result. Your Link wears it when you close the menu; the other players see your tunic's colour."});
        out.push_back({PageRow("Commands", ChatKeyWords(options_.chat_key)), Act::kOpenCommands, "The chat's commands, and the key that opens the chat."});
        break;
    case Page::kHost: {
        const bool typing_port = typing_ && typing_act_ == Act::kHostPort;
        const std::string port_value = typing_port ? port_text_ + "_" : std::to_string(options_.host_port);
        if (title_ && !session_.by_launcher) {
            TitleHostLines(out, host, guest);
            break;
        }
        if (session_.by_launcher) {
            LauncherLines(out);
        } else if (host) {
            // (who asks to join, the oldest first: let in or turned away here as with /allow and /deny in the chat)
            if (session_.state == State::kHosting && !session_.asking.empty()) {
                const std::string& who = session_.asking.front();
                out.push_back({Action("Let in " + who, "Allow"), Act::kLetIn,
                               who + " asks to join your game. Lets them in, as /allow in the chat does."});
                out.push_back({Action("Refuse " + who, "Refuse"), Act::kRefuse,
                               "Turns " + who + " away, as /deny in the chat does. They can ask again."});
                if (session_.asking.size() > 1) {
                    out.push_back({Note("+" + std::to_string(session_.asking.size() - 1) + " more ask"), Act::kNone, {}});
                }
            }
            // (a public game: relay only, so no row that holds an address or a password: no code, answer,
            // same-network, VPN or password line)
            if (!session_.public_game) {
            out.push_back({Action("Copy the code again", "Copy", session_.code.empty()), Act::kCopyInvite,
                           "Puts your code on the clipboard again. Share it in DMs only: it contains your internet address."});
            out.push_back({Action("Paste friend's answer (or Ctrl+V)", "Paste"), Act::kPasteAnswer,
                           "Only when a friend's game shows an answer code (their way in was blocked): copy the code they "
                           "send you, then press this."});
            out.push_back({Action("Same network", "Copy the line"), Act::kCopyLan,
                           "For friends in the same house, on your home network: copies one line with this PC's address. "
                           "They paste it in Join a game instead of the code."});
            out.push_back({Action("VPN", "Copy the line"), Act::kCopyVpn,
                           "For friends on a VPN with you (Radmin VPN, Hamachi, ZeroTier, Tailscale, WireGuard): copies one "
                           "line with this PC's VPN address. They paste it in Join a game instead of the code."});
            if (session_.state == State::kHosting && !session_.password.empty()) {
                if (!show_password_) {
                    out.push_back({Action("Join password", "Hidden"), Act::kShowPassword, kJoinPasswordHelp});
                } else {
                    const std::vector<std::string> shown = PasswordLines(session_.password);
                    for (std::size_t i = 0; i < shown.size(); ++i) {
                        out.push_back({Action(i == 0 ? "Join password" : "", shown[i]), Act::kShowPassword, kJoinPasswordHelp});
                    }
                }
                const bool typing_password = typing_ && typing_act_ == Act::kNewPassword;
                out.push_back({Action("Change join password", typing_password ? Masked(new_password_) + "_" : "Type"),
                               Act::kNewPassword,
                               "A new join password, typed on the keyboard; Enter gives it to your game. Players who "
                               "join from now on need it; players already in stay. No spaces, at most 128 characters."});
            }
            }   // (not a public game)
            out.push_back({Action("Port", port_value), Act::kHostPort, kHostPortHelp});
            // (the game's kind as it was hosted; a change applies at the next Host, and the row says so)
            out.push_back({Choice("Game", options_.public_game ? "Public game" : "Private game"), Act::kGameKind,
                           options_.public_game == session_.public_game
                               ? std::string(session_.public_game ? kPublicHelp : kPrivateHelp)
                               : std::string("Applies at your next Host: stop hosting, then start again.")});
            out.push_back({Action("Stop hosting", confirm_leave_ ? "Press again to confirm" : "Stop"), Act::kLeave,
                           "Ends your game for everyone in it."});
        } else {
            out.push_back({Action("Start hosting", "Start", session_.by_launcher || guest), Act::kStartHost,
                           options_.public_game ? "Starts a public game on this PC: it shows in Public games, and anyone can join without a password."
                                                : "Starts a game on this PC and makes a code for your friends."});
            out.push_back({Choice("Game", options_.public_game ? "Public game" : "Private game"), Act::kGameKind,
                           options_.public_game ? kPublicHelp : kPrivateHelp});
            if (options_.public_game) {
                out.push_back({Action("Room name", typing_ && typing_act_ == Act::kRoom ? options_.relay_room + "_"
                                                   : options_.relay_room.empty()        ? std::string("None")
                                                                                        : options_.relay_room),
                               Act::kRoom,
                               "Your public game's name in Public games: 3 to 24 letters, digits, - or _. A ready one is offered; change it if you like."});
            }
            out.push_back({Choice("Players", std::to_string(options_.max_players)), Act::kPlayers,
                           "The most players in your game, you included."});
            out.push_back({Action("Port", port_value), Act::kHostPort, kHostPortHelp});
        }
        break;
    }
    case Page::kJoin:
        if (title_ && !session_.by_launcher && !guest) {
            TitleCodeLines(out, host);
            break;
        }
        if (session_.by_launcher) {
            LauncherLines(out);
        } else if (guest) {
            if (session_.state == State::kWaitHost) {
                out.push_back({Action("Copy your answer code", "Copy", session_.code.empty()), Act::kCopyAnswer,
                               "Your friend's router blocks the way in: send them this answer code (it is copied "
                               "already); they paste it on their Host page."});
            }
            // (a note first: the title shows a page of one row with Back far below it)
            if (session_.state != State::kWaitHost) {
                out.push_back({Note(session_.state == State::kJoined ? "In your friend's game" : "Joining your friend's game"),
                               Act::kNone, {}});
            }
            out.push_back({Action("Leave the game", confirm_leave_ ? "Press again to confirm" : "Leave"), Act::kLeave,
                           "Leaves your friend's game. Their game goes on without you."});
        } else {
            const bool off = session_.by_launcher || host;
            // (the last join's answer, a refusal, as a line of the page: the title's pages have no help text; gone with
            // the next press)
            if (!note_.empty() && note_[0].rfind("Turned away", 0) == 0) out.push_back({Note(note_[0]), Act::kNone, {}});
            if (!rejoin_code_.empty()) {
                out.push_back({Action("Join " + rejoin_host_ + " again", "Join", off), Act::kRejoin,
                               "Joins the last game you were in, with the same code. It works while your friend is still "
                               "hosting that game."});
            }
            // (the games on this network and the relay's public rooms: one picked fills the rows below. After the
            // last game again, which stays the first row: one press)
            out.push_back({PageRow("Browse games", find_online_ ? "Public games" : "This network", off), Act::kOpenFind,
                           "Lists the games on this network and the public rooms on your relay, with a search. Pick "
                           "one and its room or address is filled in below."});
            const bool typing_ip = typing_ && typing_act_ == Act::kJoinIp;
            const bool typing_port = typing_ && typing_act_ == Act::kJoinPort;
            const bool typing_password = typing_ && typing_act_ == Act::kJoinPassword;
            // (a room name or an address, its port, the host's join password, then Join: the friend's code rows are
            // gone, a tester asked; a code pasted with Ctrl+V still fills the address and password)
            // (an invite code pasted on the page: shown here, as the code's row did)
            const bool code_in = join_ip_.empty() && !Trim(join_).empty();
            out.push_back({Action("Room or address",
                                  typing_ip   ? join_ip_ + "_"
                                  : code_in   ? ShownCode(std::string(Trim(join_)))
                                  : join_ip_.empty() ? std::string("Type")
                                                     : join_ip_,
                                  off),
                           Act::kJoinIp,
                           "The host's room name (found through your relay, Settings > Relay) or their IP address or "
                           "PC name. A name without a dot is tried as a PC on your network first, then as a room. "
                           "Copied an invite code or address? Press this: it is pasted."});
            out.push_back({Action("Port",
                                  typing_port ? port_text_ + "_"
                                              : join_port_.empty() ? std::to_string(CoopTitleMenu::kDefaultPort)
                                                                   : join_port_,
                                  off),
                           Act::kJoinPort,
                           "The host's UDP port (default 27510). Type a number from 1 to 65535."});
            out.push_back({Action("Password",
                                  typing_password ? Masked(join_password_) + "_"
                                                  : join_password_.empty() ? std::string("Type") : Masked(join_password_),
                                  off),
                           Act::kJoinPassword,
                           "The host's join password: they see it on their Multiplayer page (Hosting, Join "
                           "password)."});
            out.push_back({Action("Join", "Join", off || Trim(join_).empty()), Act::kJoinNow,
                           Trim(join_).empty() ? "Type the room name or address first." : "Joins your friend's game."});
        }
        break;
    case Page::kFind: {
        if (title_) {
            TitleListLines(out, host || session_.by_launcher);
            break;
        }
        out.push_back({Choice("List", find_online_ ? "Public games" : "This network"), Act::kFindList,
                       "Online: the public rooms on your relay (Settings > Relay). This network: the games hosted "
                       "in your home."});
        const bool typing_search = typing_ && typing_act_ == Act::kFindSearch;
        out.push_back({Action("Search", typing_search ? find_search_ + "_"
                                        : find_search_.empty() ? std::string("Type") : find_search_),
                       Act::kFindSearch, "Only the games whose name, or whose host's name, holds these letters."});
        out.push_back({Choice("Show", CoopBrowse::FilterWords(find_filter_)), Act::kFindFilter,
                       "All games; only the ones you can join (your version, room for one more); or every game of "
                       "your version."});
        out.push_back({Choice("Sort", CoopBrowse::SortWords(find_sort_)), Act::kFindSort,
                       "The most players first, the fewest first, or by name."});
        out.push_back({Action("Refresh", "Ask again"), Act::kFindRefresh,
                       find_online_ ? "Asks the relay for its public rooms again."
                                    : "Forgets the games heard and listens again: the ones still hosting come back "
                                      "within 2 seconds."});
        const std::vector<Found> games = FoundShown();
        for (const Found& g : games) {
            out.push_back({Action(g.name, g.players, !g.joinable), Act::kFindGame,
                           g.joinable ? "Fills in the Join page with this game: then Join (with its password, if "
                                        "it asks one)."
                                      : "This game is full, or runs another version of co-op."});
        }
        if (games.empty()) {
            const std::string none = !found_.empty() ? std::string("None match the search or Show")
                                     : !found_state_.empty()                   ? found_state_
                                     : find_online_                            ? std::string("Asking the relay...")
                                                                               : std::string("Looking for games...");
            out.push_back({Note(none), Act::kNone, {}});
        }
        break;
    }
    case Page::kLocal:
        out.push_back({Note("Split screen (experimental): two full games on this PC."), Act::kNone, {}});
        out.push_back({Note("1. Two kits (or Nucleus Co-op), each started once."), Act::kNone, {}});
        out.push_back({Note("2. Each game its own controller: Settings > Controller."), Act::kNone, {}});
        out.push_back({Note("3. Host in one game, Join in the other (Same network row)."), Act::kNone, {}});
        out.push_back({Note("Steps: PLAY-LAN.md. Native split screen comes later."), Act::kNone, {}});
        break;
    case Page::kCommands: {
        const std::string key = ChatKeyWords(options_.chat_key);
        out.push_back({Action("Open the chat", key), Act::kCommand,
                       key == "Off" ? "The chat key is off. To chat, choose a key in Settings, Chat key."
                                    : "Press " + key + " in the game to open the chat box; Enter sends, Esc closes. "
                                      "A line that starts with / is a command, not chat. To use another key: "
                                      "Settings, Chat key."});
        for (const Command& c : Commands()) {
            out.push_back({Action(c.typed), Act::kCommand, c.help});
        }
        break;
    }
    case Page::kSettings:
        out.push_back({Action("Your name", typing_ && typing_act_ == Act::kName ? options_.name + "_" : options_.name),
                       Act::kName,
                       "The name the other players see: 1 to 16 characters, typed with the keyboard."});
        out.push_back({Choice("Link's look", options_.colour.empty() ? std::string("By player number") : options_.colour),
                       Act::kLook,
                       "The colour of your Link's tunic and cap, as you and the other players see it. By player "
                       "number: each player's own colour."});
        out.push_back({Choice("Players when you host", std::to_string(options_.max_players)), Act::kPlayers,
                       "The most players in a game you host, you included."});
        out.push_back({Action("Port when you host",
                              typing_ && typing_act_ == Act::kHostPort ? port_text_ + "_"
                                                                       : std::to_string(options_.host_port)),
                       Act::kHostPort,
                       "The UDP port your game listens on when you host (default 27510). Type a number from 1 to "
                       "65535."});
        out.push_back({Action("Relay", typing_ && typing_act_ == Act::kRelay ? options_.relay + "_"
                                       : options_.relay.empty() || options_.relay == CoopTitleMenu::kDefaultRelay
                                           ? std::string(CoopTitleMenu::kDefaultRelayName)
                                           : options_.relay),
                       Act::kRelay,
                       "The relay that finds rooms by name and lists public rooms. Squadala's own is built in; type "
                       "another (host or host:port) to use it instead. Cleared: Squadala's again. A change while you "
                       "host applies at your next Host."});
        out.push_back({Action("Room name", typing_ && typing_act_ == Act::kRoom ? options_.relay_room + "_"
                                           : options_.relay_room.empty()        ? std::string("None")
                                                                                : options_.relay_room),
                       Act::kRoom,
                       "Your room's name on the relay when you host: 3 to 24 letters, digits, - or _. Friends type "
                       "it to join. Empty: a join code instead. A change while you host applies at your next Host."});
        out.push_back({Toggle("List my room", options_.list_room), Act::kListRoom,
                       "When you host with a room name, the room shows on your relay's public list (Browse games), so "
                       "players can pick it. Never your join password. Off: only players who know the name find it."});
        out.push_back({Toggle("Auto-rejoin", options_.auto_rejoin), Act::kAutoRejoin,
                       "Joins your last friend's game again by itself: a few seconds after the game starts, and when "
                       "the connection drops without you leaving. Off: you join again yourself."});
        out.push_back({Choice("Password on this network", options_.same_network_open ? "Not needed" : "Needed"),
                       Act::kSameNetwork,
                       "Not needed: when you host, players from a private address join without the join password. "
                       "Only on a network you trust: a tunnel, a VPN or a port forward can bring others in that way."});
        out.push_back({Toggle("Proximity voice chat", options_.voice), Act::kVoice,
                       "Talk with players near your Link through your microphone; they sound quieter the farther away "
                       "they are. Off: you neither send nor hear voices."});
        if (options_.voice && microphones_) {
            out.push_back({Choice("Microphone", MicWords(options_.voice_mic, *microphones_)), Act::kMic,
                           "The microphone your voice is recorded from. Default: the one Longshot's Insert menu "
                           "chooses (Gameplay: Microphone), the Ocarina's too. Left and Right go through the "
                           "microphones plugged in now. One unplugged: the others hear nothing until it is back."});
        }
        if (options_.voice) {
            // The table's words around the key (data), put together here: "Push to
            // talk (T)" as one line of a table would need the key inside it.
            const std::string mode =
                options_.voice_ptt ? CoopI18n::Tr("Push to talk") + " (" + options_.voice_key + ")"
                                   : CoopI18n::Tr("Open mic");
            // The help has the key in it twice, so its two halves are put
            // together around the key: neither is a line of the table alone.
            const std::string voice_help =
                CoopI18n::Tr("Open mic: you are heard whenever you speak (silence and quiet noise are not sent).") +
                " " + CoopI18n::Tr("Push to talk: only while you hold") + " " + options_.voice_key + ". " +
                CoopI18n::Tr("On speakers, not headphones? Push to talk keeps the others' voices from coming back to "
                             "them through your microphone.");
            out.push_back({Choice("Voice mode", mode), Act::kVoiceMode, voice_help});
        }
        out.push_back({Choice("Chat key", ChatKeyWords(options_.chat_key)), Act::kChatKey,
                       "The key that opens the chat box in the game. Off: no chat box."});
        if (!controller_row_.empty()) {
            out.push_back({InputChoice("Controller", controller_row_), Act::kController,
                           "Which controller this game reads: all of them (only while its window is in front), one "
                           "of them (also behind another window: two games on one PC, each with its own), or none."});
        }
        if (!keyboard_row_.empty()) {
            out.push_back({InputChoice("Keyboard and mouse", keyboard_row_), Act::kKeyboard,
                           "Window in front: the keyboard and mouse count while this game's window is in front. Also "
                           "in background: the keys count behind other windows too. Off: neither (a controller "
                           "only)."});
        }
        out.push_back({Toggle("Show players on map", options_.map_players), Act::kMapPlayers,
                       "The other players as arrows in their colours on the minimap and on the Map page's area and "
                       "dungeon maps (not the world map). Off: neither shows them."});
        out.push_back({Toggle("Names on map", options_.map_names), Act::kMapNames,
                       "Each player's name beside their arrow on the maps. Off: the arrows only."});
        {   // (Navi) New Game+ (coop_ngplus.h)
            const std::string hearts = std::to_string(ngplus_hearts_ / 4) + (ngplus_hearts_ % 4 ? "+" : "");
            out.push_back({Choice("New Game+", ngplus_unlocked_ == 0 ? std::string("Locked: beat Ganon")
                                               : ngplus_level_ == 0  ? std::string("Off")
                                                                     : "NG+ " + std::to_string(ngplus_level_)),
                           Act::kNgPlus,
                           ngplus_unlocked_ == 0
                               ? "Beat Ganon to unlock it: then play again with the hearts you had and tougher enemies."
                               : "Start a new file with this on: you keep your " + hearts +
                                     " hearts, and enemies and bosses get 50% more life and 25% more damage for each "
                                     "level (up to NG+ " + std::to_string(ngplus_unlocked_) +
                                     " now). In a game you host your level counts for everyone. Off: the game as it is."});
        }
        out.push_back({Choice("Saves", mp_saves_ ? "Multiplayer" : "Solo"), Act::kSaveSet,   // (Navi)
                       "Solo: your own saves. Multiplayer: a separate set of saves for playing together (made from "
                       "your solo saves the first time), so co-op progress never mixes with your own. When you join "
                       "a friend from the title screen you play the host's save, copied beside your multiplayer saves; "
                       "your saves are not touched. Switched on the title screen with no file loaded."});
        out.push_back({Toggle("Original game", options_.original_game), Act::kOriginal,
                       "From the next start, the game as it came: co-op, the pause menu's extra pages and buttons, "
                       "Link's look, the co-op pack and code mods (Save Profiles) all off. Your saves are not touched. "
                       "This switch stays in the F1 menu to turn it back."});
        break;
    case Page::kCustomize:   // (its own rows: coop_look_menu.h)
        break;
    }
    return out;
}

// The Host and Join pages of co-op that came with the game's start (Host.bat, Join.bat, coop.ini): nothing to
// start or stop here, so they say what is on and where the players are (the page's help: kByLauncher).
// The title's Join list: the last game again, the games (this network's first), then Enter a code or address.
void Menu::TitleListLines(std::vector<Line>& out, bool off) const {
    if (!rejoin_code_.empty()) {
        out.push_back({Action("Join " + rejoin_host_ + " again", "", off), Act::kRejoin,
                       "Joins the last game you were in, with the same code, while your friend still hosts it."});
    }
    const std::vector<Found>& games = found_;   // (as handed: this network's first; no search or sort here)
    for (const Found& g : games) {
        const std::string kind = !g.online ? "This network" : g.is_public ? "Public" : "Public, password";
        const std::string value = !g.joinable ? g.players : g.players + "  " + kind;
        out.push_back({Action(TitleGameName(g.name, g.host, 2 + value.size()), value, off || !g.joinable), Act::kFindGame,
                       !g.joinable ? "This game is full, or runs another version of co-op."
                                   : "A: Join   B: Back   Y: Refresh"});
    }
    if (games.empty()) out.push_back({Note("Looking for games..."), Act::kNone, {}});
    out.push_back({PageRow("Enter a code or address", "", off), Act::kOpenJoin,
                   "A friend's code, or a room name, IP or PC name.   B: Back   Y: Refresh"});
}

// The title's Join page: a code from the clipboard, or an address typed; a picked game's password.
void Menu::TitleCodeLines(std::vector<Line>& out, bool off) const {
    const bool typing_ip = typing_ && typing_act_ == Act::kJoinIp;
    const bool typing_password = typing_ && typing_act_ == Act::kJoinPassword;
    const std::string password = typing_password ? Masked(join_password_) + "_"
                                 : join_password_.empty() ? std::string("None") : Masked(join_password_);
    if (!note_.empty() && note_[0].rfind("Turned away", 0) == 0) out.push_back({Note(note_[0]), Act::kNone, {}});
    if (!picked_.empty()) {
        const std::string asks = " asks for a password";
        out.push_back({Note(TitleGameName(picked_, picked_host_, asks.size()) + asks), Act::kNone, {}});
        out.push_back({Action("Password", password, off), Act::kJoinPassword,
                       "The host sees it on their Hosting screen. Type it with Letter, or on the keyboard."});
        out.push_back({Action("Join", "", off), Act::kJoinNow, "Joins the game with this password."});
        return;
    }
    const bool code_in = join_ip_.empty() && !Trim(join_).empty();
    out.push_back({Action("Paste a code", code_in ? ShownCode(std::string(Trim(join_))) : std::string(), off),
                   Act::kCode, "Your friend sees the code on their Host screen. Copy it, then press A here."});
    out.push_back({Action("Type an address", typing_ip ? join_ip_ + "_" : join_ip_, off), Act::kJoinIp,
                   "A room name, an IP address or a PC name, typed with Letter or on the keyboard."});
    if (!join_ip_.empty()) {
        out.push_back({Action("Password", password, off), Act::kJoinPassword,
                       "The host's join password, if their game has one."});
    }
    out.push_back({Action("Join", "", off || Trim(join_).empty()), Act::kJoinNow,
                   Trim(join_).empty() ? "Paste a code or type an address first." : "Joins your friend's game."});
}

// The title's Host page: the choices, then Start hosting; while hosting, the code and password to send, who asks.
void Menu::TitleHostLines(std::vector<Line>& out, bool host, bool guest) const {
    if (!host) {
        out.push_back({Choice("Who can join", options_.public_game ? "Public" : "Private"),
                       Act::kGameKind,
                       "Private: friends join with the code you send. Public: listed in Join a game."});
        out.push_back({Choice("Players", std::to_string(options_.max_players)), Act::kPlayers,
                       "The most players in your game, you included."});
        if (options_.public_game) {
            out.push_back({Action("Room name", typing_ && typing_act_ == Act::kRoom ? options_.relay_room + "_"
                                               : options_.relay_room.empty()        ? std::string("None")
                                                                                    : options_.relay_room),
                           Act::kRoom, "Your game's name in Join a game: 3 to 24 letters, digits, - or _."});
        }
        out.push_back({Action("Start hosting", "", guest), Act::kStartHost,
                       guest ? "You are in a friend's game: leave it first (Join a game)."
                             : "Starts your game on this PC. They play on your save."});
        return;
    }
    if (session_.state == State::kHosting && !session_.asking.empty()) {
        const std::string& who = session_.asking.front();
        out.push_back({Action("Let in " + who, ""), Act::kLetIn, who + " asks to join your game."});
        out.push_back({Action("Refuse " + who, ""), Act::kRefuse, "Turns " + who + " away. They can ask again."});
    }
    const std::string players =
        std::to_string(std::max(1, session_.players)) + "/" + std::to_string(options_.max_players);
    if (session_.public_game) {
        out.push_back({Note("Room  " + options_.relay_room), Act::kNone, {}});
        out.push_back({Note("Players  " + players), Act::kNone, {}});
    } else {
        out.push_back({Action("Code", session_.code.empty() ? std::string("Starting...") : ShownCode(session_.code),
                              session_.code.empty()),
                       Act::kCopyInvite,
                       "The code is copied. Send it in a DM; your friend picks Join a game > Enter a code > Paste a code."});
        if (session_.state == State::kHosting && !session_.password.empty()) {
            const std::string shown = show_password_ ? PasswordLines(session_.password).front() : std::string("Hidden");
            out.push_back({Action("Password", shown), Act::kShowPassword,
                           "Your friend types it after picking your game. A shows it."});
        }
        out.push_back({Note("Players  " + players), Act::kNone, {}});
        out.push_back({Action("Same network", "Copy the line"), Act::kCopyLan,
                       "For friends on your home network: copies one line they paste in Enter a code."});
        out.push_back({Action("Paste friend's answer", ""), Act::kPasteAnswer,
                       "Only when your friend's game shows an answer code: copy it, then press A here."});
    }
    out.push_back({Action("Stop hosting", confirm_leave_ ? "Press again" : ""), Act::kLeave,
                   "Ends your game for everyone in it. Back keeps it going."});
}

void Menu::SetTitle(bool on) {
    if (title_ == on) return;
    title_ = on;
    picked_.clear();
    Clamp();
}

bool Menu::BackLeaves() const {
    if (typing_) return false;
    switch (page_) {
    case Page::kMain: return true;
    case Page::kJoin: return !title_ || InSession();   // (the title's Join page: back to the list)
    case Page::kFind: return title_;
    case Page::kCustomize: return false;
    case Page::kHost:
    case Page::kLocal:
    case Page::kSettings:
    case Page::kCommands: return true;
    }
    return true;
}

void Menu::RefreshFound() {
    found_.clear();
    found_state_.clear();
    request_ = Request{Request::Kind::kRefreshFound, {}};
    Clamp();
}

void Menu::LauncherLines(std::vector<Line>& out) const {
    out.push_back({Note(session_.players > 1 ? "Co-op is on: " + std::to_string(session_.players) + " players"
                                             : std::string("Co-op is on")),
                   Act::kNone, {}});
    out.push_back({Note("Co-op started with the game: Host.bat, Join.bat or coop.ini."), Act::kNone, {}});
    out.push_back({Note("Who is in the game: the Lobby tab."), Act::kNone, {}});
    out.push_back({Note("To host or join another game, start the game again."), Act::kNone, {}});
}

bool Menu::Selectable(const Line& line) const {
    return line.row.kind != RowKind::kHeading && line.row.kind != RowKind::kNote;
}

void Menu::Go(Page page, Act select) {
    if (page != page_) {
        show_password_ = false;   // (another page: the join password hidden again)
    }
    page_ = page;
    typing_ = false;
    typing_act_ = Act::kNone;
    typing_fresh_ = false;
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
    typing_act_ = Act::kNone;
    typing_fresh_ = false;
    port_text_.clear();
    confirm_leave_ = false;
    note_.clear();
    Clamp();
}

void Menu::Hide() {
    EndTyping(false);
    confirm_leave_ = false;
    show_password_ = false;
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
    case Page::kCustomize: act = Act::kOpenCustomize; break;
    case Page::kFind: act = Act::kOpenFind; break;
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
    if (typing_ &&
        std::none_of(after.begin(), after.end(), [this](const Line& l) { return l.act == typing_act_ && !l.row.fixed; })) {
        typing_ = false;   // (the typed field went away under the typing)
        typing_act_ = Act::kNone;
        typing_fresh_ = false;
        port_text_.clear();
        new_password_.clear();
    }
    if (session_.password.empty()) {
        show_password_ = false;   // (hosting stopped: a password shown later is shown on A again)
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

void Menu::PrefillJoin(const std::string& room) {
    // (a room from the relay's list: joined as room@relay, the bare-name rule (a PC first) never catches it)
    const std::string_view relay = Trim(options_.relay);
    join_ip_ = CoopTitleMenu::RoomNameOk(room)
                   ? room + "@" + std::string(relay.empty() ? std::string_view(CoopTitleMenu::kDefaultRelay) : relay)
                   : room;
    join_port_.clear();
    join_password_.clear();
    UpdateJoinFromIpPort();
    Go(Page::kJoin, Act::kJoinPassword);
    note_ = {"This game needs the host's password. Type it, then Join."};
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

void Menu::SetMicrophones(std::optional<std::vector<std::string>> listed) {
    const auto before = Lines();
    const Act act = selected_ < before.size() ? before[selected_].act : Act::kNone;
    microphones_ = std::move(listed);
    const auto after = Lines();
    const auto same = std::find_if(after.begin(), after.end(), [act](const Line& l) { return l.act == act; });
    if (same != after.end()) {
        selected_ = static_cast<std::size_t>(same - after.begin());
    }
    Clamp();
}

void Menu::SetInputRows(std::string controller, std::string keyboard) {
    const auto before = Lines();
    const Act act = selected_ < before.size() ? before[selected_].act : Act::kNone;
    controller_row_ = std::move(controller);
    keyboard_row_ = std::move(keyboard);
    const auto after = Lines();
    const auto same = std::find_if(after.begin(), after.end(), [act](const Line& l) { return l.act == act; });
    if (same != after.end()) {
        selected_ = static_cast<std::size_t>(same - after.begin());
    }
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
    switch (typing_act_) {
    case Act::kName: return &options_.name;
    case Act::kRelay: return &options_.relay;
    case Act::kRoom: return &options_.relay_room;
    case Act::kCode: return &join_;
    case Act::kJoinIp: return &join_ip_;
    case Act::kJoinPassword: return &join_password_;
    case Act::kFindSearch: return &find_search_;
    case Act::kNewPassword: return &new_password_;
    case Act::kHostPort:
    case Act::kJoinPort: return &port_text_;
    default: break;
    }
    return nullptr;
}

void Menu::SyncManualJoinFromText() {
    join_ip_.clear();
    join_port_.clear();
    join_password_.clear();
    const std::string_view t = Trim(join_);
    if (t.empty()) {
        return;
    }
    const std::size_t space = t.find_first_of(" \t");
    const std::string_view address = t.substr(0, space);
    if (!CoopTitleMenu::AddressOk(address)) {
        return;
    }
    // Invite codes (groups of 5 letters/digits separated by '-') also match AddressOk's
    // character set, so only mirror strings that look like host addresses or host:port lines.
    const bool has_dot_or_colon = address.find('.') != std::string_view::npos ||
                                  address.find(':') != std::string_view::npos;
    if (!has_dot_or_colon && space == std::string_view::npos && address.find('-') != std::string_view::npos) {
        return;
    }
    const std::size_t colon = address.find(':');
    join_ip_ = std::string(address.substr(0, colon));
    if (colon != std::string_view::npos) {
        const std::string_view port = address.substr(colon + 1);
        const auto parsed = CoopTitleMenu::ParsePort(port);
        if (!parsed || *parsed != CoopTitleMenu::kDefaultPort) {
            join_port_ = std::string(port);
        }
    }
    if (space != std::string_view::npos) {
        const std::string_view pw = Trim(t.substr(space));
        if (CoopTitleMenu::PasswordOk(pw)) {
            join_password_ = std::string(pw);
        }
    }
}

void Menu::UpdateJoinFromIpPort() {
    const std::string_view ip = Trim(join_ip_);
    if (ip.empty()) {
        join_.clear();
        return;
    }
    std::string server(ip);
    // (a bare name, no dot and no colon, stays as typed: Join tries it as a PC on the home network first, then as a
    // room on the relay, CoopTitleMenu::BareJoinName)
    const std::string_view port = Trim(join_port_);
    if (!port.empty()) {
        server += ':';
        server += port;
    }
    if (!join_password_.empty()) {
        server += ' ';
        server += join_password_;
    }
    join_ = std::move(server);
}

// Typing ends. keep: with what was typed (an invalid name, IP or port is
// not kept); else the edited field goes back to what it was.
void Menu::EndTyping(bool keep) {
    if (!typing_) {
        return;
    }
    const Act act = typing_act_;
    typing_ = false;
    replace_typed_ = false;
    // (a clipboard asked on open and still to come, Linux's late one: dropped when it comes, not a page paste)
    drop_open_paste_ = std::exchange(paste_on_open_, false);
    typing_act_ = Act::kNone;
    typing_fresh_ = false;
    if (act == Act::kRelay || act == Act::kRoom) {   // (the relay and the room name: saved when changed)
        std::string& field = act == Act::kRelay ? options_.relay : options_.relay_room;
        field = std::string(Trim(field));
        const bool ok = act == Act::kRelay ? CoopTitleMenu::RelayOk(field) : CoopTitleMenu::RoomNameOk(field);
        if (keep && ok) {
            if (act == Act::kRelay && field.empty()) field = CoopTitleMenu::kDefaultRelay;
            if (field != name_before_) request_ = Request{Request::Kind::kSave, {}};
            return;
        }
        if (keep) {
            note_ = {act == Act::kRelay ? "Relay: host or host:port (letters, digits, . and -; a port from 1 to 65535). "
                                                          "It was not changed."
                                        : "Room name: 3 to 24 letters, digits, - or _. It was not changed."};
        }
        field = name_before_;
        return;
    }
    if (act == Act::kNewPassword) {   // (never kept here: handed to the host on Enter, else dropped)
        const std::string typed(Trim(new_password_));
        new_password_.clear();
        if (!keep) {
            return;
        }
        if (typed.empty() || !CoopTitleMenu::PasswordOk(typed)) {
            note_ = {typed.empty() ? "Join password: type the new one first. It was not changed."
                                   : "Join password: no spaces, at most 128 characters. It was not changed."};
            return;
        }
        request_ = Request{Request::Kind::kSetPassword, typed};
        show_password_ = true;   // (the new one shown, once the host takes it; the selection stays on this row)
        const auto lines = Lines();
        const auto same = std::find_if(lines.begin(), lines.end(), [](const Line& l) { return l.act == Act::kNewPassword; });
        if (same != lines.end()) {
            selected_ = static_cast<std::size_t>(same - lines.begin());
        }
        return;
    }
    if (act == Act::kFindSearch) {   // (the list follows it; Esc: the search as it was)
        find_search_ = keep ? std::string(Trim(find_search_)) : search_before_;
        Clamp();
        return;
    }
    if (act == Act::kName) {
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
    } else if (act == Act::kCode) {
        if (keep) {
            SyncManualJoinFromText();
        }
    } else if (act == Act::kJoinIp) {
        if (!keep) {
            join_ip_ = ip_before_;
            return;
        }
        const std::string trimmed(Trim(join_ip_));
        if (trimmed.empty()) {
            join_ip_.clear();
            if (!ip_before_.empty()) {
                join_password_.clear();
                UpdateJoinFromIpPort();
            }
            return;
        }
        if (!CoopTitleMenu::AddressOk(trimmed)) {
            note_ = {"IP address: enter a valid IP address or host name (with optional :port). It was not changed."};
            join_ip_ = ip_before_;
            return;
        }
        const std::size_t colon = trimmed.find(':');
        if (colon != std::string::npos) {
            join_ip_ = trimmed.substr(0, colon);
            const std::string port = trimmed.substr(colon + 1);
            const auto parsed = CoopTitleMenu::ParsePort(port);
            join_port_ = parsed && *parsed == CoopTitleMenu::kDefaultPort ? std::string() : port;
        } else {
            join_ip_ = trimmed;
        }
        if (join_ip_ != ip_before_) {   // another host: its password is typed after it
            join_password_.clear();
        }
        UpdateJoinFromIpPort();
    } else if (act == Act::kJoinPassword) {
        const std::string trimmed(Trim(join_password_));
        const bool ok = trimmed.empty() || CoopTitleMenu::PasswordOk(trimmed);   // empty: no password
        if (keep && !ok) {
            note_ = {"Password: no spaces, at most 128 characters. It was not changed."};
        }
        if (!keep || !ok) {
            join_password_ = password_before_;
            return;
        }
        join_password_ = trimmed;
        if (!Trim(join_ip_).empty()) {
            UpdateJoinFromIpPort();
        }
    } else if (act == Act::kJoinPort) {
        if (!keep) {
            port_text_.clear();
            return;
        }
        const std::string trimmed(Trim(port_text_));
        port_text_.clear();
        if (trimmed.empty()) {
            join_port_.clear();
        } else if (const auto p = CoopTitleMenu::ParsePort(trimmed)) {
            join_port_ = *p == CoopTitleMenu::kDefaultPort ? std::string() : std::to_string(*p);
        } else {
            note_ = {"Port: enter a number from 1 to 65535. It was not changed."};
            return;
        }
        if (!Trim(join_ip_).empty()) {
            UpdateJoinFromIpPort();
        }
    } else if (act == Act::kHostPort) {
        if (!keep) {
            port_text_.clear();
            return;
        }
        const std::uint16_t before = options_.host_port;
        const std::string trimmed(Trim(port_text_));
        port_text_.clear();
        if (trimmed.empty()) {
            options_.host_port = CoopTitleMenu::kDefaultPort;
        } else if (const auto p = CoopTitleMenu::ParsePort(trimmed)) {
            options_.host_port = *p;
        } else {
            note_ = {"Port: enter a number from 1 to 65535. It was not changed."};
            return;
        }
        if (options_.host_port != before) {
            const bool hosting = page_ == Page::kHost && InSession() && !session_.by_launcher && session_.hosting;
            request_ = Request{hosting ? Request::Kind::kHost : Request::Kind::kSave, {}};
        }
    }
}

std::size_t Menu::CustomizeFirst() const {
    const CoopLookMenu::View c = customize_.view();
    for (std::size_t i = 0; i < c.rows.size(); ++i) {
        const auto kind = static_cast<RowKind>(c.rows[i].kind);
        if (kind != RowKind::kHeading && kind != RowKind::kNote) return i;
    }
    return 0;
}

bool Menu::Press(Input input) {
    note_.clear();
    if (page_ == Page::kCustomize) {   // (its own rows: coop_look_menu.h; Your name above them, ours)
        using L = CoopLookMenu::Input;
        const bool name_row = !customize_.editing();
        if (typing_) {   // (Your name typed on its row: as on the Settings page)
            switch (input) {
            case Input::kDecide: EndTyping(true); break;
            case Input::kBack: EndTyping(false); break;
            case Input::kErase:
                typing_fresh_ = false;
                if (std::string* t = Typed(); t != nullptr && !t->empty()) CoopTitleMenu::PopCodePoint(*t);
                break;
            case Input::kUp:
            case Input::kDown:
                EndTyping(true);
                if (input == Input::kDown) customize_name_ = false, customize_.Select(CustomizeFirst());
                break;
            case Input::kPaste:   // (the caller pastes into the field: Paste)
            case Input::kLeft:
            case Input::kRight: break;
            }
            return true;
        }
        if (name_row && customize_name_) {
            switch (input) {
            case Input::kDown: customize_name_ = false, customize_.Select(CustomizeFirst()); break;
            case Input::kUp:   // (round to the list's last row, as its own Up from its first)
                customize_name_ = false;
                customize_.Select(CustomizeFirst());
                customize_.Press(L::kUp);
                break;
            case Input::kDecide: Decide(Act::kName); break;
            case Input::kBack: Go(Page::kMain, Act::kOpenCustomize); break;
            case Input::kLeft:
            case Input::kRight:
            case Input::kPaste:
            case Input::kErase: break;
            }
            return true;
        }
        if (name_row && input == Input::kUp && customize_.view().selected == static_cast<int>(CustomizeFirst())) {
            customize_name_ = true;   // (Up from the first part: Your name)
            return true;
        }
        std::optional<L> in;
        switch (input) {
        case Input::kUp: in = L::kUp; break;
        case Input::kDown: in = L::kDown; break;
        case Input::kLeft: in = L::kLeft; break;
        case Input::kRight: in = L::kRight; break;
        case Input::kDecide: in = L::kDecide; break;
        case Input::kBack: in = L::kBack; break;
        case Input::kPaste:
        case Input::kErase: break;
        }
        if (in && !customize_.Press(*in)) {
            Go(Page::kMain, Act::kOpenCustomize);
        }
        TakeCustomize();
        return true;
    }
    const auto lines = Lines();
    const Line* line = selected_ < lines.size() && Selectable(lines[selected_]) ? &lines[selected_] : nullptr;
    if (!(input == Input::kDecide && line != nullptr && line->act == Act::kLeave)) {
        confirm_leave_ = false;
    }
    if (typing_) {
        const Act act = typing_act_;
        switch (input) {
        case Input::kUp:
        case Input::kDown:
        {
            const bool to_join = act == Act::kCode && !Trim(join_).empty();
            EndTyping(act != Act::kNewPassword);   // (a new join password goes to the host on Enter only)
            if (to_join) {
                Go(Page::kJoin, Act::kJoinNow);
            } else {
                Go(page_, act);   // rows may have come or gone (Clear the code): step from the typed one
                Move(input == Input::kUp ? -1 : 1);
            }
            return true;
        }
        case Input::kDecide:
            EndTyping(true);
            if (note_.empty() && (act == Act::kCode || act == Act::kJoinIp || act == Act::kJoinPort || act == Act::kJoinPassword) &&
                !Trim(join_).empty()) {
                Go(Page::kJoin, Act::kJoinNow);
            }
            return true;
        case Input::kBack:
            EndTyping(false);
            return true;
        case Input::kPaste:   // (Ctrl+V: the player's own paste, always used)
            paste_on_open_ = false;
            drop_open_paste_ = false;
            request_ = Request{Request::Kind::kPaste, {}};
            return true;
        case Input::kErase:
            typing_fresh_ = false;
            if (std::string* t = Typed(); t != nullptr && !t->empty()) {
                CoopTitleMenu::PopCodePoint(*t);
            }
            replace_typed_ = false;   // the old code is being edited
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
        if (line != nullptr && line->row.fixed && line->row.kind != RowKind::kAction) {
            note_ = {kNotNow, line->help};
        } else if (line != nullptr && line->row.kind != RowKind::kAction) {
            Change(line->act, input == Input::kLeft ? -1 : 1);
        }
        return true;
    case Input::kDecide:
        if (line == nullptr) {
            return true;
        }
        if (line->row.fixed) {
            note_ = {kNotNow, line->help};   // (the help alone is what selecting it showed: the press would look dead)
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
            if (title_ && !InSession()) {   // (the title's code page or a game's password: the list again)
                picked_.clear();
                Go(Page::kFind, Act::kOpenJoin);
                break;
            }
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
        case Page::kCustomize:   // (Press gave it to the page)
            break;
        case Page::kFind:
            if (title_) {
                Go(Page::kMain, Act::kOpenJoin);
                break;
            }
            Go(Page::kJoin, Act::kOpenFind);
            break;
        }
        return true;
    case Input::kPaste:
        drop_open_paste_ = false;   // (Ctrl+V: the player's own paste, always used)
        if (page_ == Page::kJoin && std::any_of(lines.begin(), lines.end(), [](const Line& l) {
                return (l.act == Act::kCode || l.act == Act::kJoinIp || l.act == Act::kJoinPort) && !l.row.fixed;
            })) {
            request_ = Request{Request::Kind::kPaste, {}};
            return true;
        }
        if (page_ == Page::kHost && std::any_of(lines.begin(), lines.end(), [](const Line& l) {
                return l.act == Act::kPasteAnswer;
            })) {   // Ctrl+V on the Host page: the same as Paste friend's answer
            request_ = Request{Request::Kind::kPasteAnswer, {}};
            return true;
        }
        return false;
    case Input::kErase:
        return true;
    }
    return true;
}

void Menu::Decide(Act act) {
    switch (act) {
    case Act::kOpenHost:
        Go(Page::kHost);
        if (!InSession() && !title_) {   // (the title: its choices first, Start hosting hosts)   // one press hosts; the page then shows the code
            request_ = Request{Request::Kind::kHost, {}};
        }
        break;
    case Act::kStartHost:
        if (options_.public_game && options_.relay_room.empty()) {   // (as kGameKind: a ready name)
            options_.relay_room = CoopTitleMenu::Options::RandomRoomName(static_cast<std::uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        }
        request_ = Request{Request::Kind::kHost, {}};
        break;
    case Act::kOpenJoin:
        // (the title: Join opens on the list; from the list, Enter a code or address opens the Join page)
        if (title_ && page_ == Page::kMain && !InSession()) {
            Go(Page::kFind);
            request_ = Request{Request::Kind::kRefreshFound, {}};
            break;
        }
        if (title_ && page_ == Page::kFind) {   // (the title's Enter a code or address: a page of its own, empty)
            join_.clear();
            SyncManualJoinFromText();
        }
        picked_.clear();
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
    case Act::kOpenCustomize:
        Go(Page::kCustomize);
        customize_.Open(look_);
        customize_name_ = false;
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
    case Act::kCopyVpn:
        request_ = Request{Request::Kind::kCopyVpn, {}};
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
        if (title_) {   // (the title's Paste a code: the clipboard, no typing; Paste fills the page)
            picked_.clear();
            request_ = Request{Request::Kind::kPaste, {}};
            break;
        }
        // The code is on the clipboard as a rule: pasted at once, in place of the code there. With nothing to
        // paste the old code stays to be edited, but the first character typed (or pasted as keystrokes, as
        // Win+V's clipboard history and on-screen keyboards do) starts a new code instead of going beside it.
        typing_ = true;
        typing_act_ = Act::kCode;
        typing_fresh_ = false;
        replace_typed_ = !join_.empty();
        request_ = Request{Request::Kind::kPaste, {}};
        break;
    case Act::kClearCode:
        join_.clear();
        SyncManualJoinFromText();
        replace_typed_ = false;
        Go(Page::kJoin, Act::kCode);
        break;
    case Act::kJoinIp:
        ip_before_ = join_ip_;
        typing_ = true;
        typing_act_ = Act::kJoinIp;
        typing_fresh_ = false;
        // A controller has no Ctrl+V: an invite code or address line on the clipboard is pasted at once, as the
        // friend's code row did (anything else there is left alone, and the field is typed as before).
        paste_on_open_ = true;
        drop_open_paste_ = false;
        request_ = Request{Request::Kind::kPaste, {}};
        break;
    case Act::kJoinPassword:
        password_before_ = join_password_;
        typing_ = true;
        typing_act_ = Act::kJoinPassword;
        typing_fresh_ = false;
        break;
    case Act::kJoinPort:
        port_text_ = join_port_.empty() ? std::to_string(CoopTitleMenu::kDefaultPort) : join_port_;
        typing_ = true;
        typing_act_ = Act::kJoinPort;
        typing_fresh_ = true;
        break;
    case Act::kHostPort:
        port_text_ = std::to_string(options_.host_port);
        typing_ = true;
        typing_act_ = Act::kHostPort;
        typing_fresh_ = true;
        break;
    case Act::kJoinNow:
        request_ = Request{Request::Kind::kJoin, join_};
        break;
    case Act::kOpenFind:
        Go(Page::kFind);
        break;
    case Act::kFindList:
    case Act::kFindFilter:
    case Act::kFindSort:
        Change(act, 1);
        break;
    case Act::kFindRefresh:   // (the list empties; the module asks again and hands the new one, SetFound)
        RefreshFound();
        break;
    case Act::kShowPassword: {
        show_password_ = !show_password_;
        const auto lines = Lines();   // (on the row's first line, however many it has now)
        const auto first =
            std::find_if(lines.begin(), lines.end(), [](const Line& l) { return l.act == Act::kShowPassword; });
        if (first != lines.end()) {
            selected_ = static_cast<std::size_t>(first - lines.begin());
        }
        Clamp();
        break;
    }
    case Act::kNewPassword:
        new_password_.clear();
        typing_ = true;
        typing_act_ = Act::kNewPassword;
        typing_fresh_ = false;
        break;
    case Act::kLetIn:
    case Act::kRefuse:
        if (!session_.asking.empty()) {
            request_ = Request{act == Act::kLetIn ? Request::Kind::kAllow : Request::Kind::kDeny, session_.asking.front()};
        }
        break;
    case Act::kFindSearch:
        search_before_ = find_search_;
        typing_ = true;
        typing_act_ = Act::kFindSearch;
        typing_fresh_ = false;
        break;
    case Act::kFindGame: {
        // (which game: the game rows before the selected one, in the order Lines put them)
        const auto lines = Lines();
        std::size_t k = 0;
        for (std::size_t i = 0; i < selected_ && i < lines.size(); ++i) k += lines[i].act == Act::kFindGame ? 1 : 0;
        const std::vector<Found> games = title_ ? found_ : FoundShown();
        if (k >= games.size()) break;
        const Found& g = games[k];
        const bool online = title_ ? g.online : find_online_;
        join_password_.clear();
        if (online) {
            // (the relay's list names rooms, never PCs: joined as room@relay straight away, so the bare-name rule of a
            // typed line, a PC first, never catches it; a name the relay sent that is not a room name is not joined)
            if (!CoopTitleMenu::RoomNameOk(g.join)) break;
            const std::string_view relay = Trim(options_.relay);
            join_ip_ = g.join + "@" + std::string(relay.empty() ? std::string_view(CoopTitleMenu::kDefaultRelay) : relay);
            join_port_.clear();   // (the relay's port is in the relay line)
        } else {
            join_ip_ = g.join;
            join_port_ = g.port > 0 && g.port != CoopTitleMenu::kDefaultPort ? std::to_string(g.port) : std::string();
        }
        if (online && g.is_public) {
            // A public game: joined at once with no password to type (its password is the room's, as its host made
            // it). The field stays empty: nothing of it is shown or kept.
            join_password_ = coop::protocol::public_join_password(g.join);
            UpdateJoinFromIpPort();
            join_password_.clear();
            Go(Page::kJoin, Act::kJoinNow);
            request_ = Request{Request::Kind::kJoin, join_};
            break;
        }
        // (a room on the relay asks its password as a rule (a private game, or an older kit's: never tried with the
        // public password, so an older host's wrong-password count is never touched); a game on this network lets
        // its own network in)
        if (online) {
            PrefillJoin(g.join);
            if (title_) {   // (the title: a page of the password and Join, the game's name over them)
                // (the name the list showed: the room's join name can differ, "test-kokiri" for "kokiri (Mido)")
                picked_ = !g.name.empty() ? g.name : g.host.empty() ? g.join : g.join + " (" + g.host + ")";
                picked_host_ = g.host;
                note_.clear();
                Go(Page::kJoin, Act::kJoinPassword);
            }
            break;
        }
        join_password_.clear();
        UpdateJoinFromIpPort();
        Go(Page::kJoin, Act::kJoinNow);
        break;
    }
    case Act::kName:
        name_before_ = options_.name;
        typing_ = true;
        typing_act_ = Act::kName;
        typing_fresh_ = false;
        break;
    case Act::kRelay:
    case Act::kRoom:
        name_before_ = act == Act::kRelay ? options_.relay : options_.relay_room;
        // (the built-in relay is not typed over: the field starts empty for one of the player's own, and left empty
        // it is the built-in one again; Esc keeps it)
        if (act == Act::kRelay && options_.relay == CoopTitleMenu::kDefaultRelay) options_.relay.clear();
        typing_ = true;
        typing_act_ = act;
        typing_fresh_ = false;
        break;
    case Act::kLook:
    case Act::kPlayers:
    case Act::kVoiceMode:
    case Act::kMic:
    case Act::kChatKey:
    case Act::kController:
    case Act::kKeyboard:
    case Act::kNgPlus:
    case Act::kSaveSet:
        Change(act, 1);
        break;
    case Act::kVoice:
    case Act::kMapPlayers:
    case Act::kMapNames:
    case Act::kSameNetwork:
    case Act::kListRoom:
    case Act::kAutoRejoin:
    case Act::kOriginal:
    case Act::kGameKind:
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
    case Act::kSameNetwork:
        options_.same_network_open = !options_.same_network_open;   // (two choices: either way flips it)
        request_ = Request{Request::Kind::kSave, {}};
        break;
    case Act::kFindList:   // (two lists: either way flips it; the module hears the other one from now)
        find_online_ = !find_online_;
        found_.clear(), found_state_.clear();
        Clamp();
        break;
    case Act::kFindFilter:
        find_filter_ = (find_filter_ + (step < 0 ? CoopBrowse::kFilters - 1 : 1)) % CoopBrowse::kFilters;
        Clamp();
        break;
    case Act::kFindSort:
        find_sort_ = (find_sort_ + (step < 0 ? CoopBrowse::kSorts - 1 : 1)) % CoopBrowse::kSorts;
        Clamp();
        break;
    case Act::kGameKind:
        options_.public_game = !options_.public_game;   // (two choices: either way flips it)
        if (options_.public_game && options_.relay_room.empty()) {   // (a public game needs a name: a ready one)
            options_.relay_room = CoopTitleMenu::Options::RandomRoomName(static_cast<std::uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        }
        // (while hosting, the page shows the session's lines in place of a row's help: the note says it plainly)
        if (session_.hosting) {
            note_.clear();
            if (options_.public_game != session_.public_game) {
                note_ = {"Applies at your next Host: stop hosting, then start again."};
            }
        }
        request_ = Request{Request::Kind::kSave, {}};
        break;
    case Act::kMic: {
        if (!microphones_) {
            break;
        }
        const auto list = MicChoices(*microphones_, options_.voice_mic);
        const auto at = std::find(list.begin(), list.end(), options_.voice_mic);
        const std::size_t n = list.size(), i = static_cast<std::size_t>(at - list.begin());
        options_.voice_mic = list[(i + (step < 0 ? n - 1 : 1)) % n];
        request_ = Request{Request::Kind::kSave, {}};
        break;
    }
    case Act::kVoiceMode:   // (two choices: either arrow flips it)
        options_.voice_ptt = !options_.voice_ptt;
        request_ = Request{Request::Kind::kSave, {}};
        break;
    case Act::kChatKey: {
        const auto& list = CoopTitleMenu::ChatKeys();
        const auto at = std::find(list.begin(), list.end(), options_.chat_key);
        const std::size_t n = list.size(), i = at == list.end() ? 0 : static_cast<std::size_t>(at - list.begin());
        options_.chat_key = list[(i + (step < 0 ? n - 1 : 1)) % n];
        request_ = Request{Request::Kind::kSave, {}};
        break;
    }
    case Act::kController:   // (one way round, either arrow: the list depends on the controllers connected now)
        request_ = Request{Request::Kind::kController, {}};
        break;
    case Act::kKeyboard:
        request_ = Request{Request::Kind::kKeyboard, {}};
        break;
    case Act::kMapPlayers:
        options_.map_players = step > 0 ? true : step < 0 ? false : !options_.map_players;
        request_ = Request{Request::Kind::kSave, {}};
        break;
    case Act::kMapNames:
        options_.map_names = step > 0 ? true : step < 0 ? false : !options_.map_names;
        request_ = Request{Request::Kind::kSave, {}};
        break;
    case Act::kListRoom:
        options_.list_room = step > 0 ? true : step < 0 ? false : !options_.list_room;
        request_ = Request{Request::Kind::kSave, {}};
        break;
    case Act::kAutoRejoin:
        options_.auto_rejoin = step > 0 ? true : step < 0 ? false : !options_.auto_rejoin;
        request_ = Request{Request::Kind::kSave, {}};
        break;
    case Act::kOriginal:
        options_.original_game = step > 0 ? true : step < 0 ? false : !options_.original_game;
        request_ = Request{Request::Kind::kSave, {}};
        break;
    case Act::kSaveSet:   // (Navi) two choices: either arrow flips it
        mp_saves_ = !mp_saves_;
        request_ = Request{Request::Kind::kSaveSet, mp_saves_ ? "multiplayer" : "solo"};
        break;
    case Act::kNgPlus:   // (Navi) Off, NG+ 1 ... the highest unlocked, round
        if (ngplus_unlocked_ > 0) {
            const int span = ngplus_unlocked_ + 1;
            ngplus_level_ = (std::clamp(ngplus_level_, 0, ngplus_unlocked_) + (step < 0 ? span - 1 : 1)) % span;
            request_ = Request{Request::Kind::kNgPlus, std::to_string(ngplus_level_)};
        }
        break;
    default:
        break;
    }
}

void Menu::Select(std::size_t row) {
    if (page_ == Page::kCustomize) {
        if (typing_) return;
        if (!customize_.editing()) {   // (row 0: Your name; the parts one row down)
            customize_name_ = row == 0;
            if (row > 0) customize_.Select(row - 1);
            return;
        }
        customize_.Select(row);
        return;
    }
    const auto lines = Lines();
    if (typing_ || row >= lines.size() || !Selectable(lines[row]) || row == selected_) {
        return;
    }
    selected_ = row;
    confirm_leave_ = false;
}

void Menu::Click(std::size_t row, int side) {
    if (page_ == Page::kCustomize) {
        note_.clear();
        if (typing_) {   // (a click elsewhere ends the name's typing, kept)
            if (row != 0 || customize_.editing()) EndTyping(true);
            return;
        }
        if (!customize_.editing()) {
            customize_name_ = row == 0;
            if (row == 0) {
                Decide(Act::kName);   // (a click on Your name: its typing)
                return;
            }
            customize_.Click(row - 1, side);
            TakeCustomize();
            return;
        }
        customize_.Click(row, side);
        TakeCustomize();
        return;
    }
    const auto lines = Lines();
    if (row >= lines.size() || !Selectable(lines[row])) {
        return;
    }
    if (typing_ && row != selected_) {
        EndTyping(typing_act_ != Act::kNewPassword);   // (a new join password goes to the host on Enter only)
        return;   // (the click ended the typing; the next one chooses)
    }
    selected_ = row;
    const Line& line = lines[row];
    if (line.row.kind == RowKind::kAction || line.row.fixed) {
        Press(Input::kDecide);   // (a dim row: Press says why)
    } else if (side != 0) {
        note_.clear();
        confirm_leave_ = false;
        Change(line.act, side);
    }
}

bool Menu::Char(std::uint32_t cp) {
    if (std::exchange(paste_on_open_, false)) {
        drop_open_paste_ = true;   // (a key typed first: the clipboard asked on open is not used when it comes)
    }
    std::string* t = Typed();
    if (t == nullptr) {
        return false;
    }
    if (cp < 0x20 || cp == 0x7F) {
        return true;
    }
    if (typing_act_ == Act::kHostPort || typing_act_ == Act::kJoinPort) {
        if (cp < '0' || cp > '9') {
            return true;
        }
        if (typing_fresh_) {
            t->clear();
            typing_fresh_ = false;
        }
        if (t->size() >= kMaxPortBytes) {
            return true;
        }
        t->push_back(static_cast<char>(cp));
        return true;
    }
    const std::string add = CoopTitleMenu::Utf8(cp);
    if (add.empty()) {
        return true;
    }
    if (replace_typed_ && t == &join_) {   // a new code, not more of the old one
        join_.clear();
    }
    replace_typed_ = false;
    if (typing_act_ == Act::kRoom && !(std::isalnum(static_cast<unsigned char>(add[0])) != 0 || add == "-" || add == "_")) {
        return true;   // (a room name: letters, digits, - and _ only)
    }
    if (typing_act_ == Act::kRelay &&
        !(add.size() == 1 && (std::isalnum(static_cast<unsigned char>(add[0])) != 0 || add == "." || add == "-" || add == ":"))) {
        return true;   // (a relay line: host[:port], letters, digits, '.', '-' and ':')
    }
    if (typing_act_ == Act::kNewPassword && add == " ") {
        return true;   // (a join password has no spaces)
    }
    const std::size_t max_bytes = typing_act_ == Act::kRoom || typing_act_ == Act::kFindSearch ? 24
                                  : typing_act_ == Act::kRelay         ? 128
                                  : typing_act_ == Act::kJoinIp        ? kMaxIpBytes
                                  : typing_act_ == Act::kJoinPassword || typing_act_ == Act::kNewPassword
                                      ? CoopTitleMenu::kMaxPasswordBytes
                                                                      : kMaxJoinBytes;
    if (typing_act_ == Act::kName ? CodePoints(*t) >= CoopTitleMenu::kMaxNameChars : t->size() + add.size() > max_bytes) {
        return true;
    }
    *t += add;
    return true;
}

bool Menu::Paste(std::string_view text) {
    const std::string line = std::string(Trim(CoopTitleMenu::OneLine(text)));
    if (std::exchange(drop_open_paste_, false)) {
        return true;
    }
    const bool on_open = std::exchange(paste_on_open_, false);
    if (on_open && typing_ && typing_act_ == Act::kJoinIp) {
        if (!JoinLineShaped(line)) {
            return true;   // (not a code or an address: the field stays open to be typed)
        }
        if (CodeShaped(line)) {   // (an invite code: the page holds it, as a Ctrl+V on the page does)
            EndTyping(false);
            join_ = line.substr(0, Utf8Start(line, std::min(line.size(), kMaxJoinBytes)));
            SyncManualJoinFromText();
            replace_typed_ = false;
            Go(Page::kJoin, Act::kJoinNow);
            return true;
        }
    }
    if (typing_ && typing_act_ == Act::kJoinIp) {
        if (line.empty()) {
            return true;
        }
        const auto target = CoopTitleMenu::ReadJoin(line, {});
        if (const auto* addr = std::get_if<CoopTitleMenu::JoinAddress>(&target)) {
            join_ip_ = addr->server;
            EndTyping(true);
            if (!addr->password.empty()) {
                join_password_ = addr->password;
                UpdateJoinFromIpPort();
            }
        } else {
            join_ip_ = line.substr(0, Utf8Start(line, std::min(line.size(), kMaxIpBytes)));
            EndTyping(true);
        }
        if (note_.empty() && !Trim(join_).empty()) {
            Go(Page::kJoin, Act::kJoinNow);
        }
        return true;
    }
    if (typing_ && typing_act_ == Act::kJoinPassword) {
        if (line.empty()) {
            return true;
        }
        join_password_ = line.substr(0, Utf8Start(line, std::min(line.size(), CoopTitleMenu::kMaxPasswordBytes)));
        EndTyping(true);
        if (note_.empty() && !Trim(join_).empty()) {
            Go(Page::kJoin, Act::kJoinNow);
        }
        return true;
    }
    if (typing_ && typing_act_ == Act::kNewPassword) {   // (in place of what was typed; Enter gives it to the host)
        if (!line.empty()) {
            new_password_ = line.substr(0, Utf8Start(line, std::min(line.size(), CoopTitleMenu::kMaxPasswordBytes)));
        }
        return true;
    }
    if (typing_ && (typing_act_ == Act::kJoinPort || typing_act_ == Act::kHostPort)) {
        if (line.empty()) {
            return true;
        }
        const Act was = typing_act_;
        typing_fresh_ = false;
        port_text_ = line.substr(0, Utf8Start(line, std::min(line.size(), kMaxPortBytes)));
        EndTyping(true);
        if (note_.empty() && was == Act::kJoinPort && !Trim(join_).empty()) {
            Go(Page::kJoin, Act::kJoinNow);
        }
        return true;
    }
    if (page_ == Page::kJoin) {
        const auto lines = Lines();
        // (a code or an address line pasted on the page fills it, as the code's row did: the room or address row
        // open to it)
        if (std::none_of(lines.begin(), lines.end(), [](const Line& l) { return l.act == Act::kJoinIp && !l.row.fixed; })) {
            return false;
        }
        if (line.empty()) {
            if (join_.empty()) {   // (with a code in the field: it stays, to be edited or typed over)
                note_ = {"Nothing to paste: copy the room name or address first. You can also type it."};
            }
            return true;
        }
        join_ = line.substr(0, Utf8Start(line, std::min(line.size(), kMaxJoinBytes)));   // in place of the old one
        SyncManualJoinFromText();
        typing_ = false;
        typing_act_ = Act::kNone;
        typing_fresh_ = false;
        replace_typed_ = false;
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
    if (page_ == Page::kCustomize) {   // (its own rows: coop_look_menu.h; Your name above them)
        const CoopLookMenu::View c = customize_.view();
        const bool name_row = !customize_.editing();
        if (name_row) {
            Row name;
            name.name = "Your name";
            name.value = options_.name;
            name.kind = RowKind::kAction;
            v.rows.push_back(std::move(name));
        }
        for (const CoopLookMenu::Row& r : c.rows) {
            Row row;
            row.name = r.name;
            row.value = r.value;
            row.kind = static_cast<RowKind>(r.kind);
            row.left = r.left;
            row.right = r.right;
            v.rows.push_back(std::move(row));
        }
        v.selected = !name_row ? c.selected : customize_name_ || typing_ ? 0 : c.selected + 1;
        v.typing = typing_;
        v.help_title = name_row && (customize_name_ || typing_) ? "Your name" : c.help_title;
        v.help = !note_.empty() ? Joined(note_)
                 : name_row && (customize_name_ || typing_)
                     ? std::string("The name the other players see over your Link and in the chat: 1 to 16 characters.")
                 : character_ ? (c.help.empty() ? CoopI18n::Tr(kCharacterNote) : c.help + " " + CoopI18n::Tr(kCharacterNote))
                              : c.help;
        v.change = c.change;
        v.confirm = c.confirm;
        v.back = c.back;
        return v;
    }
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
        v.help = "Split screen (experimental) runs two full games on this PC, one per player: about 10 GB of RAM each "
                 "and a strong graphics card. Start two kits (or use Nucleus Co-op), give each game its own controller"
                 " in Settings, Controller (controller 1, controller 2 or the keyboard), then Host a game in one and "
                 "Join a game in the other with the Same network line. The steps are in PLAY-LAN.md. Native split "
                 "screen, one game for both, comes in a later release.";
        break;
    case Page::kSettings:
        v.help_title = "Settings";
        break;
    case Page::kCommands:
        v.help_title = "Commands";
        break;
    case Page::kCustomize:   // (above)
        break;
    case Page::kFind:
        v.help_title = "Browse games";
        break;
    }
    if (!note_.empty()) {
        v.help = JoinedTr(note_);
    } else if (said && !(line != nullptr && confirm_leave_)) {
        v.help = JoinedTr(session_.lines);
    } else if (line != nullptr) {
        v.help = line->help;
    } else if (session_.by_launcher && (page_ == Page::kHost || page_ == Page::kJoin)) {
        v.help = kByLauncher;   // (LauncherLines' page has no row to select)
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
            const bool typed_row = line->act == Act::kName || line->act == Act::kHostPort ||
                                   line->act == Act::kRelay || line->act == Act::kRoom ||
                                   line->act == Act::kJoinIp || line->act == Act::kJoinPort ||
                                   line->act == Act::kJoinPassword || line->act == Act::kNewPassword;
            v.confirm = line->row.value.empty() || line->row.right ? "Open" : typed_row ? "Type" : "Choose";
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

void Menu::SetLook(const CoopLook::Look& look) {
    look_ = look;
}

void Menu::SetFound(std::vector<Found> games, std::string state) {
    if (games == found_ && state == found_state_) return;
    found_ = std::move(games);
    found_state_ = std::move(state);
    if (page_ == Page::kFind && !typing_) Clamp();
}

std::vector<Menu::Found> Menu::FoundShown() const {
    std::vector<CoopBrowse::Entry> entries;
    for (const Found& g : found_) entries.push_back({g.name, g.host, g.count, g.max, g.same_version});
    std::vector<Found> kept;
    for (const int i : CoopBrowse::Order(entries, find_search_, find_filter_, find_sort_)) {
        kept.push_back(found_[static_cast<std::size_t>(i)]);
    }
    return kept;
}

void Menu::SetSaveSet(bool multiplayer) {
    mp_saves_ = multiplayer;
}

void Menu::SetNgPlus(int level, int unlocked, int hearts) {
    ngplus_level_ = level;
    ngplus_unlocked_ = unlocked;
    ngplus_hearts_ = hearts;
}

std::vector<CoopLookMenu::Shape> Menu::Picture() const {
    return page_ == Page::kCustomize ? customize_.view().picture : std::vector<CoopLookMenu::Shape>{};
}

void Menu::TakeCustomize() {
    if (auto look = customize_.TakeChange()) {
        look_ = *look;
        options_.colour = CoopLook::ColourText(look_[CoopLook::Part::tunic]);   // the Settings page's row agrees
        request_ = Request{Request::Kind::kLookParts, {}};
    }
}

}  // namespace CoopPauseMenu
