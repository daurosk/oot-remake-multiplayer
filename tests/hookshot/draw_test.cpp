// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op over Hookshot's picture (hookshot/src/coop_draw.h): what is drawn,
// through a canvas that records the calls.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "coop_dbno_draw.h"
#include "coop_draw.h"
#include "coop_hud_layout.h"
#include "coop_map_page.h"

namespace {

struct Call {
    std::string kind;
    float x = 0, y = 0, px = 0;  // an image: px is its width
    std::uint32_t rgba = 0;
    std::string text;            // an image: its id
    int font = 0;                // a text: the font it was drawn with
};

struct Canvas {
    std::vector<Call> calls;
    int font = 0;
    HosCoopDraw d{};

    Canvas(float w, float h) {
        d.width = w;
        d.height = h;
        d.user = this;
        d.rect = [](void* u, float x0, float y0, float, float, std::uint32_t rgba, float) {
            static_cast<Canvas*>(u)->calls.push_back({"rect", x0, y0, 0, rgba, {}});
        };
        d.circle = [](void* u, float x, float y, float r, std::uint32_t rgba, float) {
            static_cast<Canvas*>(u)->calls.push_back({"circle", x, y, r, rgba, {}});
        };
        d.triangle = [](void* u, float x0, float y0, float, float, float, float, std::uint32_t rgba) {
            static_cast<Canvas*>(u)->calls.push_back({"triangle", x0, y0, 0, rgba, {}});
        };
        d.text = [](void* u, float x, float y, float px, std::uint32_t rgba, const char* s) {
            auto* c = static_cast<Canvas*>(u);
            c->calls.push_back({"text", x, y, px, rgba, s, c->font});
        };
        d.text_width = [](void*, float px, const char* s) {
            return static_cast<float>(std::string(s).size()) * px * 0.5f;  // half the size per byte
        };
        // hos_coop_draw2's calls (the game look).
        d.font = [](void*, const char*) { return 0; };
        d.use_font = [](void* u, int id) { static_cast<Canvas*>(u)->font = id; };
        d.image = [](void*, int, int, const std::uint8_t*) { return 0; };
        d.draw_image = [](void* u, int id, float x0, float y0, float x1, float, std::uint32_t tint) {
            static_cast<Canvas*>(u)->calls.push_back({"image", x0, y0, x1 - x0, tint, std::to_string(id)});
        };
    }

    std::vector<Call> Of(const std::string& kind) const {
        std::vector<Call> out;
        for (const auto& c : calls) {
            if (c.kind == kind) out.push_back(c);
        }
        return out;
    }
};

int AlphaOf(std::uint32_t rgba) {
    return static_cast<int>(rgba >> 24);
}

// The game look's ids, as Hookshot's calls would give them: fonts 1 and 2, hearts 11 (empty) to 15 (full).
CoopDraw::Look GameLook() {
    CoopDraw::Look l;
    l.regular = 1;
    l.medium = 2;
    l.hearts = {11, 12, 13, 14, 15};
    return l;
}

}  // namespace

TEST_CASE("hookshot draw: IM_COL32 layout") {
    CHECK(CoopDraw::Rgba(0x11, 0x22, 0x33, 0x44) == 0x44332211u);
}

TEST_CASE("hookshot draw: chat lines show 8 s, fade in 1 s, then go") {
    using CoopHook::ChatLine;
    const auto shown = CoopDraw::ShownChat({ChatLine{"Saria: hi", false, 100}, ChatLine{"Mido joined", true, 8500},
                                            ChatLine{"old", false, 9000}, ChatLine{"older", false, 20000}});
    REQUIRE(shown.size() == 2);
    CHECK(shown[0].text == "Saria: hi");
    CHECK(shown[0].alpha == doctest::Approx(1.0f));
    CHECK(shown[1].notice);
    CHECK(shown[1].alpha == doctest::Approx(0.5f));
    CoopDraw::Scene s;
    CHECK_FALSE(CoopDraw::Wanted(s));
    s.markers = CoopHook::Markers{640, 360, {}};
    CHECK_FALSE(CoopDraw::Wanted(s));
    s.chat = shown;
    CHECK(CoopDraw::Wanted(s));
}

TEST_CASE("hookshot draw: chat at the bottom left, oldest first, long lines cut") {
    Canvas c(1280, 720);
    CoopDraw::Scene s;
    s.chat = {{"Saria: hi", false, 1.0f}, {"Mido joined", true, 0.5f}, {std::string(300, 'x'), false, 1.0f}};
    CoopDraw::Draw(c.d, s);
    const auto texts = c.Of("text");
    REQUIRE(texts.size() == 3);
    CHECK(texts[0].text == "Saria: hi");
    CHECK(texts[0].x == doctest::Approx(16.0f));
    CHECK(texts[0].y < texts[1].y);
    CHECK(texts[2].y + texts[2].px < 720.0f - 16.0f + 1.0f);  // above the bottom margin
    CHECK(AlphaOf(texts[1].rgba) == 127);                     // the notice, half faded
    CHECK(texts[1].rgba != texts[0].rgba);                    // notices in the highlight colour
    CHECK(texts[2].text.size() < 300);
    CHECK(texts[2].text.substr(texts[2].text.size() - 3) == "...");
    CHECK(c.Of("rect").size() == 3);  // a dark box behind each
}

