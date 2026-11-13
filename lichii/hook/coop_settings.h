// Co-op settings for the emulator: server address, port, player name, join
// password and colour.
// From coop.ini in the emulator's config folder, then environment variables,
// which win. Co-op stays off unless a server is set. No emulator code here.
#pragma once

#include <array>
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
    std::string password;          // join password; empty: none
    // The player's colour, 0xRRGGBB (coop_colors.h); unset: the others give
    // this player its slot's colour.
    std::optional<std::uint32_t> color;
    // The player's own key (the Lobby, docs/protocol.md "Lobby"): from
    // player-key.txt beside coop.ini (LoadPlayerKey); sent in HELLO so the
    // host's mutes and bans follow this player, not the name.
    std::optional<std::array<std::uint8_t, 16>> player_key;
    bool enabled() const { return !server.empty(); }
};

// The key's text: 32 hex digits (any case, spaces and line ends around them
// skipped). Nothing for anything else.
std::optional<std::array<std::uint8_t, 16>> ParsePlayerKey(std::string_view text);
std::string PlayerKeyText(const std::array<std::uint8_t, 16>& key);
// The key in `file` (player-key.txt beside coop.ini). A missing or broken
// file gets a new random key written to it, once, so the kit keeps it; if
// that cannot be written the key holds for this run only (`note` says so).
// Not in coop.ini: the launcher writes that file anew from its template.
std::array<std::uint8_t, 16> LoadPlayerKey(const std::filesystem::path& file, std::string& note);

// Reads `key = value` lines. Blank lines, `#` or `;` comments and `[section]`
// headers are skipped. Keys: server (`host` or `host:port`), port, name, password,
// color (a colour name or #RRGGBB, CoopColors::Parse; "" for none).
// Each problem is added to `errors` with its line number.
void ApplyIni(std::string_view text, Settings& settings, std::vector<std::string>& errors);

// LICHII_COOP_SERVER, LICHII_COOP_PORT, LICHII_COOP_NAME, LICHII_COOP_PASSWORD,
// LICHII_COOP_COLOR, when set.
// `get` returns a variable's value, or nothing when it is not set.
void ApplyEnvironment(const std::function<std::optional<std::string>(const char*)>& get,
                      Settings& settings, std::vector<std::string>& errors);

// The ini at `ini` (a missing file is fine), then the process environment.
// Any error turns co-op off (server cleared), so a typo never connects
// somewhere unintended.
Settings LoadSettings(const std::filesystem::path& ini, std::vector<std::string>& errors);

} // namespace CoopHook
