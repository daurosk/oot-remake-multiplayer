// SPDX-License-Identifier: GPL-3.0-or-later
// Each player's look of several parts (PLAYER_LOOK, docs/protocol.md "Looks";
// the parts: hookshot/src/coop_look.h). Our own goes out when the player
// changes it and again whenever someone joins or we connect again, so a late
// joiner has it; the others' are kept by player number for the puppets
// (docs/puppet.md, "The Link customizer").
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace coop::host {
class Session;
}
namespace coop::net {
class Client;
struct Event;
}  // namespace coop::net

namespace CoopLookShare {

using Parts = std::vector<coop::protocol::LookPart>;

// Puppets in their own gear colours (docs/puppet.md, "Gear colours on the
// puppets"): the puppet pack has kSlots slots of copies of every coloured
// item, two copies a slot (copy 2 * slot - 1 and 2 * slot), each with texture
// files of its own. Every player whose look arrives gets a slot, the lowest
// free one, gear colours or not (a look with none paints the copy in the
// game's own colours): a puppet in the game's own sword shares that model's
// blade switch with our own Link's and hid its blade whenever ours was put
// away (held-sword). With all taken, the game's own items. Kept while the
// player stays; freed on leaving.
//
// The module paints the slot's copy that is not worn (it writes that copy's
// texture files in the player's look: a Job), says it is Done, and only then
// does the puppet wear it (CopyOf): the game loads the new files through its
// own file system when it makes the copy's pieces.
inline constexpr std::size_t kSlots = 4;
inline constexpr std::size_t kCopiesPerSlot = 2;
// After a copy is painted, the slot's other copy is not painted again for this
// long: the puppet has time to move off it (the equip call makes new pieces,
// the game lets the old ones and their textures go).
inline constexpr std::uint64_t kCopyDwellMs = 5000;
// True when the look colours a part other than the tunic (part 0).
bool HasGear(const Parts& parts);
// Character Select (github.com/snaildev99/OoT-Character-Select): the character a player picked in that mod
// goes out with the look, as one more part. Its part number is one the colours do not use; its "colour" is
// kCharacterMark | the stored character number (the mod's saved Int CharacterMenu_SelectedCharacter: 0 is
// Link). A build that does not know the part passes it over, as any part it does not know, and the server
// relays it as it is. On arrival it is taken out of the parts (CharacterOf has it), so nothing that paints a
// look sees it.
inline constexpr std::uint8_t kCharacterPart = 63;
inline constexpr std::uint32_t kCharacterMark = 0x010000;
inline constexpr std::uint16_t kCharacterMax = 0x0FFF;
// That mod's Customize options for the picked character (hair, eye and clothing colours, gear shown or
// hidden) ride along the same way, as part kCustomPart: its "colour" is the six options, four bits each,
// in the order of kCustomFlags (the mod's GameData Ints that hold them). Sent only with a character and
// only when one of them is set; taken out of the parts on arrival (CustomOf has it).
inline constexpr std::uint8_t kCustomPart = 62;
// 0.11.9 (the owner: one customizer, co-op's own): the indexes of the mod's option Ints (kCustomFlags) the module sets
// back to 0, the model's own, from the values read: every one read and not 0. With all held at 0 the mod's code
// paints nothing of its own, and part 62 is never sent by a 0.11.9 game.
std::vector<std::size_t> OptionsToHold(const std::array<std::optional<std::int32_t>, 6>& values);
inline constexpr std::array<const char*, 6> kCustomFlags = {
    "CharacterMenu_CustomHair",    "CharacterMenu_CustomEyes",     "CharacterMenu_CustomGear",
    "CharacterMenu_CustomClothes", "CharacterMenu_CustomClothes2", "CharacterMenu_CustomClothes3"};

// The rules, with no network: what to send and when, what the others wear.
// Not thread-safe (the functions below guard it).
class Share {
public:
    // Our look changed: it goes out at the next chance.
    void Set(Parts parts);
    // Our picked character (0: Link, or no Character Select mod): when it changed, the look goes out again.
    void SetCharacter(std::uint16_t number);
    // Our Customize options for it (kCustomPart's 24 bits): when they changed, the look goes out again.
    void SetCustom(std::uint32_t options);
    // Player `number`'s Customize options: 0 until heard, and for none set.
    std::uint32_t CustomOf(std::uint8_t number) const;
    // Player `number`'s picked character: 0 until one is heard, and for Link.
    std::uint16_t CharacterOf(std::uint8_t number) const;
    // An event of the session: a join or a (re)connect sends ours again; a
    // PLAYER_LOOK is kept; a player who left is forgotten. True when this
    // changed what a player wears (for the log and the puppets).
    bool OnEvent(const coop::net::Event& event);
    // Our look to send now (once per change or join); nothing when none is
    // due or none was ever chosen.
    std::optional<coop::protocol::PlayerLook> TakeSend();
    // What player `number` wears: nothing heard yet.
    std::optional<Parts> Of(std::uint8_t number) const;
    // Player `number`'s gear slot, 1 to kSlots: nothing when slots are off,
    // their look has no gear or every slot is taken.
    std::optional<std::size_t> SlotOf(std::uint8_t number) const;
    // A copy to paint: the player in `slot` wears a look its ready copy is not
    // painted in (or none is ready). `copy` is the slot's other copy: never
    // the one painted last, which a puppet may still wear (kept when the slot
    // is freed or a session starts again), and none within kCopyDwellMs of the
    // last paint. `now_ms`: any steady clock, the same as Done's.
    struct Job {
        std::size_t slot = 0;
        std::size_t copy = 0;
        std::uint8_t player = 0;
        Parts parts;
        bool operator==(const Job&) const = default;
    };
    std::vector<Job> Jobs(std::uint64_t now_ms) const;
    // The job's files are written: its copy is the slot's ready one, if the
    // slot is still that player's.
    void Done(const Job& job, std::uint64_t now_ms);
    // The copy player `number`'s puppet wears (1 to kSlots * kCopiesPerSlot):
    // from the moment the slot is theirs (row 198: before its first paint, the
    // copy that paint is for); nothing only without a slot.
    std::optional<std::size_t> CopyOf(std::uint8_t number) const;
    // Slots are given only where the puppets' copies are painted (Hookshot's
    // module): off, every slot is freed.
    void EnableSlots(bool on);

private:
    void Assign(std::uint8_t number);
    void Free(std::uint8_t number);

