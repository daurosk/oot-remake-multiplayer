// Tests for the fairies (lichii/hook/coop_fairy.cpp): where a fairy goes
// beside its puppet, and how it trails.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "coop_fairy.h"
#include "coop_pose.h"

namespace {

using CoopFairy::Follower;
using CoopFairy::Settings;
using CoopPuppet::Input;
using CoopPuppet::Target;

Settings Still() {
    Settings s;
    s.bob = 0.0f;
    s.circle = 0.0f;
    return s;
}

Input Puppets(std::uint8_t number, CoopPuppet::Vec3 at,
              CoopPuppet::Mat3 rot = {1, 0, 0, 0, 1, 0, 0, 0, 1}, std::string scene = "S") {
    Input in;
    in.main_base = 0x80000000;
    in.scene = std::move(scene);
    Target t;
    t.number = number;
    t.actor = "CoopPuppet_Child";
    t.position = at;
    t.rotation = rot;
    in.targets.push_back(t);
    return in;
}

} // namespace

TEST_CASE("fairy: hovers at the offset beside its puppet, as the fairy actor") {
    Follower f(Still());
    const auto out = f.Follow(Puppets(2, {10, 0, 5}), 1000);
    REQUIRE(out.targets.size() == 1);
    const auto& t = out.targets[0];
    CHECK(t.number == 2);
    CHECK(t.actor == "Dm_Npc_Navi");
    CHECK(out.scene == "S");
    CHECK(out.main_base == 0x80000000);
    const auto off = Still().offset;
    CHECK(t.position[0] == doctest::Approx(10 + off[0]));
    CHECK(t.position[1] == doctest::Approx(0 + off[1]));
    CHECK(t.position[2] == doctest::Approx(5 + off[2]));
}

TEST_CASE("fairy: the offset turns with the puppet") {
    Follower f(Still());
    // Half a turn about y: x and z flip.
    const auto out = f.Follow(Puppets(1, {0, 0, 0}, {-1, 0, 0, 0, 1, 0, 0, 0, -1}), 0);
    const auto off = Still().offset;
    CHECK(out.targets[0].position[0] == doctest::Approx(-off[0]));
    CHECK(out.targets[0].position[1] == doctest::Approx(off[1]));
    CHECK(out.targets[0].position[2] == doctest::Approx(-off[2]));
    CHECK(out.targets[0].rotation[0] == doctest::Approx(-1));
}

TEST_CASE("fairy: trails a moving puppet, jumps when it is far") {
    Follower f(Still());
    f.Follow(Puppets(1, {0, 0, 0}), 0);
    // The puppet steps 1 unit; 100 ms later the fairy has closed part of it.
    auto out = f.Follow(Puppets(1, {1, 0, 0}), 100);
    const float x0 = Still().offset[0];
    const float x = out.targets[0].position[0];
    CHECK(x > x0 + 0.1f);
    CHECK(x < x0 + 1.0f);
    // A second later it is nearly there.
    out = f.Follow(Puppets(1, {1, 0, 0}), 1100);
    CHECK(std::abs(out.targets[0].position[0] - (x0 + 1)) < 0.1f);
    // A warp of 50 units: no flight across the map.
    out = f.Follow(Puppets(1, {51, 0, 0}), 1133);
    CHECK(out.targets[0].position[0] == doctest::Approx(x0 + 51));
}

TEST_CASE("fairy: a new scene or a returning player starts at the puppet") {
    Follower f(Still());
    f.Follow(Puppets(1, {0, 0, 0}), 0);
    auto out = f.Follow(Puppets(1, {2, 0, 0}, {1, 0, 0, 0, 1, 0, 0, 0, 1}, "T"), 33);
    CHECK(out.targets[0].position[0] == doctest::Approx(2 + Still().offset[0]));
    // Player 1 leaves for a frame and comes back elsewhere.
    Input none = Puppets(1, {0, 0, 0}, {1, 0, 0, 0, 1, 0, 0, 0, 1}, "T");
    none.targets.clear();
    CHECK(f.Follow(none, 66).targets.empty());
    out = f.Follow(Puppets(1, {3, 0, 0}, {1, 0, 0, 0, 1, 0, 0, 0, 1}, "T"), 99);
    CHECK(out.targets[0].position[0] == doctest::Approx(3 + Still().offset[0]));
}

