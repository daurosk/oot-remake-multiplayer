// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ocarina notes for the other players (issue 152, docs/puppet.md "Ocarina").
// The notes are not AS events: the game's ocarina code picks the note from
// the player's input and plays the SLink asset Ocarina_<note> itself, and
// Link's AS chooses the fingers from AS variables (OcarinaLastSingleNote,
// OcarinaLastMelodyType, OcarinaFrameCount). Where that note is kept, and the
// call that plays a named SLink asset, were not found by reading `main`. This
// probe finds the note in a run: while one of our Link's AS slots plays an
// ocarina node, it logs each slot's command when it changes and every small
// word (0 to 63) of the AS controller and its slots that stepped. It only
// reads, and is off unless LICHII_COOP_OCARINA_PROBE=1.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CoopOcarina {

// The notes of the game, in the order of its SLink assets Ocarina_<name>.
// Index 0 is "no note".
const std::vector<std::string>& NoteNames();
// The index of a note name (Ocarina_A -> "A"), or 0.
std::uint8_t NoteIndex(std::string_view name);

// Issue 152, the Ocarina in the hand: our Link plays with the same
// Item_Ocarina_* actor that hangs on its belt (no new actor is made), so the
// player's own code moves it to the hand. This probe finds what it changes:
// it keeps a copy of our Link's EquipmentUser, the Ocarina actor, its
// component table and the blocks that table points to, (A) just before the
// Ocarina comes out, (B) 1.5 s into playing and (C) 1.5 s after it is put
// away, and logs every word with A == C and B != A. Reads only.
class HoldProbe {
public:
    using ReadWord = std::function<std::optional<std::uint64_t>(std::uint64_t)>;

    static constexpr std::uint64_t kParts = 0x270;          // actor -> component table
    static constexpr std::uint64_t kPartsEquipment = 0x200; // table -> EquipmentUser
    static constexpr std::uint64_t kArmorLinks = 0xe0;      // EquipmentUser: 13 links
    static constexpr std::uint32_t kArmorLinkCount = 13;
    static constexpr std::uint64_t kLinkStride = 0x18;
    static constexpr std::uint64_t kLinkHolder = 0x08;
    static constexpr std::uint64_t kLinkId = 0x10;
    static constexpr std::uint64_t kHolderActor = 0x40;
    static constexpr std::uint64_t kActorName = 0x260;      // char*
    static constexpr std::uint64_t kActorPosition = 0x31c;  // three floats
    static constexpr std::size_t kEquipmentBytes = 0x1600;
    static constexpr std::size_t kActorBytes = 0x900;
    static constexpr std::size_t kTableBytes = 0x400;
    static constexpr std::size_t kBlockBytes = 0x200;
    static constexpr std::size_t kMaxBlocks = 64;
    static constexpr std::uint64_t kSettleUs = 1'500'000;
    static constexpr std::uint64_t kRefreshUs = 1'000'000;
    static constexpr int kMaxLines = 160;

    // Once per walk: our Link and whether one of its AS slots plays an
    // Ocarina node. Returns log lines.
    std::vector<std::string> Step(const ReadWord& read, std::uint64_t link, bool playing,
                                  std::uint64_t now_us);

    struct Region {
        std::string name;
        std::uint64_t base = 0;
        std::size_t bytes = 0;
    };
    // The regions for our Link now (for tests); empty without an Ocarina.
    std::vector<Region> Regions(const ReadWord& read, std::uint64_t link) const;

private:
    using Snapshot = std::vector<std::vector<std::optional<std::uint64_t>>>;
    Snapshot Take(const ReadWord& read) const;
    std::string Place(const ReadWord& read, std::uint64_t link) const;

    enum class Phase { Idle, Playing, Away, Done };
    Phase phase_ = Phase::Idle;
    std::vector<Region> regions_;
    Snapshot before_, during_;
    std::string place_before_, place_during_;
    std::uint64_t since_us_ = 0;
    std::uint64_t taken_us_ = 0;
    int lines_ = 0;
};

class OcarinaProbe {
public:
    using ReadWord = std::function<std::optional<std::uint64_t>(std::uint64_t)>;

