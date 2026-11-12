// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_ocarina.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace CoopOcarina {

const std::vector<std::string>& NoteNames() {
    // The SLink assets Ocarina_<name>, in the order the ocarina sound object
    // keeps them (its constructor, main 0xe7c570): sound n is names[n].
    static const std::vector<std::string> names{
        "",       "LowB", "LowC",   "LowCsharp", "D",      "Dsharp", "E",     "F",
        "Fsharp", "G",    "Gsharp", "A",         "Asharp", "B",      "C",     "Csharp",
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

std::optional<CoopPuppet::Call> Voices::Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                             std::uint64_t main_base) {
    if (after_call) {
        Finish(guest, result);
    } else if (pending_ != Pending::none) {
        // A new frame and the last call never came back: the cave did not
        // make it. Ask again.
        if (pending_ == Pending::malloc) {
            allocating_ = false;
        }
        pending_ = Pending::none;
    }
    if (main_base == 0) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < voices_.size(); ++i) {
        Voice& v = voices_[i];
        if (v.playing == v.want) {
            continue;
        }
        if (v.playing && !v.handle) {  // nothing sounds (the play gave no sound): nothing to stop
            v.playing.reset();
            if (!v.want) {
                continue;
            }
        }
        if (v.playing) {
            CoopPuppet::Call c;
            c.function = main_base + kStopSound;
            c.x[0] = block_ + kHandles + kHandleSize * i;
            c.s0 = -1.0f;  // at once, as the game when the button is let go
            pending_ = Pending::stop;
            pending_voice_ = i;
            return c;
        }
        if (block_ == 0) {
            if (allocating_ || no_memory_) {
                return std::nullopt;
            }
            CoopPuppet::Call c;
            c.function = main_base + kMalloc;
            c.x[0] = kBlockSize;
            pending_ = Pending::malloc;
            allocating_ = true;
            return c;
        }
        // The request, as the object's update builds it (main 0x3294df0):
        // +0 a byte 0, +8 the name, +0x10 0, +0x18 0.0, +0x1c the volume,
        // +0x20 0, +0x24 -1.
        std::array<std::uint8_t, kRequestSize> request{};
        const std::uint64_t name = main_base + kNoteNames[v.want->note % kNoteNames.size()];
        const std::int32_t minus_one = -1;
        std::memcpy(request.data() + 0x08, &name, 8);
        std::memcpy(request.data() + 0x1c, &volume_, 4);
        std::memcpy(request.data() + 0x24, &minus_one, 4);
        if (!guest.Write(block_ + kRequest, request.data(), request.size())) {
            v.playing = v.want;  // no more tries for this note
            v.handle = false;
            continue;
        }
        CoopPuppet::Call c;
        c.function = main_base + kPlaySound;
        c.x[0] = block_ + kRequest;
        pending_ = Pending::play;
        pending_voice_ = i;
        return c;
    }
    return std::nullopt;
}

void Voices::Finish(CoopPuppet::Guest& guest, std::uint64_t result) {
    const Pending was = std::exchange(pending_, Pending::none);
    if (was == Pending::none) {
        return;
    }
    if (was == Pending::malloc) {
        allocating_ = false;
        const std::array<std::uint8_t, kBlockSize> zero{};
        if (result != 0 && guest.Write(result, zero.data(), zero.size())) {
            block_ = result;
            notes_.push_back("memory for the notes at " + Hex(result));
        } else {
            no_memory_ = true;  // these notes stay silent this run
            notes_.push_back("no memory for the notes: the other players' notes stay silent");
        }
        return;
    }
    Voice& v = voices_[pending_voice_];
    if (was == Pending::stop) {
        v.playing.reset();
        v.handle = false;
        return;
    }
    // A play: the handle the game keeps is the sound and its serial (+0x10
    // of the sound), which every later use checks against the sound's own.
    v.playing = v.want;
    v.handle = false;
    std::uint32_t serial = 0;
    if (result != 0 && guest.Read(result + 0x10, &serial, 4)) {
        const std::array<std::uint64_t, 2> words{result, serial};
        v.handle = guest.Write(block_ + kHandles + kHandleSize * pending_voice_, words.data(), sizeof(words));
    }
    if (played_ < 3 || !v.handle) {  // the first notes and any that gave no sound, for the log
        notes_.push_back(v.handle ? "the game sounds it (" + Hex(result) + " serial " + std::to_string(serial) + ")"
                                  : "the game gave no sound for that note");
    }
    ++played_;
}

void Voices::Want(std::size_t voice, std::optional<coop::protocol::OcarinaNote> note) {
    if (voice < voices_.size()) {
        if (note && note != voices_[voice].want) {
            notes_.push_back("player " + std::to_string(voice + 1) + " plays " +
                             NoteNames()[(note->note % coop::protocol::kOcarinaNotes) + 1]);
        }
        voices_[voice].want = note;
    }
}

std::vector<std::string> Voices::TakeNotes() {
    return std::exchange(notes_, {});
}

std::string Voices::Hex(std::uint64_t v) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(v));
    return text;
}

} // namespace CoopOcarina
