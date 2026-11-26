// SPDX-License-Identifier: GPL-3.0-or-later
//
// Difficulty, the rules (coop_difficulty.h). No emulator code.

#include "coop_difficulty.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

namespace CoopDifficulty {
namespace {

namespace proto = coop::protocol;

constexpr std::array<std::string_view, proto::kDifficultyPresets> kPresetKeys{"easy", "normal", "hard",
                                                                              "custom"};

std::string_view Trimmed(std::string_view s) {
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!s.empty() && blank(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && blank(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

std::optional<std::uint16_t> Percent(std::string_view s) {
    unsigned value = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || end != s.data() + s.size() || value > 0xffff) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

std::uint16_t Stepped(std::uint16_t from, int steps, std::uint16_t least, std::uint16_t most) {
    // From a value between two steps (the automatic scaler's 140), the first
    // step lands on the next slider mark, not 25 further.
    const int step = proto::kDifficultyStep;
    int value = from;
    if (steps > 0) {
        value = (value / step + steps) * step;
    } else if (steps < 0) {
        value = ((value + step - 1) / step + steps) * step;
    }
    return static_cast<std::uint16_t>(std::clamp<int>(value, least, most));
}

} // namespace

std::string_view PresetName(Preset preset) {
    switch (preset) {
    case Preset::easy:
        return "Easy";
    case Preset::normal:
        return "Normal";
    case Preset::hard:
        return "Hard";
    case Preset::custom:
        return "Custom";
    }
    return "Normal";
}

Choice WithAuto(const Choice& now, bool on) {
    Choice c = now;
    c.auto_scale = on;
    return c;
}

Choice WithPreset(const Choice& now, Preset preset) {
    Choice c = now;
    c.auto_scale = false;
    c.preset = preset;
    return c;
}

Choice WithSlider(const Choice& now, const Values& in_force, Slider slider, int steps) {
    Choice c = now;
    c.auto_scale = false;
    c.preset = Preset::custom;
    c.custom = proto::clamp_difficulty(in_force);
    switch (slider) {
    case Slider::hp:
        c.custom.hp_pct = Stepped(c.custom.hp_pct, steps, proto::kMinHpPct, proto::kMaxHpPct);
        break;
    case Slider::damage:
        c.custom.damage_pct =
            Stepped(c.custom.damage_pct, steps, proto::kMinDamagePct, proto::kMaxDamagePct);
        break;
    case Slider::count:
        c.custom.count_pct = Stepped(c.custom.count_pct, steps, proto::kMinCountPct, proto::kMaxCountPct);
        break;
    }
    c.custom = proto::bosses_as_enemies(c.custom);  // the host's sliders: bosses as the other enemies
    return c;
}

std::string Describe(const Choice& choice, const Values& values, std::size_t players) {
    std::string line;
    if (choice.auto_scale) {
        line = "Automatic (" + std::to_string(players) + (players == 1 ? " player)" : " players)");
    } else {
        line = std::string(PresetName(choice.preset));
    }
    line += ": life " + std::to_string(values.hp_pct) + "%, damage " + std::to_string(values.damage_pct) +
            "%, enemies " + std::to_string(values.count_pct) + "%";
    if (choice.auto_scale) {  // (the host's own choice: bosses as the other enemies)
        line += "; bosses: life " + std::to_string(values.boss_hp_pct) + "%, damage " +
                std::to_string(values.boss_damage_pct) + "%";
    }
    return line;
}

std::string FormatChoice(const Choice& choice) {
    const Values v = proto::clamp_difficulty(choice.custom);
    const auto preset = std::min<std::size_t>(static_cast<std::size_t>(choice.preset), kPresetKeys.size() - 1);
    std::string text = "# Co-op difficulty as this player last set it as host (docs/play-online.md, \"Difficulty\").\n";
    text += std::string("auto = ") + (choice.auto_scale ? "1" : "0") + "\n";
    text += "preset = " + std::string(kPresetKeys[preset]) + "\n";
    text += "hp = " + std::to_string(v.hp_pct) + "\n";
    text += "damage = " + std::to_string(v.damage_pct) + "\n";
    text += "count = " + std::to_string(v.count_pct) + "\n";
    return text;
}

std::optional<Choice> ParseChoice(std::string_view text) {
    Choice c;
    bool any = false;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        const std::string_view line = Trimmed(text.substr(0, end));
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            return std::nullopt;
        }
        const std::string_view key = Trimmed(line.substr(0, eq));
        const std::string_view value = Trimmed(line.substr(eq + 1));
        if (key == "auto") {
            if (value != "0" && value != "1") {
                return std::nullopt;
            }
            c.auto_scale = value == "1";
        } else if (key == "preset") {
            const auto it = std::find(kPresetKeys.begin(), kPresetKeys.end(), value);
            if (it == kPresetKeys.end()) {
                return std::nullopt;
            }
            c.preset = static_cast<Preset>(it - kPresetKeys.begin());
        } else if (key == "hp" || key == "damage" || key == "count") {
            const auto pct = Percent(value);
            if (!pct) {
                return std::nullopt;
            }
            (key == "hp" ? c.custom.hp_pct : key == "damage" ? c.custom.damage_pct : c.custom.count_pct) =
                *pct;
        } else {
            return std::nullopt;
        }
        any = true;
    }
    if (!any || !proto::valid_difficulty(c.custom)) {
        return std::nullopt;
    }
    return c;
}

namespace {

// One value of /difficulty custom: a whole percent on the slider's marks, within its ends.
std::optional<std::uint16_t> SliderValue(std::string_view s, std::uint16_t least, std::uint16_t most) {
    const auto v = Percent(s.size() > 1 && s.back() == '%' ? s.substr(0, s.size() - 1) : s);
    if (!v || *v < least || *v > most || *v % proto::kDifficultyStep != 0) {
        return std::nullopt;
    }
    return v;
}

std::string LowerWord(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

} // namespace

std::optional<Command> ParseCommand(std::string_view text) {
    constexpr std::string_view kWord = "/difficulty";
    std::string_view t = Trimmed(text);
    if (t.size() < kWord.size() || LowerWord(t.substr(0, kWord.size())) != kWord) {
        return std::nullopt;
    }
    t.remove_prefix(kWord.size());
    if (!t.empty() && t.front() != ' ' && t.front() != '\t') {
        return std::nullopt;  // "/difficultyx": not ours, sent as chat
    }
    std::vector<std::string> words;
    for (t = Trimmed(t); !t.empty(); t = Trimmed(t)) {
        const auto space = t.find_first_of(" \t");
        words.push_back(LowerWord(t.substr(0, space)));
        t = space == std::string_view::npos ? std::string_view{} : t.substr(space);
    }
    Command c;
    if (words.empty()) {
        return c;  // show
    }
    c.kind = Command::Kind::bad;
    if (words.size() == 1 && (words[0] == "auto" || words[0] == "automatic")) {
        c.kind = Command::Kind::change;
        c.auto_scale = true;
        return c;
    }
    for (std::size_t i = 0; i + 1 < kPresetKeys.size(); ++i) {   // (easy, normal, hard; custom takes values)
        if (words.size() == 1 && words[0] == kPresetKeys[i]) {
            c.kind = Command::Kind::change;
            c.preset = static_cast<Preset>(i);
            return c;
        }
    }
    if (words.size() == 4 && words[0] == "custom") {
        const auto hp = SliderValue(words[1], proto::kMinHpPct, proto::kMaxHpPct);
        const auto damage = SliderValue(words[2], proto::kMinDamagePct, proto::kMaxDamagePct);
        const auto count = SliderValue(words[3], proto::kMinCountPct, proto::kMaxCountPct);
        if (hp && damage && count) {
            c.kind = Command::Kind::change;
            c.preset = Preset::custom;
            c.custom = proto::clamp_difficulty({*hp, *damage, *count});
        }
    }
    return c;
}

Choice Applied(const Choice& now, const Command& change) {
    if (change.auto_scale) {
        return WithAuto(now, true);
    }
    Choice c = WithPreset(now, change.preset);
    if (change.preset == Preset::custom) {
        c.custom = change.custom;
    }
    return c;
}

bool OffersKept(bool hosts, std::uint8_t set_by, const std::optional<Choice>& kept, bool offered) {
    return hosts && set_by == 0 && kept && !kept->auto_scale && !offered;
}

std::int32_t EnemyTakes(std::int32_t sum, std::uint16_t hp_pct, std::int32_t& carry) {
    if (sum <= 0 || hp_pct == 0) {
        return sum;
    }
    const std::int64_t total =
        static_cast<std::int64_t>(sum) * 100 + std::clamp<std::int32_t>(carry, 0, hp_pct - 1);
    carry = static_cast<std::int32_t>(total % hp_pct);
    return static_cast<std::int32_t>(std::min<std::int64_t>(total / hp_pct, INT32_MAX));
}

std::int32_t LinkTakes(std::int32_t sum, std::uint16_t damage_pct, std::int32_t& carry) {
    if (sum <= 0) {
        return sum;
    }
    const std::int64_t total =
        static_cast<std::int64_t>(sum) * damage_pct + std::clamp<std::int32_t>(carry, 0, 99);
    carry = static_cast<std::int32_t>(total % 100);
    return static_cast<std::int32_t>(std::min<std::int64_t>(total / 100, INT32_MAX));
}

bool IsEnemy(std::string_view actor_name, bool enemy_shot) {
    return enemy_shot || actor_name.starts_with("Enemy_");
}

std::size_t Kinds::Hash(std::uint64_t address) noexcept {
    return static_cast<std::size_t>(((address >> 4) * 0x9e3779b97f4a7c15ull) >> 54);  // 10 bits: kSlots
}

bool Kinds::Mark(std::uint64_t address, Kind kind, std::uint64_t fold_key) noexcept {
    if (address == 0 || address == kFreed) {
        return false;
    }
    const auto k = static_cast<std::uint8_t>(kind);
    Slot* room = nullptr;  // the first freed or empty slot on the way: a new address goes there
    std::size_t room_at = 0;
    for (std::size_t i = 0; i < kProbes; ++i) {
        const std::size_t at = (Hash(address) + i) % kSlots;
        Slot& s = slots_[at];
        const std::uint64_t key = s.key.load(std::memory_order_acquire);
        if (key == address) {
            marked_[at] = walk_;
            s.fold_key.store(fold_key, std::memory_order_release);
            if (s.kind.load(std::memory_order_relaxed) != k) {
                s.carry.store(0, std::memory_order_relaxed);  // another actor at that address now
                s.pass.store(0, std::memory_order_relaxed);
                s.kind.store(k, std::memory_order_release);
            }
            return true;
        }
        if (key == kFreed || key == 0) {
            if (room == nullptr) {
                room = &s;
                room_at = at;
            }
            if (key == 0) {
                break;  // the end of the run: the address is not in the table
            }
            // (a freed slot: the address may still be further on)
        }
    }
    if (room == nullptr) {
        unmarked_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // (only this writer inserts: no race on the key) The kind goes in first,
    // so a reader that finds the key reads its kind.
    marked_[room_at] = walk_;
    room->carry.store(0, std::memory_order_relaxed);
    room->pass.store(0, std::memory_order_relaxed);
    room->fold_key.store(fold_key, std::memory_order_relaxed);
    room->kind.store(k, std::memory_order_release);
    room->key.store(address, std::memory_order_release);
    return true;
}

void Kinds::Sweep() noexcept {
    std::uint64_t held = 0;
    for (std::size_t at = 0; at < kSlots; ++at) {
        Slot& s = slots_[at];
        const std::uint64_t key = s.key.load(std::memory_order_relaxed);
        if (key == 0 || key == kFreed) {
            continue;
        }
        if (walk_ - marked_[at] >= kKeepWalks) {
            // Its actor is gone. The kind first: a reader that still finds
            // the key reads no enemy. (A reader inside Step with this slot
            // can at most leave a carry of under one life point in it.)
            s.kind.store(0, std::memory_order_release);
            s.key.store(kFreed, std::memory_order_release);
        } else {
            ++held;
        }
    }
    held_.store(held, std::memory_order_relaxed);
    unmarked_last_.store(unmarked_.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
    ++walk_;
}

void Kinds::Forget(std::uint64_t address) noexcept {
    const Kind kind = Of(address);
    if (kind == Kind::enemy || kind == Kind::enemy_life || kind == Kind::folded_enemy ||
        kind == Kind::folded_enemy_life || kind == Kind::boss || kind == Kind::boss_life) {
        Mark(address, Kind::none);  // (it has a slot: never refused)
    }
}

void Kinds::Pass(std::uint64_t component, std::int32_t amount, std::uint64_t now_ms) noexcept {
    Slot* s = Find(component);
    if (s == nullptr || amount <= 0) {
        return;
    }
    // A pass still waiting adds up (two replays before one step); an old one is gone.
    const bool waiting = now_ms < s->pass_until.load(std::memory_order_relaxed);
    const std::int32_t had = waiting ? s->pass.load(std::memory_order_relaxed) : 0;
    s->pass_until.store(now_ms + kPassMs, std::memory_order_relaxed);
    s->pass.store(had + amount, std::memory_order_relaxed);
}

std::int32_t Kinds::Passed(Slot& s, std::int32_t sum, std::uint64_t now_ms) noexcept {
    const std::int32_t pass = s.pass.load(std::memory_order_relaxed);
    if (pass <= 0) {
        return 0;
    }
    if (now_ms >= s.pass_until.load(std::memory_order_relaxed)) {
        s.pass.store(0, std::memory_order_relaxed);  // never taken as a hit (written as life instead)
        return 0;
    }
    const std::int32_t passed = std::min(pass, sum);
    s.pass.store(pass - passed, std::memory_order_relaxed);
    return passed;
}

const Kinds::Slot* Kinds::Find(std::uint64_t address) const noexcept {
    if (address == 0 || address == kFreed) {  // (a freed slot is passed over, never found)
        return nullptr;
    }
    for (std::size_t i = 0; i < kProbes; ++i) {
        const Slot& s = slots_[(Hash(address) + i) % kSlots];
        const std::uint64_t key = s.key.load(std::memory_order_acquire);
        if (key == 0) {
            return nullptr;
        }
        if (key == address) {
            return &s;
        }
    }
    return nullptr;
}

Kinds::Slot* Kinds::Find(std::uint64_t address) noexcept {
    return const_cast<Slot*>(static_cast<const Kinds*>(this)->Find(address));
}

Kind Kinds::Of(std::uint64_t address) const noexcept {
    const Slot* s = Find(address);
    return s == nullptr ? Kind::none : static_cast<Kind>(s->kind.load(std::memory_order_acquire));
}

void Kinds::Clear() noexcept {
    for (Slot& s : slots_) {
        s.key.store(0, std::memory_order_release);
        s.kind.store(0, std::memory_order_relaxed);
        s.fold_key.store(0, std::memory_order_relaxed);
        s.carry.store(0, std::memory_order_relaxed);
        s.pass.store(0, std::memory_order_relaxed);
    }
    marked_.fill(0);
    walk_ = 1;
    unmarked_.store(0, std::memory_order_relaxed);
    unmarked_last_.store(0, std::memory_order_relaxed);
    held_.store(0, std::memory_order_relaxed);
}

std::int32_t Kinds::Scaled(Slot& s, std::int32_t sum, std::uint16_t pct, Rule rule, std::int32_t first,
                           std::atomic<std::uint64_t>& hits) noexcept {
    if (pct == 100) {
        return sum;
    }
    // The game steps each component once a frame, one at a time, so a plain
    // load and store of its carry is enough.
    const std::uint32_t packed = s.carry.load(std::memory_order_relaxed);
    std::int32_t carry = (packed >> 16) == pct
                             ? static_cast<std::int32_t>(packed & 0xffff)
                             : static_cast<std::int32_t>(std::min<std::int64_t>(std::int64_t{first} * sum, 0xffff));
    const std::int32_t taken = rule(sum, pct, carry);
    s.carry.store(static_cast<std::uint32_t>(pct) << 16 | static_cast<std::uint32_t>(carry & 0xffff),
                  std::memory_order_relaxed);
    hits.fetch_add(1, std::memory_order_relaxed);
    return taken;
}

std::uint64_t SteadyMs() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

Kinds& TheKinds() {
    static Kinds kinds;
    return kinds;
}

unsigned ExtraCount(std::uint64_t key, std::uint16_t count_pct) {
    if (count_pct <= 100) {
        return 0;
    }
    // splitmix64's finalizer: placement hashes of one room differ in few bits.
    std::uint64_t x = key + 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x ^= x >> 31;
    const unsigned whole = count_pct / 100u - 1u;
    return whole + (x % 100u < count_pct % 100u ? 1u : 0u);
}

namespace {

std::uint64_t Mix(std::uint64_t x) {
    // splitmix64's finalizer
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

} // namespace

namespace {

// The game's own placement data (tools/puppet_pack/designed_enemies.py reads
// the Banc placements): placed enemies an extra copy beside them could spoil.
enum class Designed : std::uint8_t { rail, shutter, event, group, drop };
struct DesignedPlacement {
    std::uint64_t hash;
    Designed why;
};
constexpr DesignedPlacement kDesigned[] = {
#include "coop_difficulty_placements.inc"
};

std::string_view DesignedWhy(Designed why) {
    switch (why) {
        case Designed::rail: return "rail: a patrol on its path";
        case Designed::shutter: return "shutter: a shutter or door waits for it";
        case Designed::event: return "event: an event or a manager uses it";
        case Designed::group: return "group: linked with another enemy";
        case Designed::drop: return "drop: a set item (a key)";
    }
    return {};
}

const DesignedPlacement* DesignedOf(std::uint64_t hash) {
    // Sorted by the generator: a binary search.
    const auto* end = std::end(kDesigned);
    const auto* it = std::lower_bound(std::begin(kDesigned), end, hash,
                                      [](const DesignedPlacement& p, std::uint64_t h) { return p.hash < h; });
    return it != end && it->hash == hash ? it : nullptr;
}

// The rules by actor name, first match wins: one table, a reason each.
enum class Match : std::uint8_t { prefix, contains };
struct KindRule {
    Match match;
    std::string_view text;
    Treatment treatment;
    std::string_view why;
};
constexpr KindRule kKindRules[] = {
    // Nothing more: bosses scale already; parts, stand-ins and no fights.
    {Match::contains, "Dictionary", Treatment::as_placed, "picture book"},
    {Match::prefix, "Enemy_DungeonBoss_", Treatment::as_placed, "boss"},
    {Match::prefix, "Enemy_MiddleBoss_", Treatment::as_placed, "mini-boss"},
    {Match::contains, "BattleManager", Treatment::as_placed, "boss"},
    {Match::contains, "Haribote", Treatment::as_placed, "cutscene stand-in"},
    {Match::prefix, "Enemy_GhostPicturePerformer_", Treatment::as_placed, "painting show"},
    {Match::contains, "BigOctoChild", Treatment::as_placed, "part of a mini-boss"},
    {Match::contains, "_Hand", Treatment::as_placed, "part of a bigger enemy"},
    {Match::contains, "Attention", Treatment::as_placed, "part of a bigger enemy"},
    {Match::contains, "MagicField", Treatment::as_placed, "part of a bigger enemy"},
    {Match::contains, "FireBody", Treatment::as_placed, "part of a bigger enemy"},
    {Match::contains, "AircraftCarrier", Treatment::as_placed, "part of a bigger enemy"},
    {Match::contains, "Larva", Treatment::as_placed, "made by a bigger enemy"},
    {Match::contains, "Egg", Treatment::as_placed, "made by a bigger enemy"},
    {Match::contains, "Baby", Treatment::as_placed, "made by a bigger enemy"},
    {Match::contains, "SpawnBaby", Treatment::as_placed, "made by a bigger enemy"},
    {Match::contains, "Manager", Treatment::as_placed, "a manager, no fighter"},
    {Match::contains, "Flock", Treatment::as_placed, "a flock's manager"},
    {Match::contains, "Prefab", Treatment::as_placed, "a helper"},
    {Match::contains, "Open_", Treatment::as_placed, "a helper"},
    {Match::contains, "WipedOut", Treatment::as_placed, "a helper"},
    {Match::contains, "Horse", Treatment::as_placed, "a boss's horse"},
    {Match::contains, "Bullet", Treatment::as_placed, "a shot"},
    {Match::contains, "ShockWave", Treatment::as_placed, "a shot"},
    {Match::contains, "Battle", Treatment::as_placed, "a helper"},
    {Match::contains, "Scrub", Treatment::as_placed, "scrub: a talk, a shop or an order puzzle"},
    {Match::contains, "Skulltula_Gold", Treatment::as_placed, "Gold Skulltula: a counted token"},
    {Match::prefix, "Enemy_SkullKid", Treatment::as_placed, "story character"},
    {Match::prefix, "Enemy_CrazyFloorTile", Treatment::as_placed, "flying floor tile: part of its room"},
    {Match::prefix, "Enemy_TrickyDoor", Treatment::as_placed, "fake door: part of its room"},
    // Before "Wall": the Wallmaster falls from a ceiling, it is no wall enemy.
    {Match::prefix, "Enemy_Wallmaster", Treatment::fold, "puzzle: falls from the ceiling at a set spot"},
    {Match::contains, "Wall", Treatment::as_placed, "on a wall"},
    {Match::contains, "walltula", Treatment::as_placed, "on a wall"},
    // The puzzle and set-piece kinds: no copies, their weight instead.
    {Match::contains, "_Truth", Treatment::fold, "puzzle: hidden for the Lens of Truth"},
    {Match::contains, "AlwaysRespawn", Treatment::fold, "set piece: comes back at its spot"},
    {Match::contains, "ClimaxOnly", Treatment::fold, "set piece: an event's"},
    {Match::prefix, "Enemy_Amos", Treatment::fold, "puzzle: Armos statues"},
    {Match::prefix, "Enemy_Beamos", Treatment::fold, "puzzle: a turret at a set spot"},
    {Match::prefix, "Enemy_LikeLike", Treatment::fold, "set piece: guards a spot"},
    {Match::prefix, "Enemy_Floormaster", Treatment::fold, "set piece: splits in three"},
    {Match::prefix, "Enemy_DeadHand", Treatment::fold, "set piece: one in its room"},
    {Match::prefix, "Enemy_Poe_Big", Treatment::fold, "set piece: a Big Poe, counted for the collector"},
    {Match::prefix, "Enemy_Poe_Amy", Treatment::fold, "puzzle: a Poe sister"},
    {Match::prefix, "Enemy_Poe_Beth", Treatment::fold, "puzzle: a Poe sister"},
    {Match::prefix, "Enemy_Poe_Joelle", Treatment::fold, "puzzle: a Poe sister"},
    {Match::prefix, "Enemy_Poe_Meg", Treatment::fold, "puzzle: a Poe sister"},
    {Match::prefix, "Enemy_Poe_SistersBase", Treatment::fold, "puzzle: a Poe sister"},
    {Match::prefix, "Enemy_Poe_Sharp", Treatment::fold, "set piece: a composer brother"},
    {Match::prefix, "Enemy_Poe_Flat", Treatment::fold, "set piece: a composer brother"},
    {Match::prefix, "Enemy_Shabom", Treatment::fold, "puzzle: the bubbles of a room"},
    {Match::prefix, "Enemy_ReDead", Treatment::fold, "set piece: stands at a set spot"},
    // Water: they live in or under water, and the floor markers are all on land (extra_spots.py).
    {Match::prefix, "Enemy_Stinger", Treatment::fold, "water: lives in the water, no land spot for it"},
    {Match::prefix, "Enemy_ShellBlade", Treatment::fold, "water: lies on the bottom, no land spot for it"},
    {Match::prefix, "Enemy_Octorok", Treatment::fold, "water: shoots from the water, no land spot for it"},
    {Match::prefix, "Enemy_TorchSlug", Treatment::fold, "puzzle: relights the torches"},
    {Match::prefix, "Enemy_Anubis", Treatment::fold, "puzzle: follows Link, beaten with fire"},
    {Match::prefix, "Enemy_IronKnuckle", Treatment::fold, "set piece: a one-on-one fight"},
    {Match::prefix, "Enemy_Guay_Boss", Treatment::fold, "set piece: one big Guay"},
    {Match::prefix, "Enemy_Moblin_Club", Treatment::fold, "set piece: the meadow's gatekeeper"},
    {Match::prefix, "Enemy_FlyingPot", Treatment::fold, "set piece: the room's own pots"},
};

const KindRule* KindRuleOf(std::string_view name) {
    for (const KindRule& rule : kKindRules) {
        const bool hit = rule.match == Match::prefix ? name.starts_with(rule.text)
                                                     : name.find(rule.text) != std::string_view::npos;
        if (hit) {
            return &rule;
        }
    }
    return nullptr;
}

}  // namespace

std::uint16_t Folded(std::uint16_t pct, std::uint16_t count_pct) {
    if (count_pct <= 100) {
        return pct;
    }
    const std::uint32_t folded = static_cast<std::uint32_t>(pct) * count_pct / 100u;
    return static_cast<std::uint16_t>(std::min<std::uint32_t>(folded, 0xffff));
}

std::uint16_t FoldCount(std::uint64_t fold_key, std::uint16_t count_pct) {
    if (fold_key == 0 || count_pct <= 100) {
        return count_pct;
    }
    return static_cast<std::uint16_t>(std::min<std::uint32_t>(100u * (1u + ExtraCount(fold_key, count_pct)), 0xffff));
}

std::uint64_t PackValues(const Values& v) noexcept {
    const auto field = [](std::uint16_t pct, unsigned at) {
        return static_cast<std::uint64_t>(std::min<std::uint16_t>(pct, 0xfff)) << at;
    };
    return field(v.hp_pct, 0) | field(v.damage_pct, 12) | field(v.count_pct, 24) | field(v.boss_hp_pct, 36) |
           field(v.boss_damage_pct, 48);
}

Values UnpackValues(std::uint64_t word) noexcept {
    const auto field = [word](unsigned at) { return static_cast<std::uint16_t>(word >> at & 0xfff); };
    Values v;
    v.hp_pct = field(0);
    v.damage_pct = field(12);
    v.count_pct = field(24);
    v.boss_hp_pct = field(36);
    v.boss_damage_pct = field(48);
    return v;
}

bool ScalesAsBoss(std::string_view name) {
    const KindRule* rule = name.starts_with("Enemy_") ? KindRuleOf(name) : nullptr;
    return rule != nullptr && (rule->why == "boss" || rule->why == "mini-boss");
}

namespace {
// A placement hash our baked table does not have: a mod added it (OOTRR Difficulty), so it is never folded by kind
// either: the mod already scaled it (ootrr-diff-compat D1). Hash 0 and all ones are made actors, not placements.
bool ModAdded(std::uint64_t hash) {
    return hash != 0 && hash != ~std::uint64_t{0} && !AreaOf(hash).has_value();
}
}  // namespace

Kind AttackerKind(std::string_view name, std::uint64_t hash, bool no_spot) {
    if (ScalesAsBoss(name)) {
        return Kind::boss;
    }
    if (ModAdded(hash)) {
        return Kind::enemy;
    }
    return no_spot || TreatmentOf(name, hash) == Treatment::fold ? Kind::folded_enemy : Kind::enemy;
}

Kind LifeKind(std::string_view name, std::uint64_t hash, bool no_spot) {
    if (ScalesAsBoss(name)) {
        return Kind::boss_life;
    }
    if (ModAdded(hash)) {
        return Kind::enemy_life;
    }
    return no_spot || TreatmentOf(name, hash) == Treatment::fold ? Kind::folded_enemy_life : Kind::enemy_life;
}

Treatment TreatmentOf(std::string_view name, std::uint64_t hash) {
    if (!name.starts_with("Enemy_")) {
        return Treatment::as_placed;
    }
    const KindRule* rule = KindRuleOf(name);
    if (rule != nullptr && rule->treatment == Treatment::as_placed) {
        return Treatment::as_placed;  // a boss or a part stays so, whatever its links
    }
    if (rule != nullptr || DesignedOf(hash) != nullptr) {
        return Treatment::fold;
    }
    return Treatment::copies;
}

std::string_view TreatmentWhy(std::string_view name, std::uint64_t hash) {
    if (!name.starts_with("Enemy_")) {
        return "not an enemy";
    }
    const KindRule* rule = KindRuleOf(name);
    if (rule != nullptr && rule->treatment == Treatment::as_placed) {
        return rule->why;
    }
    if (const DesignedPlacement* placed = DesignedOf(hash)) {
        return DesignedWhy(placed->why);
    }
    return rule != nullptr ? rule->why : std::string_view{};
}

bool GetsExtras(std::string_view name, std::uint64_t hash, bool has_life, bool special) {
    if (special || !has_life || hash == 0 || hash == ~std::uint64_t{0}) {
        return false;
    }
    return TreatmentOf(name, hash) == Treatment::copies;
}

std::uint64_t ExtraHash(std::uint64_t hash, unsigned index) {
    std::uint64_t h = Mix(hash ^ Mix(0xd1ff1c0175ULL + index));
    if (h == 0 || h == ~std::uint64_t{0} || h == hash) {
        h = Mix(h + 1) | 1;  // a second try (odd, so never 0)
    }
    return h;
}

float ApartFor(std::string_view name) {
    constexpr std::array<std::string_view, 9> kBig{"Enemy_Moblin",  "Enemy_Lizalfos", "Enemy_Dinolfos",
                                                   "Enemy_Stalfos", "Enemy_Gibdo",    "Enemy_Wolfos",
                                                   "Enemy_Lynel",   "Enemy_IronKnuckle", "Enemy_Stalchild_Big"};
    constexpr std::array<std::string_view, 8> kSmall{"Enemy_Keese",   "Enemy_Octorok", "Enemy_Dekubaba",
                                                     "Enemy_Tektite", "Enemy_Bubble",  "Enemy_Guay",
                                                     "Enemy_Skulltula", "Enemy_MadScrub"};
    const auto starts = [&](const auto& list) {
        return std::any_of(list.begin(), list.end(), [&](std::string_view p) { return name.starts_with(p); });
    };
    return starts(kBig) ? 3.5f : starts(kSmall) ? 2.0f : 2.5f;
}

std::optional<Point> FindSpot(const Point& from, std::string_view name, std::span<const Marker> candidates,
                              const std::vector<Point>& apart) {
    const float least = ApartFor(name);
    const auto flat = [](const Point& a, const Point& b) {
        const float dx = a[0] - b[0];
        const float dz = a[2] - b[2];
        return std::sqrt(dx * dx + dz * dz);
    };
    const auto near = [&](const Point& at, const Point& other) { return flat(at, other) < least - 1e-3f; };
    const Point* best = nullptr;
    float best_d = 0.0f;
    for (const Marker& m : candidates) {  // nearest first; among equals the first (the same in every game)
        const Point& c = m.at;
        if ((m.fx != 0.0f || m.fz != 0.0f) && (from[0] - c[0]) * m.fx + (from[2] - c[2]) * m.fz <= 0.0f) {
            continue;  // a door's landing that faces away: the room behind it is another
        }
        const float d = flat(from, c);
        if (d > kSpotReachM || std::fabs(c[1] - from[1]) > kSpotStepM || near(c, from) ||
            std::any_of(apart.begin(), apart.end(), [&](const Point& t) { return near(c, t); })) {
            continue;
        }
        if (best == nullptr || d < best_d) {
            best = &c;
            best_d = d;
        }
    }
    return best != nullptr ? std::optional<Point>(*best) : std::nullopt;
}

namespace {

// The game's placement data for the extras' spots (tools/puppet_pack/extra_spots.py).
using SpotEnemy = AreaEnemy;
using SpotPoint = Marker;
struct SpotArea {
    std::string_view name;
    std::uint32_t enemy_begin, enemy_end, ground_begin, ground_end;
};
#include "coop_difficulty_spots.inc"

} // namespace

std::optional<Area> AreaOf(std::uint64_t hash) {
    static const std::map<std::uint64_t, std::size_t> kAreaOf = [] {
        std::map<std::uint64_t, std::size_t> m;
        for (std::size_t i = 0; i < std::size(kSpotAreas); ++i) {
            for (std::uint32_t e = kSpotAreas[i].enemy_begin; e < kSpotAreas[i].enemy_end; ++e) {
                m.emplace(kSpotEnemies[e].hash, i);  // (a hash in two areas: the first)
            }
        }
        return m;
    }();
    const auto it = kAreaOf.find(hash);
    if (it == kAreaOf.end()) {
        return std::nullopt;
    }
    const SpotArea& a = kSpotAreas[it->second];
    return Area{a.name, std::span<const AreaEnemy>(kSpotEnemies).subspan(a.enemy_begin, a.enemy_end - a.enemy_begin),
                std::span<const Marker>(kSpotGround).subspan(a.ground_begin, a.ground_end - a.ground_begin)};
}

AreaPlan PlanArea(const Area& area, std::uint16_t count_pct, const NameTest& special, std::size_t most) {
    AreaPlan plan;
    std::vector<Point> apart;  // every enemy placed here, then every spot taken
    apart.reserve(area.enemies.size());
    for (const AreaEnemy& e : area.enemies) {
        apart.push_back(e.at);
    }
    std::vector<const AreaEnemy*> sorted;
    for (const AreaEnemy& e : area.enemies) {
        sorted.push_back(&e);
    }
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const AreaEnemy* a, const AreaEnemy* b) { return a->hash < b->hash; });
    for (const AreaEnemy* e : sorted) {
        if (TreatmentOf(e->name, e->hash) != Treatment::copies || (special && special(e->name))) {
            continue;
        }
        const unsigned want = ExtraCount(e->hash, count_pct);
        if (want == 0) {
            continue;
        }
        const std::size_t before = apart.size();
        if (before - area.enemies.size() + want > most) {
            plan.no_spot.insert(e->hash);  // past the area's cap (placement id order): its weight
            continue;
        }
        while (apart.size() - before < want) {
            const auto spot = FindSpot(e->at, e->name, area.ground, apart);
            if (!spot) {
                break;
            }
            apart.push_back(*spot);
        }
        if (apart.size() - before < want) {
            apart.resize(before);  // all of them or none
            plan.no_spot.insert(e->hash);
            continue;
        }
        plan.spots[e->hash].assign(apart.begin() + static_cast<std::ptrdiff_t>(before), apart.end());
    }
    return plan;
}

std::size_t Extras::Plan(std::string_view scene, const std::vector<Original>& walk, std::uint16_t count_pct,
                         bool boss_here, std::uint64_t now_ms) {
    if (scene != scene_) {
        Reset();
        scene_ = std::string(scene);
    }
    if (boss_here || count_pct <= 100) {
        return 0;
    }
    std::vector<const Original*> sorted;
    sorted.reserve(walk.size());
    for (const Original& o : walk) {
        sorted.push_back(&o);
    }
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Original* a, const Original* b) { return a->hash < b->hash; });
    std::size_t added = 0;
    for (const Original* original : sorted) {
        const Original& o = *original;
        if (planned_.contains(o.hash) || no_spot_.contains(o.hash)) {
            continue;  // decided for this visit
        }
        const std::optional<Area> area = areas_ ? areas_(o.hash) : std::nullopt;
        if (!area) {
            // A placement our baked data lacks was added by a mod (OOTRR Difficulty: more enemies, hordes): its mod
            // already scaled it, so it keeps its own weight: no copies, no fold (ootrr-diff-compat D1).
            continue;
        }
        auto plan = plans_.find(area->name);
        if (plan == plans_.end()) {
            plan = plans_.emplace(area->name, PlanArea(*area, count_pct, special_, kMost)).first;
        }
        if (plan->second.no_spot.contains(o.hash)) {
            no_spot_.insert(o.hash);
            continue;
        }
        const auto spots = plan->second.spots.find(o.hash);
        if (spots == plan->second.spots.end()) {
            continue;  // none for it at this count
        }
        unsigned& have = planned_[o.hash];
        for (const Point& at : spots->second) {
            Extra e;
            e.key = next_key_++;
            e.hash = ExtraHash(o.hash, have);
            e.name = o.name;
            e.position = at;
            e.asked_ms = now_ms;
            extras_.push_back(std::move(e));
            ++have;
            ++added;
        }
    }
    return added;
}

void Extras::Bind(const std::map<std::uint64_t, std::uint64_t>& live) {
    for (Extra& e : extras_) {
        const auto it = live.find(e.key);
        if (!e.gone && it != live.end() && it->second != 0 && e.address != it->second) {
            if (e.address != 0) {
                by_address_.erase(e.address);
            }
            e.address = it->second;
            by_address_[e.address] = Bound{e.hash, e.name};
        }
    }
}

void Extras::Keep(const std::vector<std::uint64_t>& actors) {
    if (by_address_.empty()) {
        return;
    }
    std::vector<std::uint64_t> seen = actors;
    std::sort(seen.begin(), seen.end());
    for (Extra& e : extras_) {
        if (e.address != 0 && !e.gone && !std::binary_search(seen.begin(), seen.end(), e.address)) {
            e.gone = true;
            by_address_.erase(e.address);
        }
    }
}

std::optional<std::uint64_t> Extras::HashOf(std::uint64_t address, std::string_view name) const {
    const auto it = by_address_.find(address);
    if (it == by_address_.end() || it->second.name != name) {
        return std::nullopt;
    }
    return it->second.hash;
}

bool Extras::Holds(std::string_view name, std::uint64_t now_ms) const {
    return std::any_of(extras_.begin(), extras_.end(), [&](const Extra& e) {
        return e.address == 0 && e.name == name && now_ms - e.asked_ms < kBindWaitMs;
    });
}

void Extras::Reset() {
    scene_.clear();
    extras_.clear();
    plans_.clear();
    planned_.clear();
    no_spot_.clear();
    by_address_.clear();
}


unsigned Horde::PerRoll(std::size_t players_near, std::uint16_t count_pct) {
    if (count_pct <= 100 || players_near <= 1) {
        return 0;
    }
    return static_cast<unsigned>(std::min<std::size_t>(players_near - 1, kPerRollMost));
}

std::size_t Horde::Plan(std::string_view scene, const std::vector<Roll>& rolls, std::size_t alive,
                        const std::vector<Point>& apart, std::uint16_t count_pct, std::uint64_t now_ms) {
    if (scene != scene_) {
        Reset();
        scene_ = std::string(scene);
    }
    std::size_t pending = 0;  // asked for, not made yet: they count as alive
    for (Made& m : made_) {
        if (m.address != 0 || m.gone) {
            continue;
        }
        if (now_ms - m.asked_ms >= kBindWaitMs) {
            m.gone = true;  // the game refused it
        } else {
            ++pending;
        }
    }
    // Ours whose roll died follow a living roll of ours; with none for kOrphanMs they go.
    const Roll* living = nullptr;
    for (const Roll& roll : rolls) {
        const auto known = rolls_.find(roll.address);
        if (roll.address != 0 && !roll.dead && !Ours(roll.address) && known != rolls_.end() &&
            now_ms - known->second.first_ms >= kSettleMs) {
            living = &roll;  // (settled: the game's own, never one of ours not bound yet)
            break;
        }
    }
    bool orphans = false;
    for (Made& m : made_) {
        if (m.roll == 0 && !m.gone && !m.retired) {
            if (living != nullptr) {
                m.roll = living->address;
            } else {
                orphans = true;
            }
        }
    }
    if (!orphans) {
        orphan_since_ = 0;
    } else if (orphan_since_ == 0) {
        orphan_since_ = now_ms;
    } else if (now_ms - orphan_since_ >= kOrphanMs) {
        for (Made& m : made_) {
            m.retired = m.retired || (m.roll == 0 && !m.gone);
        }
        orphan_since_ = 0;
    }
    std::size_t added = 0;
    for (const Roll& roll : rolls) {
        if (roll.address == 0 || Ours(roll.address)) {
            continue;
        }
        const auto [entry, first] = rolls_.emplace(roll.address, Seen{now_ms, roll.dead});
        Seen& seen = entry->second;
        if (first && rises_.size() < kMost) {
            rises_.push_back(roll.position);  // where the game made it rise: its own floor
        }
        seen.dead = roll.dead;
        if (roll.dead || now_ms - seen.first_ms < kSettleMs) {
            continue;
        }
        unsigned& made = planned_[roll.address];
        const unsigned want = PerRoll(roll.players_near, count_pct);
        while (made < want && alive + pending < kMostAlive && made_.size() < kMost) {
            std::vector<Point> taken = apart;
            for (const Made& other : made_) {
                if (!other.gone) {
                    taken.push_back(other.position);
                }
            }
            const auto spot = FindSpot(roll.position, roll.name, rises_, taken);
            if (!spot) {
                break;  // no ground for one more now: tried again on a later walk
            }
            Made m;
            m.key = next_key_++;
            m.name = roll.name;
            m.position = *spot;
            m.roll = roll.address;
            m.asked_ms = now_ms;
            made_.push_back(std::move(m));
            ++made;
            ++pending;
            ++added;
        }
    }
    return added;
}

void Horde::Bind(const std::map<std::uint64_t, std::uint64_t>& live) {
    for (Made& m : made_) {
        if (m.address != 0 || m.gone) {
            continue;
        }
        const auto it = live.find(m.key);
        if (it != live.end() && it->second != 0) {
            m.address = it->second;
            by_address_[m.address] = m.key;
        }
    }
}

void Horde::Keep(std::vector<std::uint64_t> actors) {
    std::sort(actors.begin(), actors.end());
    const auto here = [&actors](std::uint64_t a) { return std::binary_search(actors.begin(), actors.end(), a); };
    for (auto it = rolls_.begin(); it != rolls_.end();) {
        if (here(it->first)) {
            ++it;
            continue;
        }
        for (Made& m : made_) {
            if (m.roll != it->first || m.gone) {
                continue;
            }
            if (it->second.dead) {
                m.roll = 0;  // killed: ours fight on, and join another roll (Plan)
            } else {
                m.retired = true;  // taken away alive (dawn, the area off, sunk): ours go too
            }
        }
        planned_.erase(it->first);
        it = rolls_.erase(it);
    }
    for (Made& m : made_) {
        if (m.address != 0 && !m.gone && !here(m.address)) {
            m.gone = true;
            by_address_.erase(m.address);
        } else if (m.address == 0 && m.retired) {
            m.gone = true;  // not made yet: never made
        }
    }
}

std::vector<std::uint64_t> Horde::Removes() const {
    std::vector<std::uint64_t> out;
    for (const Made& m : made_) {
        if (m.retired && !m.gone && m.address != 0) {
            out.push_back(m.address);
        }
    }
    return out;
}

void Horde::Reset() {
    scene_.clear();
    made_.clear();
    rolls_.clear();
    rises_.clear();
    planned_.clear();
    by_address_.clear();
    orphan_since_ = 0;
}

} // namespace CoopDifficulty
