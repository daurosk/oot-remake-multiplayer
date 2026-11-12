// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_ocarina.h"

#include <cmath>
#include <cstdio>
#include <cstring>

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

namespace {

std::optional<std::string> ReadText(const HoldProbe::ReadWord& read, std::uint64_t address) {
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

std::string HexText(std::uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

} // namespace

std::vector<HoldProbe::Region> HoldProbe::Regions(const ReadWord& read, std::uint64_t link) const {
    std::vector<Region> out;
    const auto parts = read(link + kParts);
    if (!parts || *parts == 0) {
        return out;
    }
    const auto eu = read(*parts + kPartsEquipment);
    if (!eu || *eu == 0) {
        return out;
    }
    std::uint64_t ocarina = 0;
    for (std::uint32_t i = 0; i < kArmorLinkCount && ocarina == 0; ++i) {
        const std::uint64_t at = *eu + kArmorLinks + kLinkStride * i;
        const auto id = read(at + kLinkId);
        const auto holder = read(at + kLinkHolder);
        if (!id || (*id & 0xffffffffu) == 0xffffffffu || !holder || *holder == 0) {
            continue;
        }
        const auto actor = read(*holder + kHolderActor);
        if (!actor || *actor == 0) {
            continue;
        }
        const auto name_ptr = read(*actor + kActorName);
        const auto name = name_ptr ? ReadText(read, *name_ptr) : std::nullopt;
        if (name && name->starts_with("Item_Ocarina_") && !name->ends_with("_Holder")) {
            ocarina = *actor;
        }
    }
    if (ocarina == 0) {
        return out;
    }
    out.push_back({"eu", *eu, kEquipmentBytes});
    out.push_back({"oca", ocarina, kActorBytes});
    const auto table = read(ocarina + kParts);
    if (table && *table != 0) {
        out.push_back({"ocaparts", *table, kTableBytes});
        std::size_t blocks = 0;
        for (std::uint64_t off = 0; off < kTableBytes && blocks < kMaxBlocks; off += 8) {
            const auto p = read(*table + off);
            // A pointer into the same heap as the actor.
            if (!p || *p == 0 || (*p >> 32) != (ocarina >> 32) || (*p & 7) != 0) {
                continue;
            }
            out.push_back({"ocaparts+" + HexText(off), *p, kBlockBytes});
            ++blocks;
        }
    }
    return out;
}

HoldProbe::Snapshot HoldProbe::Take(const ReadWord& read) const {
    Snapshot s;
    s.reserve(regions_.size());
    for (const auto& r : regions_) {
        std::vector<std::optional<std::uint64_t>> words;
        words.reserve(r.bytes / 8);
        for (std::uint64_t off = 0; off < r.bytes; off += 8) {
            words.push_back(read(r.base + off));
        }
        s.push_back(std::move(words));
    }
    return s;
}

std::string HoldProbe::Place(const ReadWord& read, std::uint64_t link) const {
    if (regions_.size() < 2) {
        return "?";
    }
    float own[3]{};
    float at[3]{};
    for (int i = 0; i < 3; ++i) {
        const std::uint64_t off = kActorPosition + 4 * static_cast<std::uint64_t>(i);
        const auto wa = static_cast<std::uint32_t>(read(link + off).value_or(0));
        const auto wb = static_cast<std::uint32_t>(read(regions_[1].base + off).value_or(0));
        std::memcpy(&own[i], &wa, 4);
        std::memcpy(&at[i], &wb, 4);
    }
    char buf[96];
    std::snprintf(buf, sizeof buf, "up %.2f off %.2f", static_cast<double>(at[1] - own[1]),
                  std::hypot(static_cast<double>(at[0] - own[0]), static_cast<double>(at[2] - own[2])));
    return buf;
}

std::vector<std::string> HoldProbe::Step(const ReadWord& read, std::uint64_t link, bool playing,
                                         std::uint64_t now_us) {
    std::vector<std::string> lines;
    if (link == 0 || phase_ == Phase::Done || lines_ >= kMaxLines) {
        return lines;
    }
    switch (phase_) {
    case Phase::Idle:
        if (!playing) {
            if (taken_us_ == 0 || now_us - taken_us_ >= kRefreshUs) {
                regions_ = Regions(read, link);
                before_ = Take(read);
                place_before_ = Place(read, link);
                taken_us_ = now_us;
            }
            break;
        }
        if (regions_.empty()) {
            break;  // no Ocarina was found on the belt
        }
        phase_ = Phase::Playing;
        since_us_ = now_us;
        during_.clear();
        lines.push_back("ocarina hold probe: playing; " + std::to_string(regions_.size()) +
                        " regions, ocarina " + HexText(regions_[1].base) + " before " + place_before_);
        break;
    case Phase::Playing:
        if (during_.empty() && now_us - since_us_ >= kSettleUs) {
            during_ = Take(read);
            place_during_ = Place(read, link);
            lines.push_back("ocarina hold probe: during " + place_during_);
            if (regions_.size() > 2 && regions_[2].name == "ocaparts+0x0") {
                const auto bone = ReadText(read, regions_[2].base + HandBind::kBindName + 4);
                lines.push_back("ocarina hold probe: during bone '" + bone.value_or("?") + "'");
            }
        }
        if (!playing) {
            phase_ = Phase::Away;
            since_us_ = now_us;
        }
        break;
    case Phase::Away: {
        if (playing) {
            phase_ = Phase::Playing;  // out again: wait for the next put away
            break;
        }
        if (now_us - since_us_ < kSettleUs) {
            break;
        }
        if (during_.empty()) {
            phase_ = Phase::Idle;  // played too briefly; try again
            taken_us_ = 0;
            break;
        }
        const Snapshot after = Take(read);
        lines.push_back("ocarina hold probe: after " + Place(read, link));
        int shown = 0;
        for (std::size_t r = 0; r < regions_.size(); ++r) {
            for (std::size_t w = 0; w < before_[r].size(); ++w) {
                const auto& a = before_[r][w];
                const auto& b = during_[r][w];
                const auto& c = after[r][w];
                if (!a || !b || !c || *a != *c || *a == *b) {
                    continue;
                }
                if (shown < kMaxLines - 4) {
                    lines.push_back("ocarina hold probe: " + regions_[r].name + " " +
                                    HexText(regions_[r].base) + "+" + HexText(w * 8) + ": " +
                                    HexText(*a) + " > " + HexText(*b));
                }
                ++shown;
            }
        }
        lines.push_back("ocarina hold probe: " + std::to_string(shown) +
                        " words changed while playing and back after");
        phase_ = Phase::Done;
        break;
    }
    case Phase::Done:
        break;
    }
    lines_ += static_cast<int>(lines.size());
    return lines;
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

bool HandBind::Playing(std::string_view anim) {
    return anim.find("Ocarina") != std::string_view::npos;
}

std::uint64_t HandBind::Find(const ReadWord& read, std::uint64_t actor) {
    using H = HoldProbe;
    const auto parts = read(actor + H::kParts);
    const auto eu = parts && *parts ? read(*parts + H::kPartsEquipment) : std::nullopt;
    if (!eu || *eu == 0) {
        return 0;
    }
    for (std::uint32_t i = 0; i < H::kArmorLinkCount; ++i) {
        const std::uint64_t at = *eu + H::kArmorLinks + H::kLinkStride * i;
        const auto id = read(at + H::kLinkId);
        const auto holder = read(at + H::kLinkHolder);
        if (!id || (*id & 0xffffffffu) == 0xffffffffu || !holder || *holder == 0) {
            continue;
        }
        const auto oca = read(*holder + H::kHolderActor);
        const auto name_ptr = oca && *oca ? read(*oca + H::kActorName) : std::nullopt;
        const auto name = name_ptr ? ReadText(read, *name_ptr) : std::nullopt;
        if (!name || !name->starts_with("Item_Ocarina_") || name->ends_with("_Holder")) {
            continue;
        }
        const auto table = read(*oca + H::kParts);
        const auto block = table && *table ? read(*table) : std::nullopt;
        return block && (*block & 7) == 0 ? *block : 0;
    }
    return 0;
}

void HandBind::Restore(const WriteWord& write) {
    write(block_ + kBindBone, saved_bone_);
    for (std::uint32_t i = 0; i < kMatrixWords; ++i) {
        write(block_ + kBindMatrix + 8 * i, saved_matrix_[i]);
    }
    block_ = 0;
}

std::string HandBind::Step(const ReadWord& read, const WriteWord& write, std::uint64_t actor,
                           bool playing) {
    if (actor != actor_) {
        block_ = 0;  // a new puppet: the old block is not ours to write
        actor_ = actor;
        refused_ = false;
    }
    if (actor == 0) {
        return "";
    }
    if (!playing) {
        refused_ = false;
        if (block_ == 0) {
            return "";
        }
        // Only put back what is still ours: the same Ocarina, still in hand.
        const auto bone = read(block_ + kBindBone);
        if (Find(read, actor) == block_ && bone && ((*bone >> 16) & 0xffff) == kHandBone) {
            Restore(write);
            return "ocarina back on the belt";
        }
        block_ = 0;
        return "ocarina changed while held; left as it is";
    }
    bool just = false;
    if (block_ == 0) {
        if (refused_) {
            return "";
        }
        const std::uint64_t block = Find(read, actor);
        const auto bone = block ? read(block + kBindBone) : std::nullopt;
        const auto name = block ? read(block + kBindName) : std::nullopt;
        if (!bone || !name || *name != kBeltName || ((*bone >> 16) & 0xffff) != kBeltBone) {
            refused_ = true;
            char buf[96];
            std::snprintf(buf, sizeof buf, "ocarina not moved: bind 0x%llx bone 0x%llx name 0x%llx",
                          static_cast<unsigned long long>(block),
                          static_cast<unsigned long long>(bone.value_or(0)),
                          static_cast<unsigned long long>(name.value_or(0)));
            return buf;
        }
        for (std::uint32_t i = 0; i < kMatrixWords; ++i) {
            const auto w = read(block + kBindMatrix + 8 * i);
            if (!w) {
                refused_ = true;
                return "ocarina not moved: matrix unreadable";
            }
            saved_matrix_[i] = *w;
        }
        saved_bone_ = *bone;
        block_ = block;
        just = true;
    }
    // Every frame while playing, in case the game writes the belt back.
    const auto bone = read(block_ + kBindBone);
    if (!bone) {
        block_ = 0;
        return "ocarina bind unreadable; let go";
    }
    const std::uint64_t hand = (*bone & ~(0xffffull << 16)) | (kHandBone << 16);
    static constexpr std::uint64_t kIdentity[kMatrixWords] = {
        0x3f800000ull, 0, 0x3f800000ull << 32, 0, 0, 0x3f800000ull};
    write(block_ + kBindBone, hand);
    for (std::uint32_t i = 0; i < kMatrixWords; ++i) {
        write(block_ + kBindMatrix + 8 * i, kIdentity[i]);
    }
    return just ? "ocarina to the hand" : "";
}

} // namespace CoopOcarina
