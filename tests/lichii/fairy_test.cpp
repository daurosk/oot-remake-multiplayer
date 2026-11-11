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

TEST_CASE("fairy: parked while her puppet is far from our Link, with some slack (issue 145)") {
    Settings s = Still();
    Follower f(s);
    const std::uint32_t live = 1u << 1;
    const CoopPuppet::Vec3 us{0, 0, 0};
    auto show_at = [&](float x, std::uint64_t now) {
        return f.Follow(Puppets(1, {x, 0, 0}), now, live, us).targets.at(0).show;
    };
    CHECK(show_at(10, 0) == CoopPuppet::Show::show);
    CHECK(show_at(s.far - 1, 33) == CoopPuppet::Show::show);
    CHECK(show_at(40, 66) == CoopPuppet::Show::hide);  // the compass room, two floors off
    CHECK(std::string(f.Follow(Puppets(1, {40, 0, 0}), 99, live, us).targets[0].why) ==
          "her puppet is far away");
    // Coming back: still parked between near and far, shown once nearer.
    CHECK(show_at(s.far - 1, 133) == CoopPuppet::Show::hide);
    CHECK(show_at(s.near - 1, 166) == CoopPuppet::Show::show);
    // Vertical distance counts (another floor right above).
    CHECK(show_at(0, 200) == CoopPuppet::Show::show);
    CHECK(f.Follow(Puppets(1, {0, 35, 0}), 233, live, us).targets[0].show ==
          CoopPuppet::Show::hide);
    // Without our position, or with the rule off: no distance rule.
    CHECK(f.Follow(Puppets(1, {90, 0, 0}), 266, live).targets[0].show == CoopPuppet::Show::show);
    s.far = 0.0f;
    Follower off(s);
    CHECK(off.Follow(Puppets(1, {90, 0, 0}), 0, live, us).targets[0].show == CoopPuppet::Show::show);
}