TEST_CASE("fairy: bobs within its range") {
    Follower f;  // default bob and loop
    const auto& s = f.settings();
    Target p;
    p.position = {0, 0, 0};
    float lo = 1e9f, hi = -1e9f;
    for (int ms = 0; ms < 3000; ms += 50) {
        const auto g = f.Goal(p, static_cast<float>(ms) / 1000.0f, 1);
        lo = std::min(lo, g[1]);
        hi = std::max(hi, g[1]);
        CHECK(std::abs(g[0] - s.offset[0]) <= s.circle + 1e-4f);
        CHECK(std::abs(g[2] - s.offset[2]) <= s.circle + 1e-4f);
    }
    CHECK(hi - lo > s.bob);
    CHECK(hi <= s.offset[1] + s.bob + 1e-4f);
    CHECK(lo >= s.offset[1] - s.bob - 1e-4f);
}


TEST_CASE("an adult puppet's fairy sits higher, by its shoulder") {
    Follower f(Still());
    Target child;
    child.actor = "CoopPuppet_Child";
    Target adult = child;
    adult.actor = "CoopPuppet_Adult";
    const auto c = f.Goal(child, 0.0f, 1);
    const auto a = f.Goal(adult, 0.0f, 1);
    CHECK(c[1] == doctest::Approx(1.05f));
    CHECK(a[1] == doctest::Approx(1.05f * Settings{}.adult_scale));
    CHECK(a[0] > c[0]);
}

TEST_CASE("a call (/here): the caller's Navi flies up over their head for call_ms, then back by the shoulder") {
    Settings s = Still();
    s.call_bob = 0.0f;
    Follower f(s);
    const std::uint64_t t0 = 10000;
    Input in = Puppets(2, {0, 0, 0});
    CHECK(f.Follow(in, t0).targets[0].position[1] == doctest::Approx(s.offset[1]));   // by the shoulder
    f.Call(2, t0);
    CHECK(f.Calling(2, t0));
    CHECK_FALSE(f.Calling(3, t0));   // only the caller's
    CHECK(f.Calling(2, t0 + s.call_ms - 1));
    CHECK_FALSE(f.Calling(2, t0 + s.call_ms));
    // Over the head, centred, well above the shoulder spot; flown there smoothly, not snapped.
    Target p;
    const auto up = f.Goal(p, 0.0f, 2, true);
    CHECK(up[1] == doctest::Approx(s.call_offset[1]));
    CHECK(up[1] > s.offset[1] + 0.8f);
    CHECK(up[0] == doctest::Approx(0.0f));
    const float first = f.Follow(in, t0 + 50).targets[0].position[1];
    CHECK(first > s.offset[1]);
    CHECK(first < s.call_offset[1]);
    float y = first;
    for (std::uint64_t ms = t0 + 100; ms < t0 + 2000; ms += 50) y = f.Follow(in, ms).targets[0].position[1];
    CHECK(y == doctest::Approx(s.call_offset[1]).epsilon(0.02));
    // The call over: back by the shoulder.
    for (std::uint64_t ms = t0 + s.call_ms; ms < t0 + s.call_ms + 3000; ms += 50) {
        y = f.Follow(in, ms).targets[0].position[1];
    }
    CHECK(y == doctest::Approx(s.offset[1]).epsilon(0.02));
    CHECK_FALSE(f.Calling(2, t0 + s.call_ms + 3000));
    // A new call restarts it.
    f.Call(2, t0 + 9000);
    CHECK(f.Calling(2, t0 + 9000 + s.call_ms - 1));
}

TEST_CASE("an adult caller's Navi hovers higher over the head") {
    Follower f(Still());
    Target child;
    child.actor = "CoopPuppet_Child";
    Target adult = child;
    adult.actor = "CoopPuppet_Adult";
    const auto c = f.Goal(child, 0.0f, 1, true);
    const auto a = f.Goal(adult, 0.0f, 1, true);
    CHECK(a[1] > c[1]);
}

