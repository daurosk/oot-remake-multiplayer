// SPDX-License-Identifier: GPL-3.0-or-later
// The in-game overlay: the co-op panel, notices and the chat box, drawn
// through NX2navi's plugin interface (dll/third_party/plugin_api.h; NX2navi
// docs/plugins.md). Only a game that has the interface draws it; without one
// (an NX2navi build from before it, or not NX2navi) nothing here is used and
// co-op runs as before.
//
// Threads. Three callers share one mutex-guarded state here and nothing else:
// - the DLL's frame thread publishes a Snapshot every frame and takes the
//   chat lines typed (TakeOutbox); the network thread adds notices;
// - the game's presenting thread calls the overlay callback, which copies
//   what it needs under the lock and draws outside it;
// - the game's window thread calls the key and text callbacks, which only
//   change the chat box and the outbox.
// No callback ever waits on the network: sending happens on the frame thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "overlay_model.hpp"

struct Nx2PluginApi;

namespace CoopNx2navi::Overlay {

// Where the overlay writes its log lines (the DLL's own log). Any thread.
void SetLog(void (*log)(const char* line));

// The interface, from nx2navi_plugin_init or from the game's export. The
// first acceptable table is kept; true when this call is the one that gave
// it. `how` is said in the log.
bool Offer(const Nx2PluginApi* api, const char* how);
// Asks the game's executable (NX2navi docs/plugins.md, "Ask the executable").
// 1: the interface was there (and offered), 0: no such export (a game without
// the interface), -1: the export refused this version.
int AskGame();
bool HaveApi();
// The table itself, null until one was offered: for the DLL's other users of
// the interface (puppet.hpp).
const Nx2PluginApi* Api();

// The DLL's co-op runs and its coop.ini says overlay = 1: the callbacks are
// registered as soon as the interface is there (now or later).
void Want();
// The callbacks are registered.
bool Active();

// Frame thread: this frame's state for the panel.
void Publish(Ui::Snapshot snapshot);
// Frame thread: the chat lines typed since the last call, oldest first.
std::vector<std::string> TakeOutbox();
// Any thread: a line that fades after a few seconds. A chat line also goes to
// the history the chat box shows.
void Notice(std::string text, bool chat = false);
// Another player's item lines since the last ask (the item-get moment, item_get.hpp).
std::vector<std::string> TakeItemLines();
// The game's menu was open at the last overlay frame.
bool MenuOpen();

// Takes the callbacks out and gives the input back if the chat box held it.
// Any thread; the DLL's stop export calls it.
void Shutdown();

// The other players on the game's minimap (README, "The other players on the
// minimap"): on unless coop.ini says minimap = 0; `debug` (OOT_COOP_DEBUG_MINIMAP=1)
// also draws our own dot in white, to compare with the game's arrow, and logs
// what the game's map screen holds once a second. Before Want().
void SetMinimap(bool on, bool debug);

// Name tags over the other players, in each player's colour (README, "Name
// tags"): on unless coop.ini says name_tags = 0. They need the game's camera:
// the plugin interface's world_to_screen (version 4). In an older game nothing
// is drawn, and the log says so once. Before Want().
void SetNameTags(bool on, bool debug = false);

}  // namespace CoopNx2navi::Overlay
