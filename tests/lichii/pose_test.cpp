// Tests for the pose copy's math and its track (lichii/hook/coop_pose.cpp).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "coop_pose.h"
#include "coop_puppet.h"

namespace {

using CoopPose::Quat;
using CoopPose::Rows;

bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

bool SameRotation(const Quat& a, const Quat& b) {
    const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    return Near(std::fabs(dot), 1.0f, 1e-3f);
}

CoopPose::Pose OneBone(const Quat& q, float x) {
    CoopPose::Pose p;
    p.skeleton = 7;
    CoopPose::BonePose b;
    b.bone = 3;
    b.rotation = q;
    b.translation = CoopPose::Vec3{x, 0, 0};
    p.bones = {b};
    return p;
}

}  // namespace

TEST_CASE("pose: matrix and quaternion round trip") {
    const float s = std::sqrt(0.5f);
    for (const Quat q : {Quat{0, 0, 0, 1}, Quat{s, 0, 0, s}, Quat{0, 1, 0, 0}, Quat{0, 0, 1, 0},
                         Quat{1, 0, 0, 0}, Quat{0.1f, -0.7f, 0.3f, 0.64f}}) {
        CHECK(SameRotation(CoopPose::RowsToQuat(CoopPose::QuatToRows(q)),
                           CoopPose::Unpack(CoopPose::Pack(q))));
    }
    // 90 degrees about z turns x into y: column vectors, rows as stored.
    const Rows r = CoopPose::QuatToRows({0, 0, s, s});
    CHECK(Near(r[0], 0));
    CHECK(Near(r[1], -1));
    CHECK(Near(r[3], 1));
    CHECK(CoopPose::RowsToQuat(r)[3] >= 0.0f);
}

TEST_CASE("pose: packing keeps the sign and the size") {
    const auto p = CoopPose::Pack({0, 0, 0, -2});
    CHECK(p[3] == -32767);
    CHECK(CoopPose::Unpack({0, 0, 0, 0})[3] == 1.0f);
}

TEST_CASE("pose: nlerp takes the short way") {
    const Quat a{0, 0, 0, 1};
    const Quat b{0, 0, 0, -1};  // the same rotation
    CHECK(SameRotation(CoopPose::Nlerp(a, b, 0.5f), a));
}

TEST_CASE("pose: the track blends the last two over their interval") {
    CoopPose::Track t;
    CHECK_FALSE(t.At(0));
    const float s = std::sqrt(0.5f);
    t.Push(OneBone({0, 0, 0, 1}, 0.0f), 1000);
    CHECK(t.At(1000)->bones[0].translation->at(0) == 0.0f);
    t.Push(OneBone({0, 0, s, s}, 2.0f), 1040);
    const auto mid = t.At(1060);
    REQUIRE(mid);
    CHECK(Near(mid->bones[0].translation->at(0), 1.0f));
    CHECK(SameRotation(*mid->bones[0].rotation, CoopPose::Nlerp({0, 0, 0, 1}, {0, 0, s, s}, 0.5f)));
    CHECK(Near(t.At(2000)->bones[0].translation->at(0), 2.0f));
    // Another skeleton starts over.
    auto other = OneBone({0, 0, 0, 1}, 5.0f);
    other.skeleton = 8;
    t.Push(other, 1100);
    CHECK(Near(t.At(1101)->bones[0].translation->at(0), 5.0f));
}

TEST_CASE("pose: translations and scales pack in 1/8192") {
    const auto p = CoopPose::PackUnit({0.5838f, -1.0f, 9.0f});
    CHECK(p[0] == 4782);
    CHECK(p[1] == -8192);
    CHECK(p[2] == 32767);  // clamped
    CHECK(Near(CoopPose::UnpackUnit(p)[0], 0.5838f, 1e-3f));
}

TEST_CASE("pose: a different bone list starts the blend over") {
    CoopPose::Track t;
    t.Push(OneBone({0, 0, 0, 1}, 0.0f), 0);
    auto other = OneBone({0, 0, 0, 1}, 4.0f);
    other.bones[0].bone = 9;  // bone 3 is gone: nothing to blend from
    t.Push(other, 40);
    CHECK(Near(t.At(50)->bones[0].translation->at(0), 4.0f));
}

// puppet-smooth-a-2: a pose that only adds a bone (one that moved for the first time, the rest of
// a first pose the budget split) keeps the blend; starting over held the puppet, then jumped it.
TEST_CASE("pose: a pose that adds a bone keeps the blend (timed and untimed)") {
    auto two = OneBone({0, 0, 0, 1}, 4.0f);
    two.bones.push_back(two.bones[0]);
    two.bones[1].bone = 9;
    two.bones[1].translation = CoopPose::Vec3{7.0f, 0, 0};
    CoopPose::Track t;
    t.Push(OneBone({0, 0, 0, 1}, 0.0f), 0);
    t.Push(two, 40);
    auto at = t.At(60);
    REQUIRE(at);
    REQUIRE(at->bones.size() == 2);
    CHECK(Near(at->bones[0].translation->at(0), 2.0f));  // half way, not the newest at once
    CHECK(Near(at->bones[1].translation->at(0), 7.0f));  // the new bone as first heard

    CoopPose::Track timed;
    timed.Push(OneBone({0, 0, 0, 1}, 0.0f), 1000, 1000);
    timed.Push(two, 1040, 1040);
    at = timed.At(1020 + CoopPose::kShowDelayMs);
    REQUIRE(at);
    REQUIRE(at->bones.size() == 2);
    CHECK(Near(at->bones[0].translation->at(0), 2.0f));
    CHECK(Near(at->bones[1].translation->at(0), 7.0f));

    // A part a kept bone did not have is added the same way.
    auto more = two;
    more.bones[1].scale = CoopPose::Vec3{1.0f, 1.0f, 1.0f};
    timed.Push(more, 1080, 1080);
    at = timed.At(1060 + CoopPose::kShowDelayMs);
    REQUIRE(at);
    REQUIRE(at->bones.size() == 2);
    CHECK(Near(at->bones[0].translation->at(0), 4.0f));
    CHECK(at->bones[1].scale.has_value());
}

