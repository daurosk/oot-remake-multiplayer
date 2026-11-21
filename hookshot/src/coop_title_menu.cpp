// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_title_menu.h"

#include "hos_coop_host.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace CoopTitleMenu {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

// Code points in UTF-8 text (a stray continuation byte counts as one).
std::size_t CodePoints(std::string_view s) {
    std::size_t n = 0;
    for (const char ch : s) {
        if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) {
            ++n;
        }
    }
    return n;
}

void PopCodePoint(std::string& s) {
    while (!s.empty()) {
        const unsigned char c = static_cast<unsigned char>(s.back());
        s.pop_back();
        if ((c & 0xC0) != 0x80) {
            break;
        }
    }
}

std::string Utf8(std::uint32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            return {};
        }
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF) {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

std::string OneLine(std::string_view text) {
    std::string out;
    for (const char c : text) {
        out += (c == '\r' || c == '\n' || c == '\t') ? ' ' : c;
    }
    return std::string(Trim(out));
}

namespace {

// The file's lines without their ends, and whether it uses CRLF.
std::vector<std::string> Lines(std::string_view text, bool& crlf) {
    crlf = text.find("\r\n") != std::string_view::npos;
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        out.emplace_back(line);
        at = end + 1;
    }
    return out;
}

std::string Join(const std::vector<std::string>& lines, bool crlf) {
    std::string out;
    for (const auto& l : lines) {
        out += l;
        out += crlf ? "\r\n" : "\n";
    }
    return out;
}

// "key = value" (or "key=value") without a comment mark: the value's start.
std::optional<std::string_view> ActiveValue(std::string_view line, std::string_view key) {
    const std::string_view t = Trim(line);
    if (t.size() <= key.size() || t.substr(0, key.size()) != key) {
        return std::nullopt;
    }
    const std::string_view rest = Trim(t.substr(key.size()));
    if (rest.empty() || rest.front() != '=') {
        return std::nullopt;
    }
    return Trim(rest.substr(1));
}

// "# key = ..." or "; key=...": a commented-out line of `key`.
bool CommentedKey(std::string_view line, std::string_view key) {
    std::string_view t = Trim(line);
    if (t.empty() || (t.front() != '#' && t.front() != ';')) {
        return false;
    }
    while (!t.empty() && (t.front() == '#' || t.front() == ';')) {
        t.remove_prefix(1);
    }
    return ActiveValue(t, key).has_value() || (Trim(t).size() == key.size() + 1 && Trim(t).substr(0, key.size()) == key &&
                                               Trim(t).back() == '=');
}

std::string Line(std::string_view key, std::string_view value, bool spaced) {
    std::string l(key);
    l += spaced ? " = " : "=";
    l += value;
    return l;
}

bool Off(const std::optional<std::string>& v) {
    if (!v) {
        return false;
    }
    std::string s = *v;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s == "0" || s == "no" || s == "off" || s == "false";
}

bool On(const std::optional<std::string>& v) {
    if (!v || v->empty()) {
        return false;
    }
    return !Off(v);
}

}  // namespace

std::optional<std::string> IniGet(std::string_view text, std::string_view key) {
    bool crlf = false;
    std::optional<std::string> found;
    for (const auto& line : Lines(text, crlf)) {
        if (const auto v = ActiveValue(line, key)) {
            std::string_view value = *v;
            // a comment after the value: "blue  # mine"
            for (std::size_t i = 1; i < value.size(); ++i) {
                if ((value[i] == '#' || value[i] == ';') && (value[i - 1] == ' ' || value[i - 1] == '\t')) {
                    value = Trim(value.substr(0, i));
                    break;
                }
            }
            found = std::string(value);
        }
    }
    return found;
}

std::string IniSet(std::string_view text, std::string_view key, std::string_view value, bool spaced) {
    bool crlf = false;
    auto lines = Lines(text, crlf);
    bool done = false;
    for (auto& line : lines) {
        if (ActiveValue(line, key)) {
            line = done ? "# " + line : Line(key, value, spaced);
            done = true;
        }
    }
    if (!done) {
        for (auto& line : lines) {
            if (CommentedKey(line, key)) {
                line = Line(key, value, spaced);
                done = true;
                break;
            }
        }
    }
    if (!done) {
        lines.push_back(Line(key, value, spaced));
    }
    return Join(lines, crlf || text.empty());   // (a new file: Windows line ends, as the kit's)
}

std::string IniUnset(std::string_view text, std::string_view key) {
    bool crlf = false;
    auto lines = Lines(text, crlf);
    bool changed = false;
    for (auto& line : lines) {
        if (ActiveValue(line, key)) {
            line = "# " + line;
            changed = true;
        }
    }
    return changed ? Join(lines, crlf) : std::string(text);
}

const std::vector<std::string>& Colours() {
    // The ten tunic colours (lichii/hook/coop_colors.cpp kTunics), after "" (the player number's).
    static const std::vector<std::string> list = {"",     "red",   "orange", "yellow", "green", "cyan",
                                                  "blue", "purple", "pink",  "white",  "black"};
    return list;
}

const std::vector<std::string>& ChatKeys() {
    static const std::vector<std::string> list = {"T", "Y", "U", "C", "F2", "F3", "F4", "off"};
    return list;
}