// The wing beat: finding the local Navi on the actor list and copying her
// skeleton onto each fairy.
namespace {

class WingGuest : public CoopPuppet::Guest {
public:
    void Map(std::uint64_t at, std::size_t size) { regions_[at].assign(size, 0); }
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        auto* p = Find(address, size);
        if (p == nullptr) return false;
        std::memcpy(out, p, size);
        return true;
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        auto* p = Find(address, size);
        if (p == nullptr) return false;
        std::memcpy(p, in, size);
        return true;
    }
    template <typename T>
    void Put(std::uint64_t at, T value) { REQUIRE(Write(at, &value, sizeof(T))); }
    template <typename T>
    T Get(std::uint64_t at) {
        T value{};
        REQUIRE(Read(at, &value, sizeof(T)));
        return value;
    }

private:
    std::uint8_t* Find(std::uint64_t address, std::size_t size) {
        auto it = regions_.upper_bound(address);
        if (it == regions_.begin()) return nullptr;
        --it;
        if (address + size > it->first + it->second.size()) return nullptr;
        return it->second.data() + (address - it->first);
    }
    std::map<std::uint64_t, std::vector<std::uint8_t>> regions_;
};

namespace fa = CoopFairy::addr;
namespace pa = CoopPose::addr;
constexpr std::uint64_t kMain = 0x80000000;
constexpr std::uint32_t kBones = 4;

// A named, present actor with a kBones skeleton; returns its local array.
std::uint64_t MakeActor(WingGuest& g, std::uint64_t actor, std::string_view name) {
    const std::uint64_t parts = actor + 0x1000, first = actor + 0x2000, second = actor + 0x3000,
                        holder = actor + 0x4000, skel = actor + 0x5000, local = actor + 0x6000,
                        res = actor + 0x7000, text = actor + 0x8000;
    for (std::uint64_t at : {actor, parts, first, second, holder, skel, res, text}) g.Map(at, 0x600);
    g.Map(local, kBones * pa::kLocalStride);
    g.Put(actor + fa::kActorState, fa::kStatePresent);
    g.Put(actor + fa::kActorName, text);
    REQUIRE(g.Write(text, name.data(), name.size()));
    g.Put(actor + pa::kSkeletonPath[0], parts);
    g.Put(parts + pa::kSkeletonPath[1], first);
    g.Put(first + pa::kSkeletonPath[2], second);
    g.Put(second + pa::kSkeletonPath[3], holder);
    g.Put(holder + pa::kSkeletonPath[4], skel);
    g.Put(holder + pa::kModelResource, res);
    g.Put(res, std::array<char, 4>{'F', 'M', 'D', 'L'});
    g.Put(skel + pa::kBoneCount, static_cast<std::uint16_t>(kBones));
    g.Put(skel + pa::kLocalArray, local);
    return local;
}

// The actor list: a ring through each actor's node, in order.
void MakeList(WingGuest& g, const std::vector<std::uint64_t>& actors) {
    const std::uint64_t manager = 0x500000000, lists = manager + 0x1000;
    g.Map(kMain + 0x4e91000, 0x1000);
    g.Map(manager, 0x400);
    g.Map(lists, 0x1000);
    g.Put(kMain + fa::kManagerPtr, manager);
    g.Put(manager + fa::kLists, lists);
    const std::uint64_t head = lists + fa::kListHead;
    std::uint64_t prev = head;
    for (const std::uint64_t a : actors) {
        const std::uint64_t node = a + fa::kActorNode;
        g.Put(prev, node);
        g.Put(node + fa::kNodeOwner, a);
        prev = node;
    }
    g.Put(prev, head);
}

} // namespace

TEST_CASE("fairy: FindActor walks the list for a present actor by name") {
    WingGuest g;
    const std::uint64_t link = 0x600000000, navi = 0x700000000, fairy = 0x900000000;
    MakeActor(g, link, "Player_Child");
    MakeActor(g, fairy, "Dm_Npc_Navi");
    MakeActor(g, navi, "Npc_Navi");
    MakeList(g, {link, fairy, navi});
    CHECK(CoopFairy::FindActor(g, kMain, "Npc_Navi") == navi);
    CHECK(CoopFairy::FindActor(g, kMain, "Dm_Npc_Navi") == fairy);
    CHECK(CoopFairy::FindActor(g, kMain, "Npc_Nav") == 0);  // a prefix is not the name
    g.Put(navi + fa::kActorState, std::uint32_t{2});          // not present
    CHECK(CoopFairy::FindActor(g, kMain, "Npc_Navi") == 0);
    CHECK(CoopFairy::FindActor(g, 0, "Npc_Navi") == 0);
}

