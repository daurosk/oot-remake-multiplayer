// SPDX-License-Identifier: GPL-3.0-or-later
// Shared progress and world state, as the emulator hook wires it
// (lichii/hook/coop_hook.cpp, Coop::Begin and Coop::Frame): the player's
// items, songs and keys, chests (opened in every game, their content to every
// player), one-time pickups, dungeon switches, doors, cleared rooms and water
// levels, story and quest flags, another player's room event started here
// (live events) and shared story cutscenes. The rules are the shared code's
// own (coop/host's FlagWatcher and stores, lichii/hook's lists); this file is
// only the order they are called in, with this port's memory and log.
//
// Threads: Configure, Started before the session's threads run; OnEvent on the
// network thread; Before and After on the thread that makes the session's
// frame (the game's own thread in a game with the plugin interface).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "coop/host/flag_watcher.hpp"
#include "coop/host/game_flags.hpp"
#include "coop/host/game_memory.hpp"
#include "coop/host/player_reader.hpp"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"

namespace CoopNx2navi {

class Progress {
public:
    using LogFn = std::function<void(const std::string& line)>;

    Progress();
    ~Progress();

    // Fills the session's shared list and rules. `on` false: nothing is shared.
    // `chest_text`: what the notices say for an opened chest.
    void Configure(coop::host::SessionSettings& session, bool on, LogFn log);
    // Once the session exists: the one-way keys, the pickups, the counts.
    void Started(coop::host::Session& session);
    bool Sharing() const;

    void OnEvent(const coop::net::Event& event);

    // Before Session::frame: the store its flag writes go through, or null
    // (nothing shared, or the game's memory is not readable this frame).
    // `memory` null: no game this frame.
    coop::host::FlagStore* Before(coop::host::WritableGameMemory* memory, coop::host::Session& session,
                                  const std::optional<coop::host::PlayerReading>& reading);
    // After Session::frame, with its report. Returns the notices to show (what
    // another player's change gave us is said by the caller's own notifier).
    void After(coop::host::WritableGameMemory* memory, coop::host::Session& session,
               const coop::host::FrameReport& report,
               const std::optional<coop::host::PlayerReading>& reading);

    // The words of a notice for a flag the item table does not know.
    static std::optional<std::string> ExtraNotice(coop::host::FlagKey key,
                                                  const coop::protocol::FlagEntry& after);

    // For the status line: "; progress: ..." or "".
    std::string StatusText() const;

private:
    struct State;
    std::unique_ptr<State> s_;
};

}  // namespace CoopNx2navi