TEST_CASE("hookshot draw: name tags scaled from the scene target to the picture") {
    // The scene target is 640x360, the window 1280x720: twice the size.
    CoopHook::Markers m{640, 360, {}};
    CoopHook::Marker head;
    head.x = 320;
    head.y = 180;
    head.tag = "Saria";
    head.tag_x = 320;
    head.tag_y = 150;
    head.tag_head = true;
    CoopHook::Marker dot = head;
    dot.tag_head = false;
    dot.size = 10;
    dot.tag = "";
    CoopHook::Marker edge = head;
    edge.edge = true;
    edge.tag = "Mido";
    CoopHook::Marker gone = head;
    gone.alpha = 0;
    m.items = {head, dot, edge, gone};
    Canvas c(1280, 720);
    CoopDraw::Scene s;
    s.markers = m;
    CoopDraw::Draw(c.d, s);
    CHECK(c.Of("triangle").size() == 1);  // the edge arrow
    CHECK(c.Of("circle").size() == 2);    // the dot and its dark rim (no dot for a head tag)
    const auto texts = c.Of("text");
    REQUIRE(texts.size() == 18);  // two tags, each 8 outline copies + the fill
    const auto& fill = texts[8];
    CHECK(fill.text == "Saria");
    CHECK(fill.px == doctest::Approx(25.2f));  // 3.5% of 720
    // Centred on tag_x * 2, its bottom at tag_y * 2.
    CHECK(fill.x + 5 * fill.px * 0.5f / 2 == doctest::Approx(640.0f));
    CHECK(fill.y + fill.px == doctest::Approx(300.0f));
}

TEST_CASE("hookshot draw: a table without every function draws nothing") {
    Canvas c(1280, 720);
    c.d.triangle = nullptr;
    CoopDraw::Scene s;
    s.chat = {{"Saria: hi", false, 1.0f}};
    CoopDraw::Draw(c.d, s);
    CHECK(c.calls.empty());
}

TEST_CASE("hookshot draw: the player list while F6 is held, as the co-op window's") {
    CoopHook::PlayerRow me;
    me.number = 1;
    me.name = "Saria";
    me.self = true;
    me.state = "connected";
    me.ms = 42;
    me.scene = "KokiriForest_Child";
    CoopHook::PlayerRow other;
    other.number = 2;
    other.name = "Mido";
    other.state = "reconnecting";
    other.ms = 120;
    const auto cells = CoopDraw::PlayerCells({me, other});
    REQUIRE(cells.size() == 3);
    CHECK(cells[0] == std::vector<std::string>{"#", "Player", "State", "Ping", "Scene"});
    CHECK(cells[1] == std::vector<std::string>{"P1", "Saria (you)", "connected", "42 ms", "KokiriForest_Child"});
    CHECK(cells[2] == std::vector<std::string>{"P2", "Mido", "reconnecting", "~120 ms", "?"});

    Canvas c(1280, 720);
    CoopDraw::Scene s;
    s.players = std::vector<CoopHook::PlayerRow>{me, other};
    CHECK(CoopDraw::Wanted(s));
    CoopDraw::Draw(c.d, s);
    const auto texts = c.Of("text");
    REQUIRE(texts.size() == 15);
    CHECK(texts[0].text == "#");
    CHECK(texts[0].y > 720.0f / 6);              // below a sixth of the height
    CHECK(texts[10].rgba != texts[5].rgba);     // the reconnecting row is dim
    CHECK(c.Of("rect").size() == 1);
}

TEST_CASE("hookshot draw: the panel at the top left, and F7 turns it over on each press") {
    Canvas c(1280, 720);
    CoopDraw::Scene s;
    s.panel = std::vector<CoopHook::OverlayLine>{{"coop: connected", false}, {"P2 Mido: other scene", true}};
    CoopDraw::Draw(c.d, s);
    const auto texts = c.Of("text");
    REQUIRE(texts.size() == 2);
    CHECK(texts[0].x < 40.0f);
    CHECK(texts[0].y < 40.0f);
    CHECK(texts[1].rgba != texts[0].rgba);  // dim
    CoopDraw::Keys k;
    k.Update(false, true);
    CHECK(k.panel);
    k.Update(false, true);  // still held: no change
    CHECK(k.panel);
    k.Update(true, false);
    CHECK(k.list);
    CHECK(k.panel);
    k.Update(false, true);
    CHECK_FALSE(k.panel);
    CHECK_FALSE(k.list);
}

