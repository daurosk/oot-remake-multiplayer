// SPDX-License-Identifier: GPL-3.0-or-later
//
// Row 233 (Geno's survey): dungeon pieces that exist only while a shared flag holds (`Presence.FlagName`,
// `IsNegation`: the Water Temple's water planes and waterfalls, the Shadow Temple's tower pillar, Bottom of the
// Well's water, the Forest Temple's room 7 water and elevator, two Ice Cavern pots, a Jabu-Jabu air wall). Another
// game's play can change such a flag while our Link is in the dungeon. Whether the game then takes the gated
// pieces away or puts them in, or only at the next load, is not known yet; deleting or making a placement live is
// proven for enemies only, not for terrain. This is the probe for the live check: read only, no write.
//
// Probe::Step reads each gate's flag (every kEveryUs) and, when one changes, logs which of its pieces our game has
// now and again kAfterUs later. Tested in tests/lichii/presence_gates_test.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace CoopPresenceGates {

struct Gate {
    std::uint32_t flag;    // Murmur3 of the flag's name
    bool negation;         // the piece exists while the flag does NOT hold
    const char* area;      // the Banc area (the dungeon)
    const char* actor;     // the piece's actor name
    int placements;        // how many such placements the area has
    const char* flag_name;
};

inline std::span<const Gate> Gates() {
    static constexpr Gate kGates[] = {
#include "coop_presence_gates.inc"
    };
    return kGates;
}

inline constexpr std::uint64_t kEveryUs = 1'000'000;
inline constexpr std::uint64_t kAfterUs = 3'000'000;

class Probe {
public:
    using ReadFlag = std::function<std::optional<bool>(std::uint32_t)>;
    using CountActors = std::function<std::size_t(std::string_view)>;  // live actors of that name in our game

    // Present thread. Returns the log lines (without the "coop: " start).
    std::vector<std::string> Step(std::uint64_t now_us, const ReadFlag& read, const CountActors& count) {
        std::vector<std::string> out;
        for (auto it = later_.begin(); it != later_.end();) {
            if (now_us < it->second) {
                ++it;
                continue;
            }
            out.push_back(Line(it->first, *values_[it->first], count, "3 s later"));
            it = later_.erase(it);
        }
        if (read_at_ != 0 && now_us - read_at_ < kEveryUs) {
            return out;
        }
        read_at_ = now_us == 0 ? 1 : now_us;
        for (const Gate& g : Gates()) {
            const std::optional<bool> v = read(g.flag);
            if (!v) {
                continue;
            }
            auto& was = values_[g.flag];
            if (was && *was != *v) {
                out.push_back(Line(g.flag, *v, count, "now"));
                later_[g.flag] = now_us + kAfterUs;
            }
            was = v;
        }
        return out;
    }

    // Forget every value (a new scene: a load reads presence anew, nothing to learn from its changes).
    void Reset() {
        values_.clear();
        later_.clear();
        read_at_ = 0;
    }

    static std::string Line(std::uint32_t flag, bool value, const CountActors& count, const char* when) {
        std::string text;
        std::string name;
        std::string area;
        for (const Gate& g : Gates()) {
            if (g.flag != flag) {
                continue;
            }
            name = g.flag_name;
            area = g.area;
            const bool wanted = value != g.negation;
            text += std::string(text.empty() ? "" : ", ") + g.actor + " " + (wanted ? "wanted" : "unwanted") +
                    " (" + std::to_string(count(g.actor)) + " of " + std::to_string(g.placements) + " here)";
        }
        return "presence: " + name + " (" + area + ") went " + (value ? "1" : "0") + ", " + when + ": " + text;
    }

private:
    std::map<std::uint32_t, std::optional<bool>> values_;
    std::map<std::uint32_t, std::uint64_t> later_;
    std::uint64_t read_at_ = 0;
};

}  // namespace CoopPresenceGates
