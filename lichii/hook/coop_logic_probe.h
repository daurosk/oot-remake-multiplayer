#pragma once
// Read-only probe of an actor's logic plugs (jabu-lifts, player report #65).
//
// The game's own set-bool for an actor's logic plug is the AI node ExecuteLogicSetBool's execute
// (main+0x2727540, vtable main+0x49a9550; read from the recompiled main, reports/dungeon-sync-b.md):
//   logic = [[actor + 0x270] + 0x118]; it is live when [logic + 0x20] != -1 or byte [logic + 0x30] bit 0 is 0
//   root  = [[[logic + 0x28] + 8] + 0x20]: a binary tree of plugs, left at +0, right at +8
//   each plug: key u32 at +0x18 (the node looks for its own key xor 7), type byte at +0x38 (7: bool),
//   value at [[plug + 0x28] & ~3] (a byte for a bool)
// This probe walks the same tree with reads only and lists every plug, so a run shows which key is which
// plug name (the Jabu-Jabu lift's RideOnCharacter, RideOnPlayer, IsInPlayer, ...) and when its value moves.
// LICHII_COOP_LOGIC_PROBE=1 turns it on (off by default).
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace CoopLogicProbe {

namespace addr {
constexpr std::uint64_t kActorLogic = 0x270;
constexpr std::uint64_t kLogicInner = 0x118;
constexpr std::uint64_t kLogicState = 0x20;   // -1: not live
constexpr std::uint64_t kLogicFlags = 0x30;   // bit 0: not live
constexpr std::uint64_t kLogicTable = 0x28;
constexpr std::uint64_t kTableRootHolder = 8;
constexpr std::uint64_t kRoot = 0x20;
constexpr std::uint64_t kLeft = 0;
constexpr std::uint64_t kRight = 8;
constexpr std::uint64_t kKey = 0x18;
constexpr std::uint64_t kValue = 0x28;
constexpr std::uint64_t kType = 0x38;
}  // namespace addr

constexpr std::uint8_t kBool = 7;
constexpr std::size_t kMaxPlugs = 256;

using ReadWord = std::function<std::optional<std::uint64_t>(std::uint64_t)>;

struct Plug {
    std::uint32_t key = 0;
    std::uint8_t type = 0;
    std::optional<std::uint8_t> value;  // the first byte of the value, when it can be read
    // [plug + 0x28]: the value's address, its low 2 bits flags (bit 1: the game's set-bool marks a change of it,
    // f_7103c41cf0/41d00). deku-ladder: CoopLogicSet::SetInput stores through it.
    std::uint64_t value_word = 0;
    std::uint64_t node = 0;  // the plug node's own address (deku-ladder: SetInput's ownership check)
};

// The actor's plugs in key order, or nothing when its logic is not there or not live.
std::optional<std::vector<Plug>> Read(const ReadWord& read, std::uint64_t actor);

// Logs each watched actor's plugs once, then every change of a plug's value. Reads only.
class Watch {
public:
    static constexpr std::size_t kMaxActors = 6;  // deku-switches: ladder, pillar, eye, asker(s)
    static constexpr int kMaxLines = 600;  // per session
    // `actors`: (address, name) of the actors to watch this walk.
    std::vector<std::string> Look(const ReadWord& read,
                                  const std::vector<std::pair<std::uint64_t, std::string>>& actors);
    void Reset() { seen_.clear(); }

private:
    std::map<std::uint64_t, std::map<std::uint32_t, std::optional<std::uint8_t>>> seen_;
    int lines_ = 0;
};

}  // namespace CoopLogicProbe
