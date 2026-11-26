// Tests for our own Link with no session (lichii/hook/coop_solo_view.h): row 112, #74, the Customize page's live
// picture while co-op is idle (no Host or Join).
#include <array>
#include <chrono>
#include <string>

#include <doctest/doctest.h>

#include "coop_solo_view.h"

using CoopSoloView::Clock;
using CoopSoloView::Reading;
using CoopSoloView::Store;
using CoopSoloView::View;

namespace {

Reading AdultInField() {
    Reading r;
    r.position = std::array<float, 3>{1.0f, 2.0f, 3.0f};
    r.scene = std::string("HyruleField");
    r.adult = true;
    return r;
}

}  // namespace

TEST_CASE("solo view: co-op idle gives our own Link (row 112, the Customize picture)") {
    Store store;
    const auto t0 = Clock::time_point{} + std::chrono::hours(1);
    View v;
    CHECK_FALSE(store.Get(v, t0));  // nothing read yet: the flat picture
    store.Publish(AdultInField(), t0);
    REQUIRE(store.Get(v, t0 + std::chrono::milliseconds(16)));
    CHECK(v.scene == "HyruleField");
    CHECK(v.self == std::array<float, 3>{1.0f, 2.0f, 3.0f});
    CHECK(v.adult);
    CHECK_FALSE(v.event);
}

TEST_CASE("solo view: every reopen (a later frame) is still live") {
    Store store;
    auto t = Clock::time_point{} + std::chrono::hours(1);
    View v;
    for (int frame = 0; frame < 120; ++frame) {
        t += std::chrono::milliseconds(16);
        store.Publish(AdultInField(), t);
        CHECK(store.Get(v, t));
    }
}

TEST_CASE("solo view: our own event stays live (the event picture)") {
    Store store;
    const auto t0 = Clock::time_point{} + std::chrono::hours(1);
    auto r = AdultInField();
    r.event = true;
    store.Publish(r, t0);
    View v;
    REQUIRE(store.Get(v, t0));
    CHECK(v.event);
}

TEST_CASE("solo view: no Link (title, load) or an old reading is nothing") {
    Store store;
    const auto t0 = Clock::time_point{} + std::chrono::hours(1);
    View v;
    store.Publish(AdultInField(), t0);
    CHECK_FALSE(store.Get(v, t0 + CoopSoloView::kStale));  // the game stopped calling
    store.Publish(AdultInField(), t0);
    Reading no_scene = AdultInField();
    no_scene.scene.reset();
    store.Publish(no_scene, t0);
    CHECK_FALSE(store.Get(v, t0));
    store.Publish(AdultInField(), t0);
    Reading no_position = AdultInField();
    no_position.position.reset();
    store.Publish(no_position, t0);
    CHECK_FALSE(store.Get(v, t0));
}

TEST_CASE("solo view: a session's start clears it, and `out` is left alone") {
    Store store;
    const auto t0 = Clock::time_point{} + std::chrono::hours(1);
    store.Publish(AdultInField(), t0);
    store.Clear();
    View v;
    v.scene = "kept";
    CHECK_FALSE(store.Get(v, t0));
    CHECK(v.scene == "kept");
}