TEST_CASE("hookshot draw: the other players under your hearts in the game look only, with the heart pictures") {
    CoopDraw::Scene s;
    s.hud = {{"Mido", "Kokiri Forest", {4, 4, 2, 0}, false}, {"Saria", "", {}, true}};
    CHECK(CoopDraw::Wanted(s));
    Canvas plain(1280, 720);
    CoopDraw::Draw(plain.d, s);
    CHECK(plain.calls.empty());  // the plain look has no HUD blocks (as the co-op window)

    Canvas c(1280, 720);
    CoopDraw::Draw(c.d, s, GameLook());
    const auto texts = c.Of("text");
    REQUIRE(texts.size() == 18);  // three lines, each the game's rim (4), shadow and fill
    const auto& mido = texts[5];
    CHECK(mido.text == "Mido");
    CHECK(mido.font == 2);                                      // the medium font
    CHECK(mido.x == doctest::Approx(40.0f));                    // 40 of 720 lines at 720 high
    CHECK(mido.y == doctest::Approx(CoopHudLayout::BlocksTop(1280, 720)));
    CHECK(AlphaOf(texts[4].rgba) == 150);                       // the drop shadow
    CHECK(texts[4].x > mido.x);
    CHECK(texts[11].text == "Kokiri Forest");
    CHECK(texts[11].font == 1);                                 // the regular font
    CHECK(texts[11].y > mido.y);
    CHECK(texts[17].text == "Saria");
    CHECK(AlphaOf(texts[17].rgba) == 150);                      // not connected: dim
    // Four hearts as the pictures for 4, 4, 2 and 0 quarters, 38 of 720 lines big, 0.55 of that apart.
    const auto hearts = c.Of("image");
    REQUIRE(hearts.size() == 4);
    CHECK(hearts[0].text == "15");
    CHECK(hearts[1].text == "15");
    CHECK(hearts[2].text == "13");
    CHECK(hearts[3].text == "11");
    CHECK(hearts[0].px == doctest::Approx(38.0f));
    CHECK(hearts[1].x - hearts[0].x == doctest::Approx(38.0f * 0.55f));
    CHECK(hearts[0].rgba == CoopDraw::Rgba(255, 255, 255, 255));
    CHECK(c.Of("rect").empty());
    CHECK(c.font == 0);  // Hookshot's own font again after drawing
}

TEST_CASE("hookshot draw: game look name tags in the players' colours, over the game's shadow") {
    CoopHook::Markers m{640, 360, {}};
    CoopHook::Marker other;
    other.x = 320;
    other.y = 180;
    other.tag = "Saria";
    other.tag_x = 320;
    other.tag_y = 150;
    other.tag_head = true;
    CoopHook::Marker self = other;
    self.self = true;
    self.tag = "you";
    CoopHook::Marker edge = other;
    edge.edge = true;
    edge.tag = "P3";
    m.items = {other, self, edge};
    CoopDraw::Scene s;
    s.markers = m;
    s.colours["Saria"] = CoopDraw::Lifted(0xFF0000);
    CHECK(CoopDraw::Lifted(0xFF0000) == CoopDraw::Rgba(255, 89, 89, 255));  // 35% of the way to white
    Canvas c(1280, 720);
    CoopDraw::Draw(c.d, s, GameLook());
    const auto texts = c.Of("text");
    REQUIRE(texts.size() == 18);
    CHECK(texts[5].text == "Saria");
    CHECK(texts[5].rgba == CoopDraw::Lifted(0xFF0000));
    CHECK(texts[5].font == 2);
    CHECK(texts[5].px == doctest::Approx(25.2f * 1.1f));  // the game's font a little bigger
    CHECK(texts[11].rgba == CoopDraw::Rgba(255, 210, 60, 255));  // yourself
    CHECK(texts[17].text == "P3");
    CHECK(texts[17].rgba == CoopDraw::Rgba(255, 255, 255, 255));  // nobody known: white
    // The edge arrow in that player's colour too.
    const auto arrows = c.Of("triangle");
    REQUIRE(arrows.size() == 1);
    CHECK(arrows[0].rgba == CoopDraw::Rgba(255, 255, 255, 255));
}

TEST_CASE("hookshot draw: game look chat, the name in its colour, no boxes, the box without a box") {
    CoopDraw::Scene s;
    s.chat = {{"Saria: hi", false, 1.0f}, {"Mido joined", true, 0.5f}};
    s.colours["Saria"] = CoopDraw::Lifted(0x00FF00);
    Canvas c(1280, 720);
    CoopDraw::Draw(c.d, s, GameLook());
    auto texts = c.Of("text");
    REQUIRE(texts.size() == 18);  // "Saria:", " hi", "Mido joined"
    CHECK(texts[5].text == "Saria:");
    CHECK(texts[5].rgba == CoopDraw::Lifted(0x00FF00));
    CHECK(texts[5].font == 1);
    CHECK(texts[5].x == doctest::Approx(40.0f));  // the HUD blocks' left edge (32) + the pad
    CHECK(texts[11].text == " hi");
    CHECK(texts[11].x > texts[5].x);
    CHECK(texts[11].rgba == CoopDraw::Rgba(255, 255, 255, 255));
    CHECK(texts[17].rgba == CoopDraw::Rgba(255, 206, 82, 127));  // a notice: the highlight colour, half faded
    CHECK(texts[17].px == doctest::Approx(23.0f));
    CHECK(c.Of("rect").empty());

    Canvas b(1280, 720);
    s.box = CoopDraw::ChatBoxView{"hello", true};
    CoopDraw::Draw(b.d, s, GameLook());
    texts = b.Of("text");
    REQUIRE(texts.size() == 24);
    CHECK(texts[5].text == "hello_");                           // the box first
    CHECK(texts[5].rgba == CoopDraw::Rgba(255, 122, 106, 255));  // refused: red text, no red frame
    CHECK(texts[23].y < texts[5].y);                            // the lines above it
    CHECK(b.Of("rect").empty());
}

