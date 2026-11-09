// Co-op settings for the emulator: server address, port and player name.
// From coop.ini in the emulator's config folder, then environment variables,
// which win. Co-op stays off unless a server is set. No emulator code here.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CoopHook {

struct Settings {
    std::string server;            // IP or host name; empty means co-op off
    std::uint16_t port = 27510;    // protocol default
    std::string name = "Player";   // nickname, 1 to 16 characters
    bool enabled() const { return !server.empty(); }
};

// Reads `key = value` lines. Blank lines, `#` or `;` comments and `[section]`
// headers are skipped. Keys: server (`host` or `host:port`), port, name.
// Each problem is added to `errors` with its line number.
void ApplyIni(std::string_view text, Settings& settings, std::vector<std::string>& errors);

// LICHII_COOP_SERVER, LICHII_COOP_PORT, LICHII_COOP_NAME, when set.
// `get` returns a variable's value, or nothing when it is not set.
void ApplyEnvironment(const std::function<std::optional<std::string>(const char*)>& get,
                      Settings& settings, std::vector<std::string>& errors);

// The ini at `ini` (a missing file is fine), then the process environment.
// Any error turns co-op off (server cleared), so a typo never connects
// somewhere unintended.
Settings LoadSettings(const std::filesystem::path& ini, std::vector<std::string>& errors);

} // namespace CoopHook
