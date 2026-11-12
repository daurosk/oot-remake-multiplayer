// The co-op HUD's data (docs/overlay.md, "The look"): each player's hearts
// (PLAYER_LIFE, protocol 10), the game's place names and where the files
// with the game's font and heart pictures are. The co-op window draws them.
// Built into the emulator's core library only when LICHII_COOP is on.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace coop::host {
class Session;
}
namespace coop::net {
struct Event;
}

namespace CoopHud {

// Network thread, from the session's on_event: keeps the others'
// PLAYER_LIFE, forgets a player who left, and sends ours again to a player
// who joined.
void OnEvent(const coop::net::Event& event);

// Present thread, after the session's frame (null when not connected):
// sends our Link's hearts (CoopObjects::OurLife) when they change, at most
// ten times a second, and every 5 s.
void Frame(coop::host::Session* session);

// A player's hearts in quarter hearts (4 = one heart), from their last
// PLAYER_LIFE. Nothing before the first one. Any thread.
struct Life {
    std::uint16_t life = 0;
    std::uint16_t max = 0;
    bool operator==(const Life&) const = default;
};
std::optional<Life> PlayerLife(std::uint8_t player);

// The folder with the game look (font-regular.otf, font-medium.otf,
// heart-0.png to heart-4.png, places.json; tools/puppet_pack/hud.py): the
// puppet pack's coop_hud folder under load/<title id>, or
// LICHII_COOP_HUD_DIR. Nothing when there is none or LICHII_COOP_HUD=0;
// the window then keeps its plain look. Looked up once, logged. Any thread.
std::optional<std::filesystem::path> Folder();

// The game's name for the place of `scene` (as PLAYER_SAMPLE has it), or
// the scene name made readable when places.json has none. Any thread.
std::string PlaceName(std::string_view scene);

} // namespace CoopHud