TEST_CASE("hookshot draw: game look player list, no box, Place, the header highlighted") {
    CoopHook::PlayerRow me;
    me.number = 1;
    me.name = "Saria";
    me.self = true;
    me.state = "connected";
    me.scene = "Kokiri Forest";
    const auto cells = CoopDraw::PlayerCells({me}, true);
    CHECK(cells[0][4] == "Place");
    CHECK(cells[1][4] == "Kokiri Forest");
    CoopDraw::Scene s;
    s.players = std::vector<CoopHook::PlayerRow>{me};
    Canvas c(1280, 720);
    CoopDraw::Draw(c.d, s, GameLook());
    const auto texts = c.Of("text");
    REQUIRE(texts.size() == 60);  // ten cells, each with the game's shadow
    CHECK(texts[5].text == "#");
    CHECK(texts[5].rgba == CoopDraw::kHighlight);
    CHECK(texts[5].px == doctest::Approx(24.0f));
    CHECK(c.Of("rect").empty());
}

TEST_CASE("hookshot draw: no game look without every piece, or with an older Hookshot's table") {
    const auto look = GameLook();
    Canvas c(1280, 720);
    CHECK(CoopDraw::GameLook(c.d, look));
    CoopDraw::Look missing = look;
    missing.hearts[2] = 0;  // a heart picture that did not load
    CHECK_FALSE(CoopDraw::GameLook(c.d, missing));
    CHECK_FALSE(CoopDraw::GameLook(c.d, CoopDraw::Look{}));
    Canvas old(1280, 720);
    old.d.font = nullptr;
    old.d.use_font = nullptr;
    old.d.image = nullptr;
    old.d.draw_image = nullptr;
    CHECK_FALSE(CoopDraw::GameLook(old.d, look));
    CoopDraw::Scene s;
    s.chat = {{"Saria: hi", false, 1.0f}};
    CoopDraw::Draw(old.d, s, look);
    CHECK(old.Of("rect").size() == 1);  // the plain look's box
    CHECK(old.Of("text").size() == 1);
}

TEST_CASE("hookshot draw: the chat box at the bottom left, the lines above it and unfaded") {
    using CoopHook::ChatLine;
    const auto all = CoopDraw::ShownChat({ChatLine{"old", false, 30000}, ChatLine{"new", false, 100}}, true);
    REQUIRE(all.size() == 2);  // the box is open: every kept line
    CHECK(all[0].alpha == doctest::Approx(1.0f));

    Canvas c(1280, 720);
    CoopDraw::Scene s;
    s.box = CoopDraw::ChatBoxView{"", false};
    CHECK(CoopDraw::Wanted(s));
    CoopDraw::Draw(c.d, s);
    auto texts = c.Of("text");
    REQUIRE(texts.size() == 1);
    CHECK(texts[0].text == CoopDraw::kChatPlaceholder);
    CHECK(AlphaOf(texts[0].rgba) == 160);

    Canvas t(1280, 720);
    s.box = CoopDraw::ChatBoxView{"hello", true};
    s.chat = all;
    CoopDraw::Draw(t.d, s);
    texts = t.Of("text");
    REQUIRE(texts.size() == 3);
    CHECK(texts[0].text == "hello_");               // the box (drawn first), with its caret
    CHECK(texts[2].text == "new");
    CHECK(texts[2].y + texts[2].px < texts[0].y);    // the lines above it
    CHECK(t.Of("rect").size() == 2 + 2);             // refused: a red frame behind the box, + two line boxes

    Canvas l(1280, 720);
    s.box = CoopDraw::ChatBoxView{std::string(300, 'y'), false};
    s.chat.clear();
    CoopDraw::Draw(l.d, s);
    const auto tail = l.Of("text")[0].text;
    CHECK(tail.substr(0, 3) == "...");               // the end shows, where the typing is
    CHECK(tail.substr(tail.size() - 2) == "y_");
}

TEST_CASE("hookshot draw: the item cheats panel at the top right, the chosen row marked") {
    Canvas c(1280, 720);
    CoopDraw::Scene s;
    CoopDraw::CheatsView v;
    v.filter = "boom";
    v.rows = {"IsGet.Item_Boomerang", "PouchData.Item_Boomerang.StockNum"};
    v.selected = 1;
    v.result = "gave PouchData.Item_Boomerang.StockNum";
    s.cheats = v;
    CHECK(CoopDraw::Wanted(s));
    CoopDraw::Draw(c.d, s);
    const auto texts = c.Of("text");
    REQUIRE(texts.size() == 6);  // title, find line, heading, two rows, result
    CHECK(texts[1].text == "Find: boom_");
    CHECK(texts[3].text == "IsGet.Item_Boomerang");
    CHECK(texts[0].x > 1280.0f - 16.0f - 560.0f * 15.84f / 14.0f);  // right-aligned at the margin
    CHECK(texts[0].x < 1280.0f - 600.0f);
    const auto rects = c.Of("rect");
    REQUIRE(rects.size() == 2);  // the panel and the chosen row
    CHECK(rects[1].y > texts[3].y);  // the second row is the chosen one
}