namespace {

// Guest memory as a few flat regions.
class FakeGuest : public CoopPuppet::Guest {
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

constexpr std::uint64_t kMain = 0x80000000;
constexpr std::uint32_t kBones = 8;
namespace paddr = CoopPose::addr;

// An actor whose skeleton has `count` identity bones; returns the local array.
std::uint64_t MakeActor(FakeGuest& g, std::uint64_t actor, std::string_view model = {},
                        std::uint32_t count = kBones) {
    const std::uint64_t parts = actor + 0x1000, first = actor + 0x2000, second = actor + 0x3000,
                        holder = actor + 0x4000, skel = actor + 0x5000, local = actor + 0x10000,
                        res = actor + 0x7000;
    for (std::uint64_t at : {actor, parts, first, second, holder, skel, res}) g.Map(at, 0x400);
    g.Map(local, count * paddr::kLocalStride);
    g.Put(actor + 0x2c, std::uint32_t{4});
    g.Put(actor + paddr::kSkeletonPath[0], parts);
    g.Put(parts + paddr::kSkeletonPath[1], first);
    g.Put(first + paddr::kSkeletonPath[2], second);
    g.Put(second + paddr::kSkeletonPath[3], holder);
    g.Put(holder + paddr::kSkeletonPath[4], skel);
    g.Put(holder + paddr::kModelResource, res);
    g.Put(res, std::array<char, 4>{'F', 'M', 'D', 'L'});
    if (!model.empty()) {
        const std::uint64_t name = res + 0x100;
        g.Put(res + paddr::kModelName, name);
        g.Put(name, static_cast<std::uint16_t>(model.size()));
        REQUIRE(g.Write(name + 2, model.data(), model.size()));
    }
    g.Put(skel + paddr::kBoneCount, static_cast<std::uint16_t>(count));
    g.Put(skel + paddr::kLocalArray, local);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint64_t b = local + i * paddr::kLocalStride;
        g.Put(b + 0x10, CoopPose::Vec3{1, 1, 1});
        g.Put(b + 0x20, 1.0f);
        g.Put(b + 0x34, 1.0f);
        g.Put(b + 0x48, 1.0f);
    }
    return local;
}

// The scene module's chain down to the local player, as PlayerReader walks it.
void MakePlayerChain(FakeGuest& g, std::uint64_t actor) {
    const std::uint64_t scene = 0x500000000, table = scene + 0x1000, array = scene + 0x2000,
                        manager = scene + 0x3000, link = scene + 0x4000, holder = scene + 0x5000;
    g.Map(kMain + 0x4e97000, 0x1000);
    for (std::uint64_t at : {scene, table, array, manager, link, holder}) g.Map(at, 0x400);
    g.Put(kMain + 0x4e97648, scene);
    g.Put(scene + 0x1f0, table);
    g.Put(table + 0x50, std::uint32_t{0x10});
    g.Put(table + 0x58, array);
    g.Put(array + 0x78, manager);
    g.Put(manager + 0x30, std::uint64_t{1});
    g.Put(manager + 0x40, link);
    g.Put(link + 0x10, std::uint32_t{5});
    g.Put(link + 0x8, holder);
    g.Put(holder + 0x48, std::uint32_t{5});
    g.Put(holder + 0x40, actor);
}

}  // namespace

TEST_CASE("pose: the local skeleton's moving bones reach the debug puppet") {
    FakeGuest g;
    const std::uint64_t player = 0x600000000, puppet = 0x700000000;
    const std::uint64_t mine = MakeActor(g, player);
    const std::uint64_t theirs = MakeActor(g, puppet);
    MakePlayerChain(g, player);
    std::vector<std::string> lines;
    CoopPose::SetLog([&](const std::string& l) { lines.push_back(l); }, true);
    const std::vector<std::pair<std::uint8_t, std::uint64_t>> puppets{{0, puppet}};

    CoopPose::OnCalcFrame(g, kMain, puppets, 1000);  // the first read: nothing moves yet
    const std::uint64_t bone5 = mine + 5 * paddr::kLocalStride;
    const float s = std::sqrt(0.5f);
    const CoopPose::Rows r = CoopPose::QuatToRows({0, 0, s, s});
    for (int row = 0; row < 3; ++row) {
        g.Put(bone5 + 0x20 + row * 16, CoopPose::Vec3{r[row * 3], r[row * 3 + 1], r[row * 3 + 2]});
    }
    g.Put(bone5 + 0x50, CoopPose::Vec3{0.1f, 0.2f, 0.3f});
    g.Put(bone5, std::uint32_t{0x3800000});
    CoopPose::OnCalcFrame(g, kMain, puppets, 1016);

    const std::uint64_t copy5 = theirs + 5 * paddr::kLocalStride;
    CHECK(Near(g.Get<float>(copy5 + 0x24), r[1]));
    CHECK(Near(g.Get<float>(copy5 + 0x30), r[3]));
    CHECK(Near(g.Get<float>(copy5 + 0x54), 0.2f));
    CHECK(g.Get<std::uint32_t>(copy5) == 0x3800000);
    // A bone that never moved is left alone.
    CHECK(g.Get<float>(theirs + 4 * paddr::kLocalStride + 0x20) == 1.0f);

    // Nothing wrote over it: the next frame counts it kept.
    CoopPose::OnCalcFrame(g, kMain, puppets, 2100);
    REQUIRE_FALSE(lines.empty());
    CHECK(lines.back().find("8 bones, 1 moving") != std::string::npos);
    CHECK(lines.back().find("kept 1, overwritten 0") != std::string::npos);
    CoopPose::SetLog({}, false);
}

TEST_CASE("pose: a pose of another model is not written") {
    FakeGuest g;
    const std::uint64_t player = 0x600000000, puppet = 0x700000000;
    const std::uint64_t mine = MakeActor(g, player, "Player_Child");
    const std::uint64_t theirs = MakeActor(g, puppet, "Player_Adult");
    MakePlayerChain(g, player);
    std::vector<std::string> lines;
    CoopPose::SetLog([&](const std::string& l) { lines.push_back(l); }, true);
    const std::vector<std::pair<std::uint8_t, std::uint64_t>> puppets{{0, puppet}};
    CoopPose::OnCalcFrame(g, kMain, puppets, 5000);
    g.Put(mine + 2 * paddr::kLocalStride + 0x50, CoopPose::Vec3{0.5f, 0, 0});
    CoopPose::OnCalcFrame(g, kMain, puppets, 5016);
    CHECK(g.Get<float>(theirs + 2 * paddr::kLocalStride + 0x50) == 0.0f);  // untouched
    CoopPose::OnCalcFrame(g, kMain, puppets, 6100);
    REQUIRE_FALSE(lines.empty());
    CHECK(lines.back().find("Player_Child 8 bones") != std::string::npos);
    CHECK(lines.back().find("0 written/s") != std::string::npos);
    CoopPose::SetLog({}, false);
    CHECK(CoopPose::SkeletonId(309, "Player_Child") != CoopPose::SkeletonId(309, "Player_Adult"));
    CHECK(CoopPose::BoneCountOf(CoopPose::SkeletonId(309, "Player_Adult")) == 309);
}

TEST_CASE("pose: CopySkeleton copies every bone between skeletons of one size") {
    FakeGuest g;
    const std::uint64_t navi = 0x600000000, fairy = 0x700000000, link = 0x800000000;
    const std::uint64_t from = MakeActor(g, navi, "Npc_Navi");
    const std::uint64_t to = MakeActor(g, fairy, "Npc_Navi");
    g.Put(from + 3 * paddr::kLocalStride + 0x50, CoopPose::Vec3{0.25f, 0.5f, 0});
    g.Put(from + 5 * paddr::kLocalStride + 0x24, 0.75f);  // a rotation entry
    std::string note;
    REQUIRE(CoopPose::CopySkeleton(g, navi, fairy, &note));
    CHECK(note == "Npc_Navi 8 bones -> Npc_Navi 8 bones");
    CHECK(g.Get<float>(to + 3 * paddr::kLocalStride + 0x54) == 0.5f);
    CHECK(g.Get<float>(to + 5 * paddr::kLocalStride + 0x24) == 0.75f);

    // No skeleton on one side: nothing copied, and why.
    g.Map(link, 0x400);
    CHECK_FALSE(CoopPose::CopySkeleton(g, link, fairy, &note));
    CHECK(note == "no skeleton on the source");
}