    // The AS chain (docs/puppet.md, "Where the current animation is").
    static constexpr std::uint64_t kAsComponent = 0x270;
    static constexpr std::uint64_t kAsStep1 = 0x40;
    static constexpr std::uint64_t kAsStep2 = 0x18;
    static constexpr std::uint64_t kAsStep3 = 0x18;
    static constexpr std::uint64_t kSlotCount = 0x18;
    static constexpr std::uint64_t kSlotArray = 0x20;
    static constexpr std::uint64_t kSlotStride = 0x48;
    static constexpr std::uint64_t kSlotCommand = 0x10;
    static constexpr std::size_t kMaxSlots = 8;
    static constexpr std::size_t kControllerBytes = 0x400;
    static constexpr std::size_t kSlotBytes = 0x200;
    static constexpr std::uint32_t kSmall = 64;      // a note or melody index is below this
    static constexpr int kNoisySteps = 40;           // a word that steps this often is muted
    static constexpr std::size_t kMaxShown = 24;     // steps per line
    static constexpr int kMaxLines = 600;            // per session
    static constexpr std::uint64_t kEveryUs = 33'000;

    // Once per walk with our own Link's actor. Returns log lines.
    std::vector<std::string> Look(const ReadWord& read, std::uint64_t actor, std::uint64_t now_us);
    void Reset();

    // For tests.
    bool Active() const { return active_; }

private:
    std::optional<std::string> ReadName(const ReadWord& read, std::uint64_t address) const;

    std::map<std::uint64_t, std::uint32_t> words_;  // key: (region << 32) | offset
    std::map<std::uint64_t, int> steps_;
    std::vector<std::string> commands_;
    bool active_ = false;
    std::uint64_t last_us_ = 0;
    int lines_ = 0;
};

// Issue 152, the Ocarina in the puppet's hands. The hold probe (run
// 20261111-192211, child Link) showed what the player's code changes while it
// plays: in the Ocarina actor's first component block (its model bind), the
// bone index at +0x13a goes from 0x133 (Pod_B, the belt) to 0x44 (a Weapon_
// hand bone), the 3x4 offset matrix at +0x140 becomes identity, and the bone
// name at +0x194 goes from "Pod_B" to "Weapon_...". The puppet's animation
// does not do this, so while the puppet plays an Ocarina node this does it on
// the puppet's own Ocarina (never the holder) and puts the saved words back
// after. It writes only when the bind looks exactly like a belt one (name
// "Pod_", bone Pod_B of the child's or the adult's skeleton); otherwise it
// leaves the Ocarina alone. Bone numbers are the skeletons' own (romfs
// Model/Player_<Age>.Player_<Age>.bfres, bone index at +0x20 of each 0x58
// byte bone): child Pod_B 0x133, Weapon_R 0x44; adult Pod_B 0x145,
// Weapon_R 0x42. The puppets wear the same skeletons.
class HandBind {
public:
    using ReadWord = std::function<std::optional<std::uint64_t>(std::uint64_t)>;
    using WriteWord = std::function<bool(std::uint64_t, std::uint64_t)>;

    static constexpr std::uint64_t kBindBone = 0x138;    // word; bone index in bits 16..31
    static constexpr std::uint64_t kBindMatrix = 0x140;  // 12 floats, 6 words
    static constexpr std::uint64_t kBindName = 0x190;    // word: 0x100, then the name
    static constexpr std::uint32_t kMatrixWords = 6;
    static constexpr std::uint64_t kBeltBone = 0x133;       // child Pod_B
    static constexpr std::uint64_t kHandBone = 0x44;        // child Weapon_R
    static constexpr std::uint64_t kAdultBeltBone = 0x145;  // adult Pod_B
    static constexpr std::uint64_t kAdultHandBone = 0x42;   // adult Weapon_R
    // The hand bone for a belt bone of either skeleton; 0 for any other bone.
    static std::uint64_t HandFor(std::uint64_t belt_bone);
    static constexpr std::uint64_t kBeltName = 0x5f646f5000000100;  // 0x100, "Pod_"

    // Whether a puppet animation (AS command) is an Ocarina one.
    static bool Playing(std::string_view anim);
    // The model bind block of the actor's Ocarina accessory, or 0.
    static std::uint64_t Find(const ReadWord& read, std::uint64_t actor);

    // Once a frame for one puppet. Returns a log line when it moves the
    // Ocarina, puts it back or refuses ("" otherwise).
    std::string Step(const ReadWord& read, const WriteWord& write, std::uint64_t actor,
                     bool playing);
    bool Held() const { return block_ != 0; }

private:
    void Restore(const WriteWord& write);
    std::uint64_t actor_ = 0;
    std::uint64_t block_ = 0;  // set while held
    std::uint64_t saved_bone_ = 0;
    std::uint64_t hand_bone_ = 0;  // set while held: this skeleton's Weapon_R
    std::uint64_t saved_matrix_[kMatrixWords] = {};
    bool refused_ = false;     // logged once per play
};

} // namespace CoopOcarina
