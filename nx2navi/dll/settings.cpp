// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings.hpp"

#include <cstdlib>
#include <string>

namespace CoopNx2navi {
namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

}  // namespace

Settings ParseSettings(std::string_view text, std::vector<std::string>& errors) {
    Settings settings;
    // Our own key is taken out and its line left empty, so the emulator's
    // reader, which refuses keys it does not know, sees the rest with the
    // same line numbers.
    std::string rest;
    rest.reserve(text.size());
    std::size_t line_number = 0;
    if (text.substr(0, 3) == "\xef\xbb\xbf") {
        text.remove_prefix(3);  // a byte order mark an editor may have added
    }
    while (!text.empty()) {
        const auto end = text.find('\n');
        const std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        ++line_number;
        const std::string_view trimmed = Trim(line);
        const auto eq = trimmed.find('=');
        const std::string_view key =
            eq == std::string_view::npos ? std::string_view{} : Trim(trimmed.substr(0, eq));
        bool* flag = key == "share_progress" ? &settings.share_progress
                     : key == "overlay"      ? &settings.overlay
                     : key == "puppet"       ? &settings.puppet
                     : key == "join_at_host" ? &settings.join_at_host
                     : key == "objects"      ? &settings.objects
                     : key == "minimap"      ? &settings.minimap
                     : key == "name_tags"    ? &settings.name_tags
                     : key == "objects_boss" ? &settings.objects_boss
                     : key == "objects_world" ? &settings.objects_world
                     : key == "time"         ? &settings.time
                     : key == "drops"        ? &settings.drops
                     : key == "objects_friendly" ? &settings.objects_friendly
                     : key == "host_save"    ? &settings.host_save
                                             : nullptr;
        if (key == "objects_rolls_apart") {
            const std::string value(Trim(trimmed.substr(eq + 1)));
            char* end = nullptr;
            const long metres = std::strtol(value.c_str(), &end, 10);
            if (!value.empty() && end != nullptr && *end == 0 && metres >= 0 && metres <= 200) {
                settings.objects_rolls_apart = static_cast<int>(metres);
            } else {
                errors.push_back("coop.ini line " + std::to_string(line_number) +
                                 ": objects_rolls_apart must be a number of metres from 0 to 200");
            }
        } else if (key == "objects_ai_hold") {
            const std::string_view value = Trim(trimmed.substr(eq + 1));
            if (value == "0" || value == "1" || value == "2" || value == "3") {
                settings.objects_ai_hold = value[0] - '0';
            } else {
                errors.push_back("coop.ini line " + std::to_string(line_number) +
                                 ": objects_ai_hold must be 0, 1, 2 or 3");
            }
        } else if (key == "follow") {
            const std::string_view value = Trim(trimmed.substr(eq + 1));
            if (value == "host") {
                settings.follow = Settings::Follow::host;
            } else if (value == "anyone") {
                settings.follow = Settings::Follow::anyone;
            } else if (value == "off" || value == "0") {
                settings.follow = Settings::Follow::off;
            } else {
                errors.push_back("coop.ini line " + std::to_string(line_number) +
                                 ": follow must be host, anyone or off");
            }
        } else if (flag != nullptr) {
            const std::string_view value = Trim(trimmed.substr(eq + 1));
            if (value == "1") {
                *flag = true;
            } else if (value == "0") {
                *flag = false;
            } else {
                errors.push_back("coop.ini line " + std::to_string(line_number) + ": " +
                                 std::string(key) + " must be 0 or 1");
            }
        } else {
            rest.append(line);
        }
        rest.push_back('\n');
    }
    CoopHook::ApplyIni(rest, settings.coop, errors);
    if (!errors.empty()) {
        settings.coop.server.clear();
    }
    return settings;
}

std::string SettingsIni(const Settings& s, bool with_password) {
    std::string out = "# Written by the co-op mod (the in-game Multiplayer menu).\r\n";
    const auto line = [&out](std::string_view key, std::string_view value) {
        out.append(key).append(" = ").append(value).append("\r\n");
    };
    line("server", s.coop.server);
    line("port", std::to_string(s.coop.port));
    line("name", s.coop.name);
    if (with_password && !s.coop.password.empty()) {
        line("password", s.coop.password);
    }
    line("share_progress", s.share_progress ? "1" : "0");
    line("overlay", s.overlay ? "1" : "0");
    line("puppet", s.puppet ? "1" : "0");
    line("follow", s.follow == Settings::Follow::host ? "host"
                   : s.follow == Settings::Follow::anyone ? "anyone" : "off");
    line("join_at_host", s.join_at_host ? "1" : "0");
    line("minimap", s.minimap ? "1" : "0");
    line("name_tags", s.name_tags ? "1" : "0");
    line("objects", s.objects ? "1" : "0");
    line("objects_boss", s.objects_boss ? "1" : "0");
    line("objects_world", s.objects_world ? "1" : "0");
    line("time", s.time ? "1" : "0");
    line("drops", s.drops ? "1" : "0");
    line("objects_friendly", s.objects_friendly ? "1" : "0");
    line("objects_rolls_apart", std::to_string(s.objects_rolls_apart));
    line("objects_ai_hold", std::to_string(s.objects_ai_hold));
    line("host_save", s.host_save ? "1" : "0");
    return out;
}

}  // namespace CoopNx2navi