namespace {

// `piece` in `actor`'s hand slot `slot`, in `state` (4 out, 6 put away).
void Hold(FakeGuest& g, std::uint64_t actor, std::uint32_t slot, std::uint64_t piece,
          std::uint32_t state = 4) {
    namespace pa = CoopPuppet::addr;
    const std::uint64_t parts = actor + 0x1000;  // as MakeActor maps it
    const std::uint64_t eu = actor + 0x8000, holder = actor + 0x9000;
    std::uint8_t probe = 0;
    if (!g.Read(eu, &probe, 1)) g.Map(eu, 0x400);  // once: Map clears it
    if (!g.Read(holder, &probe, 1)) g.Map(holder, 0x400);
    g.Put(parts + pa::kPartsEquipmentUser, eu);
    for (std::uint32_t s = 0; s < 8; ++s) {
        if (g.Get<std::uint32_t>(eu + pa::kEquipSlots + pa::kEquipSlotStride * s + pa::kEquipSlotId) == 0) {
            g.Put(eu + pa::kEquipSlots + pa::kEquipSlotStride * s + pa::kEquipSlotId, 0xFFFFFFFFu);
        }
    }
    const std::uint64_t link = eu + pa::kEquipSlots + pa::kEquipSlotStride * slot;
    g.Put(link + pa::kEquipSlotId, std::uint32_t{0x40 + slot});
    g.Put(link + pa::kEquipSlotHolder, holder + 0x40 * slot);
    g.Put(holder + 0x40 * slot + pa::kHolderActor, piece);
    g.Put(piece + pa::kActorState, state);
}

}  // namespace

