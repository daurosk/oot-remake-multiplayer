// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_gold_mode.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <random>
#include <set>

#include "coop/protocol/traffic_crypto.hpp"

namespace CoopGoldMode {

namespace proto = coop::protocol;

namespace {
std::atomic<Mode> g_choice{Mode::shared};
std::atomic<Mode> g_in_force{Mode::shared};
std::atomic<bool> g_told{false};  // this connection has a mode (a host's settings, or our own Host)
std::mutex g_race_mutex;
std::string g_own_race;   // SetOwnRaceId, or from g_own_key and g_playthrough
std::optional<std::array<std::uint8_t, 16>> g_own_key;   // SetOwnKey
std::string g_playthrough;   // SetPlaythrough
bool g_host_own = false;     // hosting the session this game started (OnHosting with no told mode)
std::string g_race_sent;     // MarkRaceSent
std::string g_race;       // the session's race id ("" until known)
std::filesystem::path g_race_dir;          // SetRaceDir
std::optional<std::string> g_cached_id;    // the id g_cached is for (nullopt: none read yet)
Race g_cached;

bool IsRaceId(std::string_view id) {
    return id.size() == 16 && std::all_of(id.begin(), id.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
}  // namespace

std::string_view Value(Mode mode) {
    return mode == Mode::own ? "own" : "shared";
}

std::optional<Mode> Parse(std::string_view value) {
    if (value == "shared") return Mode::shared;
    if (value == "own") return Mode::own;
    return std::nullopt;
}

std::optional<Mode> FromSettings(const std::vector<proto::ModSetting>& settings) {
    for (const proto::ModSetting& s : settings) {
        if (s.mod_id == kModId && s.key == kKey) return Parse(s.value);
    }
    return std::nullopt;
}

proto::ModSetting AsSetting(Mode mode) {
    return proto::ModSetting{std::string(kModId), std::string(kKey), std::string(Value(mode))};
}

void SetChoice(Mode mode) {
    g_choice.store(mode);
}

Mode Choice() {
    return g_choice.load();
}

void OnConnection() {
    g_told.store(false);
    g_in_force.store(Mode::shared);
    std::lock_guard lock(g_race_mutex);
    g_race.clear();
    g_cached_id.reset();
    g_cached = Race{};
    g_host_own = false;
    g_race_sent.clear();
}

void OnHosting() {
    // A host after a migration keeps the mode its old host told it, and its race id (the game the session plays).
    if (!g_told.exchange(true)) {
        g_in_force.store(g_choice.load());
        std::lock_guard lock(g_race_mutex);
        g_race = g_own_race;
        g_host_own = true;
    }
}

void OnWelcomed(bool hosting) {
    OnConnection();
    if (hosting) OnHosting();
}

void OnHostSettings(const std::vector<proto::ModSetting>& settings) {
    if (const auto mode = FromSettings(settings)) {
        g_in_force.store(*mode);
        g_told.store(true);
    }
    for (const proto::ModSetting& s : settings) {
        if (s.mod_id == kModId && s.key == kRaceKey && IsRaceId(s.value)) {
            std::lock_guard lock(g_race_mutex);
            g_race = s.value;
        }
    }
}

std::string RaceIdOf(const std::array<std::uint8_t, 16>& player_key) {
    return RaceIdOf(player_key, {});
}

std::string RaceIdOf(const std::array<std::uint8_t, 16>& player_key, std::string_view playthrough) {
    std::string label = "oot-coop gold race v1";
    if (!playthrough.empty()) {
        label = "oot-coop gold race v2 ";
        label += playthrough;
    }
    const auto tag = proto::player_key_tag(player_key, label);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string id;
    for (const std::uint8_t b : tag) {
        id.push_back(kHex[b >> 4]);
        id.push_back(kHex[b & 15]);
    }
    return id;
}

void SetOwnRaceId(std::string id) {
    std::lock_guard lock(g_race_mutex);
    g_own_race = IsRaceId(id) ? std::move(id) : std::string();
}

namespace {
// Under g_race_mutex: our own id from the key and the playthrough; the session's too while we host our own.
void OwnRaceLocked() {
    if (!g_own_key) return;
    g_own_race = RaceIdOf(*g_own_key, g_playthrough);
    if (g_host_own) g_race = g_own_race;
}

std::optional<std::map<std::string, std::string>> ParsePlaythroughs(std::string_view text) {
    std::map<std::string, std::string> out;
    bool header = false;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        if (!header) {
            if (line != "oot-coop playthroughs 1") return std::nullopt;
            header = true;
            continue;
        }
        // "slot <id> <name>": the name last, as it may hold spaces.
        if (line.size() < 22 || line.substr(0, 5) != "slot " || line[21] != ' ') return std::nullopt;
        const std::string_view id = line.substr(5, 16);
        if (!IsRaceId(id)) return std::nullopt;
        out[std::string(line.substr(22))] = std::string(id);
    }
    if (!header) return std::nullopt;
    return out;
}
}  // namespace

std::string NewPlaythroughId() {
    std::random_device rd;
    const std::uint64_t v = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    char out[17];
    std::snprintf(out, sizeof out, "%016llx", static_cast<unsigned long long>(v));
    return out;
}

Playthroughs::Playthroughs(std::filesystem::path dir) : dir_(std::move(dir)) {
    if (dir_.empty()) return;
    std::ifstream in(dir_ / "playthroughs.txt", std::ios::binary);
    if (!in) return;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (auto kept = ParsePlaythroughs(text)) slots_ = std::move(*kept);
}

std::string Playthroughs::OnNewGame() {
    if (!new_game_) {
        new_game_ = true;
        current_ = NewPlaythroughId();
    }
    return current_;
}

std::string Playthroughs::OnSlot(const std::string& slot, bool by_write) {
    if (slot.empty() || slot.find('\n') != std::string::npos) return {};
    std::string& id = slots_[slot];
    if (by_write && !current_.empty()) {
        if (id != current_) {
            id = current_;   // the game saved what it plays into this slot
            Save();
        }
        return current_;
    }
    if (id.empty()) {
        id = NewPlaythroughId();
        Save();
    }
    current_ = id;
    new_game_ = false;
    return current_;
}

void Playthroughs::Save() const {
    if (dir_.empty()) return;
    std::string text = "oot-coop playthroughs 1\n";
    for (const auto& [slot, id] : slots_) text += "slot " + id + " " + slot + "\n";
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    const std::filesystem::path to = dir_ / "playthroughs.txt";
    std::filesystem::path tmp = to;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) return;
    }
    std::filesystem::rename(tmp, to, ec);
    if (ec) std::filesystem::remove(tmp, ec);
}