bool NameOk(std::string_view name) {
    const std::string_view t = Trim(name);
    if (t.empty() || CodePoints(t) > kMaxNameChars) {
        return false;
    }
    return std::none_of(t.begin(), t.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u < 0x20 || u == 0x7F;
    });
}

bool PasswordOk(std::string_view password) {
    if (password.empty() || password.size() > kMaxPasswordBytes) {
        return false;
    }
    return std::none_of(password.begin(), password.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u <= 0x20 || u == 0x7F;
    });
}

std::string Options::RandomRoomName(std::uint64_t seed) {
    static constexpr const char* kWords[] = {"forest", "lake",  "field", "river",  "valley", "meadow",
                                             "canyon", "grove", "cliff", "marsh",  "harbor", "summit"};
    std::uint64_t x = seed * 0x9E3779B97F4A7C15ull + 0xD1B54A32D192ED03ull;   // (splitmix64)
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;
    const char* word = kWords[x % (sizeof kWords / sizeof kWords[0])];
    const unsigned digits = static_cast<unsigned>((x >> 16) % 9000) + 1000;
    return std::string(word) + "-" + std::to_string(digits);
}

Options Options::Read(std::string_view coop_ini, std::string_view options_ini) {
    Options o;
    if (const auto v = IniGet(coop_ini, "name"); v && NameOk(*v)) {
        o.name = std::string(Trim(*v));
    }
    if (const auto v = IniGet(options_ini, "color"); v && !v->empty()) {
        // A tunic name (any case) shows as that row value; any other colour the game reads (gray, brown, #F82,
        // 0xFF8020, #FF8020FF, rgb(...): CoopColors::Parse) is kept as the player wrote it, never dropped.
        o.colour_in_file = *v;
        std::string c = *v;
        std::transform(c.begin(), c.end(), c.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        const auto& list = Colours();
        o.colour = std::find(list.begin(), list.end(), c) != list.end() ? c : *v;
        if (o.colour == c && c != *v) {
            o.colour_in_file = c;   // ("Orange": the same colour; written only if the row changes it)
        }
    }
    o.tunics = !Off(IniGet(options_ini, "LICHII_COOP_TUNICS"));
    o.hud = !Off(IniGet(options_ini, "LICHII_COOP_HUD"));
    if (const auto v = IniGet(options_ini, "LICHII_COOP_CHAT_KEY"); v && !v->empty()) {
        std::string k = *v;
        std::transform(k.begin(), k.end(), k.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        if (k == "OFF") {
            k = "off";
        }
        const auto& list = ChatKeys();
        o.chat_key = std::find(list.begin(), list.end(), k) != list.end() ? k : "T";
    }
    o.show_players = On(IniGet(options_ini, "LICHII_COOP_SHOW_PLAYERS"));
    o.pvp = On(IniGet(options_ini, "pvp"));
    o.approve_joins = On(IniGet(options_ini, "approve_joins"));
    o.same_network_open = On(IniGet(options_ini, "same_network_open"));
    o.auto_rejoin = On(IniGet(options_ini, "auto_rejoin"));
    o.original_game = On(IniGet(options_ini, "original_game"));
    o.list_room = On(IniGet(options_ini, "list_room"));
    o.public_game = On(IniGet(options_ini, "public_game"));
    o.upnp = !Off(IniGet(options_ini, "upnp"));
    if (const auto v = IniGet(options_ini, "server_password"); v && PasswordOk(*v)) {
        o.password = *v;
    }
    o.debug_panel = On(IniGet(options_ini, "LICHII_COOP_DEBUG_PANEL"));
    o.cheats = On(IniGet(options_ini, "LICHII_COOP_CHEATS"));
    {
        const auto v = IniGet(options_ini, "LICHII_COOP_VOICE");
        o.voice = On(v);
        std::string m(Trim(v.value_or("")));
        std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        // push to talk only when the line turns voice on and is not open (an older on, 1, yes: as it meant);
        // no line or off: voice off, and turning it on in the menu gives open mic
        o.voice_ptt = o.voice && m != "open" && m != "open-mic";
        const std::string k(Trim(IniGet(options_ini, "LICHII_COOP_VOICE_KEY").value_or("")));
        if (k.size() == 1 && std::isalnum(static_cast<unsigned char>(k[0]))) {
            o.voice_key = std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(k[0]))));
        } else if (!k.empty()) {
            o.voice_key = "your key";   // (a key code: coop_voice's SettingsFrom reads it)
        }
        // (the whole line, as the launcher hands it over: a device's name may hold " #" or " ;", no comment here)
        for (const auto& [k, v] : KitVariables(options_ini)) {
            if (k == "LICHII_COOP_VOICE_MIC") {
                o.voice_mic = v;
            }
        }
    }
    o.map_players = !Off(IniGet(options_ini, "LICHII_COOP_MAP"));
    o.map_names = !Off(IniGet(options_ini, "LICHII_COOP_MAP_NAMES"));
    // The relay lines Host.bat reads too (coop-options.ini, "Advanced: relay"): the Multiplayer tab's Host uses them
    // the same way. Read only: the menu writes none of them.
    o.relay = std::string(Trim(IniGet(options_ini, "relay").value_or(kDefaultRelay)));
    if (o.relay.empty()) o.relay = kDefaultRelay;   // (an empty line: the built-in one, which is never removed)
    o.relay_key = std::string(Trim(IniGet(options_ini, "relay_key").value_or("")));
    o.relay_room = std::string(Trim(IniGet(options_ini, "relay_room").value_or("")));
    o.list_name = std::string(Trim(IniGet(options_ini, "list_name").value_or("")));
    if (const auto v = IniGet(options_ini, "max_players"); v && !v->empty() && v->size() <= 2 &&
        std::all_of(v->begin(), v->end(), [](char c) { return c >= '0' && c <= '9'; })) {
        o.max_players = std::clamp(std::stoi(*v), kMinPlayers, kMaxPlayers);
    }
    if (const auto v = IniGet(options_ini, "server_port")) {
        if (const auto p = ParsePort(*v)) {
            o.host_port = *p;
        }
    } else if (const auto v = IniGet(coop_ini, "host_port")) {
        if (const auto p = ParsePort(*v)) {
            o.host_port = *p;
        }
    }
    return o;
}

