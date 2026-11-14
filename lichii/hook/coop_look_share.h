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
// files of its own. A player whose look colours any part but the tunic gets a
// slot, the lowest free one; with all taken, the game's own items. Kept while
// the player keeps such a look and stays; freed on leaving.
//
// The module paints the slot's copy that is not worn (it writes that copy's
// texture files in the player's look: a Job), says it is Done, and only then
// does the puppet wear it (CopyOf): the game loads the new files through its
// own file system when it makes the copy's pieces.
inline constexpr std::size_t kSlots = 4;
inline constexpr std::size_t kCopiesPerSlot = 2;
// True when the look colours a part other than the tunic (part 0).
bool HasGear(const Parts& parts);

// The rules, with no network: what to send and when, what the others wear.
// Not thread-safe (the functions below guard it).
class Share {
public:
    // Our look changed: it goes out at the next chance.
    void Set(Parts parts);
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
    // painted in (or none is ready). `copy` is the slot's other copy.
    struct Job {
        std::size_t slot = 0;
        std::size_t copy = 0;
        std::uint8_t player = 0;
        Parts parts;
        bool operator==(const Job&) const = default;
    };
    std::vector<Job> Jobs() const;
    // The job's files are written: its copy is the slot's ready one, if the
    // slot is still that player's.
    void Done(const Job& job);
    // The copy player `number`'s puppet wears (1 to kSlots * kCopiesPerSlot):
    // nothing before one is painted for them.
    std::optional<std::size_t> CopyOf(std::uint8_t number) const;
    // Slots are given only where the puppets' copies are painted (Hookshot's
    // module): off, every slot is freed.
    void EnableSlots(bool on);

private:
    void Assign(std::uint8_t number);
    void Free(std::uint8_t number);

    std::optional<Parts> ours_;
    bool due_ = false;
    std::map<std::uint8_t, Parts> theirs_;
    bool slots_on_ = false;
    std::array<std::uint8_t, kSlots> slots_{};   // the player in each slot, 0 for none
    struct Ready {
        std::size_t copy = 0;       // 0: none painted
        std::uint8_t player = 0;
        Parts parts;
    };
    std::array<Ready, kSlots> ready_{};
};

// The hook's one Share. Set: any thread. OnEvent: the network thread, with
// its client (sends at once). Frame: the game's thread, each frame. Of: any.
void Set(Parts parts);
void OnEvent(const coop::net::Event& event, coop::net::Client& client);
void Frame(coop::host::Session* session);
std::optional<Parts> Of(std::uint8_t number);
std::optional<std::size_t> SlotOf(std::uint8_t number);
std::vector<Share::Job> Jobs();
void Done(const Share::Job& job);
std::optional<std::size_t> CopyOf(std::uint8_t number);
void EnableSlots(bool on);

}  // namespace CoopLookShare
