// The co-op entry points the patched emulator calls. Built into the
// emulator's core library only when LICHII_COOP is on.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "coop_settings.h"

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

// Hookshot's menu (hookshot/src/coop_host_menu.h): starts co-op with these
// settings, not coop.ini's. False when co-op runs already, after Shutdown,
// for a code mod, or when the client could not start. Start() before it
// does not connect anywhere when coop.ini names no server.
bool Begin(Settings settings);
// Says goodbye and stops what Begin started; Begin may run again. The ended
// session is kept until Shutdown, so a frame or the overlay still holding
// it stays valid. For the title screen: puppets the game made stay.
void End();

// The player chose another colour while the game runs (hookshot/src/
// coop_look.h): the next frame sends it in PLAYER_EQUIP, so the others'
// puppet of this player changes; a session started later wears it too, not
// the settings' colour. Unset after a colour was sent: the others keep that
// colour until the next start. Any thread; works with co-op off.
void SetColor(std::optional<std::uint32_t> color);

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
    bool tag_head = false;     // over the player's head: drawn as a name tag, no dot
};
struct Markers {
    float width = 0.0f, height = 0.0f;
    std::vector<Marker> items;  // draw in order: far to near
};
// The markers for the last frame, or nothing when the game's camera is not
// known (the panel is then all there is). Any thread.
std::optional<Markers> CurrentMarkers();

// Chat and the player list, for the co-op window. Any thread.
//
// Sends `text` as chat. False when co-op is off, not connected, or the text
// is refused (empty, too long, control characters); then nothing is added.
// The server does not echo chat, so a sent line is added here as "you: ...".
bool SendChat(const std::string& text);
// The last chat lines and room notices (coop_chat.h), oldest first; text and
// when it came.
struct ChatLine {
    std::string text;
    bool notice = false;  // "Saria joined", "connection lost", not chat
    std::uint64_t age_ms = 0;
};
std::vector<ChatLine> ChatLines();
// Everyone in the room, this player first, then by number.
struct PlayerRow {
    std::uint8_t number = 0;
    std::string name;
    bool self = false;
    std::string state;                  // connected, reconnecting, quiet, ...
    std::optional<std::uint32_t> ms;    // self: round trip; others: sample delay
    std::optional<std::string> scene;
};
std::vector<PlayerRow> PlayerList();

} // namespace CoopHook
