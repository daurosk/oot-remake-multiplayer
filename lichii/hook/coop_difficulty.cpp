// SPDX-License-Identifier: GPL-3.0-or-later
//
// Difficulty, the rules (coop_difficulty.h). No emulator code.

#include "coop_difficulty.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>

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
            s.kind.store(k, std::memory_order_release);
            s.key.store(address, std::memory_order_release);
            return true;
        }
        if (key == address) {
            if (s.kind.load(std::memory_order_relaxed) != k) {
                s.carry.store(0, std::memory_order_relaxed);  // another actor at that address now
                s.kind.store(k, std::memory_order_release);
            }
            return true;
        }
    }
    return false;
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
    }
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

} // namespace CoopDifficulty
