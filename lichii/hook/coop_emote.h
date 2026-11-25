// SPDX-License-Identifier: GPL-3.0-or-later
//
// Emotes and pings: /emote <name>, or the name alone (/wave, /yes, /no, /look,
// /here, /wow) in chat. No emulator code here; coop_emote_game.cpp sends the
// message, coop_puppet.cpp plays it. Tested in tests/lichii/emote_test.cpp.
//
// What happens (docs/protocol.md "Emotes", EMOTE 615):
// 1. The sender types the command. Alone in the game, or too soon after the
//    last emote (kCooldownMs), it is refused with a chat notice and nothing
//    is sent. Otherwise EMOTE (the emote's id) goes to the server, which
//    relays it to everyone else like LINK_SOUND. It is never kept: a player
//    who joins later gets nothing.
// 2. A game that shows the sender's puppet starts the emote's sound on it
//    (the Link sounds' own path, coop_link_sound.h) and holds the puppet on
//    the emote's AS command (the game's own animation, slot 0) for the
//    emote's hold time (Holds). The hold ends early when the sender's own
//    animation changes from what it was when the emote started (they moved
//    or did something), and at once when the sender leaves.
// The sender's own screen shows only a chat notice: their own Link is never
// animated by the emote (their own game controls it). Solo: nothing is sent.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "coop/protocol/messages.hpp"

namespace CoopEmote {

// One emote. The id is the number on the wire (never reused or renumbered).
struct Def {
    std::uint8_t id = 0;
    const char* name = "";      // the command: "wave" for /wave
    const char* alias = "";     // a second name, or ""
    const char* command = "";   // the game's AS command the puppet plays (slot 0)
    const char* sound = "";     // the game's Link sound started on the puppet, or "" for none
    std::uint32_t hold_ms = 0;  // how long the puppet is held on `command`
    const char* notice = "";    // our chat notice: "you wave"
};

std::span<const Def> Table();
const Def* ById(std::uint8_t id);
// Whole name or alias, any case; nothing for a name not in the table.
const Def* ByName(std::string_view name);

inline constexpr std::uint64_t kCooldownMs = 1500;  // one emote per player this often
inline constexpr std::uint32_t kMaxHoldMs = 6000;   // the longest hold, whatever the table says

// A chat line that is a command, not chat.
struct Command {
    enum class Kind : std::uint8_t { list, play, unknown };
    Kind kind = Kind::list;
    const Def* def = nullptr;  // play
    std::string name;          // unknown: what the player typed
};

// Nothing for text that is chat. "/emote" alone lists the names, "/emote wave" and "/wave" play one, "/emote x"
// is `unknown`. Any case, spaces around. Other commands' words ("/help", "/pvp", "/tp") are not taken.
std::optional<Command> Parse(std::string_view text);

// "emotes: /wave, /yes, /no, /look, /here, /wow"
std::string ListLine();

// The sender's rule: one emote per kCooldownMs.
class Limiter {
public:
    bool Allow(std::uint64_t now_ms);

private:
    std::optional<std::uint64_t> last_ms_;
};

// The receiver's holds, one per player number (1 to kMaxPlayers). Under the caller's lock.
class Holds {
public:
    // EMOTE from `number`. False for an id the table lacks or a number out of range (ignored).
    bool Start(std::uint8_t number, std::uint8_t id, std::uint64_t now_ms);
    // Each frame, for a shown puppet: `sampled` is the animation command the sender's samples show now. The emote
    // to play instead, or nothing (no hold, run out, or the sender's animation changed). The first call of a hold
    // takes `sampled` as the sender's animation at the start.
    const Def* Apply(std::uint8_t number, std::string_view sampled, std::uint64_t now_ms);
    void Clear(std::uint8_t number);
    void ClearAll();

private:
    struct Hold {
        const Def* def = nullptr;
        std::uint64_t until_ms = 0;
        bool base_known = false;
        std::string base;
    };
    std::array<Hold, std::size_t{coop::protocol::kMaxPlayers} + 1> holds_{};
};

} // namespace CoopEmote
