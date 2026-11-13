// Tests for local co-op's view layout and its switchboard
// (lichii/hook/coop_local_layout.h, coop_local.h).
// <ostream> before doctest: it prints std::string_view, which MSVC's
// <string_view> can only do with <ostream> in.
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "coop_local.h"
#include "coop_local_layout.h"

using CoopLocal::Config;
using CoopLocal::Core;
using CoopLocal::Device;
using CoopLocal::Seat;
using CoopLocal::State;
using CoopLocal::Status;
using CoopLocal::Why;
using CoopLocalLayout::Place;
using CoopLocalLayout::Rect;
using CoopLocalLayout::RenderSize;
using CoopLocalLayout::Size;
using CoopLocalLayout::Split;
using CoopLocalLayout::Views;

namespace {

long long Area(const Rect& r) { return static_cast<long long>(r.w) * r.h; }

bool Overlap(const Rect& a, const Rect& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

// A game side that records what it is asked and answers as the test says.
class FakeGame final : public CoopLocal::Game {
public:
    bool supported = true;
    bool in_play = true;
    std::vector<Device> devices{{0, "Keyboard and mouse", true}, {1, "Pad A", false}, {2, "Pad B", false},
                                {3, "Pad C", false}};
    bool add_ok = true;
    Made made = Made::Yes;
    bool remove_ok = true;
    std::vector<std::string> log;
    std::vector<Status> applied;

    bool Supported() const override { return supported; }
    std::vector<Device> Devices() const override { return devices; }
    bool InPlay() const override { return in_play; }
    bool Add(int player, const Seat& seat) override {
        log.push_back("add " + std::to_string(player) + " " + seat.name + " on " + std::to_string(seat.device));
        return add_ok;
    }
    Made Added(int player) override {
        log.push_back("added? " + std::to_string(player));
        return made;
    }
    bool Remove(int player) override {
        log.push_back("remove " + std::to_string(player));
        return remove_ok;
    }
    void Apply(const Status& status) override { applied.push_back(status); }
};

Config Two() { return Config{{Seat{0, "", {}}, Seat{1, "Zel", 0x3366ffu}}, Split::Stacked, 80}; }

} // namespace

TEST_CASE("local layout: one player has the whole picture") {
    const Views v = Place(1, Split::Side, 2560, 1440);
    CHECK(v.count == 1);
    CHECK(v.rect[0] == Rect{0, 0, 2560, 1440});
}

TEST_CASE("local layout: two players side by side or stacked") {
    const Views side = Place(2, Split::Side, 2560, 1440);
    CHECK(side.count == 2);
    CHECK(side.rect[0] == Rect{0, 0, 1280, 1440});
    CHECK(side.rect[1] == Rect{1280, 0, 1280, 1440});
    const Views stacked = Place(2, Split::Stacked, 2560, 1440);
    CHECK(stacked.rect[0] == Rect{0, 0, 2560, 720});
    CHECK(stacked.rect[1] == Rect{0, 720, 2560, 720});
    CHECK(CoopLocalLayout::Ratio(side.rect[0]) == doctest::Approx(8.0 / 9.0));
    CHECK(CoopLocalLayout::Ratio(stacked.rect[0]) == doctest::Approx(32.0 / 9.0));
}

TEST_CASE("local layout: three and four players get quarters of one shape") {
    const Views three = Place(3, Split::Stacked, 1920, 1080);
    CHECK(three.count == 3);
    CHECK(three.rect[0] == Rect{0, 0, 960, 540});
    CHECK(three.rect[1] == Rect{960, 0, 960, 540});
    CHECK(three.rect[2] == Rect{480, 540, 960, 540});
    const Views four = Place(4, Split::Side, 1920, 1080);
    CHECK(four.count == 4);
    CHECK(four.rect[2] == Rect{0, 540, 960, 540});
    CHECK(four.rect[3] == Rect{960, 540, 960, 540});
}

TEST_CASE("local layout: views never overlap, stay inside and cover the picture when they can") {
    const std::vector<std::pair<int, int>> pictures{{2560, 1440}, {1921, 1081}, {1280, 800}, {3440, 1440}, {5, 7}, {4, 4}};
    for (const auto& [w, h] : pictures) {
        for (int players = 1; players <= 4; players++) {
            for (const Split split : {Split::Side, Split::Stacked}) {
                CAPTURE(w);
                CAPTURE(h);
                CAPTURE(players);
                const Views v = Place(players, split, w, h);
                REQUIRE(v.count == players);
                long long area = 0;
                for (int i = 0; i < v.count; i++) {
                    const Rect& r = v.rect[static_cast<std::size_t>(i)];
                    CHECK(r.w >= 2);
                    CHECK(r.h >= 2);
                    CHECK(r.x >= 0);
                    CHECK(r.y >= 0);
                    CHECK(r.x + r.w <= w);
                    CHECK(r.y + r.h <= h);
                    area += Area(r);
                    for (int j = 0; j < i; j++) CHECK_FALSE(Overlap(r, v.rect[static_cast<std::size_t>(j)]));
                }
                if (players != 3) CHECK(area == static_cast<long long>(w) * h);
            }
        }
    }
}

TEST_CASE("local layout: no views for a bad count or a picture too small") {
    CHECK(Place(0, Split::Side, 1920, 1080).count == 0);
    CHECK(Place(5, Split::Side, 1920, 1080).count == 0);
    CHECK(Place(-1, Split::Side, 1920, 1080).count == 0);
    CHECK(Place(2, Split::Side, 3, 1080).count == 0);
    CHECK(Place(2, Split::Side, 1920, 0).count == 0);
}

TEST_CASE("local layout: a view's fractions of the picture") {
    const Views v = Place(4, Split::Side, 2000, 1000);
    const CoopLocalLayout::Fraction f = CoopLocalLayout::Of(v.rect[3], 2000, 1000);
    CHECK(f.x == doctest::Approx(0.5));
    CHECK(f.y == doctest::Approx(0.5));
    CHECK(f.w == doctest::Approx(0.5));
    CHECK(f.h == doctest::Approx(0.5));
    CHECK(CoopLocalLayout::Of(v.rect[0], 0, 1000).w == 0.0f);
}

TEST_CASE("local layout: render size follows the scale in steps of 8 pixels") {
    CHECK(RenderSize(Rect{0, 0, 1280, 1440}, 100) == Size{1280, 1440});
    CHECK(RenderSize(Rect{0, 0, 1280, 1440}, 50) == Size{640, 720});
    CHECK(RenderSize(Rect{0, 0, 1280, 1440}, 70) == Size{896, 1008});
    CHECK(RenderSize(Rect{0, 0, 1280, 1440}, 10) == Size{640, 720});     // clamped to 50
    CHECK(RenderSize(Rect{0, 0, 1280, 1440}, 400) == Size{1280, 1440});  // clamped to 100
    CHECK(RenderSize(Rect{0, 0, 1281, 721}, 100) == Size{1280, 720});
    CHECK(RenderSize(Rect{0, 0, 1284, 724}, 100) == Size{1288, 728});
    CHECK(RenderSize(Rect{0, 0, 200, 100}, 50) == Size{160, 88});        // the least
    CHECK(RenderSize(Rect{0, 0, 100, 50}, 50) == Size{104, 56});         // a tiny view: its own size
    CHECK(RenderSize(Rect{0, 0, 0, 720}, 100) == Size{});
    for (int percent = 50; percent <= 100; percent++) {
        const Size s = RenderSize(Rect{0, 0, 1277, 713}, percent);
        CHECK(s.w % 8 == 0);
        CHECK(s.h % 8 == 0);
        CHECK(s.w <= 1280);
        CHECK(s.h <= 720);
    }
}

TEST_CASE("local layout: two views at full scale render about one picture's pixels") {
    const long long full = 2560LL * 1440;
    CHECK(CoopLocalLayout::RenderPixels(Place(1, Split::Side, 2560, 1440), 100) == full);
    CHECK(CoopLocalLayout::RenderPixels(Place(2, Split::Side, 2560, 1440), 100) == full);
    CHECK(CoopLocalLayout::RenderPixels(Place(4, Split::Side, 2560, 1440), 100) == full);
    CHECK(CoopLocalLayout::RenderPixels(Place(2, Split::Stacked, 2560, 1440), 50) == full / 4);
    CHECK(CoopLocalLayout::RenderPixels(Views{}, 100) == 0);
}

TEST_CASE("local co-op: start makes each extra player in turn, then applies the seats") {
    FakeGame game;
    Core core(game);
    CHECK(core.Get().state == State::Off);
    Config config = Two();
    config.seats.push_back(Seat{2, "Third", {}});
    CHECK(core.Start(config) == Why::Ok);
    CHECK(core.Get().state == State::Off);   // nothing of the game before the frame hook
    CHECK(game.log.empty());

    game.made = CoopLocal::Game::Made::Pending;
    core.Frame();
    CHECK(core.Get().state == State::Starting);
    CHECK(game.log == std::vector<std::string>{"add 1 Zel on 1", "added? 1"});
    core.Frame();
    CHECK(game.log.back() == "added? 1");
    CHECK(game.log.size() == 3);   // not asked for twice
    game.made = CoopLocal::Game::Made::Yes;
    core.Frame();
    CHECK(game.log == std::vector<std::string>{"add 1 Zel on 1", "added? 1", "added? 1", "added? 1",
                                               "add 2 Third on 2", "added? 2"});
    const Status s = core.Get();
    CHECK(s.state == State::On);
    CHECK(s.why == Why::Ok);
    CHECK(s.players == 3);
    CHECK(s.split == Split::Stacked);
    CHECK(s.scale == 80);
    CHECK(s.seat[0].name == "Player 1");   // an empty name gets its number
    CHECK(s.seat[1] == Seat{1, "Zel", 0x3366ffu});
    REQUIRE(game.applied.size() == 1);
    CHECK(game.applied[0].players == 3);
}

TEST_CASE("local co-op: start refuses what cannot work, and says why") {
    FakeGame game;
    Core core(game);
    CHECK(core.Start(Config{{Seat{0, "", {}}}, Split::Side, 100}) == Why::TooFewPlayers);
    Config five;
    for (int i = 0; i < 5; i++) five.seats.push_back(Seat{i, "", {}});
    CHECK(core.Start(five) == Why::TooManyPlayers);
    CHECK(core.Start(Config{{Seat{0, "", {}}, Seat{0, "", {}}}, Split::Side, 100}) == Why::DeviceTwice);
    CHECK(core.Start(Config{{Seat{0, "", {}}, Seat{9, "", {}}}, Split::Side, 100}) == Why::NoDevice);
    CHECK(core.Start(Config{{Seat{0, "", {}}, Seat{-1, "", {}}}, Split::Side, 100}) == Why::NoDevice);
    CHECK(core.Start(Config{{Seat{0, "", {}}, Seat{1, std::string(17, 'a'), {}}}, Split::Side, 100}) == Why::BadName);
    CHECK(core.Start(Config{{Seat{0, "", {}}, Seat{1, std::string(16, 'a'), {}}}, Split::Side, 100}) == Why::Ok);
    CHECK(core.Start(Two()) == Why::Busy);   // one start at a time
    game.supported = false;
    CHECK(core.Start(Two()) == Why::NotSupported);
    CHECK(core.Devices().empty());
    core.Frame();
    CHECK(game.log.size() == 2);   // the accepted start still runs
    for (const Why why : {Why::Ok, Why::NotSupported, Why::NotInGame, Why::Busy, Why::TooFewPlayers,
                          Why::TooManyPlayers, Why::NoDevice, Why::DeviceTwice, Why::BadName, Why::NoSuchPlayer,
                          Why::Failed})
        CHECK_FALSE(CoopLocal::Text(why).empty());
}

TEST_CASE("local co-op: a start outside play ends at once with the reason") {
    FakeGame game;
    game.in_play = false;
    Core core(game);
    CHECK(core.Start(Two()) == Why::Ok);
    core.Frame();
    CHECK(core.Get().state == State::Off);
    CHECK(core.Get().why == Why::NotInGame);
    CHECK(game.log.empty());
    game.in_play = true;
    core.Frame();
    CHECK(core.Get().state == State::Off);   // the request is gone, not kept for later
    CHECK(core.Start(Two()) == Why::Ok);
    CHECK(core.Get().why == Why::Ok);
    core.Frame();
    CHECK(core.Get().state == State::On);
}

TEST_CASE("local co-op: a player the game cannot make undoes everything") {
    SUBCASE("the request is refused") {
        FakeGame game;
        game.add_ok = false;
        Core core(game);
        CHECK(core.Start(Two()) == Why::Ok);
        core.Frame();
        CHECK(core.Get().state == State::Off);
        CHECK(core.Get().why == Why::Failed);
        CHECK(game.log == std::vector<std::string>{"add 1 Zel on 1"});   // nothing to remove
        REQUIRE(game.applied.size() == 1);
        CHECK(game.applied[0].players == 0);
    }
    SUBCASE("the second extra player fails: the first is removed too, the last first") {
        FakeGame game;
        Config config = Two();
        config.seats.push_back(Seat{2, "Third", {}});
        config.seats.push_back(Seat{3, "Fourth", {}});
        // Player 1 arrives; player 2 is asked for and then said to have failed.
        struct Failing final : CoopLocal::Game {
            FakeGame& g;
            explicit Failing(FakeGame& game) : g(game) {}
            bool Supported() const override { return true; }
            std::vector<Device> Devices() const override { return g.devices; }
            bool InPlay() const override { return true; }
            bool Add(int player, const Seat& seat) override { return g.Add(player, seat); }
            Made Added(int player) override {
                g.Added(player);
                return player == 2 ? Made::No : Made::Yes;
            }
            bool Remove(int player) override { return g.Remove(player); }
            void Apply(const Status& status) override { g.Apply(status); }
        };
        Failing failing(game);
        Core second(failing);
        CHECK(second.Start(config) == Why::Ok);
        second.Frame();
        CHECK(game.log == std::vector<std::string>{"add 1 Zel on 1", "added? 1", "add 2 Third on 2", "added? 2",
                                                   "remove 2", "remove 1"});
        CHECK(second.Get().state == State::Off);
        CHECK(second.Get().why == Why::Failed);
    }
    SUBCASE("a player that never arrives is given up after the wait") {
        FakeGame game;
        game.made = CoopLocal::Game::Made::Pending;
        Core core(game);
        CHECK(core.Start(Two()) == Why::Ok);
        for (int i = 0; i < CoopLocal::kStartFrames; i++) core.Frame();
        CHECK(core.Get().state == State::Starting);
        core.Frame();
        CHECK(core.Get().state == State::Off);
        CHECK(core.Get().why == Why::Failed);
        CHECK(game.log.back() == "remove 1");
    }
}

TEST_CASE("local co-op: stop removes the players and waits until the game has done it") {
    FakeGame game;
    Core core(game);
    CHECK(core.Start(Two()) == Why::Ok);
    core.Frame();
    REQUIRE(core.Get().state == State::On);
    game.log.clear();
    game.remove_ok = false;
    core.Stop();
    CHECK(core.Get().state == State::On);   // at the next frame hook
    core.Frame();
    CHECK(core.Get().state == State::Stopping);
    CHECK(core.Start(Two()) == Why::Busy);
    core.Frame();
    CHECK(game.log == std::vector<std::string>{"remove 1", "remove 1"});
    game.remove_ok = true;
    core.Frame();
    const Status s = core.Get();
    CHECK(s.state == State::Off);
    CHECK(s.why == Why::Ok);
    CHECK(s.players == 0);
    REQUIRE(game.applied.size() == 2);
    CHECK(game.applied[1].players == 0);
    core.Stop();   // nothing when off
    core.Frame();
    CHECK(game.applied.size() == 2);
}

TEST_CASE("local co-op: stop before the first frame hook or while starting") {
    FakeGame game;
    Core core(game);
    CHECK(core.Start(Two()) == Why::Ok);
    core.Stop();
    core.Frame();
    CHECK(core.Get().state == State::Off);
    CHECK(game.log.empty());

    game.made = CoopLocal::Game::Made::Pending;
    CHECK(core.Start(Two()) == Why::Ok);
    core.Frame();
    REQUIRE(core.Get().state == State::Starting);
    core.Stop();
    core.Frame();
    CHECK(core.Get().state == State::Off);
    CHECK(core.Get().why == Why::Ok);
    CHECK(game.log.back() == "remove 1");
}

TEST_CASE("local co-op: split, scale and device change while on, applied once at the frame hook") {
    FakeGame game;
    Core core(game);
    CHECK(core.SetDevice(1, 2) == Why::NoSuchPlayer);   // off
    CHECK(core.Start(Two()) == Why::Ok);
    core.Frame();
    REQUIRE(game.applied.size() == 1);
    core.Frame();
    CHECK(game.applied.size() == 1);   // nothing changed

    CHECK(core.SetSplit(Split::Side) == Why::Ok);
    CHECK(core.SetScale(20) == Why::Ok);
    CHECK(core.SetDevice(1, 2) == Why::Ok);
    core.Frame();
    REQUIRE(game.applied.size() == 2);
    CHECK(game.applied[1].split == Split::Side);
    CHECK(game.applied[1].scale == 50);
    CHECK(game.applied[1].seat[1].device == 2);
    CHECK(core.SetSplit(Split::Side) == Why::Ok);   // the same again: no Apply
    CHECK(core.SetScale(50) == Why::Ok);
    CHECK(core.SetDevice(1, 2) == Why::Ok);
    core.Frame();
    CHECK(game.applied.size() == 2);

    CHECK(core.SetDevice(1, 0) == Why::DeviceTwice);   // player 1 has it
    CHECK(core.SetDevice(1, 9) == Why::NoDevice);
    CHECK(core.SetDevice(2, 3) == Why::NoSuchPlayer);
    CHECK(core.SetDevice(-1, 3) == Why::NoSuchPlayer);
}

TEST_CASE("local co-op: a pulled controller marks its seat lost until it is back") {
    FakeGame game;
    Core core(game);
    CHECK(core.Start(Two()) == Why::Ok);
    core.Frame();
    game.devices.erase(game.devices.begin() + 1);   // Pad A
    core.Frame();
    CHECK(core.Get().state == State::On);   // the player stays
    CHECK(core.Get().lost[1]);
    CHECK_FALSE(core.Get().lost[0]);
    REQUIRE(game.applied.size() == 2);
    CHECK(game.applied[1].lost[1]);
    game.devices.push_back(Device{1, "Pad A", false});
    core.Frame();
    CHECK_FALSE(core.Get().lost[1]);
    CHECK(game.applied.size() == 3);
}

TEST_CASE("local co-op: the menu's functions without a game side") {
    CoopLocal::Install(nullptr);
    CHECK(CoopLocal::Devices().empty());
    CHECK(CoopLocal::Start(Two()) == Why::NotSupported);
    CHECK(CoopLocal::Get().state == State::Off);
    CHECK(CoopLocal::SetSplit(Split::Side) == Why::NotSupported);
    CHECK(CoopLocal::SetScale(70) == Why::NotSupported);
    CHECK(CoopLocal::SetDevice(0, 0) == Why::NotSupported);
    CoopLocal::Stop();

    FakeGame game;
    Core core(game);
    CoopLocal::Install(&core);
    CHECK(CoopLocal::Devices().size() == 4);
    CHECK(CoopLocal::Start(Two()) == Why::Ok);
    core.Frame();
    CHECK(CoopLocal::Get().state == State::On);
    CHECK(CoopLocal::SetScale(70) == Why::Ok);
    CoopLocal::Stop();
    core.Frame();
    CHECK(CoopLocal::Get().state == State::Off);
    CoopLocal::Install(nullptr);
}