    std::optional<Parts> ours_;
    bool due_ = false;
    std::uint16_t character_ = 0;
    bool character_said_ = false;   // a character went out once: a look goes out without colours too
    std::map<std::uint8_t, Parts> theirs_;
    std::map<std::uint8_t, std::uint16_t> their_character_;
    std::uint32_t custom_ = 0;
    std::map<std::uint8_t, std::uint32_t> their_custom_;
    bool slots_on_ = false;
    std::array<std::uint8_t, kSlots> slots_{};   // the player in each slot, 0 for none
    struct Ready {
        std::size_t copy = 0;       // the copy painted last (0: none); kept for a new player in the slot
        std::uint8_t player = 0;    // who it is painted for (0: nobody now)
        Parts parts;
        std::uint64_t at_ms = 0;    // when
    };
    void Forget(std::size_t i);
    std::array<Ready, kSlots> ready_{};
};

// Which pick goes out (coop_hook reads the mod's Ints twice a second): the character and its Customize options as
// one value (OwnPick::Of). A guest plays the host's save (CoopSave), whose saved Int is the HOST's pick: until the
// guest picks a character on that save, the pick and options it had on its own save go out (kept in
// coop-character.txt for the next start; none known: Link), so the others never see the guest as the host's
// character, nor the guest's own character in options set for another. The options are unsaved (0 after any load):
// a change of them alone is no new pick. A new save load is a new start (CoopSave::SaveLoadStarts). The guest's own screen still shows the save's
// pick (the mod's code reads those Ints; we leave the game's data alone). Not thread-safe (the game's thread only).
class OwnPick {
public:
    // The character (low 16 bits) and its Customize options (kCustomFlags' nibbles, the 24 bits above).
    static std::uint64_t Of(std::uint16_t character, std::uint32_t custom) {
        return character | (static_cast<std::uint64_t>(custom & 0xFFFFFFu) << 16);
    }
    static std::uint16_t CharacterIn(std::uint64_t pick) { return static_cast<std::uint16_t>(pick & 0xFFFFu); }
    static std::uint32_t CustomIn(std::uint64_t pick) { return static_cast<std::uint32_t>(pick >> 16) & 0xFFFFFFu; }
    // `read`: the saved Ints now (Of; 0: Link, or no mod); `on_host_save`: this game plays the host's save;
    // `loads`: CoopSave::SaveLoadStarts(). The pick to send.
    std::uint64_t Update(std::uint64_t read, bool on_host_save, std::uint64_t loads);
    // The pick kept from an earlier start (coop-character.txt beside coop.ini), before the first Update: a guest that
    // joins before it plays its own save still sends its own.
    void Seed(std::uint64_t own) { own_ = own; kept_ = own; }
    // Our own pick changed (read on our own save) since the last call: the value to keep for the next start.
    std::optional<std::uint64_t> TakeOwnChange();

private:
    std::uint64_t own_ = 0;                 // the last pick read on our own save
    std::optional<std::uint64_t> loads_;    // the host's save load the baseline is of
    std::uint64_t at_load_ = 0;             // the host's pick, as that load brought it
    bool repicked_ = false;                 // the guest changed it on the host's save since that load
    std::optional<std::uint64_t> kept_;     // what TakeOwnChange last gave (or Seed)
};

// The hook's one Share. Set: any thread. OnEvent: the network thread, with
// its client (sends at once). Frame: the game's thread, each frame. Of: any.
void Set(Parts parts);
void SetCharacter(std::uint16_t number);
std::uint16_t CharacterOf(std::uint8_t number);
void SetCustom(std::uint32_t options);
std::uint32_t CustomOf(std::uint8_t number);
void OnEvent(const coop::net::Event& event, coop::net::Client& client);
void Frame(coop::host::Session* session);
std::optional<Parts> Of(std::uint8_t number);
std::optional<std::size_t> SlotOf(std::uint8_t number);
std::vector<Share::Job> Jobs(std::uint64_t now_ms);
void Done(const Share::Job& job, std::uint64_t now_ms);
std::optional<std::size_t> CopyOf(std::uint8_t number);
void EnableSlots(bool on);

}  // namespace CoopLookShare