bool RoomNameOk(std::string_view room) {
    if (room.empty()) return true;
    if (room.size() < 3 || room.size() > 24) return false;
    return std::all_of(room.begin(), room.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '-' || c == '_';
    });
}

bool RelayOk(std::string_view relay) {
    if (relay.empty()) return true;
    if (relay.size() > 128) return false;
    // host[:port], as the server reads it (server/relay_link.cpp parse_relay_address): a host of letters, digits, '.'
    // and '-', then a port from 1 to 65535 when one is given.
    std::string_view host = relay;
    if (const auto colon = relay.rfind(':'); colon != std::string_view::npos) {
        const std::string_view port = relay.substr(colon + 1);
        if (port != Trim(port) || !ParsePort(port)) return false;
        host = relay.substr(0, colon);
    }
    return !host.empty() && std::all_of(host.begin(), host.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '.' || c == '-';
    });
}

void Options::Write(std::string& coop_ini, std::string& options_ini) const {
    coop_ini = IniSet(coop_ini, "name", Trim(name), true);
    std::string o = options_ini;
    if (colour != colour_in_file) {   // (unchanged: the line stays exactly as the player wrote it)
        o = colour.empty() ? IniUnset(o, "color") : IniSet(o, "color", colour, true);
    }
    o = tunics ? IniUnset(o, "LICHII_COOP_TUNICS") : IniSet(o, "LICHII_COOP_TUNICS", "0", false);
    o = hud ? IniUnset(o, "LICHII_COOP_HUD") : IniSet(o, "LICHII_COOP_HUD", "0", false);
    o = chat_key == "T" ? IniUnset(o, "LICHII_COOP_CHAT_KEY") : IniSet(o, "LICHII_COOP_CHAT_KEY", chat_key, false);
    o = show_players ? IniSet(o, "LICHII_COOP_SHOW_PLAYERS", "1", false) : IniUnset(o, "LICHII_COOP_SHOW_PLAYERS");
    o = IniSet(o, "pvp", pvp ? "on" : "off", true);
    o = auto_rejoin ? IniSet(o, "auto_rejoin", "on", true) : IniUnset(o, "auto_rejoin");
    o = original_game ? IniSet(o, "original_game", "on", true) : IniUnset(o, "original_game");
    o = same_network_open ? IniSet(o, "same_network_open", "on", true) : IniUnset(o, "same_network_open");
    o = list_room ? IniSet(o, "list_room", "on", true) : IniUnset(o, "list_room");
    o = public_game ? IniSet(o, "public_game", "on", true) : IniUnset(o, "public_game");
    // (the relay: the kit's default needs no line; a room name, or none)
    // A line whose value did not change is kept as the player wrote it (its spacing and a comment after it).
    if (std::string(Trim(IniGet(o, "relay").value_or(kDefaultRelay))) != relay) {
        o = relay == kDefaultRelay ? IniUnset(o, "relay") : IniSet(o, "relay", relay, true);
    }
    if (std::string(Trim(IniGet(o, "relay_room").value_or(""))) != relay_room) {
        o = relay_room.empty() ? IniUnset(o, "relay_room") : IniSet(o, "relay_room", relay_room, true);
    }
    o = upnp ? IniUnset(o, "upnp") : IniSet(o, "upnp", "no", true);
    o = password.empty() ? IniUnset(o, "server_password") : IniSet(o, "server_password", password, true);
    o = debug_panel ? IniSet(o, "LICHII_COOP_DEBUG_PANEL", "1", false) : IniUnset(o, "LICHII_COOP_DEBUG_PANEL");
    o = cheats ? IniSet(o, "LICHII_COOP_CHEATS", "1", false) : IniUnset(o, "LICHII_COOP_CHEATS");
    // (voice-mode: open = on with open mic, ptt = push to talk, the line gone = off, the default)
    o = voice ? IniSet(o, "LICHII_COOP_VOICE", VoiceMode(), false) : IniUnset(o, "LICHII_COOP_VOICE");
    // (kept while voice is off too: turning it on again uses the same microphone)
    o = voice_mic.empty() ? IniUnset(o, "LICHII_COOP_VOICE_MIC") : IniSet(o, "LICHII_COOP_VOICE_MIC", voice_mic, false);
    o = IniSet(o, "max_players", std::to_string(std::clamp(max_players, kMinPlayers, kMaxPlayers)), true);
    const std::uint16_t port = host_port != 0 ? host_port : kDefaultPort;
    o = port == kDefaultPort ? IniUnset(o, "server_port") : IniSet(o, "server_port", std::to_string(port), true);
    o = map_players ? IniUnset(o, "LICHII_COOP_MAP") : IniSet(o, "LICHII_COOP_MAP", "0", false);
    o = IniUnset(o, "LICHII_COOP_HUD_SIZE");   // (0.11.8's "Player list" size, removed: a kit's old line goes)
    o = map_names ? IniUnset(o, "LICHII_COOP_MAP_NAMES") : IniSet(o, "LICHII_COOP_MAP_NAMES", "0", false);
    options_ini = std::move(o);
}

