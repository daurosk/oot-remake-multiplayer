// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op over Hookshot's picture (hookshot/src/coop_draw.h): what is drawn,
// through a canvas that records the calls.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "coop_draw.h"

namespace {

struct Call {
    std::string kind;
    float x = 0, y = 0, px = 0;
    std::uint32_t rgba = 0;
    std::string text;
};

struct Canvas {
    std::vector<Call> calls;
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
            static_cast<Canvas*>(u)->calls.push_back({"text", x, y, px, rgba, s});
        };
        d.text_width = [](void*, float px, const char* s) {
            return static_cast<float>(std::string(s).size()) * px * 0.5f;  // half the size per byte
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
