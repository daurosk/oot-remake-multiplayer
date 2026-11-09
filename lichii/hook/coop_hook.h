// The co-op entry points the patched emulator calls. Built into the
// emulator's core library only when LICHII_COOP is on.
#pragma once

#include <optional>
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

// A player drawn over the game, in the pixels of `Markers::width` x `height`
// (the game's scene target; the window scales it to the picture).
struct Marker {
    float x = 0.0f, y = 0.0f;  // centre, from the top left, y down
    float size = 0.0f;         // across
    float alpha = 1.0f;
    bool edge = false;         // just off screen: an arrow at the edge
    float arrow_angle = 0.0f;  // radians, 0 right, pi/2 down
    bool self = false;         // the local player (LICHII_COOP_DEBUG_CAMERA only)
    std::string tag;           // the name tag
    float tag_x = 0.0f, tag_y = 0.0f;  // bottom centre of the text, or top when below
    bool tag_below = false;
};
struct Markers {
    float width = 0.0f, height = 0.0f;
    std::vector<Marker> items;  // draw in order: far to near
};
// The markers for the last frame, or nothing when the game's camera is not
// known (the panel is then all there is). Any thread.
std::optional<Markers> CurrentMarkers();

} // namespace CoopHook
