// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_pickups.h"
#include "coop_gold_mode.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>

namespace CoopPickups {

namespace {

constexpr std::size_t kMaxKeys = 4096;

struct Noted {
    std::mutex mutex;
    std::set<std::uint64_t> known;
    std::vector<std::uint64_t> order;
};

Noted& TheNoted() {
    static Noted noted;
    return noted;
}

// The list's lines are short; a longer file is not ours.
constexpr std::size_t kMaxOneTimeBytes = 4u << 20;

struct Listed {
    std::mutex mutex;
    std::set<std::uint64_t> all;
    std::set<std::uint64_t> holders;
};

Listed& TheListed() {
    static Listed listed;
    return listed;
}

bool Hex(std::string_view s, std::uint64_t& out) {
    if (s.empty() || s.size() > 16) {
        return false;
    }
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out, 16);
    return ec == std::errc{} && end == s.data() + s.size();
}

std::filesystem::path PathOf(std::string_view utf8) {
    std::u8string u;
    u.reserve(utf8.size());
    for (const char c : utf8) {
        u.push_back(static_cast<char8_t>(c));
    }
    return std::filesystem::path(u);
}

std::optional<std::string> ReadBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream text;
    text << in.rdbuf();
    if (in.bad()) {
        return std::nullopt;
    }
    return text.str();
}

} // namespace

bool IsPickup(std::string_view name) {
    if (name == "Enemy_Skulltula_Gold" || name == "Obj_Token_Gold" ||
        name == "Obj_Heart_Container" || name.starts_with("Obj_RupeeToken_")) {
        return true;
    }
    // Placed rupees and recovery hearts (breakables, issue 102): only placed
    // ones are noted (a drop has no placement hash). Every placement of them
    // has a 64-bit key that the game never resets (ResetTypeValue 24, read
    // from the romfs). The minigames' rupees are each player's own game.
    // Not the lying small keys: they have no 64-bit key at all; taking one
    // goes as a WORLD_BREAK (CoopWorld::IsLyingKey).
    if (name.starts_with("Obj_Rupee_") || name == "Obj_Heart" || name == "Obj_Heart_InWater") {
        return true;
    }
    // Obj_Heart_Piece, its digging-hole and minigame kinds, the Gerudo Valley one.
    return name.starts_with("Obj_") && name.find("Heart_Piece") != std::string_view::npos;
}

void Note(std::string_view name, std::uint64_t hash) {
    if (hash == 0 || hash == ~std::uint64_t{0} || !IsPickup(name)) {
        return;
    }
    if (CoopGoldMode::NotShared() && (name == "Enemy_Skulltula_Gold" || name == "Obj_Token_Gold")) {
        return;  // row 63, Not shared: each game's Gold Skulltulas are its own
    }
    Noted& n = TheNoted();
    std::lock_guard lock(n.mutex);
    if (n.order.size() < kMaxKeys && n.known.insert(hash).second) {
        n.order.push_back(hash);
    }
}

std::vector<std::uint64_t> Since(std::size_t from) {
    Noted& n = TheNoted();
    std::lock_guard lock(n.mutex);
    if (from >= n.order.size()) {
        return {};
    }
    return {n.order.begin() + static_cast<std::ptrdiff_t>(from), n.order.end()};
}

void Reset() {
    {
        Noted& n = TheNoted();
        std::lock_guard lock(n.mutex);
        n.known.clear();
        n.order.clear();
    }
    SetOneTime({});
}

std::string Fnv64Hex(std::string_view bytes) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const char c : bytes) {
        h = (h ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
    }
    char out[17];
    constexpr char kDigits[] = "0123456789abcdef";
    for (int i = 15; i >= 0; --i) {
        out[i] = kDigits[h & 0xf];
        h >>= 4;
    }
    out[16] = '\0';
    return out;
}