std::vector<std::pair<std::string, std::string>> Options::Environment() const {
    return {
        {"LICHII_COOP_COLOR", colour},
        {"LICHII_COOP_TUNICS", tunics ? "" : "0"},
        {"LICHII_COOP_HUD", hud ? "" : "0"},
        {"LICHII_COOP_CHAT_KEY", chat_key == "T" ? "" : chat_key},
        {"LICHII_COOP_SHOW_PLAYERS", show_players ? "1" : ""},
        {"LICHII_COOP_DEBUG_PANEL", debug_panel ? "1" : ""},
        {"LICHII_COOP_CHEATS", cheats ? "1" : ""},
        {"LICHII_COOP_VOICE", voice ? VoiceMode() : ""},
        {"LICHII_COOP_VOICE_MIC", voice_mic},
        {"LICHII_COOP_MAP", map_players ? "" : "0"},
        {"LICHII_COOP_MAP_NAMES", map_names ? "" : "0"},
    };
}

std::string Options::VoiceMode() const { return voice_ptt ? "ptt" : "open"; }

std::string CoopIniFor(std::string_view coop_ini, std::string_view server, std::string_view name,
                       std::string_view password) {
    std::string t = IniSet(coop_ini, "server", server, true);
    t = IniUnset(t, "port");   // the port is in `server`
    t = IniSet(t, "name", Trim(name), true);
    t = password.empty() ? IniUnset(t, "password") : IniSet(t, "password", password, true);
    return t;
}

std::vector<std::pair<std::string, std::string>> KitVariables(std::string_view options_ini) {
    bool crlf = false;
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& raw : Lines(options_ini, crlf)) {
        const std::string_view t = Trim(raw);
        if (t.empty() || t.front() == '#' || t.front() == ';') {
            continue;
        }
        if (ActiveValue(t, "color")) {
            if (const auto c = IniGet(t, "color"); c && !c->empty()) {
                out.emplace_back("LICHII_COOP_COLOR", *c);
            }
            continue;
        }
        const bool coop = t.rfind("LICHII_COOP_", 0) == 0, nx2 = t.rfind("EDEN_NX2_", 0) == 0;
        const std::size_t eq = t.find('=');
        if ((!coop && !nx2) || eq == std::string_view::npos) {
            continue;
        }
        const std::string_view key = Trim(t.substr(0, eq));
        if (!std::all_of(key.begin(), key.end(), [](char c) {
                return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
            })) {
            continue;
        }
        out.emplace_back(std::string(key), std::string(Trim(t.substr(eq + 1))));
    }
    return out;
}

