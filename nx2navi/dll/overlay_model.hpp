// SPDX-License-Identifier: GPL-3.0-or-later
// What the in-game overlay shows, as plain data: the co-op panel's lines
// (connection, you, one line per other player), the notices that fade, the
// chat history and the chat box's text. The DLL fills a Snapshot on its frame
// thread and the notices on its network thread; the overlay callback turns
// them into lines with Panel() and draws them (overlay.cpp). No Windows code
// and no game code: tested on its own (tests/nx2navi_test.cpp).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CoopNx2navi::Ui {

// A notice is shown for kNoticeMs, the last kNoticeFadeMs of it fading out.
inline constexpr std::uint64_t kNoticeMs = 6000;
inline constexpr std::uint64_t kNoticeFadeMs = 1500;
inline constexpr std::size_t kNoticesKept = 6;
// Chat lines kept for the history shown while the chat box is open.
inline constexpr std::size_t kHistoryKept = 8;
// A player who sent no sample for this long is not in the game (the title
// screen, a load or a menu: a sender sends at least 4 samples a second while
// its player can be read, and none otherwise; coop/host/src/sampler.cpp).
inline constexpr std::uint64_t kStaleUs = 2'000'000;

enum class Link : std::uint8_t { connecting, connected, lost, refused };

using Vec3 = std::array<float, 3>;

// Ourselves, from this frame's PlayerReader::read().
struct Local {
    bool game_found = false;  // main of the right build is mapped
    bool in_game = false;     // the player could be read this frame
    bool event = false;       // a cutscene or talk runs here (no minimap markers then)
    bool child = false;       // our Link is the child (the others in our scene are the same age)
    std::optional<std::string> scene;
    Vec3 position{};
    int max_life = 0;         // our own most life in quarter hearts (0 not read): two rows of hearts above 40
};

// Where the other players' block starts, as a share of the game picture's height: right under the
// game's own hearts, magic bar and item row, which end at about 0.27 (one row of hearts) and one
// heart row lower when our Link has more than ten hearts.
inline float PeerBlockTop(int own_max_life) { return own_max_life > 40 ? 0.335f : 0.29f; }

// Another player, from the client's room and from presence.
struct Peer {
    std::uint8_t number = 0;
    std::string name;
    bool known = false;  // presence has the player (PLAYER_JOINED or WELCOME)
    bool fresh = false;  // a sample came within kStaleUs
    std::optional<std::string> scene;
    std::optional<Vec3> position;  // where presence shows the player now
    // Their hearts in quarter hearts, from their last PLAYER_LIFE (max 0: none came yet), and how long
    // ago they last lost some, in ms (a large number: never): their name tag is red for a moment then.
    int life = 0;
    int max_life = 0;
    std::uint64_t hurt_ago_ms = ~std::uint64_t{0};
    // Their magic (the game's PlayerStatus.MP of MPMax), from their PLAYER_EXTRA; max 0: not told.
    int magic = 0;
    int max_magic = 0;
};

// How long a player's name tag stays red after they lost life, and the red's share then (1 at the
// loss, 0 from kHurtFlashMs on).
inline constexpr std::uint64_t kHurtFlashMs = 700;
inline float HurtFlash(std::uint64_t hurt_ago_ms) {
    return hurt_ago_ms >= kHurtFlashMs ? 0.0f
                                       : 1.0f - static_cast<float>(hurt_ago_ms) / static_cast<float>(kHurtFlashMs);
}
// The other players' hearts under our own (the plain look: no picture of the game's): how many
// quarters of heart `index` (0 first) are filled, 0 to 4.
inline int HeartQuarters(int life, int index) {
    const int q = life - 4 * index;
    return q < 0 ? 0 : q > 4 ? 4 : q;
}

// The game's own heart picture drawn for 0 to 4 quarters: the parts of the full heart that show, as
// fractions of the picture (the game's quarter masks are its quadrants: one quarter is the top left,
// two the left half, three everything but the top right). Returns how many rectangles (0 to 2).
struct UvRect {
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
};
inline int HeartFill(int quarters, UvRect out[2]) {
    switch (quarters) {
    case 1: out[0] = {0.0f, 0.0f, 0.5f, 0.5f}; return 1;
    case 2: out[0] = {0.0f, 0.0f, 0.5f, 1.0f}; return 1;
    case 3: out[0] = {0.0f, 0.0f, 0.5f, 1.0f}; out[1] = {0.5f, 0.5f, 1.0f, 1.0f}; return 2;
    case 4: out[0] = {0.0f, 0.0f, 1.0f, 1.0f}; return 1;
    default: return 0;
    }
}
// The filled share of another player's magic bar, 0 to 1 (0 when their game told none).
inline float MagicShare(int magic, int max_magic) {
    if (max_magic <= 0 || magic <= 0) {
        return 0.0f;
    }
    return magic >= max_magic ? 1.0f : static_cast<float>(magic) / static_cast<float>(max_magic);
}

// The panel's own texts: English, or French in a French game (the hook's notices, "Host got the
// map", stay as the hook writes them). Any thread.
void SetFrench(bool on);
bool French();