std::optional<OneTime> ParseOneTime(std::string_view text, std::string* why) {
    const auto fail = [why](std::size_t line, std::string_view what) -> std::optional<OneTime> {
        if (why != nullptr) {
            *why = "line " + std::to_string(line) + ": " + std::string(what);
        }
        return std::nullopt;
    };
    OneTime out;
    bool head = false;
    bool served = false;
    std::size_t number = 0;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        ++number;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (number == 1) {
            if (line != "coop-one-time-keys 1") {
                return fail(number, "not a one-time list (coop-one-time-keys 1)");
            }
            head = true;
            continue;
        }
        if (line.empty() || line.starts_with("game ")) {
            continue;
        }
        if (line.starts_with("served ")) {
            // The path may hold spaces: the hash is the last word.
            const std::string_view rest = line.substr(7);
            const std::size_t space = rest.rfind(' ');
            if (space == std::string_view::npos || space == 0) {
                return fail(number, "a served line without a path and a hash");
            }
            const std::string_view path = rest.substr(0, space);
            const std::string_view fnv = rest.substr(space + 1);
            std::uint64_t ignored = 0;
            if (path == "-" && fnv == "-") {
                out.served.clear();
                out.served_fnv.clear();
            } else if (fnv.size() == 16 && Hex(fnv, ignored)) {
                out.served = std::string(path);
                out.served_fnv = std::string(fnv);
            } else {
                return fail(number, "a served line with a bad hash");
            }
            served = true;
            continue;
        }
        const bool pickup = line.starts_with("pickup ");
        if (!pickup && !line.starts_with("holder ")) {
            return fail(number, "not a pickup or holder line");
        }
        std::uint64_t key = 0;
        if (!Hex(line.substr(7), key) || key == 0 || key == ~std::uint64_t{0}) {
            return fail(number, "a bad key");
        }
        (pickup ? out.pickups : out.holders).push_back(key);
    }
    if (!head) {
        return fail(1, "empty");
    }
    if (!served) {
        return fail(number, "no served line");
    }
    return out;
}

std::optional<OneTime> LoadOneTime(const std::string& file, std::string& note) {
    const std::filesystem::path path = PathOf(file);
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        note = "no " + file + " (written at the kit's start): one-time pickups are shared once seen in a game";
        return std::nullopt;
    }
    if (size > kMaxOneTimeBytes) {
        note = file + " is too large: not read";
        return std::nullopt;
    }
    const auto text = ReadBytes(path);
    std::string why;
    std::optional<OneTime> list = text ? ParseOneTime(*text, &why) : std::nullopt;
    if (!list) {
        note = file + (text ? " not read (" + why + ")" : std::string(" could not be read"));
        return std::nullopt;
    }
    note.clear();
    if (!list->holders.empty()) {
        const auto now = list->served.empty() ? std::nullopt : ReadBytes(path.parent_path() / PathOf(list->served));
        if (!now || Fnv64Hex(*now) != list->served_fnv) {
            note = std::to_string(list->holders.size()) + " one-time holders left out: " +
                   (list->served.empty() ? std::string("no served list") : list->served) +
                   " changed since the kit's start (start the game from the kit again)";
            list->holders.clear();
        }
    }
    return list;
}

void SetOneTime(const OneTime& list) {
    Listed& l = TheListed();
    std::lock_guard lock(l.mutex);
    l.all.clear();
    l.holders.clear();
    l.all.insert(list.pickups.begin(), list.pickups.end());
    l.all.insert(list.holders.begin(), list.holders.end());
    l.holders.insert(list.holders.begin(), list.holders.end());
}

bool IsOneTime(std::uint64_t hash) {
    Listed& l = TheListed();
    std::lock_guard lock(l.mutex);
    return l.all.contains(hash);
}

bool IsOneTimeHolder(std::uint64_t hash) {
    Listed& l = TheListed();
    std::lock_guard lock(l.mutex);
    return l.holders.contains(hash);
}

} // namespace CoopPickups