std::optional<std::uint16_t> ParsePort(std::string_view text) {
    const std::string_view port = Trim(text);
    if (port.empty() || port.size() > 5 ||
        !std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    const int p = std::stoi(std::string(port));
    if (p < 1 || p > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(p);
}

bool AddressOk(std::string_view typed) {
    std::string_view t = Trim(typed);
    // Through a relay (issue 261): "<join code or room name>@relay[:port]", the relay's address checked as any other.
    if (const std::size_t at = t.find('@'); at != std::string_view::npos) {
        const std::string_view what = t.substr(0, at);
        if (what.size() < 3 || what.size() > 24 ||
            !std::all_of(what.begin(), what.end(), [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
            })) {
            return false;
        }
        t = t.substr(at + 1);
    }
    const std::size_t colon = t.find(':');
    const std::string_view host = t.substr(0, colon);
    if (host.empty() || host.size() > 253 ||
        !std::all_of(host.begin(), host.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-';
        })) {
        return false;
    }
    if (colon == std::string_view::npos) {
        return true;
    }
    const std::string_view port = t.substr(colon + 1);
    return port == Trim(port) && ParsePort(port).has_value();
}

std::optional<std::string> BareJoinName(std::string_view join_line) {
    const std::string_view t = Trim(join_line);
    const std::string_view address = t.substr(0, t.find_first_of(" \t"));
    if (address.empty() || address.find_first_of(".:@") != std::string_view::npos || !RoomNameOk(address)) {
        return std::nullopt;
    }
    return std::string(address);
}

std::string RoomJoinLine(std::string_view join_line, std::string_view relay) {
    const std::string_view t = Trim(join_line);
    const std::size_t space = t.find_first_of(" \t");
    const std::string_view password = space == std::string_view::npos ? std::string_view{} : Trim(t.substr(space));
    std::string out = std::string(t.substr(0, space)) + "@" + std::string(Trim(relay));
    if (!password.empty()) out += " " + std::string(password);
    return out;
}

JoinTarget ReadJoin(std::string_view typed, const CodeResult& code) {
    const std::string_view t = Trim(typed);
    if (t.empty()) {
        return std::string("Paste or type the invite code your host sent you first.");
    }
    if (code.invite) {
        return *code.invite;
    }
    if (code.looks_like_code) {
        return code.error.empty() ? std::string("That invite code is not right; ask the host to copy it again.")
                                  : code.error;
    }
    const std::size_t space = t.find_first_of(" 	");
    const std::string_view address = t.substr(0, space);
    const std::string_view password = space == std::string_view::npos ? std::string_view{} : Trim(t.substr(space));
    if (AddressOk(address)) {
        if (!password.empty() && !PasswordOk(password)) {
            return std::string("The join password after the address: no spaces, at most 128 characters.");
        }
        return JoinAddress{std::string(address), std::string(password)};
    }
    return std::string("That is not an invite code or a server address.");
}

std::string LanLabel(std::string_view ipv4) {
    int part[4] = {0, 0, 0, 0};
    int n = 0;
    bool digit = false;
    for (const char c : ipv4) {
        if (c >= '0' && c <= '9') {
            part[n] = part[n] * 10 + (c - '0');
            digit = true;
            if (part[n] > 255) {
                return {};
            }
        } else if (c == '.' && digit && n < 3) {
            ++n;
            digit = false;
        } else {
            return {};
        }
    }
    if (n != 3 || !digit) {
        return {};
    }
    if (part[0] == 26) {
        return "Radmin VPN";
    }
    if (part[0] == 25) {
        return "Hamachi";
    }
    if (part[0] == 100 && part[1] >= 64 && part[1] <= 127) {
        return "Tailscale";
    }
    if (part[0] == 10 || (part[0] == 172 && part[1] >= 16 && part[1] <= 31) || (part[0] == 192 && part[1] == 168)) {
        return "home network";
    }
    return {};
}

std::vector<LanAddress> LanAddresses(const std::vector<std::string>& ipv4s) {
    std::vector<LanAddress> vpn;
    std::vector<LanAddress> home;
    for (const std::string& a : ipv4s) {
        const std::string label = LanLabel(a);
        const auto same = [&a](const LanAddress& x) { return x.address == a; };
        if (label.empty() || std::any_of(vpn.begin(), vpn.end(), same) || std::any_of(home.begin(), home.end(), same)) {
            continue;
        }
        (label == "home network" ? home : vpn).push_back(LanAddress{a, label});
    }
    vpn.insert(vpn.end(), home.begin(), home.end());
    return vpn;
}

std::string LanJoinText(std::string_view address, std::uint16_t port, std::string_view password) {
    std::string t(address);
    t += ":" + std::to_string(port);
    if (!password.empty()) {
        t += " ";
        t += password;
    }
    return t;
}

std::vector<std::string> LanLines(const std::vector<LanAddress>& addresses, std::uint16_t port,
                                  std::string_view password, bool copied) {
    if (addresses.empty()) {
        return {"No home-network or VPN address was found on this PC.",
                "On a VPN? Start your VPN program (Radmin VPN, Hamachi), then press this again."};
    }
    std::vector<std::string> out = {
        "Friends on your " + addresses.front().label + " paste this line in Join (address and password):",
        LanJoinText(addresses.front().address, port, password),
        copied ? "It is copied: paste it into your chat with them (Ctrl+V)."
               : "Type it into your chat with them, exactly as it is."};
    if (addresses.size() > 1) {
        std::string other = "If that does not work, try another of this PC's addresses in its place:";
        for (std::size_t i = 1; i < addresses.size() && i < 4; ++i) {
            other += " " + addresses[i].address + " (" + addresses[i].label + ")";
        }
        out.push_back(other);
    }
#ifdef _WIN32
    out.push_back("If Windows asks about the firewall, allow the game on private networks.");
#endif
    return out;
}

std::string VpnLabel(std::string_view adapter) {
    std::string a;
    for (const char c : adapter) {
        a.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    const auto has = [&a](std::string_view s) { return a.find(s) != std::string::npos; };
    const auto starts = [&a](std::string_view s) {
        return a.size() > s.size() && a.compare(0, s.size(), s) == 0 &&
               std::isdigit(static_cast<unsigned char>(a[s.size()])) != 0;
    };
    if (has("radmin")) {
        return "Radmin VPN";
    }
    if (has("hamachi") || starts("ham")) {
        return "Hamachi";
    }
    if (has("zerotier") || (a.size() > 2 && a.compare(0, 2, "zt") == 0 && a.find(' ') == std::string::npos)) {
        return "ZeroTier";
    }
    if (has("tailscale")) {
        return "Tailscale";
    }
    if (has("wireguard") || starts("wg")) {
        return "WireGuard";
    }
    return {};
}

namespace {
// A dotted IPv4 a friend's PC could reach over some network: not 0.x, loopback, link-local or multicast.
bool Reachable(std::string_view ipv4) {
    int part[4] = {0, 0, 0, 0};
    int n = 0;
    bool digit = false;
    for (const char c : ipv4) {
        if (c >= '0' && c <= '9') {
            part[n] = part[n] * 10 + (c - '0');
            digit = true;
            if (part[n] > 255) {
                return false;
            }
        } else if (c == '.' && digit && n < 3) {
            ++n;
            digit = false;
        } else {
            return false;
        }
    }
    return n == 3 && digit && part[0] != 0 && part[0] != 127 && part[0] < 224 && !(part[0] == 169 && part[1] == 254);
}

// Linux's bridges for containers and virtual machines on this PC only (Docker, libvirt, LXC, Podman): no friend
// reaches them.
bool VirtualBridge(std::string_view adapter) {
    const auto starts = [&adapter](std::string_view s) { return adapter.substr(0, s.size()) == s; };
    return starts("docker") || starts("virbr") || starts("br-") || starts("veth") || starts("lxcbr") ||
           starts("podman") || starts("cni");
}

std::vector<std::string> AddressLines(const std::vector<LanAddress>& a, std::uint16_t port, std::string_view password,
                                      bool copied) {
    std::vector<std::string> out = {
        "Friends on your " + a.front().label + " paste this line in Join (address and password):",
        LanJoinText(a.front().address, port, password),
        copied ? "It is copied: paste it into your chat with them (Ctrl+V)."
               : "Type it into your chat with them, exactly as it is."};
    if (a.size() > 1) {
        std::string other = "If that does not work, try another of this PC's addresses in its place:";
        for (std::size_t i = 1; i < a.size() && i < 4; ++i) {
            other += " " + a[i].address + (a[i].label == a.front().label ? "" : " (" + a[i].label + ")");
        }
        out.push_back(other);
    }
#ifdef _WIN32
    out.push_back("If Windows asks about the firewall, allow the game on private networks.");
#endif
    return out;
}
}  // namespace

LanSplit SplitLan(const std::vector<LocalCard>& cards) {
    LanSplit out;
    std::vector<LanAddress> home_routed;
    std::vector<LanAddress> home_other;
    const auto seen = [&](const std::string& address) {
        const auto same = [&address](const LanAddress& x) { return x.address == address; };
        return std::any_of(out.vpn.begin(), out.vpn.end(), same) ||
               std::any_of(home_routed.begin(), home_routed.end(), same) ||
               std::any_of(home_other.begin(), home_other.end(), same);
    };
    for (const LocalCard& c : cards) {
        if (!Reachable(c.address) || seen(c.address) || VirtualBridge(c.adapter)) {
            continue;
        }
        std::string vpn = VpnLabel(c.adapter);
        const std::string range = LanLabel(c.address);
        if (vpn.empty() && !range.empty() && range != "home network") {
            vpn = range;   // no name it knows (none, renamed, another language): the address range says which VPN
        }
        if (!vpn.empty()) {
            out.vpn.push_back(LanAddress{c.address, vpn});
        } else if (range == "home network") {
            (c.gateway ? home_routed : home_other).push_back(LanAddress{c.address, range});
        }
    }
    out.home = std::move(home_routed);
    out.home.insert(out.home.end(), home_other.begin(), home_other.end());
    return out;
}

std::vector<std::string> HomeLines(const std::vector<LanAddress>& home, std::uint16_t port, std::string_view password,
                                   bool copied) {
    if (home.empty()) {
        return {"No home-network address was found on this PC.",
                "Friends somewhere else join with your invite code; on a VPN with you, they use the VPN row."};
    }
    return AddressLines(home, port, password, copied);
}

std::vector<std::string> VpnLines(const std::vector<LanAddress>& vpn, std::uint16_t port, std::string_view password,
                                  bool copied) {
    if (vpn.empty()) {
        return {"No VPN found.",
                "Using one (Radmin VPN, Hamachi, ZeroTier, Tailscale, WireGuard)? Start it, then press this again."};
    }
    return AddressLines(vpn, port, password, copied);
}

std::string HostHeadline(bool copied) {
    return copied ? "Invite code copied to your clipboard. Share it in DMs only: it contains your internet address."
                  : "Your invite code is below. Share it in DMs only: it contains your internet address.";
}


std::vector<std::string> PlainStatus(const HostView& v) {
    std::vector<std::string> out;
    if (v.host && v.problem == HOS_COOP_PROBLEM_BAD_CODE && !v.invite.empty()) {   // the friend's answer code
        return {"That answer code didn't work: ask your friend to copy it again.", "Your code for friends is still:",
                v.invite};
    }
    switch (v.problem) {
    case HOS_COOP_PROBLEM_BAD_CODE:
    case HOS_COOP_PROBLEM_NO_TARGET:
        return {"That code is not right.", "Ask your friend to copy their code again and send it to you."};
    case HOS_COOP_PROBLEM_PORT_TAKEN:
    case HOS_COOP_PROBLEM_ALREADY_RUNNING:
        return {"Another co-op window is already open on this PC.", "Close it (or restart the PC), then press Host again."};
    case HOS_COOP_PROBLEM_HELPER_PORT_TAKEN:
        return {"Another co-op window is already open on this PC.", "Close it (or restart the PC), then press Join again."};
    case HOS_COOP_PROBLEM_NETWORK:
    case HOS_COOP_PROBLEM_CLIENT_FAILED:
    case HOS_COOP_PROBLEM_INTERNAL:
        return {"Co-op could not start.", "Check that this PC is online, then try again. If it keeps failing, restart the game."};
    case HOS_COOP_PROBLEM_BAD_NAME:
        return {"Your name is too long.", "Pick a shorter one in Multiplayer, Settings."};
    default:
        break;
    }
    if (v.host) {
        switch (v.state) {
        case HOS_COOP_STARTING:
            return {"Starting..."};
        case HOS_COOP_HOSTING:
            if (!v.invite.empty()) {
                out = {HostHeadline(v.copied), v.invite};
                if (!v.copied) {
                    out.push_back("Press \"Copy the code again\" to put it on the clipboard.");
                }
                out.push_back(v.players <= 1 ? "Waiting for your friends to join..."
                                             : std::to_string(v.players - 1) + " friend(s) joined.");
            } else if (!v.invite_done) {
                out = {"Making your code..."};
            } else {
                out = {"No code could be made: your internet connection blocks it.",
                       "Friends on the same home network or VPN can still join."};
            }
            if (v.problem == HOS_COOP_PROBLEM_UPNP_REFUSED || v.problem == HOS_COOP_PROBLEM_SYMMETRIC_NAT) {
                out.push_back("Your router may block friends: if they cannot join, turn on UPnP in the router's settings.");
            }
            return out;
        case HOS_COOP_FAILED:
            return {"Hosting did not start.", "Check that this PC is online, then press Host again."};
        default:
            return {};
        }
    }
    switch (v.state) {
    case HOS_COOP_STARTING:
    case HOS_COOP_JOINING:
        return {"Joining your friend..."};
    case HOS_COOP_WAIT_HOST:
        // (row 209: where the host pastes it, by the row's own name on their Host page)
        if (v.problem == HOS_COOP_PROBLEM_NO_WAY) {
            // Row 209: still no way in after the helper's wait (join_helper punch_ms). That is also a host who has not
            // pasted the code yet, so the code stays and the VPN is for when a paste did not help.
            return {"Your friend's router blocks the way in.",
                    "Send your friend this answer code; they press \"Paste friend's answer\" on their Host page:", v.answer,
                    "If they pasted it and nothing happens, both routers are strict or a provider shares one address (CGNAT): both join a free VPN network (Radmin VPN or Tailscale); the host copies the line from \"VPN\" on their Host page and you paste it in Join a game."};
        }
        return {"Your friend's router blocks the way in.",
                "Send your friend this answer code; they press \"Paste friend's answer\" on their Host page:", v.answer,
                "Or ask them to turn on UPnP in their router's settings and host again."};
    case HOS_COOP_CONNECTED:
        if (!v.refused.empty()) {
            return {RefusedLine(v.refused), v.banned ? "Press Leave." : "Press Leave, then join again."};
        }
        if (!v.in_session) {
            return {"Joining your friend's game..."};
        }
        return {"Connected! Choose your file and play."};
    case HOS_COOP_FAILED:
        return {"Your friend could not be reached.",
                "Check that they are hosting right now and that the code is their newest one.",
                "Same home network or VPN as them? Ask for the line from \"Same network\" or \"VPN\" on their Host page."};
    default:
        return {};
    }
}

int ShownState(int state, bool host, bool in_session) {
    return !host && state == HOS_COOP_CONNECTED && !in_session ? HOS_COOP_JOINING : state;
}

std::string ShortReason(const std::string& reason) {
    return reason.substr(0, reason.find(':'));
}

std::string RefusedLine(const std::string& reason) {
    return "Your friend's game turned this one away: " + ShortReason(reason) + ".";
}

void SaveWait::Start(std::uint64_t now_ms, std::string host) {
    phase_ = Phase::kWait;
    host_ = Trim(host).empty() ? std::string("your friend") : std::string(Trim(host));
    last_ms_ = now_ms;
    waited_ms_ = 0;
    live_seen_ = false;
}

SaveWait::Tick SaveWait::Step(std::uint64_t now_ms, const Input& in) {
    Tick t;
    const std::uint64_t dt = std::min<std::uint64_t>(now_ms >= last_ms_ ? now_ms - last_ms_ : 0, kStepGapMs);
    last_ms_ = now_ms;
    if (in.in_game && watching()) {
        phase_ = Phase::kDone;   // (Link is in: the host's save, or a file the player chose)
        return t;
    }
    if (in.session_live) {
        live_seen_ = true;
    } else if (live_seen_ && waiting()) {
        phase_ = Phase::kHostGone;   // (the host dropped before the save came: nothing more comes)
        return t;
    }
    if (phase_ == Phase::kChose) {
        if (!in.title_up) {
            chose_gone_ = true;
            back_ms_ = 0;
        } else if (chose_gone_) {
            back_ms_ += dt;
            if (back_ms_ >= kBackMs) {
                phase_ = Phase::kWait;   // (backed out of their pick: the host's save again)
                waited_ms_ = 0;
                t.rearm = true;
            }
        }
        return t;
    }
    if (waiting()) {
        if (in.host_save_ready) {
            phase_ = Phase::kReady;   // (late too: the player loads it themselves now)
            return t;
        }
        if (in.save_coming) {
            phase_ = Phase::kWait;   // (on its way: "Getting ...", and the wait starts over)
            waited_ms_ = 0;
        } else if (in.title_up) {
            waited_ms_ += dt;
        }
        if (phase_ == Phase::kWait && waited_ms_ >= kWaitSaveMs) {
            phase_ = Phase::kLate;
        }
    }
    return t;
}

std::string SaveWait::Line() const {
    switch (phase_) {
    case Phase::kWait:
        return "Getting " + host_ + "'s save: don't load a save yet...";
    case Phase::kLate:
        return host_ + "'s save is not here yet (the chat says Success when it is): Continue plays your own save, "
                       "New Game starts a new one.";
    case Phase::kReady:
        return host_ + "'s save is in: load any save.";
    case Phase::kHostNone:
        return host_ + " has no save yet: New Game starts one, Continue plays your own.";
    case Phase::kHostGone:
        return host_ + "'s game ended before the save came: Continue plays your own save, New Game starts a new "
                       "one.";
    default:
        return {};
    }
}

SaveListRefresh::Action SaveListRefresh::Step(const Input& in) {
    if (in.stale) {
        pending_ = true;   // (another switch while the game reads: it reads again after this one)
    }
    if (asked_) {
        if (in.manager && !in.busy && in.state == kRead) {
            asked_ = false;
            return Action::done;
        }
        if (in.now_ms - asked_ms_ >= kGiveUpMs) {
            asked_ = false;
            return Action::gave_up;
        }
        return Action::none;
    }
    if (!pending_) {
        wait_ = Wait::none;
        return Action::none;
    }
    wait_ = !in.title ? Wait::title : !in.manager ? Wait::manager : in.busy ? Wait::busy : Wait::none;
    if (wait_ != Wait::none) {
        return Action::none;   // (the game's own reading first: asked once it is idle)
    }
    pending_ = false;
    asked_ = true;
    asked_ms_ = in.now_ms;
    return Action::ask;
}

TitleArrangement ArrangementFor(bool any_save) {
    TitleArrangement a;
    if (any_save) {
        a.btn_num_frame = 3;
        a.sound_type = 1;
        a.texts = {{{"Pa_Button_00/T_Btn_00", "0001"}, {"Pa_Button_01/T_Btn_00", "0000"},
                    {"Pa_Button_02/T_Btn_00", "0002"}}};
        a.count = 3;
    } else {
        a.btn_num_frame = 2;
        a.sound_type = 0;
        a.texts = {{{"Pa_Button_00/T_Btn_00", "0000"}, {"Pa_Button_01/T_Btn_00", "0002"}, {}}};
        a.count = 2;
    }
    return a;
}

TitleMenuLook MenuFor(bool any_save, bool multiplayer, bool one_start) {
    TitleMenuLook l;
    if (multiplayer) {
        l.btn_num_frame = 4;
        // (Join first: most players join a friend's game; docs/hookshot.md "The title menu")
        l.rows = {{{"0101", TitleDo::kJoin}, {"0100", TitleDo::kHost}, {"0102", TitleDo::kSettings},
                   {"0106", TitleDo::kBack}}};
        l.count = 4;
        l.focus = 0;
        return l;
    }
    // Options always on the game's own Options button (its third with a save, its second without), Multiplayer on the
    // slot left: an Options row on another slot chose the game's Options in its place, and the game showed that
    // button pressed, the one with Multiplayer's word on it, a moment first (a tester's run, 2026-11-19).
    if (any_save && !one_start) {
        // (Continue and New Game each their own, no file select of a mod's that does both: the fourth slot shown)
        l.btn_num_frame = 4;
        l.rows = {{{"0001", TitleDo::kGame},
                   {"0000", TitleDo::kGame},
                   {"0105", TitleDo::kGame},
                   {"0104", TitleDo::kMultiplayer}}};
        l.count = 4;
        return l;
    }
    l.btn_num_frame = 3;
    if (any_save) {   // (one Start for Continue and New Game: Start on Continue's slot, Multiplayer on New Game's)
        l.rows = {{{"0103", TitleDo::kGame}, {"0104", TitleDo::kMultiplayer}, {"0105", TitleDo::kGame}, {}}};
    } else {          // (no save: New Game, or Start, then the game's Options, then Multiplayer on the third slot)
        l.rows = {{{one_start ? "0103" : "0000", TitleDo::kGame}, {"0105", TitleDo::kGame},
                   {"0104", TitleDo::kMultiplayer}, {}}};
    }
    l.count = 3;
    return l;
}

const char* MenuHint(TitleDo action) {
    switch (action) {
    case TitleDo::kJoin: return "Play in a friend's game or a public game. You play on their save.";
    case TitleDo::kHost: return "Start a game on this PC; your friends join it. They play on your save.";
    case TitleDo::kSettings: return "Your name, voice, chat key, controller, map.";
    case TitleDo::kBack: return "Back to the title.";
    default: break;
    }
    return nullptr;
}

int RowOf(const TitleMenuLook& look, TitleDo action) {
    for (std::size_t i = 0; i < look.count; ++i) {
        if (look.rows[i].action == action) return static_cast<int>(i);
    }
    return -1;
}

const char* SkipTitleHold(const SkipTitleInput& in) {
    if (in.session_live) {
        return "a co-op session is live";
    }
    if (in.save_wait || in.save_coming) {
        return "the host's save is on its way";
    }
    switch (in.host_state) {
    case HOS_COOP_IDLE: return nullptr;
    case HOS_COOP_STARTING:
    case HOS_COOP_HOSTING: return "co-op is hosting";
    case HOS_COOP_FAILED: return "a co-op host or join was tried";
    default: return "co-op is joining";
    }
}

}  // namespace CoopTitleMenu
