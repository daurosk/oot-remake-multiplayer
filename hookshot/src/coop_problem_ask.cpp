// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_problem_ask.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace CoopProblemAsk {
namespace {

constexpr std::string_view kPrefix = "crash-";
constexpr std::string_view kStuckPrefix = "stuck-";

// Days since 1970-01-01 of a date in the Gregorian calendar (H. Hinnant's days_from_civil).
std::int64_t DaysFromCivil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

int Digits(std::string_view s, std::size_t at, std::size_t n) {
    int v = 0;
    for (std::size_t i = at; i < at + n; ++i) v = v * 10 + (s[i] - '0');
    return v;
}

bool Listed(std::string_view asked, std::string_view name) {
    std::size_t at = 0;
    while (at <= asked.size()) {
        std::size_t end = asked.find('\n', at);
        if (end == std::string_view::npos) end = asked.size();
        std::string_view line = asked.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line == name) return true;
        at = end + 1;
    }
    return false;
}

}  // namespace

std::optional<std::int64_t> CrashTime(std::string_view name) {
    // crash-YYYYMMDDTHHMMSSZ[-<pid>-<n>].txt, as hos_crash_record_name takes it; stuck-YYYYMMDDTHHMMSSZ.txt (StuckName)
    if (name.substr(0, kPrefix.size()) != kPrefix && !IsStuck(name)) return std::nullopt;
    const std::string_view p = name.substr(kPrefix.size());   // (both prefixes are 6 long)
    if (p.size() < 16) return std::nullopt;
    for (std::size_t i = 0; i < 16; ++i) {
        const char c = p[i];
        if (i == 8 ? c != 'T' : i == 15 ? c != 'Z' : (c < '0' || c > '9')) return std::nullopt;
    }
    std::string_view s = p.substr(16);
    const auto number = [&s]() {
        std::size_t n = 0;
        while (n < s.size() && s[n] >= '0' && s[n] <= '9') ++n;
        s.remove_prefix(n);
        return n > 0;
    };
    if (!s.empty() && s.front() == '-') {
        s.remove_prefix(1);
        if (!number() || s.empty() || s.front() != '-') return std::nullopt;
        s.remove_prefix(1);
        if (!number()) return std::nullopt;
    }
    if (s != ".txt") return std::nullopt;
    const int mo = Digits(p, 4, 2), d = Digits(p, 6, 2), h = Digits(p, 9, 2), mi = Digits(p, 11, 2), se = Digits(p, 13, 2);
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return std::nullopt;
    const std::int64_t days = DaysFromCivil(Digits(p, 0, 4), static_cast<unsigned>(mo), static_cast<unsigned>(d));
    return days * 86400 + h * 3600 + mi * 60 + se;
}

bool IsStuck(std::string_view name) { return name.substr(0, kStuckPrefix.size()) == kStuckPrefix; }

std::string StuckName(std::int64_t now_s) {
    // (H. Hinnant's civil_from_days, the inverse of DaysFromCivil)
    const std::int64_t days = (now_s >= 0 ? now_s : now_s - 86399) / 86400, secs = now_s - days * 86400;
    const std::int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2 ? 1 : 0);
    const auto two = [](std::int64_t v) {
        return std::string{static_cast<char>('0' + v / 10), static_cast<char>('0' + v % 10)};
    };
    std::string year = std::to_string(y);
    if (year.size() < 4) year.insert(0, 4 - year.size(), '0');
    return std::string(kStuckPrefix) + year + two(m) + two(d) + "T" + two(secs / 3600) + two(secs / 60 % 60) +
           two(secs % 60) + "Z.txt";
}

std::optional<std::string> Due(std::span<const std::string> pending, std::string_view asked, std::int64_t now_s) {
    std::optional<std::string> best;
    std::int64_t best_t = 0;
    for (const std::string& name : pending) {
        const auto t = CrashTime(name);
        if (!t || *t > now_s + 300 || now_s - *t > kMaxAgeS || Listed(asked, name)) continue;
        if (!best || *t > best_t || (*t == best_t && name > *best)) best = name, best_t = *t;
    }
    return best;
}

std::string AskedAfter(std::span<const std::string> pending) {
    std::vector<std::string> names;
    for (const std::string& name : pending) {
        if (CrashTime(name)) names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    std::string out;
    for (const std::string& name : names) out += name + "\n";
    return out;
}

std::vector<std::string> PendingNames(const std::filesystem::path& game_folder) {
    std::vector<std::string> names;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(game_folder / "bug-reports" / "pending", ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto u8 = it->path().filename().u8string();   // (std::string or std::u8string: the bytes either way)
        std::string name;
        for (const auto c : u8) name.push_back(static_cast<char>(c));
        names.push_back(std::move(name));
    }
    return names;
}

namespace {

std::string ReadSmall(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream s;
    s << in.rdbuf();
    return std::move(s).str().substr(0, 64 << 10);
}

}  // namespace

std::string ReadAsked(const std::filesystem::path& game_folder) {
    return ReadSmall(game_folder / "bug-reports" / "coop-asked.txt");
}

std::string ReadSent(const std::filesystem::path& game_folder) {
    return ReadSmall(game_folder / "bug-reports" / "coop-sent.txt");
}

std::vector<std::string> StuckNames(const std::filesystem::path& game_folder) {
    std::vector<std::string> names;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(game_folder / "bug-reports" / "coop-stuck", ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto u8 = it->path().filename().u8string();
        std::string name;
        for (const auto c : u8) name.push_back(static_cast<char>(c));
        if (IsStuck(name)) names.push_back(std::move(name));
    }
    return names;
}

bool WriteStuck(const std::filesystem::path& game_folder, const std::string& name, const std::string& text) {
    std::error_code ec;
    const std::filesystem::path dir = game_folder / "bug-reports" / "coop-stuck";
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir / name, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    return static_cast<bool>(out.flush());
}

bool RemoveStuck(const std::filesystem::path& game_folder, const std::string& name) {
    std::error_code ec;
    std::filesystem::remove(game_folder / "bug-reports" / "coop-stuck" / name, ec);
    return !ec;
}

bool WriteAsked(const std::filesystem::path& game_folder, const std::string& text) {
    std::error_code ec;
    const std::filesystem::path dir = game_folder / "bug-reports";
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path = dir / "coop-asked.txt", part = dir / "coop-asked.txt.part";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << text;
        if (!out.flush()) return false;
    }
    std::filesystem::rename(part, path, ec);
    return !ec;
}

}  // namespace CoopProblemAsk
