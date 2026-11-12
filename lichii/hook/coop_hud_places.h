// The game's place names for the co-op HUD (docs/overlay.md, "The look"):
// reads the places.json the puppet pack tool writes from the player's own
// game files ({"scene": "name", ...}, tools/puppet_pack/hud.py) and finds a
// scene's name. Header only, standard C++ only, so the tests build it.
#pragma once

#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace CoopHudPlaces {

using Places = std::map<std::string, std::string, std::less<>>;

namespace detail {

inline void SkipSpace(std::string_view s, std::size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
        ++i;
    }
}

inline void PutUtf8(std::string& out, std::uint32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

inline std::optional<std::uint32_t> Hex4(std::string_view s, std::size_t at) {
    if (at + 4 > s.size()) {
        return std::nullopt;
    }
    std::uint32_t v = 0;
    for (std::size_t k = 0; k < 4; ++k) {
        const char c = s[at + k];
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= static_cast<std::uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v |= static_cast<std::uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            v |= static_cast<std::uint32_t>(c - 'A' + 10);
        } else {
            return std::nullopt;
        }
    }
    return v;
}

// A JSON string at s[i] ('"'); i ends after its closing quote.
inline std::optional<std::string> String(std::string_view s, std::size_t& i) {
    if (i >= s.size() || s[i] != '"') {
        return std::nullopt;
    }
    std::string out;
    for (++i; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"') {
            ++i;
            return out;
        }
        if (c != '\\') {
            out += c;
            continue;
        }
        if (++i >= s.size()) {
            return std::nullopt;
        }
        switch (s[i]) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
            auto c1 = Hex4(s, i + 1);
            if (!c1) {
                return std::nullopt;
            }
            i += 4;
            std::uint32_t cp = *c1;
            if (cp >= 0xD800 && cp < 0xDC00 && i + 2 < s.size() && s[i + 1] == '\\' &&
                s[i + 2] == 'u') {
                if (auto c2 = Hex4(s, i + 3); c2 && *c2 >= 0xDC00 && *c2 < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (*c2 - 0xDC00);
                    i += 6;
                }
            }
            PutUtf8(out, cp);
            break;
        }
        default:
            return std::nullopt;
        }
    }
    return std::nullopt;
}

} // namespace detail

// {scene: name} from the text of places.json: one flat object of strings.
// Nothing when the text is not that.
inline std::optional<Places> Parse(std::string_view text) {
    using namespace detail;
    std::size_t i = 0;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) {
        i = 3;  // a UTF-8 byte order mark
    }
    SkipSpace(text, i);
    if (i >= text.size() || text[i] != '{') {
        return std::nullopt;
    }
    ++i;
    Places out;
    SkipSpace(text, i);
    if (i < text.size() && text[i] == '}') {
        return out;
    }
    while (true) {
        SkipSpace(text, i);
        auto key = String(text, i);
        SkipSpace(text, i);
        if (!key || i >= text.size() || text[i] != ':') {
            return std::nullopt;
        }
        ++i;
        SkipSpace(text, i);
        auto value = String(text, i);
        if (!value) {
            return std::nullopt;
        }
        out[std::move(*key)] = std::move(*value);
        SkipSpace(text, i);
        if (i < text.size() && text[i] == ',') {
            ++i;
            continue;
        }
        if (i < text.size() && text[i] == '}') {
            return out;
        }
        return std::nullopt;
    }
}

// The scene name without its age or variant: KokiriForest_Child and
// KakarikoVillage_Adult_Alternative -> KokiriForest, KakarikoVillage
// (as tools/puppet_pack/hud.py scene_base).
inline std::string_view SceneBase(std::string_view scene) {
    constexpr std::string_view kAlt = "_Alternative";
    if (scene.size() > kAlt.size() && scene.ends_with(kAlt)) {
        scene.remove_suffix(kAlt.size());
    }
    for (const std::string_view age : {std::string_view("_Child"), std::string_view("_Adult")}) {
        if (scene.size() > age.size() && scene.ends_with(age)) {
            scene.remove_suffix(age.size());
            break;
        }
    }
    return scene;
}

// "KokiriForest" -> "Kokiri Forest": the scene name made readable, for a
// scene the game has no place name for (or no places.json).
inline std::string Readable(std::string_view scene) {
    const std::string_view base = SceneBase(scene);
    std::string out;
    for (std::size_t i = 0; i < base.size(); ++i) {
        const char c = base[i];
        if (c == '_') {
            if (!out.empty() && out.back() != ' ') {
                out += ' ';
            }
            continue;
        }
        const bool upper = std::isupper(static_cast<unsigned char>(c)) != 0;
        const bool after_lower = i > 0 && std::islower(static_cast<unsigned char>(base[i - 1])) != 0;
        if (upper && after_lower) {
            out += ' ';
        }
        out += c;
    }
    return out;
}

// The place name the HUD shows for `scene` (as PLAYER_SAMPLE has it): the
// game's own name when places has it, else the readable scene name.
inline std::string Name(const Places& places, std::string_view scene) {
    if (const auto it = places.find(scene); it != places.end()) {
        return it->second;
    }
    if (const auto it = places.find(SceneBase(scene)); it != places.end()) {
        return it->second;
    }
    return Readable(scene);
}

} // namespace CoopHudPlaces