void SetOwnKey(const std::array<std::uint8_t, 16>& player_key) {
    std::lock_guard lock(g_race_mutex);
    g_own_key = player_key;
    OwnRaceLocked();
}

void SetPlaythrough(std::string playthrough) {
    std::lock_guard lock(g_race_mutex);
    g_playthrough = IsRaceId(playthrough) ? std::move(playthrough) : std::string();
    OwnRaceLocked();
}

void MarkRaceSent(std::string id) {
    std::lock_guard lock(g_race_mutex);
    g_race_sent = std::move(id);
}

bool RaceSendDue() {
    if (!NotShared()) return false;
    std::lock_guard lock(g_race_mutex);
    return g_host_own && !g_race.empty() && g_race != g_race_sent;
}

std::string RaceId() {
    std::lock_guard lock(g_race_mutex);
    return g_race;
}

std::optional<proto::ModSetting> RaceSetting() {
    if (!NotShared()) return std::nullopt;
    std::string id = RaceId();
    if (id.empty()) return std::nullopt;
    return proto::ModSetting{std::string(kModId), std::string(kRaceKey), std::move(id)};
}

bool Told() {
    return g_told.load();
}

Mode InForce() {
    return g_in_force.load(std::memory_order_relaxed);
}

namespace {

// The save's layout (tools/autotest/save_flags.py; all little-endian): magic, the pool's offset at 8, from 0x20 a
// table of 8-byte records (a 0 first word opens type 0, 1, ... in order; the end record 0x20 is followed by a pair
// whose second word is the 64-bit key set's offset, 0 for none). The set: ascending u64 keys, a u64 0, the file's end.
struct SaveView {
    std::size_t end_pair = 0;  // the end record's pair
    std::size_t set_at = 0;    // the key set (0: none)
    std::vector<std::uint64_t> keys;
    std::optional<std::size_t> count_at;  // the Int's value word
    std::optional<std::size_t> get_at;    // the Bool's value word
    std::map<std::uint32_t, std::size_t> flag_at;  // each race flag's value word (a Bool or an Int)
};

std::uint32_t Word(const std::vector<std::uint8_t>& d, std::size_t at) {
    return static_cast<std::uint32_t>(d[at]) | static_cast<std::uint32_t>(d[at + 1]) << 8 |
           static_cast<std::uint32_t>(d[at + 2]) << 16 | static_cast<std::uint32_t>(d[at + 3]) << 24;
}
void PutWord(std::vector<std::uint8_t>& d, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) d[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint64_t Quad(const std::vector<std::uint8_t>& d, std::size_t at) {
    return static_cast<std::uint64_t>(Word(d, at)) | static_cast<std::uint64_t>(Word(d, at + 4)) << 32;
}

std::optional<SaveView> ReadSave(const std::vector<std::uint8_t>& d, std::uint32_t count_hash,
                                 std::uint32_t get_hash, std::span<const std::uint32_t> flags, std::string& why) {
    constexpr std::uint32_t kMagic = 0x01020304;
    constexpr std::size_t kTable = 0x20;
    constexpr std::uint32_t kBool = 0;
    constexpr std::uint32_t kInt = 2;
    constexpr std::uint32_t kEnd = 0x20;
    if (d.size() < kTable || Word(d, 0) != kMagic) {
        why = "not a saved game";
        return std::nullopt;
    }
    const std::size_t pool = Word(d, 8);
    if (pool < kTable || pool > d.size()) {
        why = "bad pool offset";
        return std::nullopt;
    }
    SaveView v;
    std::optional<std::uint32_t> kind;
    for (std::size_t at = kTable; at + 8 <= pool; at += 8) {
        const std::uint32_t first = Word(d, at);
        const std::uint32_t second = Word(d, at + 4);
        if (first == 0) {
            if (second != (kind ? *kind + 1 : 0)) {
                why = "types out of order";
                return std::nullopt;
            }
            if (second == kEnd) {
                if (at + 16 > pool) {
                    why = "the end record's pair runs into the pool";
                    return std::nullopt;
                }
                v.end_pair = at + 8;
                break;
            }
            kind = second;
        } else if (!kind) {
            why = "an entry before the first type";
            return std::nullopt;
        } else if (*kind == kInt && first == count_hash) {
            v.count_at = at + 4;
        } else if (*kind == kBool && first == get_hash) {
            v.get_at = at + 4;
        } else if ((*kind == kBool || *kind == kInt) && std::find(flags.begin(), flags.end(), first) != flags.end()) {
            v.flag_at.emplace(first, at + 4);
        }
    }
    if (v.end_pair == 0) {
        why = "no end record";
        return std::nullopt;
    }
    v.set_at = Word(d, v.end_pair + 4);
    if (v.set_at == 0) return v;
    if (v.set_at < pool || v.set_at + 8 > d.size()) {
        why = "the key set is outside the file";
        return std::nullopt;
    }
    for (std::size_t at = v.set_at;; at += 8) {
        if (at + 8 > d.size()) {
            why = "the key set has no closing zero";
            return std::nullopt;
        }
        const std::uint64_t k = Quad(d, at);
        if (k == 0) {
            if (at + 8 != d.size()) {
                why = "bytes after the key set";
                return std::nullopt;
            }
            break;
        }
        if (!v.keys.empty() && k <= v.keys.back()) {
            why = "the key set is not ascending";
            return std::nullopt;
        }
        v.keys.push_back(k);
    }
    return v;
}

}  // namespace

GoldKept KeepRaceIn(std::vector<std::uint8_t>& host, const Race* race,
                    std::span<const std::uint64_t> skulltula_keys, std::uint32_t count_hash,
                    std::uint32_t get_hash, std::span<const std::uint32_t> flag_hashes) {
    GoldKept out;
    std::set<std::uint64_t> gold;
    for (const std::uint64_t k : skulltula_keys) {
        gold.insert(k);
        gold.insert(k + 2);
    }
    const auto h = ReadSave(host, count_hash, get_hash, flag_hashes, out.why);
    if (!h) {
        out.why = "the host's save: " + out.why;
        return out;
    }
    std::vector<std::uint64_t> keys;
    for (const std::uint64_t k : h->keys) {
        if (gold.count(k) != 0) {
            ++out.host_keys;
        } else {
            keys.push_back(k);
        }
    }
    if (race != nullptr) {
        for (const std::uint64_t k : race->keys) {
            if (gold.count(k) != 0) keys.push_back(k);
        }
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    out.own_keys = static_cast<int>(
        std::count_if(keys.begin(), keys.end(), [&](std::uint64_t k) { return gold.count(k) != 0; }));
    if (h->count_at) out.host_count = static_cast<std::int32_t>(Word(host, *h->count_at));
    if (race != nullptr) out.own_count = race->count;
    const std::uint32_t own_get = (race != nullptr && race->get) ? 1 : 0;

    if (h->count_at) PutWord(host, *h->count_at, static_cast<std::uint32_t>(out.own_count));
    if (h->get_at) PutWord(host, *h->get_at, own_get);
    // The race's flags (a reward claimed, a wallet tier, the Stone of Agony): ours, 0 where our race has none.
    for (const auto& [hash, at] : h->flag_at) {
        std::uint32_t own = 0;
        if (race != nullptr) {
            if (const auto it = race->flags.find(hash); it != race->flags.end()) own = it->second;
        }
        PutWord(host, at, own);
        ++out.flags;
    }
    if (h->set_at == 0) {
        // No key set to rewrite: the host's save has no gold keys either. Ours are not added (a new set's place is
        // not known), so the Skulltulas we took are alive again in this save.
        out.keys_left = out.own_keys > 0;
        out.own_keys = 0;
    } else {
        host.resize(h->set_at);
        for (const std::uint64_t k : keys) {
            for (int i = 0; i < 8; ++i) host.push_back(static_cast<std::uint8_t>(k >> (8 * i)));
        }
        host.insert(host.end(), 8, 0);
    }
    out.done = true;
    return out;
}

void ResetForTest() {
    g_choice.store(Mode::shared);
    g_in_force.store(Mode::shared);
    g_told.store(false);
    std::lock_guard lock(g_race_mutex);
    g_own_race.clear();
    g_own_key.reset();
    g_playthrough.clear();
    g_host_own = false;
    g_race_sent.clear();
    g_race.clear();
    g_race_dir.clear();
    g_cached_id.reset();
    g_cached = Race{};
}

bool Race::Join(const Race& other) {
    bool grew = false;
    for (const std::uint64_t k : other.keys) grew |= keys.insert(k).second;
    if (other.count > count) {
        count = other.count;
        grew = true;
    }
    if (other.get && !get) {
        get = true;
        grew = true;
    }
    for (const auto& [hash, value] : other.flags) {
        if (value == 0) continue;
        auto [it, added] = flags.emplace(hash, value);
        if (added || value > it->second) {
            it->second = value;
            grew = true;
        }
    }
    return grew;
}

namespace {
constexpr std::string_view kRaceHeader = "oot-coop gold race 1";
}  // namespace

std::string RaceText(const Race& race) {
    std::string out(kRaceHeader);
    out += "\ncount " + std::to_string(race.count) + "\nget " + (race.get ? "1" : "0") + "\n";
    char line[32];
    for (const std::uint64_t k : race.keys) {
        std::snprintf(line, sizeof line, "key %016llx\n", static_cast<unsigned long long>(k));
        out += line;
    }
    for (const auto& [hash, value] : race.flags) {
        std::snprintf(line, sizeof line, "flag %08x %u\n", static_cast<unsigned>(hash), static_cast<unsigned>(value));
        out += line;
    }
    return out;
}

std::optional<Race> ParseRace(std::string_view text) {
    Race race;
    bool header = false;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        if (!header) {
            if (line != kRaceHeader) return std::nullopt;
            header = true;
            continue;
        }
        const std::size_t sp = line.find(' ');
        if (sp == std::string_view::npos) return std::nullopt;
        const std::string_view name = line.substr(0, sp);
        const std::string_view value = line.substr(sp + 1);
        const char* const first = value.data();
        const char* const last = value.data() + value.size();
        if (name == "key") {
            std::uint64_t k = 0;
            const auto r = std::from_chars(first, last, k, 16);
            if (r.ec != std::errc() || r.ptr != last || k == 0) return std::nullopt;
            race.keys.insert(k);
        } else if (name == "count") {
            std::int32_t n = 0;
            const auto r = std::from_chars(first, last, n);
            if (r.ec != std::errc() || r.ptr != last || n < 0) return std::nullopt;
            race.count = n;
        } else if (name == "get") {
            if (value != "0" && value != "1") return std::nullopt;
            race.get = value == "1";
        } else if (name == "flag") {
            // flag <8 hex> <value>: a race flag's hash and its value (never 0)
            const std::size_t gap = value.find(' ');
            if (gap != 8 || gap + 1 == value.size()) return std::nullopt;
            std::uint32_t hash = 0;
            std::uint32_t v = 0;
            const auto rh = std::from_chars(first, first + gap, hash, 16);
            const auto rv = std::from_chars(first + gap + 1, last, v);
            if (rh.ec != std::errc() || rh.ptr != first + gap || rv.ec != std::errc() || rv.ptr != last || hash == 0 ||
                v == 0) {
                return std::nullopt;
            }
            race.flags[hash] = v;
        } else {
            return std::nullopt;
        }
    }
    if (!header) return std::nullopt;
    return race;
}

std::filesystem::path RaceFile(const std::filesystem::path& dir, std::string_view race_id) {
    if (!IsRaceId(race_id)) return {};
    return dir / (std::string(race_id) + ".txt");
}

std::optional<Race> LoadRace(const std::filesystem::path& dir, std::string_view race_id) {
    const std::filesystem::path file = RaceFile(dir, race_id);
    if (file.empty()) return std::nullopt;
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return ParseRace(text);
}

bool SaveRace(const std::filesystem::path& dir, std::string_view race_id, const Race& race) {
    const std::filesystem::path to = RaceFile(dir, race_id);
    if (to.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::filesystem::path tmp = to;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        const std::string text = RaceText(race);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) return false;
    }
    std::filesystem::rename(tmp, to, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

namespace {
// Under g_race_mutex: the race for the session's id, read from its file when the id changed.
Race& CachedLocked() {
    if (g_cached_id != g_race) {
        g_cached_id = g_race;
        g_cached = Race{};
        if (!g_race.empty() && !g_race_dir.empty()) {
            if (auto kept = LoadRace(g_race_dir, g_race)) g_cached = std::move(*kept);
        }
    }
    return g_cached;
}
}  // namespace

void SetRaceDir(std::filesystem::path dir) {
    std::lock_guard lock(g_race_mutex);
    g_race_dir = std::move(dir);
    g_cached_id.reset();
}

std::filesystem::path RaceDir() {
    std::lock_guard lock(g_race_mutex);
    return g_race_dir;
}

Race TheRace() {
    std::lock_guard lock(g_race_mutex);
    return CachedLocked();
}

bool GrowRace(const Race& seen) {
    std::lock_guard lock(g_race_mutex);
    Race& race = CachedLocked();
    if (!race.Join(seen)) return false;
    if (!g_race.empty() && !g_race_dir.empty()) SaveRace(g_race_dir, g_race, race);
    return true;
}

std::optional<Race> ReadRace(coop::host::FlagStore& game, std::span<const std::uint64_t> skulltula_keys,
                             std::uint32_t count_hash, std::uint32_t get_hash,
                             std::span<const std::uint32_t> flag_hashes) {
    const auto count = game.read(proto::FlagKey(count_hash, std::uint16_t{0}));
    if (!count) return std::nullopt;   // no save in the game yet
    Race race;
    race.count = static_cast<std::int32_t>(count->value);
    if (const auto get = game.read(proto::FlagKey(get_hash, std::uint16_t{0}))) race.get = get->value != 0;
    for (const std::uint64_t k : skulltula_keys) {
        for (const std::uint64_t key : {k, k + 2}) {
            const auto now = game.read(proto::FlagKey::key64_flag(key));
            if (now && now->value != 0) race.keys.insert(key);
        }
    }
    for (const std::uint32_t hash : flag_hashes) {
        const auto now = game.read(proto::FlagKey(hash, std::uint16_t{0}));
        if (now && now->value != 0) race.flags[hash] = now->value;
    }
    return race;
}

PutBack PutRaceBack(coop::host::FlagStore& game, const Race& race, std::uint32_t count_hash, std::uint32_t get_hash) {
    PutBack out;
    const auto put = [&](const proto::FlagEntry& entry) {
        if (game.write(entry)) {
            ++out.written;
        } else {
            ++out.failed;
        }
    };
    for (const std::uint64_t key : race.keys) {
        const proto::FlagKey k = proto::FlagKey::key64_flag(key);
        const auto now = game.read(k);
        if (!now) {
            ++out.unread;
        } else if (now->value == 0) {
            put(proto::FlagEntry(k, proto::FlagKind::boolean, 1));
        }
    }
    const proto::FlagKey count_key(count_hash, std::uint16_t{0});
    if (const auto now = game.read(count_key); !now) {
        ++out.unread;
    } else if (static_cast<std::int32_t>(now->value) < race.count) {
        put(proto::FlagEntry(count_key, now->kind, static_cast<std::uint32_t>(race.count)));
    }
    if (race.get) {
        const proto::FlagKey get_key(get_hash, std::uint16_t{0});
        if (const auto now = game.read(get_key); !now) {
            ++out.unread;
        } else if (now->value == 0) {
            put(proto::FlagEntry(get_key, now->kind, 1));
        }
    }
    for (const auto& [hash, value] : race.flags) {
        const proto::FlagKey key(hash, std::uint16_t{0});
        if (const auto now = game.read(key); !now) {
            ++out.unread;
        } else if (now->value < value) {
            put(proto::FlagEntry(key, now->kind, value));
        }
    }
    return out;
}

}  // namespace CoopGoldMode
