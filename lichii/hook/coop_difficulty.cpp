// SPDX-License-Identifier: GPL-3.0-or-later
//
// Difficulty, the rules (coop_difficulty.h). No emulator code.

#include "coop_difficulty.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>

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
    return c;
}

std::string Describe(const Choice& choice, const Values& values, std::size_t players) {
    std::string line;
    if (choice.auto_scale) {
        line = "Automatic (" + std::to_string(players) + (players == 1 ? " player)" : " players)");
    } else {
        line = std::string(PresetName(choice.preset));
    }
    return line + ": life " + std::to_string(values.hp_pct) + "%, damage " +
           std::to_string(values.damage_pct) + "%, enemies " + std::to_string(values.count_pct) + "%";
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

bool Kinds::Mark(std::uint64_t address, Kind kind) noexcept {
    if (address == 0) {
        return false;
    }
    const auto k = static_cast<std::uint8_t>(kind);
    for (std::size_t i = 0; i < kProbes; ++i) {
        Slot& s = slots_[(Hash(address) + i) % kSlots];
        const std::uint64_t key = s.key.load(std::memory_order_acquire);
        if (key == 0) {
            // (only this writer inserts: no race on the key) The kind goes in
            // first, so a reader that finds the key reads its kind.
            s.carry.store(0, std::memory_order_relaxed);
            s.pass.store(0, std::memory_order_relaxed);
            s.kind.store(k, std::memory_order_release);
            s.key.store(address, std::memory_order_release);
            return true;
        }
        if (key == address) {
            if (s.kind.load(std::memory_order_relaxed) != k) {
                s.carry.store(0, std::memory_order_relaxed);  // another actor at that address now
                s.pass.store(0, std::memory_order_relaxed);
                s.kind.store(k, std::memory_order_release);
            }
            return true;
        }
    }
    unmarked_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void Kinds::Forget(std::uint64_t address) noexcept {
    const Kind kind = Of(address);
    if (kind == Kind::enemy || kind == Kind::enemy_life || kind == Kind::folded_enemy ||
        kind == Kind::folded_enemy_life) {
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
    if (address == 0) {
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
        s.carry.store(0, std::memory_order_relaxed);
        s.pass.store(0, std::memory_order_relaxed);
    }
    unmarked_.store(0, std::memory_order_relaxed);
}

std::int32_t Kinds::Scaled(Slot& s, std::int32_t sum, std::uint16_t pct, Rule rule,
                           std::atomic<std::uint64_t>& hits) noexcept {
    if (pct == 100) {
        return sum;
    }
    // The game steps each component once a frame, one at a time, so a plain
    // load and store of its carry is enough.
    const std::uint32_t packed = s.carry.load(std::memory_order_relaxed);
    std::int32_t carry = (packed >> 16) == pct ? static_cast<std::int32_t>(packed & 0xffff) : 0;
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

std::array<float, 3> ExtraPlace(const std::array<float, 3>& at, std::uint64_t extra_hash) {
    // Eight directions: an angle from the id, the same in every game.
    constexpr float kDiagonal = 0.70710678f;
    constexpr std::array<std::array<float, 2>, 8> kWays{{{1.0f, 0.0f},
                                                         {kDiagonal, kDiagonal},
                                                         {0.0f, 1.0f},
                                                         {-kDiagonal, kDiagonal},
                                                         {-1.0f, 0.0f},
                                                         {-kDiagonal, -kDiagonal},
                                                         {0.0f, -1.0f},
                                                         {kDiagonal, -kDiagonal}}};
    const auto& way = kWays[extra_hash % kWays.size()];
    return {at[0] + way[0] * kExtraOffsetM, at[1], at[2] + way[1] * kExtraOffsetM};
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
        const unsigned want = ExtraCount(o.hash, count_pct);
        unsigned& have = planned_[o.hash];
        while (have < want && extras_.size() < kMost) {
            Extra e;
            e.key = next_key_++;
            e.hash = ExtraHash(o.hash, have);
            e.name = o.name;
            e.position = ExtraPlace(o.position, e.hash);
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
    planned_.clear();
    by_address_.clear();
}


unsigned Horde::PerRoll(std::size_t players_near, std::uint16_t count_pct) {
    if (count_pct <= 100 || players_near <= 1) {
        return 0;
    }
    return static_cast<unsigned>(std::min<std::size_t>(players_near - 1, kPerRollMost));
}

std::size_t Horde::Plan(std::string_view scene, const std::vector<Roll>& rolls, std::size_t alive,
                        std::uint16_t count_pct, std::uint64_t now_ms) {
    if (scene != scene_) {
        Reset();
        scene_ = std::string(scene);
    }
    std::size_t pending = 0;  // asked for, not made yet: they count as alive
    for (const Made& m : made_) {
        pending += m.address == 0 && !m.gone ? 1 : 0;
    }
    std::size_t added = 0;
    for (const Roll& roll : rolls) {
        if (roll.address == 0 || Ours(roll.address)) {
            continue;
        }
        const std::uint64_t seen = first_seen_.emplace(roll.address, now_ms).first->second;
        if (now_ms - seen < kSettleMs) {
            continue;
        }
        unsigned& made = planned_[roll.address];
        const unsigned want = PerRoll(roll.players_near, count_pct);
        while (made < want && alive + pending < kMostAlive && made_.size() < kMost) {
            Made m;
            m.key = next_key_++;
            m.name = roll.name;
            m.position = ExtraPlace(roll.position, ExtraHash(roll.address, made));
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

void Horde::Keep(const std::vector<std::uint64_t>& actors) {
    const auto here = [&actors](std::uint64_t a) { return std::find(actors.begin(), actors.end(), a) != actors.end(); };
    std::erase_if(first_seen_, [&](const auto& e) { return !here(e.first); });
    std::erase_if(planned_, [&](const auto& e) { return !here(e.first); });
    for (Made& m : made_) {
        if (m.address != 0 && !m.gone && !here(m.address)) {
            m.gone = true;
            by_address_.erase(m.address);
        }
    }
}

void Horde::Reset() {
    scene_.clear();
    made_.clear();
    first_seen_.clear();
    planned_.clear();
    by_address_.clear();
}

} // namespace CoopDifficulty
