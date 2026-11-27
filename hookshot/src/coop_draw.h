// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op over Hookshot's picture (docs/hookshot.md): what the emulator's co-op
// window draws with Qt (lichii/hook/coop_window.cpp, its plain look), drawn
// here through Hookshot's Dear ImGui (hos_coop_draw.h): the chat lines, the
// players' name tags, the player list (F6 held) and the panel (F7). The chat
// box (T, coop_chatbox.h) and, with LICHII_COOP_CHEATS=1, the item cheats (F8,
// coop_cheats_panel.h) too.
//
// The game look (docs/overlay.md, "The look"), as the co-op window's: with
// the HUD pack's font and heart pictures (CoopHud::Folder, loaded through
// hos_coop_draw2's font and image calls: Look below) every co-op text is drawn
// like the game's HUD: no box, the game's font, a soft dark shadow, players'
// names in their colours, and the other players under your hearts with the
// game's heart pictures. Without them (an old pack, an older Hookshot,
// LICHII_COOP_HUD=0) the plain look: boxes, outlined name tags, no HUD blocks.
// The panel (F7) and the item cheats keep the plain look in both.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "coop/overlay/map_icons.hpp"
#include "coop_dbno_game.h"
#include "coop_hook.h"
#include "coop_i18n.h"
#include "coop_map_icons.h"
#include "coop_voice.h"
#include "hos_coop_draw.h"