TEST_CASE("fairy: Wings copies the local Navi's pose onto every fairy each frame") {
    WingGuest g;
    const std::uint64_t navi = 0x700000000, f1 = 0x900000000, f2 = 0xa00000000;
    const std::uint64_t from = MakeActor(g, navi, "Npc_Navi");
    const std::uint64_t to1 = MakeActor(g, f1, "Dm_Npc_Navi");
    const std::uint64_t to2 = MakeActor(g, f2, "Dm_Npc_Navi");
    MakeList(g, {f1, f2, navi});
    std::vector<std::string> lines;
    CoopFairy::Wings wings("Npc_Navi", [&](const std::string& l) { lines.push_back(l); });

    // A wing bone turns: each frame's pose reaches both fairies.
    for (int frame = 0; frame < 3; ++frame) {
        const float angle = 0.1f * static_cast<float>(frame + 1);
        g.Put(from + 2 * pa::kLocalStride + 0x24, angle);
        wings.Frame(g, kMain, {f1, f2}, 1000 + 16 * frame);
        CHECK(g.Get<float>(to1 + 2 * pa::kLocalStride + 0x24) == angle);
        CHECK(g.Get<float>(to2 + 2 * pa::kLocalStride + 0x24) == angle);
    }
    CHECK(wings.navi() == navi);
    CHECK(wings.copies() == 6);
    // One "copying" line and one per fairy, not one per frame.
    CHECK(lines.size() == 3);

    // Navi gone (a scene change): no copy, a line, and a search again later.
    g.Put(navi + fa::kActorState, std::uint32_t{0});
    wings.Frame(g, kMain, {f1, f2}, 2000);
    CHECK(wings.navi() == 0);
    CHECK(wings.copies() == 6);
    g.Put(navi + fa::kActorState, fa::kStatePresent);
    wings.Frame(g, kMain, {f1, f2}, 2100);  // searched at 2000: too soon
    CHECK(wings.navi() == 0);
    wings.Frame(g, kMain, {f1, f2}, 2600);
    CHECK(wings.navi() == navi);
    CHECK(wings.copies() == 8);
}

TEST_CASE("fairy: hidden, held or shown with her puppet") {
    Follower f(Still());
    for (const auto show : {CoopPuppet::Show::hide, CoopPuppet::Show::hold, CoopPuppet::Show::show}) {
        Input in = Puppets(2, {10, 0, 5});
        in.targets[0].show = show;
        in.targets[0].why = "why";
        const auto out = f.Follow(in, 1000);
        REQUIRE(out.targets.size() == 1);
        CHECK(out.targets[0].show == show);
        CHECK(std::string(out.targets[0].why) == "why");
    }
}

TEST_CASE("fairy: carries her puppet's wait for the drawing (issue 93)") {
    Follower f(Still());
    Input in = Puppets(1, {0, 0, 0});
    in.camera = true;
    in.drawn_since_ms = 1234;
    in.settle_ms = 1000;
    const auto out = f.Follow(in, 2000);
    CHECK(out.camera);
    CHECK(out.drawn_since_ms == 1234);
    CHECK(out.settle_ms == 1000);
}

TEST_CASE("fairy: parked while her puppet is not live, placed from the puppet (issue 145)") {
    Follower f(Still());
    Input in = Puppets(1, {10, 0, 5});
    // Puppet 1 is still being made (or refused, or gone): no bit.
    auto out = f.Follow(in, 1000, 0u);
    REQUIRE(out.targets.size() == 1);
    CHECK(out.targets[0].show == CoopPuppet::Show::hide);
    CHECK(std::string(out.targets[0].why) == "her puppet is not shown");
    // Still placed from the puppet's spot (the director parks her under it).
    CHECK(out.targets[0].position[0] == doctest::Approx(10 + Still().offset[0]));
    // Another player's puppet is live, not hers.
    out = f.Follow(in, 1033, 1u << 2);
    CHECK(out.targets[0].show == CoopPuppet::Show::hide);
    // Her puppet is live: she shows.
    out = f.Follow(in, 1066, 1u << 1);
    CHECK(out.targets[0].show == CoopPuppet::Show::show);
    // A puppet hidden by the plan keeps its own reason.
    in.targets[0].show = CoopPuppet::Show::hide;
    in.targets[0].why = "our cutscene";
    out = f.Follow(in, 1100, 0u);
    CHECK(out.targets[0].show == CoopPuppet::Show::hide);
    CHECK(std::string(out.targets[0].why) == "our cutscene");
    // Held with a live puppet: held.
    in.targets[0].show = CoopPuppet::Show::hold;
    out = f.Follow(in, 1133, 1u << 1);
    CHECK(out.targets[0].show == CoopPuppet::Show::hold);
}