TEST_CASE("hookshot draw: the mouse finds the cheats panel's rows where they are drawn") {
    CoopDraw::CheatsView v;
    v.rows = {"IsGet.Item_Boomerang", "PouchData.Item_Boomerang.StockNum", "PlayerStatus.MaxLife"};
    v.selected = 0;
    Canvas c(1280, 720);
    CoopDraw::Scene s;
    s.cheats = v;
    CoopDraw::Draw(c.d, s);
    const auto texts = c.Of("text");
    REQUIRE(texts.size() == 6);  // title, find line, heading, three rows
    const auto box = CoopDraw::CheatsAt(1280, 720, v);
    CHECK(c.Of("rect")[0].x == doctest::Approx(box.left));
    for (std::size_t i = 0; i < 3; ++i) {
        const auto& row = texts[3 + i];
        CHECK(row.y == doctest::Approx(box.rows_top + static_cast<float>(i) * box.line));
        const auto hit = CoopDraw::CheatsRowAt(1280, 720, v, row.x + 10, row.y + row.px / 2);
        REQUIRE(hit);
        CHECK(*hit == i);
    }
    CHECK_FALSE(CoopDraw::CheatsRowAt(1280, 720, v, box.left - 5, texts[3].y + 5));  // left of the panel
    CHECK_FALSE(CoopDraw::CheatsRowAt(1280, 720, v, texts[3].x, texts[0].y + 5));   // the title
    CHECK_FALSE(CoopDraw::CheatsRowAt(1280, 720, v, texts[3].x, texts[5].y + box.line + 5));  // below the last row
}

TEST_CASE("hookshot draw: a title screen line, with or without the Minus button") {
    {
        Canvas c(1280, 720);
        CoopDraw::Scene s;
        s.title = CoopDraw::TitleView{"Loading Saria's save..."};   // (the host's save loading: no button)
        CHECK(CoopDraw::Wanted(s));
        CoopDraw::Draw(c.d, s, GameLook());
        bool said = false;
        for (const auto& t : c.Of("text")) {
            said |= t.text == "Loading Saria's save...";
        }
        CHECK(said);
        CHECK(c.Of("circle").empty());   // no button: nothing to press
    }
    {
        Canvas c(1280, 720);
        CoopDraw::Scene s;
        s.title = CoopDraw::TitleView{"Multiplayer", true};
        CoopDraw::Draw(c.d, s, GameLook());
        CHECK(c.Of("circle").size() == 2);   // the Minus button
    }
    CoopDraw::Scene none;
    none.title = CoopDraw::TitleView{};
    CHECK_FALSE(CoopDraw::Wanted(none));
}

namespace {

// A font file with only the tables MetricsOf reads: hhea (ascender, descender), head (unitsPerEm) and OS/2 (version,
// sCapHeight); os2_version 0: no OS/2 table.
std::vector<std::uint8_t> FakeFont(int ascender, int descender, int em, int os2_version, int cap) {
    std::vector<std::uint8_t> b;
    auto put = [&](std::uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i) {
            b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    };
    const int tables = os2_version ? 3 : 2;
    put(0x4F54544Fu, 4), put(static_cast<std::uint32_t>(tables), 2), put(0, 6);
    const std::uint32_t dir_end = 12 + 16 * static_cast<std::uint32_t>(tables);
    put(0x68686561u, 4), put(0, 4), put(dir_end, 4), put(36, 4);             // hhea
    put(0x68656164u, 4), put(0, 4), put(dir_end + 36, 4), put(54, 4);        // head
    if (os2_version) {
        put(0x4F532F32u, 4), put(0, 4), put(dir_end + 90, 4), put(96, 4);   // OS/2
    }
    std::vector<std::uint8_t> hhea(36), head(54), os2(96);
    auto set = [](std::vector<std::uint8_t>& t, std::size_t at, int v) {
        t[at] = static_cast<std::uint8_t>((v >> 8) & 0xFF), t[at + 1] = static_cast<std::uint8_t>(v & 0xFF);
    };
    set(hhea, 4, ascender), set(hhea, 6, descender);
    set(head, 18, em);
    set(os2, 0, os2_version), set(os2, 88, cap);
    b.insert(b.end(), hhea.begin(), hhea.end());
    b.insert(b.end(), head.begin(), head.end());
    if (os2_version) {
        b.insert(b.end(), os2.begin(), os2.end());
    }
    return b;
}

}  // namespace

TEST_CASE("hookshot draw: a font's metrics from its hhea, head and OS/2 tables") {
    const auto m = CoopDraw::MetricsOf(FakeFont(880, -120, 1000, 4, 700));
    CHECK(m.ascent == doctest::Approx(0.88f));
    CHECK(m.cap == doctest::Approx(0.70f));
    const auto no_cap = CoopDraw::MetricsOf(FakeFont(800, -200, 1000, 1, 0));   // OS/2 version 1: no sCapHeight
    CHECK(no_cap.ascent == doctest::Approx(0.8f));
    CHECK(no_cap.cap == doctest::Approx(0.7f));   // Longshot's 0.7 em
    const auto no_os2 = CoopDraw::MetricsOf(FakeFont(1900, -500, 2048, 0, 0));
    CHECK(no_os2.ascent == doctest::Approx(1900.0f / 2400.0f));
    CHECK(no_os2.cap == doctest::Approx(0.7f * 2048 / 2400.0f).epsilon(0.01));
    const CoopDraw::FontMetrics plain;
    for (const auto& junk : {std::vector<std::uint8_t>{}, std::vector<std::uint8_t>(40, 0xFF), FakeFont(0, 0, 1000, 4, 700)}) {
        const auto j = CoopDraw::MetricsOf(junk);
        CHECK(j.ascent == plain.ascent);
        CHECK(j.cap == plain.cap);
    }
}

