#include "coop_settings.h"

#include "coop_colors.h"

#include <cstdlib>
#include <random>
#include <fstream>
#include <sstream>

namespace CoopHook {
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

// Drops a trailing comment: '#' or ';' after a space or tab, as in
// "server = 192.168.1.20  # IP". One without a space before it stays part of
// the value, so a name like "Link#2" is kept.
std::string_view StripComment(std::string_view value) {
    for (std::size_t i = 1; i < value.size(); ++i) {
        if ((value[i] == '#' || value[i] == ';') && (value[i - 1] == ' ' || value[i - 1] == '\t')) {
            return Trim(value.substr(0, i));
        }
    }
    return value;
}

std::optional<std::uint16_t> ParsePort(std::string_view s) {
    if (s.empty() || s.size() > 5) {
        return std::nullopt;
    }
    unsigned value = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    if (value == 0 || value > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

// 1 to 16 characters (UTF-8 code points), no control characters. The server
// checks it again; this only catches typos early.
bool ValidName(std::string_view name) {
    std::size_t chars = 0;
    for (const char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
        if ((c & 0xc0) != 0x80) {
            ++chars;
        }
    }
    return chars >= 1 && chars <= 16;
}

// Host names and IPv4 addresses: letters, digits, '.', '-', '_'.
bool ValidHost(std::string_view host) {
    if (host.empty() || host.size() > 253) {
        return false;
    }
    for (const char c : host) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

void Set(std::string_view key, std::string_view value, const std::string& where,
         Settings& settings, std::vector<std::string>& errors) {
    if (key == "server") {
        std::string_view host = value;
        if (const auto colon = value.rfind(':'); colon != std::string_view::npos) {
            host = value.substr(0, colon);
            const auto port = ParsePort(value.substr(colon + 1));
            if (!port) {
                errors.push_back(where + ": bad port in server '" + std::string(value) + "'");
                return;
            }
            settings.port = *port;
        }
        if (!ValidHost(host)) {
            errors.push_back(where + ": bad server address '" + std::string(value) + "'");
            return;
        }
        settings.server = std::string(host);
    } else if (key == "port") {
        const auto port = ParsePort(value);
        if (!port) {
            errors.push_back(where + ": port must be 1 to 65535");
            return;
        }
        settings.port = *port;
    } else if (key == "name") {
        if (!ValidName(value)) {
            errors.push_back(where + ": name must be 1 to 16 characters");
            return;
        }
        settings.name = std::string(value);
    } else if (key == "password") {
        // The join password (empty: none). Its HMAC answers the server's
        // challenge; the server, not this, says whether it is right.
        bool control = false;
        for (const char ch : value) {
            const auto b = static_cast<unsigned char>(ch);
            control = control || b < 0x20 || b == 0x7f;
        }
        if (value.size() > 128 || control) {
            errors.push_back(where + ": password must be at most 128 characters, no control characters");
            return;
        }
        settings.password = std::string(value);
    } else if (key == "color") {
        // docs/puppet.md, "Colour". Empty: none chosen (the slot's colour).
        if (value.empty()) {
            settings.color.reset();
            return;
        }
        const auto color = CoopColors::Parse(value);
        if (!color) {
            errors.push_back(where + ": color must be #RRGGBB or one of " + CoopColors::Names());
            return;
        }
        settings.color = color;
    } else {
        errors.push_back(where + ": unknown key '" + std::string(key) + "'");
    }
}

} // namespace

void ApplyIni(std::string_view text, Settings& settings, std::vector<std::string>& errors) {
    std::size_t line_number = 0;
    while (!text.empty()) {
        const auto end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        ++line_number;

        line = Trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';' || line.front() == '[') {
            continue;
        }
        const auto eq = line.find('=');
        const std::string where = "coop.ini line " + std::to_string(line_number);
        if (eq == std::string_view::npos) {
            errors.push_back(where + ": expected key = value");
            continue;
        }
        Set(Trim(line.substr(0, eq)), StripComment(Trim(line.substr(eq + 1))), where, settings,
            errors);
    }
}

void ApplyEnvironment(const std::function<std::optional<std::string>(const char*)>& get,
                      Settings& settings, std::vector<std::string>& errors) {
    static constexpr struct {
        const char* variable;
        const char* key;
    } kVariables[] = {
        {"LICHII_COOP_SERVER", "server"},
        {"LICHII_COOP_PORT", "port"},
        {"LICHII_COOP_NAME", "name"},
        {"LICHII_COOP_PASSWORD", "password"},
        {"LICHII_COOP_COLOR", "color"},
    };
    // Server first, so LICHII_COOP_PORT wins over a port inside it.
    for (const auto& v : kVariables) {
        if (const auto value = get(v.variable)) {
            Set(v.key, Trim(*value), v.variable, settings, errors);
        }
    }
}

std::optional<std::array<std::uint8_t, 16>> ParsePlayerKey(std::string_view text) {
    text = Trim(text);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) {
        text.remove_suffix(1);
    }
    if (text.size() != 32) {
        return std::nullopt;
    }
    const auto digit = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    std::array<std::uint8_t, 16> key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        const int hi = digit(text[2 * i]);
        const int lo = digit(text[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        key[i] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    return key;
}

std::string PlayerKeyText(const std::array<std::uint8_t, 16>& key) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t b : key) {
        out += kHex[b >> 4];
        out += kHex[b & 15];
    }
    return out;
}

std::array<std::uint8_t, 16> LoadPlayerKey(const std::filesystem::path& file, std::string& note) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(file, ec)) {
        std::ifstream in(file, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        if (const auto key = ParsePlayerKey(text.str())) {
            return *key;
        }
    }
    std::random_device random;
    std::array<std::uint8_t, 16> key{};
    for (std::size_t i = 0; i < key.size(); i += 4) {
        const std::uint32_t r = random();
        for (std::size_t j = 0; j < 4; ++j) {
            key[i + j] = static_cast<std::uint8_t>(r >> (8 * j));
        }
    }
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << PlayerKeyText(key) << "\n";
    note = out.good() ? "a new player key was made in " + file.string()
                      : "the player key could not be written to " + file.string() + ": it holds for this run only";
    return key;
}

Settings LoadSettings(const std::filesystem::path& ini, std::vector<std::string>& errors) {
    Settings settings;
    std::error_code ec;
    if (std::filesystem::is_regular_file(ini, ec)) {
        std::ifstream file(ini, std::ios::binary);
        std::ostringstream text;
        text << file.rdbuf();
        ApplyIni(text.str(), settings, errors);
    }
    ApplyEnvironment(
        [](const char* name) -> std::optional<std::string> {
#ifdef _MSC_VER
            char* value = nullptr;
            std::size_t size = 0;
            if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
                return std::nullopt;
            }
            std::string result(value);
            std::free(value);
            return result;
#else
            const char* value = std::getenv(name);
            if (value == nullptr) {
                return std::nullopt;
            }
            return std::string(value);
#endif
        },
        settings, errors);
    if (!errors.empty()) {
        settings.server.clear();
    }
    return settings;
}

} // namespace CoopHook
