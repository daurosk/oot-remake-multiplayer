// The co-op entry points the patched emulator calls. Built into the
// emulator's core library only when LICHII_COOP is on.
#pragma once

#include <string>
#include <vector>

namespace Core {
class System;
}

namespace CoopHook {

// Reads the settings (coop.ini in the config folder, then LICHII_COOP_*
// environment variables) and, when a server is set, starts the network
// thread. Safe to call more than once; OnFrame calls it too. Does nothing
// after Shutdown.
void Start();

// Runs once per emulated frame, after the guest's present, on the thread
// that handles the present.
void OnFrame(Core::System& system);

// Says goodbye to the server and joins the network thread. Call once, when
// the program closes (after emulation has stopped).
void Shutdown();

// What the co-op window shows: one line of text each. Empty when co-op is
// off. Any thread.
struct OverlayLine {
    std::string text;
    bool dim = false;  // a player that is not drawn (other scene, no position)
};
std::vector<OverlayLine> OverlayLines();

} // namespace CoopHook