TEST_CASE("hookshot draw: the title's Minus button and its word share a middle at every window shape") {
    const CoopDraw::TitleView t{"Multiplayer", true};
    const CoopDraw::FontMetrics fonts[] = {{}, {0.88f, 0.70f}, {0.75f, 0.52f}, {0.97f, 0.70f}};
    // 16:9 (720p, 1080p, 4K), 16:10, 21:9 and 32:9 ultrawide, a 4:3 window, a small window.
    const float sizes[][2] = {{1280, 720}, {1920, 1080}, {3840, 2160}, {1920, 1200}, {2560, 1080}, {5120, 1440}, {1024, 768}, {640, 480}};
    for (const auto& f : fonts) {
        for (const auto& wh : sizes) {
            const auto b = CoopDraw::TitleAt(wh[0], wh[1], t, f);
            const float caps_mid = b.text_top + f.ascent * b.px - 0.5f * f.cap * b.px;   // baseline minus half the capitals
            CHECK(std::abs(caps_mid - b.cy) < 0.01f);
            // The 16:9 picture's units: the same look, only bigger or smaller.
            const float u = std::min(wh[0] / 16.0f, wh[1] / 9.0f) * 9.0f / 720.0f;
            const float left = (wh[0] - 16.0f * u * 80.0f) / 2;
            CHECK(b.cx - b.r == doctest::Approx(left + 48.0f * u));
            CHECK(b.text_x - (b.cx + b.r) == doctest::Approx(10.0f * u));   // the word after the button, a gap
            CHECK(b.cy + b.r < wh[1]);                                       // on the window
        }
    }
    // Without the button the word starts where the button would.
    const auto plain = CoopDraw::TitleAt(1920, 1080, CoopDraw::TitleView{"Loading Saria's save..."}, {});
    const auto with = CoopDraw::TitleAt(1920, 1080, t, {});
    CHECK(plain.text_x == doctest::Approx(with.cx - with.r));
    CHECK(plain.text_top == doctest::Approx(with.text_top));
}

TEST_CASE("hookshot draw: the title prompt draws its word where TitleAt says, on the button's middle") {
    Canvas c(1920, 1080);
    CoopDraw::Scene s;
    s.title = CoopDraw::TitleView{"Multiplayer", true};
    auto look = GameLook();
    look.regular_metrics = {0.86f, 0.66f};
    CoopDraw::Draw(c.d, s, look);
    const auto b = CoopDraw::TitleAt(1920, 1080, *s.title, look.regular_metrics);
    const auto circles = c.Of("circle");
    REQUIRE(circles.size() == 2);
    CHECK(circles[0].x == doctest::Approx(b.cx));
    CHECK(circles[0].y == doctest::Approx(b.cy));
    bool word = false;
    for (const auto& t : c.Of("text")) {
        if (t.text == "Multiplayer" && t.rgba == CoopDraw::Rgba(255, 255, 255, 255)) {   // (the fill, not its shadow)
            word = true;
            CHECK(t.x == doctest::Approx(b.text_x));
            CHECK(t.y == doctest::Approx(b.text_top));
            CHECK(t.px == doctest::Approx(b.px));
        }
    }
    CHECK(word);
}

// Down but not out (hookshot/src/coop_dbno_draw.h).
TEST_CASE("dbno ring: the dots lit follow the revive, the caption says who") {
    using CoopDbnoDraw::Lit;
    CHECK(Lit(0.0f) == 0);
    CHECK(Lit(-1.0f) == 0);
    CHECK(Lit(0.5f) == CoopDbnoDraw::kDots / 2);
    CHECK(Lit(0.99f) == CoopDbnoDraw::kDots - 1);
    CHECK(Lit(1.0f) == CoopDbnoDraw::kDots);
    CHECK(Lit(7.0f) == CoopDbnoDraw::kDots);
    using View = CoopDbno::View;
    CHECK(CoopDbnoDraw::Caption(View{View::Kind::downed, "", 0, 0, false}) == "You are down");
    CHECK(CoopDbnoDraw::Caption(View{View::Kind::downed, "Saria", 0, 0, false}) == "Saria is reviving you");
    CHECK(CoopDbnoDraw::Caption(View{View::Kind::reviving, "Saria", 0, 0, false}) == "Hold A to revive Saria");
    CHECK(CoopDbnoDraw::Caption(View{View::Kind::reviving, "Saria", 0, 0, true}) == "Reviving Saria");
}

TEST_CASE("dbno ring: drawn with the clock when the scene has it, and wanted for it alone") {
    CoopDraw::Scene s;
    CHECK_FALSE(CoopDraw::Wanted(s));
    s.dbno = CoopDbno::View{CoopDbno::View::Kind::downed, "", 32'000'000, 0.25f, false};
    CHECK(CoopDraw::Wanted(s));
    Canvas c(1280, 720);
    CoopDraw::Draw(c.d, s);
    // The backing disc and the ring's dots, a quarter of them lit.
    CHECK(c.Of("circle").size() == 1 + CoopDbnoDraw::kDots);
    std::size_t lit = 0;
    for (const auto& call : c.Of("circle")) {
        lit += call.rgba == CoopDraw::Rgba(120, 230, 120, 255) ? 1 : 0;
    }
    CHECK(lit == CoopDbnoDraw::kDots / 4);
    std::vector<std::string> texts;
    for (const auto& call : c.Of("text")) {
        texts.push_back(call.text);
    }
    CHECK(texts == std::vector<std::string>{"0:32", "You are down"});
}