namespace {

// A synthetic dungeon "D": room 0 around the origin, room 3 up and 40 units
// west, rooms 5 and 6 side by side 5 units apart.
CoopRooms::Table Dungeon() {
    return CoopRooms::Table(std::vector<CoopRooms::Point>{
        {"D", 0, 0.0f, 100.0f, 0.0f},
        {"D", 0, 5.0f, 100.0f, 5.0f},
        {"D", 3, -40.0f, 120.0f, 0.0f},
        {"D", 3, -25.0f, 120.0f, 0.0f},
        {"D", 5, 0.0f, 80.0f, 30.0f},
        {"D", 6, -5.0f, 80.0f, 30.0f},
    });
}

}  // namespace

TEST_CASE("fairy: only in our room of a dungeon (issue 145)") {
    std::vector<std::string> lines;
    Follower f(Still(), Dungeon(), [&](const std::string& l) { lines.push_back(l); });
    const CoopPuppet::Vec3 ours{0.0f, 100.0f, 2.0f};  // room 0
    // A in room 3, about 40 units off: her Navi is parked, placed from A's
    // puppet, and the log names both rooms.
    Input in = Puppets(1, {-40.0f, 120.0f, 0.0f}, {1, 0, 0, 0, 1, 0, 0, 0, 1}, "D");
    auto out = f.Follow(in, 1000, ~0u, ours);
    REQUIRE(out.targets.size() == 1);
    CHECK(out.targets[0].show == CoopPuppet::Show::hide);
    CHECK(std::string(out.targets[0].why) == "her player is in another room");
    CHECK(out.targets[0].position[0] == doctest::Approx(-40.0f + Still().offset[0]));
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "rooms: P1 in room 3, we in room 0: Navi parked");
    f.Follow(in, 1016, ~0u, ours);
    CHECK(lines.size() == 1);  // once, not each frame
    // Both in room 3, 15 units apart (more than room_near): shown.
    in.targets[0].position = {-40.0f, 120.0f, 0.0f};
    out = f.Follow(in, 1033, ~0u, CoopPuppet::Vec3{-25.0f, 120.0f, 0.0f});
    CHECK(out.targets[0].show == CoopPuppet::Show::show);
    REQUIRE(lines.size() == 2);
    CHECK(lines[1] == "rooms: P1 in room 3, we in room 3: no longer parked for the room");
    // Without our position, with the rule off, or with no table: no rule.
    CHECK(f.Follow(in, 1066).targets[0].show == CoopPuppet::Show::show);
    Settings off = Still();
    off.rooms = false;
    Follower g(off, Dungeon());
    CHECK(g.Follow(in, 1066, ~0u, ours).targets[0].show == CoopPuppet::Show::show);
    Follower none(Still());
    CHECK(none.Follow(in, 1066, ~0u, ours).targets[0].show == CoopPuppet::Show::show);
    // A scene with no rooms in the table (a field): shown at any distance.
    Input field = Puppets(1, {40.0f, 0.0f, 0.0f}, {1, 0, 0, 0, 1, 0, 0, 0, 1}, "Field");
    CHECK(f.Follow(field, 1100, ~0u, CoopPuppet::Vec3{0, 0, 0}).targets[0].show ==
          CoopPuppet::Show::show);
    // Back in D: a plan's own hide and the not-live park keep their reasons.
    in.targets[0].show = CoopPuppet::Show::hide;
    in.targets[0].why = "our cutscene";
    out = f.Follow(in, 1133, ~0u, ours);
    CHECK(std::string(out.targets[0].why) == "our cutscene");
    in.targets[0].show = CoopPuppet::Show::show;
    out = f.Follow(in, 1166, 0u, ours);
    CHECK(std::string(out.targets[0].why) == "her puppet is not shown");
}

