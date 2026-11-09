// Tests for the pose copy's math and its track (lichii/hook/coop_pose.cpp).
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

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
    auto two = OneBone({0, 0, 0, 1}, 4.0f);
    two.bones.push_back(two.bones[0]);
    two.bones[1].bone = 9;
    t.Push(two, 40);
    CHECK(Near(t.At(50)->bones[0].translation->at(0), 4.0f));
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

// An actor whose skeleton has kBones identity bones; returns the local array.
std::uint64_t MakeActor(FakeGuest& g, std::uint64_t actor) {
    const std::uint64_t parts = actor + 0x1000, first = actor + 0x2000, second = actor + 0x3000,
                        holder = actor + 0x4000, skel = actor + 0x5000, local = actor + 0x6000,
                        res = actor + 0x7000;
    for (std::uint64_t at : {actor, parts, first, second, holder, skel, res}) g.Map(at, 0x400);
    g.Map(local, kBones * paddr::kLocalStride);
    g.Put(actor + 0x2c, std::uint32_t{4});
    g.Put(actor + paddr::kSkeletonPath[0], parts);
    g.Put(parts + paddr::kSkeletonPath[1], first);
    g.Put(first + paddr::kSkeletonPath[2], second);
    g.Put(second + paddr::kSkeletonPath[3], holder);
    g.Put(holder + paddr::kSkeletonPath[4], skel);
    g.Put(holder + paddr::kModelResource, res);
    g.Put(res, std::array<char, 4>{'F', 'M', 'D', 'L'});
    g.Put(skel + paddr::kBoneCount, static_cast<std::uint16_t>(kBones));
    g.Put(skel + paddr::kLocalArray, local);
    for (std::uint32_t i = 0; i < kBones; ++i) {
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