namespace {

// A player on the minimap, `dx` and `dz` metres from our Link at the origin.
coop::overlay::map::Player OnMap(std::uint8_t n, float dx, float dz, std::uint32_t rgb) {
    coop::overlay::map::Player p;
    p.number = n;
    p.position = {dx, 0, dz};
    p.scene = "HyruleField";
    p.rgb = rgb;
    return p;
}

}  // namespace

TEST_CASE("hookshot draw: the minimap where the game's layout has it, on any picture size") {
    auto b = CoopDraw::MinimapBox(3840, 2160);
    CHECK(b.cx() == doctest::Approx(1920 + 1470));
    CHECK(b.cy() == doctest::Approx(1080 + 544));
    CHECK(b.w == doctest::Approx(552));
    b = CoopDraw::MinimapBox(1920, 1080);  // half
    CHECK(b.cx() == doctest::Approx(1695));
    CHECK(b.cy() == doctest::Approx(812));
    CHECK(b.w == doctest::Approx(276));
    b = CoopDraw::MinimapBox(2560, 1080);  // ultrawide: the 16:9 picture in the middle
    CHECK(b.cx() == doctest::Approx(320 + 1695));
    CHECK(b.h == doctest::Approx(276));
    coop::overlay::map::Capture field;
    field.height = 736;
    CHECK(CoopDraw::MinimapMetres(field) == doctest::Approx(736.0f * 552 / 960));
    CHECK(CoopDraw::MinimapMetres(field, 2.0f) == doctest::Approx(736.0f * 552 / 960 / 2));
    CHECK(CoopDraw::MinimapMetres(field, 0.0f) == 0.0f);
}

TEST_CASE("hookshot draw: the others on the minimap in their colours, names under them, wanted for them alone") {
    Canvas c(1920, 1080);
    CoopDraw::Scene s;
    CoopDraw::MapShown m;
    m.scene = "HyruleField";
    m.metres_across = 276;  // a pixel a metre on this picture
    m.players = {OnMap(2, 10, -20, 0xFF0000), OnMap(3, 0, 0, 0x00FF00)};
    m.players[0].rotation = coop::overlay::map::Quat{0, 0, 0, 1};  // looks +Z: an arrow
    m.names = {"Saria", "Darunia"};
    s.map = &m;
    CHECK(CoopDraw::Wanted(s));
    CoopDraw::Draw(c.d, s);
    const auto tris = c.Of("triangle");
    REQUIRE(tris.size() == 2);  // rim, then the arrow
    CHECK(tris[1].rgba == CoopDraw::Rgba(255, 0, 0, 255));
    CHECK(tris[1].x == doctest::Approx(1705).epsilon(0.001));  // the tip, below the player at (1705, 792)
    CHECK(tris[1].y > 792);
    const auto dots = c.Of("circle");
    REQUIRE(dots.size() == 2);  // no rotation: a dot on a rim
    CHECK(dots[1].x == doctest::Approx(1695));
    CHECK(dots[1].y == doctest::Approx(812));
    CHECK(dots[1].rgba == CoopDraw::Rgba(0, 255, 0, 255));
    const auto texts = c.Of("text");
    REQUIRE(texts.size() >= 2);
    bool saria = false;
    for (const auto& t : texts) saria = saria || t.text == "Saria";
    CHECK(saria);

    // Names off: arrows only. No one: nothing wanted.
    Canvas quiet(1920, 1080);
    m.names.clear();
    CoopDraw::Draw(quiet.d, s);
    CHECK(quiet.Of("text").empty());
    m.players.clear();
    CHECK_FALSE(CoopDraw::Wanted(s));
}

TEST_CASE("hookshot draw: a player far off the minimap is held at its edge, paler") {
    Canvas c(1920, 1080);
    CoopDraw::Scene s;
    CoopDraw::MapShown m;
    m.scene = "HyruleField";
    m.metres_across = 276;
    m.players = {OnMap(2, 5000, 0, 0x0000FF)};
    s.map = &m;
    CoopDraw::Draw(c.d, s);
    const auto dots = c.Of("circle");
    REQUIRE(dots.size() == 2);
    const auto box = CoopDraw::MinimapBox(1920, 1080);
    CHECK(dots[1].x < box.x + box.w);
    CHECK(dots[1].x > box.x + box.w - 20);
    CHECK(AlphaOf(dots[1].rgba) < 255);
}