TEST_CASE("pose: the piece in a hand goes as a pose of its own skeleton (issue 158)") {
    FakeGuest g;
    const std::uint64_t player = 0x600000000, puppet = 0x700000000;
    const std::uint64_t sling = 0x610000000, copy = 0x710000000, sword = 0x620000000;
    MakeActor(g, player, "Player_Child");
    MakeActor(g, puppet, "Player_Child");
    const std::uint64_t mine = MakeActor(g, sling, "Weapon_Range_SlingShot_t158");
    const std::uint64_t theirs = MakeActor(g, copy, "Weapon_Range_SlingShot_t158");
    MakeActor(g, sword, "Weapon_Sword_t158");
    MakePlayerChain(g, player);
    Hold(g, player, 3, sling);
    Hold(g, player, 0, sword);  // the sword is not a piece that is sent
    Hold(g, puppet, 3, copy);
    CoopPose::SetLog({}, false);
    const std::uint32_t id = CoopPose::SkeletonId(kBones, "Weapon_Range_SlingShot_t158");
    const std::vector<std::pair<std::uint8_t, std::uint64_t>> puppets{{3, puppet}};

    // Ours is pulled back: bone 2 moved, and the puppet's copy is still at rest.
    g.Put(mine + 2 * paddr::kLocalStride + 0x50, CoopPose::Vec3{0.0f, -0.3f, 0.0f});
    CoopPose::OnCalcFrame(g, kMain, puppets, 100'000);
    auto out = CoopPose::TakeOutgoing(5, 100'000);
    REQUIRE(out.size() == 2);  // Link's pose, then the slingshot's
    CHECK(out[1].skeleton == id);
    CHECK(out[1].bones.size() == kBones);  // every bone of a piece
    for (auto& m : out) {
        m.player_number = 3;
        CoopPose::Receive(m, 100'000);
    }
    CoopPose::OnCalcFrame(g, kMain, puppets, 100'016);
    CHECK(Near(g.Get<float>(theirs + 2 * paddr::kLocalStride + 0x54), -0.3f));

    // Unchanged: not sent again until kPieceRefreshMs has gone.
    CoopPose::OnCalcFrame(g, kMain, puppets, 100'070);  // one send interval (15/s) on
    CHECK(CoopPose::TakeOutgoing(5, 100'070).size() == 1);
    CoopPose::OnCalcFrame(g, kMain, puppets, 100'000 + CoopPose::kPieceRefreshMs + 10);
    CHECK(CoopPose::TakeOutgoing(5, 100'000 + CoopPose::kPieceRefreshMs + 10).size() == 2);

    // Shot: the band goes back. The puppet's copy lets go too.
    g.Put(mine + 2 * paddr::kLocalStride + 0x50, CoopPose::Vec3{0.0f, 0.0f, 0.0f});
    CoopPose::OnCalcFrame(g, kMain, puppets, 100'400);
    out = CoopPose::TakeOutgoing(5, 100'400);
    REQUIRE(out.size() == 2);
    for (auto& m : out) {
        m.player_number = 3;
        CoopPose::Receive(m, 100'400);
    }
    CoopPose::OnCalcFrame(g, kMain, puppets, 100'800);  // one interval on: the newest
    CHECK(Near(g.Get<float>(theirs + 2 * paddr::kLocalStride + 0x54), 0.0f));

    // Put away (state 6): neither sent nor written.
    Hold(g, player, 3, sling, 6);
    Hold(g, puppet, 3, copy, 6);
    g.Put(theirs + 2 * paddr::kLocalStride + 0x54, 0.7f);
    CoopPose::OnCalcFrame(g, kMain, puppets, 101'000);
    CHECK(CoopPose::TakeOutgoing(5, 101'000).size() == 1);
    CoopPose::OnCalcFrame(g, kMain, puppets, 101'016);
    CHECK(g.Get<float>(theirs + 2 * paddr::kLocalStride + 0x54) == 0.7f);
    // Poses still came lately: the puppet plays no AS command of its own.
    CHECK(CoopPose::Drives(3, 101'016));
}

TEST_CASE("pose: a bone our game holds still at its own value reaches the puppet whole (issue 152)") {
    // held-piece: only bones that moved since the first read went in the moving pose, so a bone held
    // still away from the puppet's value (Weapon_R, the Ocarina's hand bone) was never sent and the
    // Ocarina hung turned. Every bone also goes whole now and then (StillPicker).
    FakeGuest g;
    const std::uint64_t player = 0x600000000, puppet = 0x700000000;
    const std::uint64_t mine = MakeActor(g, player, "Player_t152");  // a new skeleton: a first read of its own
    const std::uint64_t theirs = MakeActor(g, puppet, "Player_t152");
    MakePlayerChain(g, player);
    CoopPose::SetLog({}, false);
    const std::vector<std::pair<std::uint8_t, std::uint64_t>> puppets{{4, puppet}};
    // Bone 4 is held at a turn of its own from the first read on: it never "moves".
    const std::uint64_t bone4 = mine + 4 * paddr::kLocalStride;
    const float h = std::sqrt(0.5f);
    const CoopPose::Rows r = CoopPose::QuatToRows({h, 0, 0, h});
    for (int row = 0; row < 3; ++row) {
        g.Put(bone4 + 0x20 + row * 16, CoopPose::Vec3{r[row * 3], r[row * 3 + 1], r[row * 3 + 2]});
    }
    g.Put(bone4 + 0x50, CoopPose::Vec3{0.0f, 0.25f, 0.0f});
    CoopPose::OnCalcFrame(g, kMain, puppets, 200'000);
    auto out = CoopPose::TakeOutgoing(5, 200'000);
    REQUIRE_FALSE(out.empty());
    bool has4 = false;
    for (const auto& b : out[0].bones) {
        if (b.bone == 4) {
            has4 = b.rotation.has_value() && b.translation.has_value() && b.scale.has_value();
        }
    }
    CHECK(has4);  // whole, though it never moved
    // puppet-budget: the whole bones are inside the send's byte budget, with the moving ones.
    std::size_t send_bytes = 0;
    for (const auto& m : out) send_bytes += CoopPose::WireBytes(m);
    CHECK(send_bytes <= CoopPose::PlanFor(0).bytes);
    for (auto& m : out) {
        m.player_number = 4;
        CoopPose::Receive(m, 200'000);
    }
    CoopPose::OnCalcFrame(g, kMain, puppets, 200'016);
    const std::uint64_t copy4 = theirs + 4 * paddr::kLocalStride;
    CHECK(Near(g.Get<float>(copy4 + 0x24), r[1]));
    CHECK(Near(g.Get<float>(copy4 + 0x28), r[2]));
    CHECK(Near(g.Get<float>(copy4 + 0x54), 0.25f));

    // Sent again only after kStillRefreshMs.
    CoopPose::StillPicker picker;
    std::vector<coop::protocol::PoseBone> all(20);
    for (std::size_t i = 0; i < all.size(); ++i) all[i].bone = static_cast<std::uint16_t>(i);
    CHECK(picker.Pick(1, all, 1000).size() == CoopPose::kStillBones);
    CHECK(picker.Pick(1, all, 1066).front().bone == CoopPose::kStillBones);  // the next ones, never sent
    CHECK(picker.Pick(1, all, 1133).size() == all.size() - 2 * CoopPose::kStillBones);
    CHECK(picker.Pick(1, all, 1200).empty());
    CHECK(picker.Pick(1, all, 1000 + CoopPose::kStillRefreshMs).size() == CoopPose::kStillBones);
    // Merged with the moving pick: one list in bone order, a bone in both once.
    std::vector<coop::protocol::PoseBone> moving(2), whole(2);
    moving[0].bone = 1; moving[1].bone = 5;
    whole[0].bone = 3; whole[1].bone = 5; whole[1].scale.emplace();
    const auto merged = CoopPose::MergeBones(moving, whole);
    REQUIRE(merged.size() == 3);
    CHECK(merged[0].bone == 1);
    CHECK(merged[1].bone == 3);
    CHECK(merged[2].bone == 5);
    CHECK(merged[2].scale.has_value());
}

TEST_CASE("pose: Fill keeps a part a partial pose leaves out (a whole bone sent earlier)") {
    CoopPose::Track track;
    CoopPose::Pose whole;
    whole.skeleton = 9;
    CoopPose::BonePose b;
    b.bone = 2;
    b.rotation = CoopPose::Quat{0, 0, 0, 1};
    b.translation = CoopPose::Vec3{0.5f, 0, 0};
    b.scale = CoopPose::Vec3{1, 1, 1};
    whole.bones.push_back(b);
    track.Push(whole, 100);
    CoopPose::Pose part;
    part.skeleton = 9;
    CoopPose::BonePose p;
    p.bone = 2;
    p.rotation = CoopPose::Quat{1, 0, 0, 0};
    part.bones.push_back(p);
    const auto got = track.Fill(part);
    REQUIRE(got.bones.size() == 1);
    CHECK((*got.bones[0].rotation)[0] == 1.0f);       // the new part
    REQUIRE(got.bones[0].translation.has_value());     // the old one kept
    CHECK((*got.bones[0].translation)[0] == 0.5f);
    CHECK(got.bones[0].scale.has_value());
}

TEST_CASE("pose: poses of the highest player number are kept (16 players), one past it is not") {
    CoopPose::SetLog({}, false);
    const std::uint8_t top = coop::protocol::kMaxPlayers;
    for (const std::uint8_t n : {top, static_cast<std::uint8_t>(top + 1)}) {
        coop::protocol::PlayerPose m;
        m.player_number = n;
        m.skeleton = 0x16161616;
        m.bones.push_back(coop::protocol::PoseBone{1, 0, std::array<std::int16_t, 4>{0, 0, 0, 32767}, std::nullopt, std::nullopt});
        CoopPose::Receive(m, 900'000);
        CHECK(CoopPose::Drives(n, 900'010) == (n == top));
    }
}

namespace {
namespace proto16 = coop::protocol;

// A Link-sized skeleton in motion: 300 moving bones, each turning a little
// every frame (bone i by i % 40 packed units a frame: limbs much, fingers
// little), a third with a moving translation too, as a run plays.
std::vector<proto16::PoseBone> Running(int frame) {
    std::vector<proto16::PoseBone> bones;
    for (std::uint16_t i = 0; i < 300; ++i) {
        proto16::PoseBone b;
        b.bone = i;
        const auto turn = static_cast<std::int16_t>((frame * (i % 40)) % 20000);
        b.rotation = std::array<std::int16_t, 4>{turn, 0, 0, 30000};
        if (i % 3 == 0) b.translation = std::array<std::int16_t, 3>{turn, 0, 0};
        bones.push_back(b);
    }
    return bones;
}

std::size_t Bytes(std::vector<proto16::PoseBone> bones) {
    proto16::PlayerPose m;
    m.skeleton = 0x1234;
    m.bones = std::move(bones);
    std::vector<std::uint8_t> out;
    REQUIRE(proto16::encode(proto16::Message{m}, out) == proto16::Error::none);
    return out.size();
}

CoopPose::Pose Unpacked(const std::vector<proto16::PoseBone>& bones) {
    CoopPose::Pose p;
    p.skeleton = 0x1234;
    for (const auto& b : bones) {
        CoopPose::BonePose o;
        o.bone = b.bone;
        o.flags = b.flags;
        if (b.rotation) o.rotation = CoopPose::Unpack(*b.rotation);
        if (b.translation) o.translation = CoopPose::UnpackUnit(*b.translation);
        p.bones.push_back(o);
    }
    return p;
}
}  // namespace

TEST_CASE("pose: 16 players: a running Link costs about 13 KB/s, not 160 (bone budget, 15/s)") {
    // Before: every moving bone, 30 times a second.
    std::size_t before = 0;
    for (int f = 0; f < 60; f += 2) before += Bytes(Running(f));  // 30 sends in 1 s of 60 frames
    // Now: 15 sends a second, at most kFloorSendBytes each (48 bones of 18 bytes).
    CoopPose::BonePicker picker;
    std::size_t now = 0;
    for (int f = 0; f < 60; f += 4) {
        const auto sent = picker.Pick(0x1234, Running(f), static_cast<std::uint64_t>(1000 + f * 1000 / 60));
        CHECK(Bytes(sent) <= CoopPose::kFloorSendBytes);
        now += Bytes(sent);
    }
    MESSAGE("pose bytes per second: before " << before << ", now " << now);
    CHECK(before > 100'000);
    CHECK(now < 16'000);
    // 16 players together: each game sends to 15, the host relays 16 x 15.
    MESSAGE("16 together, host relays 16 x 15: " << 16 * 15 * now * 8 / 1'000'000 << " Mbit/s");
    CHECK(16 * 15 * now * 8 < 32'000'000);
}

TEST_CASE("pose: the receiver keeps the bones a pose leaves out, and catches up once still") {
    CoopPose::BonePicker picker;
    CoopPose::Track track;
    std::uint64_t ms = 5000;
    auto send = [&](const std::vector<proto16::PoseBone>& full) {
        const auto sent = picker.Pick(0x1234, full, ms);
        CHECK(Bytes(sent) <= CoopPose::kFloorSendBytes);  // the default budget: the floor's bytes
        CoopPose::Pose got = track.Fill(Unpacked(sent));
        track.Push(std::move(got), ms);
        ms += 1000 / CoopPose::kSendHz;
    };
    // The first pose: only what the budget holds of 300, the rest follow in later ones.
    send(Running(0));
    REQUIRE(track.At(ms));
    CHECK(track.At(ms)->bones.size() < 300);
    for (int f = 4; f < 120; f += 4) send(Running(f));
    // Moving: every bone has been sent; the fast ones are near.
    auto shown = *track.At(ms + 1000);
    REQUIRE(shown.bones.size() == 300);
    // Still: within the refresh time every bone matches the sender exactly.
    const auto still = Running(120);
    const std::uint64_t sends = 2 * CoopPose::kBoneRefreshMs * CoopPose::kSendHz / 1000 + 1;
    for (std::uint64_t k = 0; k < sends; ++k) send(still);
    shown = *track.At(ms + 1000);
    const auto want = Unpacked(still);
    REQUIRE(shown.bones.size() == want.bones.size());
    for (std::size_t i = 0; i < want.bones.size(); ++i) {
        INFO("bone " << i);
        CHECK(shown.bones[i].bone == want.bones[i].bone);
        CHECK(*shown.bones[i].rotation == *want.bones[i].rotation);
    }
    // And then, still: only the refreshes go, within the budget.
    CHECK(Bytes(picker.Pick(0x1234, still, ms)) <= CoopPose::kFloorSendBytes);
}

TEST_CASE("pose: Fill puts the newest pose's bones under a partial one, in bone order") {
    CoopPose::Track track;
    CoopPose::Pose full;
    full.skeleton = 7;
    for (std::uint16_t b : {1, 3, 5}) {
        CoopPose::BonePose p;
        p.bone = b;
        p.rotation = CoopPose::Quat{0, 0, 0, 1};
        full.bones.push_back(p);
    }
    track.Push(full, 100);
    CoopPose::Pose part;
    part.skeleton = 7;
    CoopPose::BonePose p;
    p.bone = 3;
    p.rotation = CoopPose::Quat{1, 0, 0, 0};
    part.bones.push_back(p);
    p.bone = 4;
    part.bones.push_back(p);
    const auto got = track.Fill(part);
    REQUIRE(got.bones.size() == 4);
    CHECK(got.bones[0].bone == 1);
    CHECK(got.bones[1].bone == 3);
    CHECK((*got.bones[1].rotation)[0] == 1.0f);
    CHECK(got.bones[2].bone == 4);
    CHECK(got.bones[3].bone == 5);
    part.skeleton = 8;  // another skeleton: nothing of this track's
    CHECK(track.Fill(part).bones.size() == 2);
}

TEST_CASE("pose: with every limb moving fast, a still bone is still sent within the refresh bound") {
    // 80 limb bones turn thousands of packed units a send (a run); 220 others
    // change once, a little (a hand that closed), then hold still. The limbs
    // win every change slot; the refresh tier still gets the others across.
    constexpr std::uint16_t kLimbs = 80, kAll = 300;
    auto pose = [&](int send) {
        std::vector<proto16::PoseBone> bones;
        for (std::uint16_t i = 0; i < kAll; ++i) {
            proto16::PoseBone b;
            b.bone = i;
            std::int16_t v = 0;
            if (i < kLimbs) {
                v = static_cast<std::int16_t>((send * 3001 + i * 97) % 30000 - 15000);
            } else if (send >= 10) {
                v = static_cast<std::int16_t>(i % 2 == 0 ? 5 : 600);  // under kBoneStep, and over it
            }
            b.rotation = std::array<std::int16_t, 4>{v, 0, 0, 30000};
            bones.push_back(b);
        }
        return bones;
    };
    const std::uint64_t interval = 1000 / CoopPose::kSendHz;
    const std::uint64_t bound =
        CoopPose::kBoneRefreshMs + (kAll / CoopPose::kRefreshBones + 2) * interval;
    CoopPose::BonePicker picker;
    CoopPose::Track track;
    std::vector<std::uint64_t> last(kAll, 0);
    std::uint64_t worst = 0, ms = 10'000;
    for (int send = 0; send < 120; ++send, ms += interval) {  // 8 s
        const auto full = pose(send);
        const auto sent = picker.Pick(0x1234, full, ms);
        CHECK(Bytes(sent) <= CoopPose::kFloorSendBytes);
        for (const auto& b : sent) last[b.bone] = ms;
        track.Push(track.Fill(Unpacked(sent)), ms);
        if (send >= 10) {  // after everyone was sent once
            for (std::uint16_t i = 0; i < kAll; ++i) worst = std::max(worst, ms - last[i]);
        }
        if (send == 10 + static_cast<int>(bound / interval) + 1) {
            // The hand's change has reached the receiver, small or not.
            const auto shown = *track.At(ms + 1000);
            const auto want = Unpacked(full);
            REQUIRE(shown.bones.size() == kAll);
            for (std::uint16_t i = kLimbs; i < kAll; ++i) {
                INFO("bone " << i);
                CHECK(*shown.bones[i].rotation == *want.bones[i].rotation);
            }
        }
    }
    MESSAGE("longest a bone went unsent: " << worst << " ms (bound " << bound << ")");
    CHECK(worst <= bound);
}

// jitter-3: poses with their sample time are shown at the place's own time.
TEST_CASE("pose: a timed track is shown at now - kShowDelayMs, whenever the poses arrive") {
    CoopPose::Track t;
    // Made 66 ms apart (15 Hz); they arrive unevenly (the network's jitter).
    const std::uint64_t arrive[] = {1010, 1090, 1140, 1230};
    for (int i = 0; i < 4; ++i) {
        t.Push(OneBone({0, 0, 0, 1}, static_cast<float>(i)), arrive[i], 1000 + 66 * static_cast<std::uint64_t>(i));
    }
    CHECK(t.Timed());
    // At 1199 it shows 1099: 1.5 of the way (0 at 1000, 1 at 1066, 2 at 1132).
    const auto mid = t.At(1199);
    REQUIRE(mid);
    CHECK(Near(mid->bones[0].translation->at(0), 1.5f));
    CHECK(Near(t.At(1000)->bones[0].translation->at(0), 0.0f));    // before them all: the first
    CHECK(Near(t.At(5000)->bones[0].translation->at(0), 3.0f));    // past the newest: held
    // Even frames show even steps although the arrivals were not.
    float last = t.At(1150)->bones[0].translation->at(0);
    for (std::uint64_t now = 1166; now <= 1298; now += 16) {
        const float x = t.At(now)->bones[0].translation->at(0);
        CHECK(Near(x - last, 16.0f / 66.0f, 1e-3f));
        last = x;
    }
}

TEST_CASE("pose: a late timed pose is left out, an untimed one goes back to arrival order") {
    CoopPose::Track t;
    t.Push(OneBone({0, 0, 0, 1}, 0.0f), 1000, 900);
    t.Push(OneBone({0, 0, 0, 1}, 2.0f), 1050, 966);
    t.Push(OneBone({0, 0, 0, 1}, 9.0f), 1060, 950);  // made before the newest: out of order
    CHECK(Near(t.At(1033)->bones[0].translation->at(0), 1.0f));  // shows 933: halfway 900..966
    t.Push(OneBone({0, 0, 0, 1}, 4.0f), 1200);  // no time (no clock yet): by arrival, as before
    CHECK_FALSE(t.Timed());
    CHECK(Near(t.At(5000)->bones[0].translation->at(0), 4.0f));
}

TEST_CASE("pose: the evenness number reads an even turn as 0 and an uneven one higher") {
    for (const bool even : {true, false}) {
        CoopPose::Evenness e;
        std::optional<float> u;
        int moving = 0;
        for (int i = 0; i <= 400 && !u; ++i) {
            // Even: 0.05 rad a frame. Uneven: 0.03 and 0.07 in turn.
            const float a = 0.05f * static_cast<float>(i) - (even || i % 2 == 0 ? 0.0f : 0.02f);
            u = e.Step(OneBone({0, 0, std::sin(a / 2), std::cos(a / 2)}, 0.0f), 1000 + 16 * static_cast<std::uint64_t>(i), &moving);
        }
        REQUIRE(u);
        CHECK(moving > 100);
        if (even) {
            CHECK(*u < 0.01f);
        } else {
            CHECK(*u > 0.5f);
        }
    }
}

// idle-twitch (owner: "other puppets still twitch a little while standing still").
namespace {

float SwingAt(std::uint64_t ms) { return std::sin(2.0f * 3.14159265f * static_cast<float>(ms % 1000) / 1000.0f); }

// Standing, breathing: 300 bones each turning +-1.5 degrees about x at 0.3 Hz (their own phase);
// bone 0 swings fast (a real move: +-1 rad at 1 Hz, up to 6 rad a second) when `swing`.
std::vector<proto16::PoseBone> Breathing(std::uint64_t ms, bool swing = false) {
    std::vector<proto16::PoseBone> bones;
    for (std::uint16_t i = 0; i < 300; ++i) {
        const float t = static_cast<float>(ms) / 1000.0f;
        float angle = 0.026f * std::sin(2.0f * 3.14159265f * 0.3f * t + 0.1f * static_cast<float>(i));
        if (i == 0 && swing) angle = SwingAt(ms);
        proto16::PoseBone b;
        b.bone = i;
        b.rotation = CoopPose::Pack({std::sin(angle / 2.0f), 0.0f, 0.0f, std::cos(angle / 2.0f)});
        bones.push_back(b);
    }
    return bones;
}

float AngleOf(const Quat& q) { return 2.0f * std::atan2(q[0], q[3]); }

struct Played {
    float worst_step = 0.0f;  // the most any bone turned in one frame, radians
    float swing_lag = 0.0f;   // the most bone 0's shown angle is off its angle at the shown time
};

// Sends at kSendHz through the picker, timed (made = sent), shown every 16 ms frame.
Played Play(bool eased, bool swing) {
    CoopPose::BonePicker picker;
    CoopPose::Track track;
    std::uint64_t next_send = 5000;
    std::optional<CoopPose::Pose> last;
    Played out;
    for (std::uint64_t now = 5000; now < 9000; now += 16) {
        while (next_send <= now) {
            const auto sent = Unpacked(picker.Pick(0x1234, Breathing(next_send, swing), next_send));
            if (eased) {
                track.Add(sent, next_send, next_send);
            } else {
                track.Push(track.Fill(sent), next_send, next_send);
            }
            next_send += 1000 / CoopPose::kSendHz;
        }
        const auto shown = track.At(now);
        if (!shown) continue;
        if (now > 7000 && last && last->bones.size() == shown->bones.size()) {
            for (std::size_t i = swing ? 1 : 0; i < shown->bones.size(); ++i) {
                const float step = std::fabs(AngleOf(*shown->bones[i].rotation) - AngleOf(*last->bones[i].rotation));
                out.worst_step = std::max(out.worst_step, step);
            }
        }
        last = shown;
        if (swing && now > 7000 && !shown->bones.empty()) {
            out.swing_lag = std::max(out.swing_lag, std::fabs(SwingAt(now - CoopPose::kShowDelayMs) -
                                                              AngleOf(*shown->bones[0].rotation)));
        }
    }
    return out;
}

}  // namespace

TEST_CASE("pose: a still player's breathing bones are eased over their own send gap, not stepped") {
    // The true motion: at most 2 pi 0.3 Hz x 0.026 rad a second, ~0.8 mrad a 16 ms frame.
    const float true_step = 2.0f * 3.14159265f * 0.3f * 0.026f * 0.016f;
    const Played before = Play(false, false);
    const Played after = Play(true, false);
    MESSAGE("worst bone step a frame: held and stepped " << before.worst_step << " rad, eased "
                                                          << after.worst_step << " rad (true " << true_step << ")");
    CHECK(before.worst_step > 3.0f * true_step);  // the stepping the owner saw
    CHECK(after.worst_step < 1.6f * true_step);   // now about the real motion
}

TEST_CASE("pose: a fast bone (a real move) is shown as before, with no lag added") {
    const Played before = Play(false, true);
    const Played after = Play(true, true);
    CHECK(before.swing_lag < 0.1f);
    CHECK(after.swing_lag <= before.swing_lag + 1e-4f);
}

TEST_CASE("pose: Add without times or on a new skeleton eases nothing") {
    CoopPose::Track track;
    CHECK(track.Add(Unpacked(Breathing(5000)), 5000, 0) == 0);
    CHECK(track.Add(Unpacked(Breathing(5500)), 5500, 0) == 0);
    REQUIRE(track.At(5500));
    CHECK(!track.Timed());
    CoopPose::Track timed;
    CHECK(timed.Add(Unpacked(Breathing(5000)), 5000, 5000) == 0);  // a first: as sent
    REQUIRE(timed.At(5100));
}

TEST_CASE("pose: a stopped stream or another skeleton leaves the puppet to its AS commands (puppet-anim)") {
    FakeGuest g;
    const std::uint64_t puppet = 0x700000000, other = 0x710000000;
    const std::uint64_t theirs = MakeActor(g, puppet, "Player_Child");
    MakeActor(g, other, "Player_Adult");
    CoopPose::SetLog({}, false);
    auto pose_of = [](std::uint8_t n, std::string_view model, std::int16_t x) {
        coop::protocol::PlayerPose m;
        m.player_number = n;
        m.skeleton = CoopPose::SkeletonId(kBones, model);
        m.bones.push_back(coop::protocol::PoseBone{
            2, 0, std::nullopt, std::array<std::int16_t, 3>{x, 0, 0}, std::nullopt});
        return m;
    };
    const std::uint64_t t0 = 3'000'000;
    const std::uint64_t bone2x = theirs + 2 * paddr::kLocalStride + 0x50;

    // Player 5's poses arrive and are written on its puppet: the copy drives it.
    CoopPose::Receive(pose_of(5, "Player_Child", 2048), t0);
    CoopPose::OnCalcFrame(g, kMain, {{5, puppet}}, t0 + 16);
    CHECK(Near(g.Get<float>(bone2x), 0.25f));
    CHECK(CoopPose::Drives(5, t0 + 16));
    CHECK(CoopPose::Writes(5, t0 + 16));

    // The stream stops. Within kFreshMs the last pose still shows; after it the
    // puppet is not written on any more (no pose held for ever) and the director
    // plays its AS commands again.
    g.Put(bone2x, 0.5f);  // what the puppet's own animation would leave there
    CoopPose::OnCalcFrame(g, kMain, {{5, puppet}}, t0 + CoopPose::kFreshMs + 100);
    CHECK(g.Get<float>(bone2x) == 0.5f);
    CHECK_FALSE(CoopPose::Drives(5, t0 + CoopPose::kFreshMs + 100));
    CHECK_FALSE(CoopPose::Writes(5, t0 + CoopPose::kFreshMs + 100));

    // Player 6's poses arrive, but of a skeleton its puppet does not have: they
    // count as arriving (Drives) but none is written (Writes), so the director does
    // not leave the puppet frozen waiting for the copy.
    const std::uint64_t t1 = t0 + 10'000;
    CoopPose::Receive(pose_of(6, "Player_Child", 2048), t1);
    CoopPose::OnCalcFrame(g, kMain, {{6, other}}, t1 + 16);
    CHECK(CoopPose::Drives(6, t1 + 16));
    CHECK_FALSE(CoopPose::Writes(6, t1 + 16));
}

// puppet-smooth (owner 2026-11-16: "Animations seem a bit jittery and choppy").
TEST_CASE("pose: the stream is sized by who watches it, 16 players keep the floor") {
    struct Want {
        std::size_t watchers;
        int hz;
        std::size_t bytes;
    };
    for (const Want w : {Want{0, 30, 3960}, Want{1, 30, 3960}, Want{2, 15, 2640}, Want{3, 15, 1320},
                         Want{4, CoopPose::kSendHz, CoopPose::kFloorSendBytes},
                         Want{15, CoopPose::kSendHz, CoopPose::kFloorSendBytes}}) {
        INFO("watchers " << w.watchers);
        const CoopPose::StreamPlan p = CoopPose::PlanFor(w.watchers);
        CHECK(p.hz == w.hz);
        CHECK(p.bytes == w.bytes);
        // Each stream within its relay share (kPoseRelayBytes over all streams); the floor above it.
        // A send every IntervalMs() (66 ms at 15 Hz: 15.15 a second).
        CHECK(p.bytes * 1000 <= CoopPose::BudgetFor(w.watchers) * p.IntervalMs());
        if (w.watchers <= 3) CHECK(CoopPose::BudgetFor(w.watchers) == CoopPose::ShareFor(w.watchers));
    }
    // Two players: every bone a walk moves fits 30 times a second, with the whole bones.
    CHECK(CoopPose::PlanFor(1).bytes >= CoopPose::kPoseHeaderBytes + CoopPose::kFullBodyBones * CoopPose::kBoneBytes +
                                            CoopPose::kStillBones * CoopPose::kWholeBoneBytes);
}

namespace {

// A walk: bones 0-119 swing at 1.5 Hz, 0.05 to 0.5 rad (a slow shoulder to a fast leg), each at
// its own phase; bones 120-299 still (face, fingers at rest).
float WalkAngle(std::uint16_t i, std::uint64_t ms) {
    if (i >= 120) return 0.0f;
    const float amp = 0.05f + 0.05f * static_cast<float>(i % 10);
    const float t = static_cast<float>(ms) / 1000.0f;
    return amp * std::sin(2.0f * 3.14159265f * 1.5f * t + 0.37f * static_cast<float>(i));
}

std::vector<proto16::PoseBone> Walking(std::uint64_t ms) {
    std::vector<proto16::PoseBone> bones;
    for (std::uint16_t i = 0; i < 300; ++i) {
        const float a = WalkAngle(i, ms);
        proto16::PoseBone b;
        b.bone = i;
        b.rotation = CoopPose::Pack({std::sin(a / 2.0f), 0.0f, 0.0f, std::cos(a / 2.0f)});
        bones.push_back(b);
    }
    return bones;
}

struct Watched {
    float uneven = 0.0f;
    float raw = 0.0f;
    float jumps_per_s = 0.0f;
    float worst_off = 0.0f;   // the most a swinging bone is off its true angle at the shown time
    float worst_rate = 0.0f;  // the fastest any bone turned in one frame, rad/s (a jerk)
};

// The other player walks, sent by the picker at `plan`, timed, through Track::Add; shown on uneven
// frames (15 and 23 ms in turn: ~52 a second, as two games on one PC ran).
Watched Watch(CoopPose::StreamPlan plan) {
    CoopPose::BonePicker picker;
    CoopPose::Track track;
    CoopPose::Evenness even;
    Watched out;
    std::optional<CoopPose::Pose> last;
    std::uint64_t last_ms = 0;
    std::uint64_t next_send = 5000;
    std::uint64_t now = 5000;
    for (int frame = 0; now < 11'000; ++frame) {
        now += frame % 2 ? 23 : 15;
        while (next_send <= now) {
            const auto sent =
                Unpacked(picker.Pick(0x1234, Walking(next_send), next_send, plan.bytes - CoopPose::kPoseHeaderBytes));
            track.Add(sent, next_send, next_send);
            next_send += static_cast<std::uint64_t>(1000 / plan.hz);
        }
        const auto shown = track.At(now);
        if (!shown) continue;
        if (even.Step(*shown, now)) {
            out.uneven = even.Last().uneven;
            out.raw = even.Last().raw;
            out.jumps_per_s = even.Last().jumps_per_s;
        }
        if (now > 7000 && shown->bones.size() == 300 && last && last->bones.size() == 300) {
            const float dt = static_cast<float>(now - last_ms) / 1000.0f;
            for (std::uint16_t i = 0; i < 120; ++i) {
                const float step = std::fabs(AngleOf(*shown->bones[i].rotation) - AngleOf(*last->bones[i].rotation));
                out.worst_rate = std::max(out.worst_rate, step / dt);
                const float off = std::fabs(AngleOf(*shown->bones[i].rotation) -
                                            WalkAngle(i, now - CoopPose::kShowDelayMs));
                out.worst_off = std::max(out.worst_off, off);
            }
        }
        last = shown;
        last_ms = now;
    }
    return out;
}

}  // namespace

TEST_CASE("pose: a walking Link plays smooth for 2 players (no held-then-jumped bones)") {
    // The old stream: 48 bones (rotation only here, 12 bytes each) a pose.
    const CoopPose::StreamPlan old_plan{CoopPose::kSendHz, CoopPose::kPoseHeaderBytes + CoopPose::kMaxSendBones * 12};
    const Watched before = Watch(old_plan);
    const Watched after = Watch(CoopPose::PlanFor(1));
    MESSAGE("before (15/s x 48): uneven " << before.uneven << ", raw " << before.raw << ", jumps "
                                                << before.jumps_per_s << "/s, worst off " << before.worst_off);
    MESSAGE("after (2 players): uneven " << after.uneven << ", raw " << after.raw << ", jumps "
                                         << after.jumps_per_s << "/s, worst off " << after.worst_off);
    CHECK(before.jumps_per_s > 50.0f);                      // the stair-steps the owner saw
    CHECK(after.jumps_per_s < 0.05f * before.jumps_per_s);  // about none
    CHECK(after.worst_off < 0.5f * before.worst_off);
    CHECK(after.worst_off < 0.05f);  // within ~3 degrees of the true walk, shown 100 ms behind
}

TEST_CASE("pose: evenness is taken by frame time: an even move on uneven frames reads even") {
    CoopPose::Evenness e;
    std::optional<float> u;
    std::uint64_t now = 1000;
    float a = 0.0f;
    for (int i = 0; i <= 600 && !u; ++i) {
        const std::uint64_t dt = i % 2 ? 23 : 15;
        now += dt;
        a += 3.0f * static_cast<float>(dt) / 1000.0f;  // 3 rad a second, steady
        u = e.Step(OneBone({std::sin(a / 2), 0, 0, std::cos(a / 2)}, 0.0f), now);
    }
    REQUIRE(u);
    CHECK(e.Last().uneven < 0.05f);
    CHECK(e.Last().raw > 0.3f);  // the old per-frame number read the frame rate
    CHECK(e.Last().jumps_per_s == 0.0f);
}

// puppet-smooth-a-2: the jumps count is a stair-step (moved, held, moved), not a puppet's start.
TEST_CASE("pose: jumps count a bone held between moves, not its first move after a start") {
    auto at = [](float a) { return OneBone({std::sin(a / 2), 0, 0, std::cos(a / 2)}, 0.0f); };
    CoopPose::Evenness e;
    std::uint64_t now = 1000;
    std::optional<float> u;
    // Held on its first pose (as a puppet coming into view is), then moving: no jump.
    float a = 0.0f;
    for (int i = 0; i < 10; ++i) e.Step(at(a), now += 16);
    for (int i = 0; i < 20; ++i) e.Step(at(a += 0.05f), now += 16);
    // Then held 6 frames and on again with a step: one jump.
    for (int i = 0; i < 6; ++i) e.Step(at(a), now += 16);
    for (int i = 0; i <= 600 && !u; ++i) u = e.Step(at(a += 0.05f), now += 16);
    REQUIRE(u);
    CHECK(e.Last().jumps_per_s * static_cast<float>(CoopPose::kEvenReportMs) / 1000.0f == doctest::Approx(1.0f).epsilon(0.02));
}

// puppet-smooth-a-2: still bones (face, fingers) are sent together, so they age together; by age
// alone a clump of them outranked limbs turning slowly (near a turning point) every
// kBoneRefreshMs, and those limbs were held a pose or two, then jumped. A changed bone goes first.
TEST_CASE("pose: a bone that changed is sent before still bones that only aged (148 bones a pose)") {
    // 148 bones of 12 bytes a pose at 15/s: room for the 120 moving ones and a clump of aged still ones.
    const CoopPose::StreamPlan plan{15, CoopPose::kPoseHeaderBytes + 148 * 12};
    constexpr std::uint16_t kMoving = 120, kAll = 300;
    auto pose = [&](int send) {
        std::vector<proto16::PoseBone> bones;
        for (std::uint16_t i = 0; i < kAll; ++i) {
            proto16::PoseBone b;
            b.bone = i;
            // A slow limb: 20 packed units a send (over kBoneStep, far under kBoneRefreshMs).
            const std::int16_t v = i < kMoving ? static_cast<std::int16_t>(send * 20 + i) : 0;
            b.rotation = std::array<std::int16_t, 4>{v, 0, 0, 30000};
            bones.push_back(b);
        }
        return bones;
    };
    CoopPose::BonePicker picker;
    const std::uint64_t interval = 1000 / static_cast<std::uint64_t>(plan.hz);
    std::uint64_t ms = 10'000;
    int missed = 0;
    for (int send = 0; send < 4 * plan.hz; ++send, ms += interval) {  // 4 s: several clumps aged
        const auto sent = picker.Pick(0x1234, pose(send), ms, plan.bytes - CoopPose::kPoseHeaderBytes);
        CHECK(Bytes(sent) <= plan.bytes);
        if (send < 4) continue;  // everyone sent once first (300 bones, 148 a pose)
        std::vector<bool> in(kAll, false);
        for (const auto& b : sent) in[b.bone] = true;
        for (std::uint16_t i = 0; i < kMoving; ++i) missed += in[i] ? 0 : 1;
    }
    CHECK(missed == 0);
}

// puppet-budget: the whole bones (StillPicker, issue 152) and the held pieces (issue 158) went on top
// of the moving bones' budget (230 B x 30/s x 18 = 124,200 B/s against a 120,000 share at 1 watcher).
// Everything one send carries now fits the plan: measured on the encoded messages TakeOutgoing gives,
// a walking 300-bone Link with still bones and two 64-bone pieces in hand, for 0 to 15 watchers.
TEST_CASE("pose: every pose a send carries stays in the stream's budget, 0 to 15 watchers (puppet-budget)") {
    FakeGuest g;
    const std::uint64_t player = 0x640000000, bow = 0x650000000, tool = 0x660000000;
    constexpr std::uint32_t kLink = 300, kPiece = CoopPose::kMaxPieceBones;
    const std::uint64_t mine = MakeActor(g, player, "Player_budget", kLink);
    const std::uint64_t piece_a = MakeActor(g, bow, "Weapon_budget_a", kPiece);
    const std::uint64_t piece_b = MakeActor(g, tool, "Weapon_budget_b", kPiece);
    MakePlayerChain(g, player);
    Hold(g, player, 3, bow);
    Hold(g, player, 5, tool);
    CoopPose::SetLog({}, false);
    // Bones 0-199 turn (0-59 also move), 200-299 hold still; every piece bone moves.
    auto move = [&](std::uint64_t ms) {
        const float t = static_cast<float>(ms % 100'000) / 1000.0f;
        for (std::uint32_t i = 0; i < 200; ++i) {
            const float a = 0.3f * std::sin(9.0f * t + 0.37f * static_cast<float>(i));
            const CoopPose::Rows r = CoopPose::QuatToRows({std::sin(a / 2), 0, 0, std::cos(a / 2)});
            const std::uint64_t bone = mine + i * paddr::kLocalStride;
            for (int row = 0; row < 3; ++row) {
                g.Put(bone + 0x20 + row * 16, CoopPose::Vec3{r[row * 3], r[row * 3 + 1], r[row * 3 + 2]});
            }
            if (i < 60) g.Put(bone + 0x50, CoopPose::Vec3{0.1f * a, 0.0f, 0.0f});
        }
        for (const std::uint64_t piece : {piece_a, piece_b}) {
            for (std::uint32_t i = 0; i < kPiece; ++i) {
                const float y = 0.2f * std::sin(7.0f * t + static_cast<float>(i));
                g.Put(piece + i * paddr::kLocalStride + 0x50, CoopPose::Vec3{0.0f, y, 0.0f});
            }
        }
    };
    std::uint64_t now = 50'000'000;
    for (const std::size_t watchers : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3},
                                       std::size_t{4}, std::size_t{5}, std::size_t{8}, std::size_t{15}}) {
        INFO("watchers " << watchers);
        now += 100'000;  // the last run's watchers are long gone
        const CoopPose::StreamPlan plan = CoopPose::PlanFor(watchers);
        const std::uint64_t measure = now + 1000, end = measure + 3000;
        std::size_t sends = 0, bytes = 0, most = 0, whole_bones = 0, piece_poses = 0;
        for (int frame = 0; now < end; ++frame) {
            now += frame % 3 ? 17 : 16;
            move(now);
            for (std::size_t n = 1; n <= watchers; ++n) {
                proto16::PlayerPose m;
                m.player_number = static_cast<std::uint8_t>(n);
                m.skeleton = CoopPose::SkeletonId(kLink, "Player_Child");
                m.bones.resize(1);
                m.bones[0].rotation = std::array<std::int16_t, 4>{0, 0, 0, 32767};
                CoopPose::Receive(m, now);
            }
            CoopPose::OnCalcFrame(g, kMain, {}, now);
            const auto out = CoopPose::TakeOutgoing(now * 1000, now);
            if (out.empty() || now < measure) continue;
            std::size_t send = 0;
            for (const auto& m : out) {
                const auto wire = proto16::encode(proto16::Message{m});
                REQUIRE(wire);
                CHECK(wire->size() == CoopPose::WireBytes(m));  // the budget counts what goes out
                send += wire->size();
                if (m.skeleton != out[0].skeleton) ++piece_poses;
            }
            for (const auto& b : out[0].bones) whole_bones += b.rotation && b.translation && b.scale ? 1 : 0;
            CHECK(send <= plan.bytes);
            most = std::max(most, send);
            bytes += send;
            ++sends;
        }
        REQUIRE(sends > 0);
        // Bytes a second: n sends fill n intervals of IntervalMs().
        const std::size_t per_second = bytes * 1000 / (sends * plan.IntervalMs());
        MESSAGE("watchers " << watchers << ": " << plan.hz << "/s, " << sends << " sends, " << per_second
                            << " B/s of " << CoopPose::BudgetFor(watchers) << " (share "
                            << CoopPose::ShareFor(watchers) << "), fullest send " << most << " of " << plan.bytes);
        CHECK(sends <= 3000 / plan.IntervalMs() + 1);
        CHECK(per_second <= CoopPose::BudgetFor(watchers));
        if (watchers <= 3) CHECK(per_second <= CoopPose::ShareFor(watchers));
        CHECK(whole_bones > 0);  // both pickers in the measured sends
        CHECK(piece_poses > 0);
        CHECK(most * 10 >= plan.bytes * 9);  // the budget was the limit, not the pose
    }
}