TEST_CASE("fairy: a player's room is tracked while ours is not known") {
    Follower f(Still(), Dungeon());
    // Our spot is not known (far from every placement) while A walks from
    // room 0 into room 3; once ours is known again, A is in room 3 at once.
    Input in = Puppets(1, {0.0f, 100.0f, 0.0f}, {1, 0, 0, 0, 1, 0, 0, 0, 1}, "D");
    const CoopPuppet::Vec3 lost{200.0f, 0.0f, 200.0f};
    f.Follow(in, 1000, ~0u, lost);
    in.targets[0].position = {-40.0f, 120.0f, 0.0f};
    f.Follow(in, 1033, ~0u, lost);
    const auto out = f.Follow(in, 1066, ~0u, CoopPuppet::Vec3{0.0f, 100.0f, 2.0f});
    CHECK(out.targets[0].show == CoopPuppet::Show::hide);
}

TEST_CASE("fairy: another room but within room_near of us is shown (a doorway)") {
    const CoopPuppet::Vec3 ours{0.0f, 80.0f, 30.0f};  // room 5
    Input in = Puppets(1, {-5.0f, 80.0f, 30.0f}, {1, 0, 0, 0, 1, 0, 0, 0, 1}, "D");  // room 6
    Follower loose(Still(), Dungeon());
    CHECK(loose.Follow(in, 1000, ~0u, ours).targets[0].show == CoopPuppet::Show::show);
    Settings s = Still();
    s.room_near = 0.0f;
    Follower strict(s, Dungeon());
    CHECK(strict.Follow(in, 1000, ~0u, ours).targets[0].show == CoopPuppet::Show::hide);
}

// ---------------------------------------------------------------- row 316: NAVI_SAMPLE

namespace {
namespace proto = coop::protocol;
using CoopFairy::NaviSamples;
using CoopFairy::NaviSeen;
using CoopFairy::NaviSender;

proto::NaviSample Sampled(std::uint8_t number, CoopPuppet::Vec3 offset,
                          proto::Quat turn = {0.0f, 0.0f, 0.0f, 1.0f}) {
    return proto::NaviSample{number, proto::kNaviShown, offset, turn};
}
}  // namespace

TEST_CASE("navi sample: the fairy sits where her player's game has her, turned with the puppet (row 316)") {
    Follower f;  // the real settings: bob and circle on, which a sampled place never gets
    const std::map<std::uint8_t, proto::NaviSample> navis{{2, Sampled(2, {0.5f, 1.2f, -0.4f})}};
    auto out = f.Follow(Puppets(2, {10, 0, 5}), 1000, ~0u, std::nullopt, navis);
    REQUIRE(out.targets.size() == 1);
    CHECK(out.targets[0].position[0] == doctest::Approx(10.5f));
    CHECK(out.targets[0].position[1] == doctest::Approx(1.2f));
    CHECK(out.targets[0].position[2] == doctest::Approx(4.6f));
    CHECK(out.targets[0].show == CoopPuppet::Show::show);
    // Half a turn about y: the offset turns with the puppet, and so does she.
    out = f.Follow(Puppets(2, {10, 0, 5}, {-1, 0, 0, 0, 1, 0, 0, 0, -1}), 1500, ~0u, std::nullopt, navis);
    CHECK(out.targets[0].position[0] == doctest::Approx(9.5f));
    CHECK(out.targets[0].position[1] == doctest::Approx(1.2f));
    CHECK(out.targets[0].position[2] == doctest::Approx(5.4f));
    CHECK(out.targets[0].rotation[0] == doctest::Approx(-1.0f));
    CHECK(out.targets[0].rotation[8] == doctest::Approx(-1.0f));
    // No trailing on top of hers: a moved puppet has her at once where its game has her.
    out = f.Follow(Puppets(2, {13, 0, 5}), 1533, ~0u, std::nullopt, navis);
    CHECK(out.targets[0].position[0] == doctest::Approx(13.5f));
}