TEST_CASE("hookshot map page: a pane's box from its global matrix, in the layout's units") {
    float m[12] = {};
    m[0] = 1.0f, m[4] = 1.0f, m[9] = 0.0f, m[10] = 0.0f;  // the middle, unscaled
    auto b = CoopMapPage::PaneBox(m, CoopMapPage::kFieldW, CoopMapPage::kFieldH);
    REQUIRE(b);
    CHECK(b->cx() == doctest::Approx(1920));
    CHECK(b->cy() == doctest::Approx(1080));
    CHECK(b->w == doctest::Approx(2600));
    m[0] = m[4] = 2.0f, m[9] = 100.0f, m[10] = 200.0f;  // zoomed in twice, moved right and up
    b = CoopMapPage::PaneBox(m, 2600, 1872);
    REQUIRE(b);
    CHECK(b->cx() == doctest::Approx(2020));
    CHECK(b->cy() == doctest::Approx(880));
    CHECK(b->h == doctest::Approx(3744));
    m[0] = 0.0f;  // not shown
    CHECK_FALSE(CoopMapPage::PaneBox(m, 2600, 1872));
    m[0] = std::nanf("");
    CHECK_FALSE(CoopMapPage::PaneBox(m, 2600, 1872));
    const auto s = CoopDraw::LayoutToScreen({0, 0, 3840, 2160}, 1920, 1080);
    CHECK(s.w == doctest::Approx(1920));
    CHECK(s.x == doctest::Approx(0));
}

TEST_CASE("hookshot draw: the Map page's map shows everyone in our place's map, and the minimap hides behind menus") {
    coop::overlay::map::Capture field;
    field.location = "HyruleField";
    field.height = 100;
    field.floors.push_back({});
    CoopDraw::MapShown m;
    m.scene = "HyruleField";
    m.capture = &field;
    m.metres_across = 276;
    m.players = {OnMap(2, 25, 0, 0xFF0000)};  // a quarter of the picture's height right of its centre
    m.names = {"Saria"};
    m.page = coop::overlay::map::Box{1920 - 1000, 1080 - 500, 2000, 1000};  // 200 m across
    CoopDraw::Scene s;
    s.map = &m;
    CHECK(CoopDraw::Wanted(s));
    Canvas c(1920, 1080);
    CoopDraw::Draw(c.d, s);
    const auto dots = c.Of("circle");
    REQUIRE(dots.size() == 2);
    CHECK(dots[1].x == doctest::Approx(960 + 0.125f * 1000));  // 25 of 200 m: 1/8 of the half-size picture
    CHECK(dots[1].y == doctest::Approx(540));

    m.page.reset();
    m.minimap = false;  // a game menu in front, no Map page: nothing
    CHECK_FALSE(CoopDraw::Wanted(s));
    Canvas none(1920, 1080);
    CoopDraw::Draw(none.d, s);
    CHECK(none.calls.empty());
}

TEST_CASE("hookshot draw: a player on another floor is paler, the floors after the name") {
    CHECK(CoopDraw::FloorWords(0).empty());
    CHECK(CoopDraw::FloorWords(1) == " (1 below)");
    CHECK(CoopDraw::FloorWords(-2) == " (2 above)");
    coop::overlay::map::Capture jabu;
    jabu.location = "InsideJabuJabuBelly";
    jabu.scenes = {"InsideJabuJabuBelly"};
    jabu.camera_y = 200;
    jabu.height = 300;
    jabu.dungeon = true;
    jabu.floors = {{}, {93.0f, 105.0f}, {128.0f, 133.0f}};
    CoopDraw::MapShown m;
    m.scene = "InsideJabuJabuBelly";
    m.self = {0, 100, 0};
    m.capture = &jabu;
    m.metres_across = 276;
    m.players = {OnMap(2, 5, 0, 0x00FF00)};
    m.players[0].scene = "InsideJabuJabuBelly";
    m.players[0].position[1] = 68;  // floor 2: one below ours
    m.names = {"Ruto"};
    CoopDraw::Scene s;
    s.map = &m;
    Canvas c(1920, 1080);
    CoopDraw::Draw(c.d, s);
    const auto dots = c.Of("circle");
    REQUIRE(dots.size() == 2);
    CHECK(AlphaOf(dots[1].rgba) < 160);
    bool named = false;
    for (const auto& t : c.Of("text")) named = named || t.text == "Ruto (1 below)";
    CHECK(named);
}

TEST_CASE("hookshot map page: our place's map until Y turns it to the World Map, and back") {
    CoopMapPage::PageWatch w;
    CHECK_FALSE(w.Update(false, false));  // no page
    CHECK(w.Update(true, false));         // opens on our place
    CHECK(w.Update(true, false));
    CHECK_FALSE(w.Update(true, true));    // Y: the World Map
    CHECK_FALSE(w.Update(true, true));    // (held: one press)
    CHECK_FALSE(w.Update(true, false));
    CHECK(w.Update(true, true));          // Y again: Current Location
    CHECK(w.Update(true, false));
    CHECK_FALSE(w.Update(true, true));    // World Map again, then the page closes
    CHECK_FALSE(w.Update(false, false));
    CHECK(w.Update(true, true));          // a new page opens on our place, even with Y still held
    CHECK(w.Update(true, false));
}

TEST_CASE("hookshot map page: a tap held for one present counts as a press, like a long one") {
    CoopMapPage::PageWatch w;
    CHECK(w.Update(true, false));
    CHECK_FALSE(w.Update(true, true));  // the mouse's tap on the Y prompt: one present
    CHECK_FALSE(w.Update(true, false));
    CHECK_FALSE(w.Update(true, false));
    CHECK(w.Update(true, true));        // and back
    CHECK(w.Update(true, false));
}
