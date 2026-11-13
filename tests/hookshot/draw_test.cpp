// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op over Hookshot's picture (hookshot/src/coop_draw.h): what is drawn,
// through a canvas that records the calls.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "coop_dbno_draw.h"
#include "coop_draw.h"
#include "coop_hud_layout.h"

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
        s.title = CoopDraw::TitleView{"Loading Saria's save..."};   // (the only line the title has now)
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