TEST_CASE("navi sample: her turn is the puppet's turned by the sample's") {
    // A quarter turn about y for her, relative to her Link; the puppet unturned: hers exactly.
    const float h = std::sqrt(0.5f);
    const auto m = Sampled(1, {0, 1, 0}, {0.0f, h, 0.0f, h});
    Target p;
    const auto r = Follower::SampledTurn(p, m);
    CHECK(r[0] == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(r[2] == doctest::Approx(1.0f));
    CHECK(r[6] == doctest::Approx(-1.0f));
    // The puppet half turned: both turns, three quarters.
    p.rotation = {-1, 0, 0, 0, 1, 0, 0, 0, -1};
    const auto r2 = Follower::SampledTurn(p, m);
    CHECK(r2[2] == doctest::Approx(-1.0f));
    CHECK(r2[6] == doctest::Approx(1.0f));
}

TEST_CASE("navi sample: an adult's fairy gets no shoulder scale, only her own place") {
    Follower f;
    Input in = Puppets(3, {0, 0, 0});
    in.targets[0].actor = "CoopPuppet_Adult";
    const auto out = f.Follow(in, 1000, ~0u, std::nullopt, {{3, Sampled(3, {0.3f, 1.6f, 0.2f})}});
    CHECK(out.targets[0].position[0] == doctest::Approx(0.3f));
    CHECK(out.targets[0].position[1] == doctest::Approx(1.6f));
    CHECK(out.targets[0].position[2] == doctest::Approx(0.2f));
}

TEST_CASE("navi sample: their Navi not out parks ours of them, her puppet's own hide still wins") {
    Follower f(Still());
    const std::map<std::uint8_t, proto::NaviSample> hidden{{2, proto::NaviSample{2, 0, {}, {0, 0, 0, 1}}}};
    auto out = f.Follow(Puppets(2, {10, 0, 5}), 1000, ~0u, std::nullopt, hidden);
    CHECK(out.targets[0].show == CoopPuppet::Show::hide);
    CHECK(std::string(out.targets[0].why) == "her player's Navi is not out");
    Input in = Puppets(2, {10, 0, 5});
    in.targets[0].show = CoopPuppet::Show::hide;
    in.targets[0].why = "our cutscene";
    out = f.Follow(in, 1100, ~0u, std::nullopt, hidden);
    CHECK(std::string(out.targets[0].why) == "our cutscene");
    // Out again: shown at her place.
    out = f.Follow(Puppets(2, {10, 0, 5}), 1200, ~0u, std::nullopt, {{2, Sampled(2, {0, 1, 0})}});
    CHECK(out.targets[0].show == CoopPuppet::Show::show);
    CHECK(out.targets[0].position[1] == doctest::Approx(1.0f));
}

TEST_CASE("navi sample: no sample (an older build) is the shoulder place, logged once per change") {
    std::vector<std::string> lines;
    Follower f(Still(), {}, [&](const std::string& l) { lines.push_back(l); });
    auto out = f.Follow(Puppets(2, {10, 0, 5}), 1000);
    CHECK(out.targets[0].position[0] == doctest::Approx(10 + Still().offset[0]));
    f.Follow(Puppets(2, {10, 0, 5}), 1033);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("no NAVI_SAMPLE from P2") != std::string::npos);
    f.Follow(Puppets(2, {10, 0, 5}), 1066, ~0u, std::nullopt, {{2, Sampled(2, {0, 1, 0})}});
    REQUIRE(lines.size() == 2);
    CHECK(lines[1].find("P2's Navi placed from their game") != std::string::npos);
}

TEST_CASE("navi sample: a call still flies her up over their head") {
    Settings s = Still();
    s.call_bob = 0.0f;
    Follower f(s);
    f.Call(2, 1000);
    float y = 0.0f;
    for (std::uint64_t ms = 1000; ms < 3000; ms += 50) {
        y = f.Follow(Puppets(2, {0, 0, 0}), ms, ~0u, std::nullopt, {{2, Sampled(2, {0.4f, 0.9f, 0})}})
                .targets[0].position[1];
    }
    CHECK(y == doctest::Approx(s.call_offset[1]).epsilon(0.02));
}