struct Snapshot {
    Link link = Link::connecting;
    std::string server;   // "host:port"
    std::string refused;  // the reason, when refused
    std::uint8_t self = 0;
    std::string self_name;
    Local local;
    std::vector<Peer> peers;  // lowest number first, ourselves left out
};

// The direction from a position delta, in the world's axes: north is -Z and
// east is +X (an assumption about the game's map, not checked; it is not the
// camera's direction). "" when the horizontal distance is below 0.5 units.
std::string Compass(double dx, double dz);

std::string LinkLine(const Snapshot& snapshot);
std::string LocalLine(const Local& local);
std::string PeerLine(const Peer& peer, const Local& local);

struct Notice {
    std::string text;
    std::uint64_t at_ms = 0;
};
// 1 while new, falling to 0 over the last kNoticeFadeMs; 0 once expired.
float NoticeAlpha(const Notice& notice, std::uint64_t now_ms);
void AddNotice(std::deque<Notice>& notices, std::string text, std::uint64_t now_ms);
void PruneNotices(std::deque<Notice>& notices, std::uint64_t now_ms);
void AddHistory(std::deque<std::string>& history, std::string line);

// UTF-8 helpers for the chat box. Code points, as the protocol counts them.
std::size_t Utf8Count(std::string_view text);
// Appends `codepoint` unless it is a control character, a surrogate, beyond
// U+10FFFF or would make the text longer than `max_codepoints`. True when
// appended.
bool Utf8Append(std::string& text, std::uint32_t codepoint, std::size_t max_codepoints);
// Takes the last code point off.
void Utf8PopBack(std::string& text);

enum class Role : std::uint8_t { title, local, peer, notice, chat, input, hint };
struct Line {
    std::string text;
    Role role = Role::peer;
    float alpha = 1.0f;
    int colour = -1;  // PeerColour of the player the line is about; -1: the role's own colour
};

// The colour of another player on this screen: 0 green, 1 blue, 2 yellow, by
// player number with our own left out, so the up to three others always get
// the three colours (never red: the game's own marker for ourselves is red).
// The same index colours that player's panel line, chat lines and map marker.
// -1 for ourselves or a number outside 1 to 4.
int PeerColour(std::uint8_t self, std::uint8_t number);

// The game's own map transform, as its map screen keeps it (overlay.cpp reads
// it from the game): the world point at the picture's centre and the world
// length the picture's height shows, and the picture's pane in the layout's
// canvas (origin at the centre, +y up, kCanvasW x kCanvasH for the whole
// window).
inline constexpr float kCanvasW = 3840.0f;
inline constexpr float kCanvasH = 2160.0f;
struct MapView {
    float centre_x = 0.0f, centre_z = 0.0f;  // world
    float world_span = 0.0f;                 // world units across the picture
    float pane_x = 0.0f, pane_y = 0.0f;      // the picture's centre, canvas
    float pane_w = 0.0f, pane_h = 0.0f;      // its size, canvas (scale applied)
};
// Where a world position is on the window (pixels, origin top left), north up
// (+X right, +Z down). Nothing when the view is not sane (a value not finite,
// no span, no size) or the point lies outside the picture: a marker is never
// drawn from a transform that is not known.
std::optional<std::array<float, 2>> MapPoint(const MapView& view, float world_x, float world_z,
                                             float window_w, float window_h);

// A name tag over another player's head (README, "Name tags"): where its text
// goes, given the point over the head on the window (from the game's camera),
// the text's size and the game picture's rectangle in the window. Centred on
// the point and above it, kept inside the picture at the sides and the top.
// Nothing for a point outside the picture (the player is off screen: a tag is
// never pinned to the edge) or sizes that are not sane.
struct TagBox {
    float x = 0.0f, y = 0.0f;  // the text's top left corner
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;  // the dark plate behind it
};
std::optional<TagBox> NameTagBox(float point_x, float point_y, float text_w, float text_h, float pad,
                                 float picture_x, float picture_y, float picture_w, float picture_h);
// How far over a player's position the tag's point is, in the game's units
// (metres): over the head of an adult Link, lower for a child.
inline constexpr float kTagHeightAdult = 1.9f;
inline constexpr float kTagHeightChild = 1.25f;
// How strongly a tag is drawn for a player `distance` units away (0 to 1):
// full up to kTagNear, fading to kTagFarAlpha at kTagFar and staying there (a
// far player's tag is still the only mark of where they are). 0 for a
// distance that is not a number.
inline constexpr float kTagNear = 10.0f;
inline constexpr float kTagFar = 40.0f;
inline constexpr float kTagFarAlpha = 0.3f;
float TagAlpha(float distance);

// The chat box: open or not, its text, the history it shows.
struct ChatView {
    bool open = false;
    std::string input;
    std::deque<std::string> history;
};

// Every line of the panel, top to bottom.
std::vector<Line> Panel(const Snapshot& snapshot, const std::deque<Notice>& notices,
                        const ChatView& chat, std::uint64_t now_ms);

}  // namespace CoopNx2navi::Ui
