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
    o.upnp = !Off(IniGet(options_ini, "upnp"));
    if (const auto v = IniGet(options_ini, "server_password"); v && PasswordOk(*v)) {
        o.password = *v;
    }
    o.debug_panel = On(IniGet(options_ini, "LICHII_COOP_DEBUG_PANEL"));
    o.cheats = On(IniGet(options_ini, "LICHII_COOP_CHEATS"));
    o.voice_in_file = IniGet(options_ini, "LICHII_COOP_VOICE").value_or("");
    o.voice = On(o.voice_in_file);
    if (const auto v = IniGet(options_ini, "max_players"); v && !v->empty() && v->size() <= 2 &&
        std::all_of(v->begin(), v->end(), [](char c) { return c >= '0' && c <= '9'; })) {
        o.max_players = std::clamp(std::stoi(*v), kMinPlayers, kMaxPlayers);
    }
    return o;
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
    o = upnp ? IniUnset(o, "upnp") : IniSet(o, "upnp", "no", true);
    o = password.empty() ? IniUnset(o, "server_password") : IniSet(o, "server_password", password, true);
    o = debug_panel ? IniSet(o, "LICHII_COOP_DEBUG_PANEL", "1", false) : IniUnset(o, "LICHII_COOP_DEBUG_PANEL");
    o = cheats ? IniSet(o, "LICHII_COOP_CHEATS", "1", false) : IniUnset(o, "LICHII_COOP_CHEATS");
    // (voip-101: ptt = on, the line gone = off, the default; "open" written by hand stays as it is)
    if (!voice) {
        o = IniUnset(o, "LICHII_COOP_VOICE");
    } else if (!On(IniGet(o, "LICHII_COOP_VOICE"))) {
        o = IniSet(o, "LICHII_COOP_VOICE", VoiceMode(), false);
    }
    o = IniSet(o, "max_players", std::to_string(std::clamp(max_players, kMinPlayers, kMaxPlayers)), true);
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
    };
}

std::string Options::VoiceMode() const {
    std::string s(Trim(voice_in_file));
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s == "open" ? "open" : "ptt";
}

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

bool AddressOk(std::string_view typed) {
    const std::string_view t = Trim(typed);
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
    if (port.empty() || port.size() > 5 ||
        !std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return false;
    }
    const int p = std::stoi(std::string(port));
    return p >= 1 && p <= 65535;
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
        std::string other = "Or with another address in front:";
        for (std::size_t i = 1; i < addresses.size() && i < 4; ++i) {
            other += " " + addresses[i].address + " (" + addresses[i].label + ")";
        }
        out.push_back(other);
    }
    out.push_back("If Windows asks about the firewall, allow the game on private networks.");
    return out;
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
                out = {"Send this code to your friends:", v.invite};
                out.push_back(v.copied ? "It is copied: paste it into your chat with them (Ctrl+V)."
                                       : "Press \"Copy the code again\", then paste it into your chat with them.");
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
        return {"Your friend's router blocks the way in.", "Send your friend this answer code; they paste it into their game:",
                v.answer, "Or ask them to turn on UPnP in their router's settings and host again."};
    case HOS_COOP_CONNECTED:
        return {"Connected! Choose your file and play."};
    case HOS_COOP_FAILED:
        return {"Your friend could not be reached.",
                "Check that they are hosting right now and that the code is their newest one.",
                "Same home network or VPN as them? Ask for the line from \"Same network or VPN\" on their Host page."};
    default:
        return {};
    }
}

void AutoLoad::Start(std::uint64_t now_ms, std::string host) {
    phase_ = Phase::kWait;
    host_ = Trim(host).empty() ? std::string("your friend") : std::string(Trim(host));
    since_ = now_ms;
    next_press_ = 0;
    focused_ = false;
    revealed_ = false;
}

AutoLoad::Tick AutoLoad::Step(std::uint64_t now_ms, bool host_save_ready, bool title_up, bool in_game) {
    Tick t;
    if (in_game && active()) {
        phase_ = Phase::kDone;   // (Link is in: the host's save, or a file the player chose)
        return t;
    }
    if (phase_ == Phase::kWait) {
        if (host_save_ready && title_up) {
            phase_ = Phase::kLoad;
            since_ = now_ms;
            next_press_ = now_ms + 600;   // (Continue shown, then the cursor, then A)
        } else if (now_ms - since_ >= kWaitSaveMs) {
            phase_ = Phase::kNoSave;
        }
        return t;
    }
    if (phase_ != Phase::kLoad) {
        return t;
    }
    if (now_ms - since_ >= kLoadMs) {
        phase_ = Phase::kStuck;
        return t;
    }
    if (!revealed_ && title_up) {
        revealed_ = true;
        t.reveal_continue = true;
        return t;
    }
    if (!focused_ && title_up && now_ms - since_ >= 300) {
        focused_ = true;
        t.focus_continue = true;
    }
    if (now_ms >= next_press_) {
        if (now_ms < next_press_ + kPressHoldMs) {
            t.press_a = true;
        } else {
            next_press_ = now_ms + kPressEveryMs;
        }
    }
    return t;
}

std::string AutoLoad::Line() const {
    switch (phase_) {
    case Phase::kWait:
        return "Getting " + host_ + "'s save...";
    case Phase::kLoad:
        return "Loading " + host_ + "'s save...";
    case Phase::kNoSave:
        return host_ + "'s save did not arrive: choose a file to play.";
    case Phase::kStuck:
        return "Press Continue to play " + host_ + "'s save.";
    default:
        return {};
    }
}

}  // namespace CoopTitleMenu