TEST_CASE("navi samples: kept per player, stale after kStaleMs, forgotten when the player leaves") {
    NaviSamples k;
    k.Receive(Sampled(2, {0, 1, 0}), 1000);
    k.Receive(Sampled(0, {0, 1, 0}), 1000);  // our own (unrelayed): never kept
    k.Receive(Sampled(4, {1, 1, 0}), 1200);
    auto fresh = k.Fresh(1000 + NaviSamples::kStaleMs - 1);
    CHECK(fresh.size() == 2);
    CHECK(fresh.count(0) == 0);
    fresh = k.Fresh(1000 + NaviSamples::kStaleMs);
    CHECK(fresh.size() == 1);
    CHECK(fresh.count(4) == 1);
    k.Forget(4);
    CHECK(k.Fresh(1300).empty() == false);  // player 2 still fresh at 1300
    CHECK(k.Fresh(1300).count(4) == 0);
}

TEST_CASE("navi sender: her place and turn in our Link's frame, out or not, at its rates") {
    NaviSender s;
    NaviSeen seen;
    seen.present = true;
    seen.position = {10.0f, 1.0f, 4.0f};  // 1 m up, 1 m behind (world -z) our Link at (10, 0, 5)
    seen.rotation = {-1, 0, 0, 0, 1, 0, 0, 0, -1};  // turned as our Link is
    // Our Link turned half about y: world -z is his forward +z, world x his -x.
    const CoopPuppet::Quat half{0.0f, 1.0f, 0.0f, 0.0f};
    auto m = s.Tick(seen, CoopPuppet::Vec3{10, 0, 5}, half, 0);
    REQUIRE(m);
    CHECK(m->flags == proto::kNaviShown);
    CHECK(m->offset[0] == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(m->offset[1] == doctest::Approx(1.0f));
    CHECK(m->offset[2] == doctest::Approx(1.0f));
    // Her own turn the same as his: identity relative to him.
    CHECK(std::fabs(m->rotation[3]) == doctest::Approx(1.0f).epsilon(1e-4));
    // The same again within kStillEveryMs: nothing; moving: every kEveryMs.
    CHECK_FALSE(s.Tick(seen, CoopPuppet::Vec3{10, 0, 5}, half, NaviSender::kEveryMs));
    CHECK(s.Tick(seen, CoopPuppet::Vec3{10, 0, 5}, half, NaviSender::kStillEveryMs));
    const std::uint64_t t = NaviSender::kStillEveryMs;
    CHECK_FALSE(s.Tick(seen, CoopPuppet::Vec3{10.5f, 0, 5}, half, t + NaviSender::kEveryMs - 1));
    CHECK(s.Tick(seen, CoopPuppet::Vec3{10.5f, 0, 5}, half, t + NaviSender::kEveryMs));
    // Not out: sent at once, as zero offset and identity (a message the codec takes), then at the still rate.
    seen.present = false;
    m = s.Tick(seen, CoopPuppet::Vec3{10.5f, 0, 5}, half, t + NaviSender::kEveryMs + 1);
    REQUIRE(m);
    CHECK(m->flags == 0);
    CHECK(m->offset == CoopPuppet::Vec3{0, 0, 0});
    CHECK(m->rotation == proto::Quat{0, 0, 0, 1});
    CHECK_FALSE(s.Tick(seen, CoopPuppet::Vec3{10.5f, 0, 5}, half, t + 100));
    CHECK(s.Tick(seen, CoopPuppet::Vec3{10.5f, 0, 5}, half, t + NaviSender::kEveryMs + 1 + NaviSender::kStillEveryMs));
    // Our place unread: nothing (her place would be a guess).
    seen.present = true;
    CHECK_FALSE(s.Tick(seen, std::nullopt, half, 100000));
    // Far (a Z-target beyond kMaxNaviOffset): held at the bound, never a refused message.
    seen.position = {10.0f, 0.0f, 500.0f};
    m = s.Tick(seen, CoopPuppet::Vec3{10, 0, 5}, std::nullopt, 200000);
    REQUIRE(m);
    CHECK(m->offset[2] == doctest::Approx(proto::kMaxNaviOffset));
    std::vector<std::uint8_t> bytes;
    CHECK(proto::encode(proto::Message{*m}, bytes) == proto::Error::none);
}
