// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_ocarina.h"

#include <cstdio>

namespace CoopOcarina {

const std::vector<std::string>& NoteNames() {
    // The SLink assets Ocarina_<name> (SLink2/slink2.Product.111.bslnk).
    static const std::vector<std::string> names{
        "",       "LowB", "LowC",  "LowCsharp", "C",     "Csharp", "D",      "Dsharp",
        "E",      "F",    "Fsharp", "G",        "Gsharp", "A",     "Asharp", "B",
        "HighD",  "HighDsharp", "HighE", "HighF"};
    return names;
}

std::uint8_t NoteIndex(std::string_view name) {
    if (name.starts_with("Ocarina_")) {
        name.remove_prefix(8);
    }
    const auto& names = NoteNames();
    for (std::size_t i = 1; i < names.size(); ++i) {
        if (names[i] == name) {
            return static_cast<std::uint8_t>(i);
        }
    }
    return 0;
}

std::optional<std::string> OcarinaProbe::ReadName(const ReadWord& read, std::uint64_t address) const {
    if (address == 0) {
        return std::nullopt;
    }
    std::string out;
    for (std::uint64_t at = address; out.size() < 64; at += 8) {
        const auto word = read(at);
        if (!word) {
            return std::nullopt;
        }
        for (int i = 0; i < 8; ++i) {
            const char c = static_cast<char>((*word >> (8 * i)) & 0xff);
            if (c == '\0') {
                return out;
            }
            if (c < 0x20 || c > 0x7e) {
                return std::nullopt;
            }
            out.push_back(c);
        }
    }
    return std::nullopt;
}

void OcarinaProbe::Reset() {
    words_.clear();
    steps_.clear();
    commands_.clear();
    active_ = false;
    last_us_ = 0;
}

std::vector<std::string> OcarinaProbe::Look(const ReadWord& read, std::uint64_t actor,
                                            std::uint64_t now_us) {
    std::vector<std::string> lines;
    if (lines_ >= kMaxLines || actor == 0 || (last_us_ != 0 && now_us - last_us_ < kEveryUs)) {
        return lines;
    }
    last_us_ = now_us;
    std::uint64_t at = actor + kAsComponent;
    for (const std::uint64_t step : {std::uint64_t{0}, kAsStep1, kAsStep2, kAsStep3}) {
        const auto next = read(at + step);
        if (!next || *next == 0) {
            return lines;
        }
        at = *next;
    }
    const std::uint64_t controller = at;
    const auto count_word = read(controller + kSlotCount);
    const auto array = read(controller + kSlotArray);
    if (!count_word || !array || *array == 0) {
        return lines;
    }
    const auto count = static_cast<std::int32_t>(*count_word & 0xffffffff);
    if (count <= 0) {
        return lines;
    }
    std::vector<std::uint64_t> slots;
    std::vector<std::string> commands;
    for (std::int32_t i = 0; i < count && slots.size() < kMaxSlots; ++i) {
        const auto slot = read(*array + kSlotStride * static_cast<std::uint64_t>(i));
        if (!slot || *slot == 0) {
            break;
        }
        slots.push_back(*slot);
        const auto name_ptr = read(*slot + kSlotCommand);
        commands.push_back(name_ptr ? ReadName(read, *name_ptr).value_or("?") : "?");
    }
    bool ocarina = false;
    for (const auto& c : commands) {
        ocarina = ocarina || c.find("carina") != std::string::npos;
    }
    if (!ocarina) {
        if (active_) {
            lines.push_back("ocarina probe: ocarina put away");
            ++lines_;
        }
        active_ = false;
        words_.clear();
        commands_.clear();
        return lines;
    }
    if (!active_) {
        active_ = true;
        lines.push_back("ocarina probe: our Link plays the ocarina, " + std::to_string(slots.size()) +
                        " AS slots");
        ++lines_;
    }
    if (commands != commands_) {
        std::string line = "ocarina probe: slots";
        for (std::size_t i = 0; i < commands.size(); ++i) {
            line += " " + std::to_string(i) + "=" + commands[i];
        }
        lines.push_back(line);
        ++lines_;
        commands_ = commands;
    }
    // Regions: 0 the controller, 1.. the slots.
    std::vector<std::pair<std::uint64_t, std::size_t>> regions{{controller, kControllerBytes}};
    for (const auto s : slots) {
        regions.emplace_back(s, kSlotBytes);
    }
    std::string steps;
    std::size_t shown = 0;
    for (std::size_t r = 0; r < regions.size(); ++r) {
        for (std::uint64_t off = 0; off < regions[r].second; off += 8) {
            const auto word = read(regions[r].first + off);
            if (!word) {
                continue;
            }
            for (int half = 0; half < 2; ++half) {
                const auto value = static_cast<std::uint32_t>(*word >> (32 * half));
                const std::uint64_t key = (std::uint64_t{r} << 32) | (off + 4 * half);
                const auto it = words_.find(key);
                if (it == words_.end()) {
                    words_.emplace(key, value);
                    continue;
                }
                const std::uint32_t before = it->second;
                it->second = value;
                if (before == value || before >= kSmall || value >= kSmall) {
                    continue;
                }
                if (++steps_[key] > kNoisySteps) {
                    continue;  // a timer or counter
                }
                if (shown < kMaxShown) {
                    char buf[64];
                    std::snprintf(buf, sizeof buf, " %s+0x%llx:%u>%u",
                                  r == 0 ? "ctl" : ("slot" + std::to_string(r - 1)).c_str(),
                                  static_cast<unsigned long long>(off + 4 * half), before, value);
                    steps += buf;
                }
                ++shown;
            }
        }
    }
    if (shown > 0) {
        lines.push_back("ocarina probe: " + std::to_string(shown) + " small words stepped:" + steps);
        ++lines_;
    }
    return lines;
}

} // namespace CoopOcarina