namespace CoopDraw {

// As the co-op window: a chat line shows 8 s, then fades out in 1 s.
inline constexpr std::uint64_t kChatShowMs = 8000;
inline constexpr std::uint64_t kChatFadeMs = 1000;

// IM_COL32.
constexpr std::uint32_t Rgba(int r, int g, int b, int a) {
    return static_cast<std::uint32_t>(r & 0xFF) | static_cast<std::uint32_t>(g & 0xFF) << 8 |
           static_cast<std::uint32_t>(b & 0xFF) << 16 | static_cast<std::uint32_t>(a & 0xFF) << 24;
}

// A font's vertical metrics as Hookshot's Dear ImGui sizes it: a text `px`
// high spans the font's ascender to its descender (stb_truetype's
// ScaleForPixelHeight, hhea), so its baseline is ascent * px below the top
// it is drawn at and its capitals are cap * px high. The defaults are for a
// font we cannot read (Hookshot's own): Longshot's own prompts' cap height
// (vk_ui.h mid_top: capitals 0.7 of px) and a usual ascent.
struct FontMetrics {
    float ascent = 0.8f;  // hhea ascender / (ascender - descender)
    float cap = 0.7f;     // OS/2 sCapHeight / (ascender - descender)
};
// From a TTF/OTF file's bytes (its hhea, head and OS/2 tables); the defaults
// when they are not there or make no sense.
FontMetrics MetricsOf(const std::vector<std::uint8_t>& font);
// The top to draw a `px` text at so its capitals are centred on y, as
// Longshot centres a prompt's label on its button (vk_ui.h mid_top, which
// reads the ascent from the font the same way).
float MidTop(const FontMetrics& m, float px, float y);

// The game look's pieces, as hos_coop_draw2's font and image calls gave them
// (0: not loaded). The co-op window's GameLook.
struct Look {
    int regular = 0;             // font-regular.otf
    FontMetrics regular_metrics;  // its metrics (MetricsOf on the file)
    int medium = 0;              // font-medium.otf (the regular one when it is missing)
    std::array<int, 5> hearts{};  // heart-0..4.png: 0 to 4 quarters filled
    // The game's own map arrow (MapIconPlayer_00, Link's arrow on the maps: tools/puppet_pack/hud.py), pointing up:
    // map-arrow.png (its shadow and rim) and map-arrow-fill.png (its inside, white: drawn tinted with the player's
    // colour). Drawn turned to each player's facing by Hookshot (hos_coop_draw3's draw_image_turned, ImGui's
    // AddImageQuad), at any angle as 0.11.12's drawn arrow and as the game turns its own pane. 0: not loaded (a pack
    // made before them, or a Hookshot without draw_image_turned): names only.
    // (Before: one picture per 15-degree turn, 48 pictures, near Hookshot's 64 a module may load, and coarser than
    // 0.11.12; run regress-fps-map-2b12cd6a lost them all at 32 turns.)
    int map_arrow = 0;
    int map_arrow_fill = 0;
    bool MapArrows() const { return map_arrow != 0 && map_arrow_fill != 0; }
    // Every picture the module loads into Hookshot: 5 hearts + the arrow's 2. Hookshot keeps at most 64
    // (hookshot/patches/0001-coop-bridge.patch, image(): images_.size() >= 64 returns 0); LoadLook logs a failed one.
    static constexpr int kPictures = 5 + 2;
    static constexpr int kHookshotPictures = 64;
    static_assert(kPictures <= kHookshotPictures, "the game look's pictures must fit Hookshot's 64");
    bool On() const {
        for (const int h : hearts) {
            if (h == 0) {
                return false;
            }
        }
        return regular != 0 && medium != 0;
    }
};

// The game look's highlight colour (room notices, the list's header): the co-op window's kHighlight.
inline constexpr std::uint32_t kHighlight = Rgba(255, 206, 82, 255);

// A player's colour for names (CoopPuppet::NameColor, 0xRRGGBB) lightened as
// the co-op window's PlayerColour, so it reads on the dark shadow.
constexpr std::uint32_t Lifted(std::uint32_t rgb) {
    const auto lift = [](std::uint32_t c) { return static_cast<int>(c + (255 - c) * 35 / 100); };
    return Rgba(lift((rgb >> 16) & 0xFF), lift((rgb >> 8) & 0xFF), lift(rgb & 0xFF), 255);
}

struct ChatShown {
    std::string text;
    bool notice = false;
    float alpha = 1.0f;
};

// The chat lines still showing, oldest first, with their fade. While the
// chat box is open every kept line shows, unfaded (as the co-op window).
std::vector<ChatShown> ShownChat(const std::vector<CoopHook::ChatLine>& lines, bool box_open = false);

// The chat box, when it is open.
struct ChatBoxView {
    std::string text;
    bool failed = false;  // the last Enter was refused (not connected, or text the protocol refuses)
};

// What the box shows: the text with a caret, the start cut ("...") to fit
// `room` pixels; the placeholder when empty, in the player's language.
inline constexpr const char* kChatPlaceholder = "Say something (Enter sends, Esc closes)";
inline std::string ChatPlaceholder() { return CoopI18n::Tr(kChatPlaceholder); }

// The item cheats panel, when it is open.
struct CheatsView {
    std::string filter;
    std::vector<std::string> rows;  // labels
    std::size_t selected = 0;
    std::string result;             // CoopCheats::LastResult
};

// Where the cheats panel is on a `width` x `height` picture, as DrawCheats
// draws it: its rectangle and its rows (row i's text at rows_top + i * line).
struct CheatsBox {
    float left = 0, top = 0, right = 0, bottom = 0;
    float rows_top = 0, line = 0, px = 0;
};
CheatsBox CheatsAt(float width, float height, const CheatsView& c);
// The row under (x, y), for the mouse; nothing outside the rows.
std::optional<std::size_t> CheatsRowAt(float width, float height, const CheatsView& c, float x, float y);

// A line on the title screen, bottom left, while it is up (coop_title.cpp):
// the "(-) Multiplayer" prompt (Minus opens the menu's Multiplayer tab), or
// the host's save loading without the button.
struct TitleView {
    std::string prompt;   // the words; "" for none
    bool button = false;  // the Minus button before them
};
// Where DrawTitle puts a title line on a `width` x `height` window: the Minus
// button's centre and radius (when it has one) and the words' top left, the
// words' capitals centred on the button (MidTop with the font's metrics).
struct TitleBox {
    float cx = 0, cy = 0, r = 0;
    float text_x = 0, text_top = 0, px = 0;
};
TitleBox TitleAt(float width, float height, const TitleView& t, const FontMetrics& m);
inline constexpr const char* kMinusText = "(-)";   // the Minus button, as words (no drawn button)

// The other players on the HUD minimap and on the pause menu's Map page
// (docs/overlay.md, "Players on the map"; coop/overlay/map_icons.hpp): our
// Link's place, the game's minimap panes, our place's map and the others with their
// colours (Lifted) and names. A player on another floor of a dungeon is
// paler, "(1 below)" / "(2 above)" after the name.
struct MapShown {
    std::string scene;
    std::array<float, 3> self{};
    const coop::overlay::map::Capture* capture = nullptr;  // our place's map (the table's: it outlives the scene)
    std::vector<coop::overlay::map::Player> players;
    std::vector<std::string> names;  // players[i]'s; empty: no names (the setting)
    // The game's minimap panes as it placed them this frame (CoopMapPage::Minimap), or none: the game
    // shows no minimap now (none placed, a game menu in front, no camera). Icons only with our `capture`.
    std::optional<coop::overlay::map::MinimapPanes> minimap;
    std::optional<coop::overlay::map::Box> page;  // the Map page's map picture, in the layout's units
    int page_floor = 0;              // the floor the page shows, as floor_of counts them
    int minimap_floor = 0;           // the floor the minimap shows (ours), as floor_of counts them
};

// The game's HUD layout (MapMenu_00) is 3840 x 2160 and fits the picture
// keeping its aspect, as the game's picture does (DrawMarkers): a box in the
// layout's units (x right, y down from its top left) on a picture of
// `width` x `height`.
inline constexpr float kLayoutW = coop::overlay::map::kLayoutW, kLayoutH = coop::overlay::map::kLayoutH;
coop::overlay::map::Box LayoutToScreen(const coop::overlay::map::Box& layout, float width, float height);
// The words after a name for a player `delta` floors from the one shown
// ("" on the same floor): floors count down as floor_of's do.
std::string FloorWords(int delta);
// The map arrow's turn, radians clockwise on screen, for a facing `angle` on the map (map::Icon::angle: radians,
// 0 right, pi/2 down): the game's arrow points up unturned, so a facing to the right is a quarter turn.
float MapArrowTurn(float angle);
// The voice state after a name as words (" (talking)", " (muted)", "" quiet): the game's own heart lines'
// (coop_party_hud.h).
std::string VoiceWords(CoopVoice::Icon icon);
// The others on the maps for the game's own map icons (coop_map_icons.h): where DrawMap puts them, in the layout's
// units (the picture as the layout: 3840 x 2160), with their names (and floor words) and our own place.
CoopMapIcons::Frame MapMarks(const MapShown& m);

// What to draw this present.
struct Scene {
    std::optional<TitleView> title;
    std::optional<CheatsView> cheats;
    std::optional<ChatBoxView> box;
    std::vector<ChatShown> chat;
    std::optional<CoopHook::Markers> markers;
    std::optional<std::vector<CoopHook::PlayerRow>> players;  // the player list key is held
    std::optional<std::vector<CoopHook::OverlayLine>> panel;  // the panel is on
    std::optional<CoopDbno::View> dbno;  // down but not out: we are down, or beside someone who is
    const MapShown* map = nullptr;        // players on the maps (the module's, kept between presents)
    // The game look: players' colours (Lifted, alpha 255) by what tags and
    // chat lines call them: their names, "P1".."P4", "you". Nobody here: white.
    std::map<std::string, std::uint32_t, std::less<>> colours;
};

// Anything to draw.
bool Wanted(const Scene& scene);

// Draws the markers (scaled from the game's scene target to the picture),
// the HUD blocks, the panel at the top left, the chat lines at the bottom
// left, then the player list over the top middle. The game look when `look`
// is On and the table has hos_coop_draw2's calls; else the plain look (and no
// HUD blocks, as the co-op window).
void Draw(const HosCoopDraw& d, const Scene& scene, const Look& look = {});

// Whether Draw would use the game look with this table.
bool GameLook(const HosCoopDraw& d, const Look& look);

// The player list's cells: a header row, then one row per player (as the
// co-op window's: "P1", the name with " (you)", state, ping, scene; the
// game look calls the last column "Place", its rows carrying place names).
std::vector<std::vector<std::string>> PlayerCells(const std::vector<CoopHook::PlayerRow>& rows,
                                                  bool place = false);

// The F6 list's size: its text size, each column's width, the gap between columns, a row's height, and the whole
// list's width and height. PlayerListAt starts at `px` and makes it smaller until the list (`rows` rows, a row
// `line_k` times the text size, `measure(px)` its columns' widths) fits in the picture below `top`, with a margin;
// never under kListLeastPx (a list that cannot fit at that size is as small as it gets).
inline constexpr float kListLeastPx = 10.0f;
struct PlayerListSize {
    float px = 0;
    std::vector<float> widths;
    float gap = 0, line = 0, width = 0, height = 0;
};
PlayerListSize PlayerListAt(float width, float height, float top, std::size_t rows, float px, float line_k,
                            const std::function<std::vector<float>(float)>& measure);

// The keys (docs/hookshot.md): F6 opens the game's - menu on the Players view (list_press, once as F6 goes down;
// coop_players_open.h), or, without our - tabs, holding it shows the drawn list (list); F7 turns the panel on and
// off (developer builds). Tab, an emulator key, is the game's Plus on Hookshot (Enter too before Longshot v3.2.0).
// Update turns the panel over when F7 goes down.
struct Keys {
    bool list = false;
    bool list_press = false;   // F6 went down at this Update
    bool list_was_down = false;
    bool panel = false;
    bool panel_was_down = false;
    void Update(bool list_down, bool panel_down) {
        list = list_down;
        list_press = list_down && !list_was_down;
        list_was_down = list_down;
        if (panel_down && !panel_was_down) {
            panel = !panel;
        }
        panel_was_down = panel_down;
    }
};

}  // namespace CoopDraw
