#include "coop_settings.h"

#include <cstdlib>
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
        Set(Trim(line.substr(0, eq)), Trim(line.substr(eq + 1)), where, settings, errors);
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
    };
    // Server first, so LICHII_COOP_PORT wins over a port inside it.
    for (const auto& v : kVariables) {
        if (const auto value = get(v.variable)) {
            Set(v.key, Trim(*value), v.variable, settings, errors);
        }
    }
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
