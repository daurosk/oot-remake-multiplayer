// A pick in Snail's Character Select page whose model is not installed (owner, 0.11.9: his NPC characters are an
// opt-in download; without it his full picker stays). The mod's picker stores the pick (CharacterMenu_SelectedCharacter,
// the mod's own number of the character) and its model / cloth / voice hooks build a path from that number every time
// the game loads the player: a number whose files are not there would load missing files. So the hook reads the pick
// every frame and puts a dead one back at once (the last installed pick, or 0: Link), with one chat line.
// Installed: what the puppet pack found (tools/puppet_pack/build.py find_characters keeps only numbers whose
// romfs/Model/Player_<age>_C<file> file is there; coop_puppet.json "characters": {"numbers": ...}).
// Another player's pick we do not have: their puppet is Link already (Planner::ActorFor finds no file), and the
// same line is said once per player number (not per frame), again only when they pick another missing one.
// Header only, standard C++ only.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace CoopCharSlots {

// The chat line (plain words; the kit's start shows the NPC line and N switches the download on).
inline std::string MissingLine(bool own, const std::string& who = {}) {
    if (own) {
        return "This character is not downloaded: press N at the kit's start to get Snail's NPC characters.";
    }
    return (who.empty() ? std::string("A player") : who) +
           " plays a character you have not downloaded, shown as Link (press N at the kit's start to get Snail's NPC "
           "characters).";
}

// `pick` (the mod's stored number) can be loaded: 0 or less is Link; else the pack must have its file.
inline bool Installed(std::int32_t pick, const std::map<std::uint16_t, std::uint16_t>& files) {
    return pick <= 0 || (pick <= 0xFFFF && files.count(static_cast<std::uint16_t>(pick)) != 0);
}

// Our own pick.
class Own {
public:
    struct Step {
        std::optional<std::int32_t> set;   // the number to store at once in place of the pick
        bool say = false;                  // say MissingLine(true) now
    };
    // Each frame with the pick read (nothing: no such flag, no mod).
    Step Check(std::optional<std::int32_t> pick, const std::map<std::uint16_t, std::uint16_t>& files) {
        Step s;
        if (!pick) return s;
        if (Installed(*pick, files)) {
            last_good_ = *pick;
            told_ = 0;
            return s;
        }
        s.set = Installed(last_good_, files) ? last_good_ : 0;
        s.say = told_ != *pick;   // (the same missing pick again at once, as the page re-stores it: no second line)
        told_ = *pick;
        return s;
    }

private:
    std::int32_t last_good_ = 0;
    std::int32_t told_ = 0;
};

// Other players' picks: true when the line is to be said for `number` now.
class Others {
public:
    static constexpr std::size_t kNumbers = 17;   // player numbers 1..16
    bool Tell(std::uint8_t number, std::uint16_t character, const std::map<std::uint16_t, std::uint16_t>& files) {
        if (number >= kNumbers) return false;
        if (Installed(character, files)) {
            told_[number] = 0;
            return false;
        }
        if (told_[number] == character) return false;
        told_[number] = character;
        return true;
    }

private:
    std::array<std::uint16_t, kNumbers> told_{};
};

// Rows 239/240/241 (0.11.9, black-join-0119): a pick whose model is not here (one of Snail's NPC characters on a kit
// with his UI-only part: a host's save worn with one, or a pick from before the NPC download was opt-in) and the
// world never drew after a save load: the same frame at 60 fps for good. 0.11.9's Own ran only inside a co-op session
// and only in play: a solo game never had it (the friend's log: co-op off, the UI-only part, the black frame), and a
// guest had it only from the load's first frame (E'tykalu's logs). LANE1: the put-back made on the TITLE, before
// Continue, loads Link with the world (black-join-0119, runs bj-title*). So Own now runs on every frame of the
// module, co-op on or off, on the title too (coop_hook.cpp PickGuardFrame); the save files are put right as well
// (FixSavePick, below).

// Player_<Age>_C<digits>...: an NPC character's model file in the Character Select mod's romfs/Model (its UI-only part
// ships Player_<Age>_Cba_Animation, which is not one).
inline bool IsNpcModelName(const std::string& name) {
    for (const char* prefix : {"Player_Child_C", "Player_Adult_C"}) {
        const std::string p(prefix);
        if (name.size() > p.size() && name.compare(0, p.size(), p) == 0 && name[p.size()] >= '0' &&
            name[p.size()] <= '9') {
            return true;
        }
    }
    return false;
}

// The saved game's records (tools/autotest/save_flags.py, all little-endian): magic 0x01020304, the pool's offset at
// 8, from 0x20 a table of 8-byte records. A record whose first word is 0 opens a type (its second word the type id,
// 0 to 0x1f in order, then the end record 0x20); any other first word is a value's hash, and for the inline types
// (Int is type 2) the second word is the value. There is no checksum. The offset of the Int `hash`'s value word, or
// nothing (not a save, a table out of order, or no such Int).
inline std::optional<std::size_t> SaveIntAt(const std::uint8_t* data, std::size_t size, std::uint32_t hash) {
    constexpr std::uint32_t kMagic = 0x01020304;
    constexpr std::size_t kTable = 0x20;
    constexpr std::uint32_t kInt = 2;
    constexpr std::uint32_t kEnd = 0x20;
    const auto word = [&](std::size_t at) {
        return static_cast<std::uint32_t>(data[at]) | static_cast<std::uint32_t>(data[at + 1]) << 8 |
               static_cast<std::uint32_t>(data[at + 2]) << 16 | static_cast<std::uint32_t>(data[at + 3]) << 24;
    };
    if (data == nullptr || size < kTable || word(0) != kMagic) return std::nullopt;
    const std::size_t pool = word(8);
    if (pool < kTable || pool > size) return std::nullopt;
    std::optional<std::uint32_t> kind;
    for (std::size_t at = kTable; at + 8 <= pool; at += 8) {
        const std::uint32_t first = word(at), second = word(at + 4);
        if (first == 0) {
            if (second != (kind ? *kind + 1 : 0)) return std::nullopt;
            if (second == kEnd) return std::nullopt;
            kind = second;
        } else if (!kind) {
            return std::nullopt;
        } else if (*kind == kInt && first == hash) {
            return at + 4;
        }
    }
    return std::nullopt;
}

// What FixSavePick did: the pick the save had (nothing: no such value, no mod's layout) and whether it was changed.
struct SaveFix {
    std::optional<std::int32_t> was;
    bool changed = false;
};
// A progress.sav's bytes: a stored pick whose model is not installed becomes 0 (Link), in place. `pick_hash`: the
// flag_hash of CharacterMenu_SelectedCharacter.
inline SaveFix FixSavePick(std::uint8_t* data, std::size_t size, std::uint32_t pick_hash,
                           const std::map<std::uint16_t, std::uint16_t>& files) {
    SaveFix out;
    const auto at = SaveIntAt(data, size, pick_hash);
    if (!at) return out;
    std::uint32_t raw = 0;
    for (int i = 3; i >= 0; --i) raw = raw << 8 | data[*at + static_cast<std::size_t>(i)];
    out.was = static_cast<std::int32_t>(raw);
    if (Installed(*out.was, files)) return out;
    for (int i = 0; i < 4; ++i) data[*at + static_cast<std::size_t>(i)] = 0;
    out.changed = true;
    return out;
}

}  // namespace CoopCharSlots
