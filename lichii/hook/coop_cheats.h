// Debug-only item cheats for testing item sync (docs/items.md, "Live
// check"): give the local player an item flag as a pickup would. Off unless
// LICHII_COOP_CHEATS=1.
//
// The window (coop_cheats_window.cpp, Qt thread) queues a request; the
// present thread applies it on the next frame of the right game by writing
// the game's own flag, so the item watcher sees an ordinary change and the
// sync sends it like a real pickup.
#pragma once

#include <string>
#include <vector>

namespace coop::host {
class FlagStore;
}

namespace CoopCheats {

// LICHII_COOP_CHEATS is set and not "0".
bool On();

// The item flags that can be given (every shared item flag), sorted by name.
std::vector<std::string> Items();

// Queues one gift of the item flag `name`: a Bool flag becomes 1, an Int
// flag goes up by one. A `PouchData.<item>.StockNum` also sets
// `IsGet.<item>` when there is one. Any thread.
void Give(const std::string& name);

// What the last gift did, for the window. Any thread.
std::string LastResult();

// Present thread, once per frame of the right game: applies queued gifts to
// the game's flags (the GameFlagStore, not the item merge).
void Frame(coop::host::FlagStore& game);

} // namespace CoopCheats
