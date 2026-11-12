// Tests for the pose copy's math and its track (lichii/hook/coop_pose.cpp).
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
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
std::uint64_t MakeActor(FakeGuest& g, std::uint64_t actor, std::string_view model = {}) {
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
    if (!model.empty()) {
        const std::uint64_t name = res + 0x100;
        g.Put(res + paddr::kModelName, name);
        g.Put(name, static_cast<std::uint16_t>(model.size()));
        REQUIRE(g.Write(name + 2, model.data(), model.size()));
    }
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

TEST_CASE("pose: poses of the highest player number are kept (16 players); one past it is not") {
    CoopPose::SetLog({}, false);
    const std::uint8_t top = coop::protocol::kMaxPlayers;
    for (const std::uint8_t n : {top, static_cast<std::uint8_t>(top + 1)}) {
        coop::protocol::PlayerPose m;
        m.player_number = n;
        m.skeleton = 0x16161616;
        m.bones.push_back(coop::protocol::PoseBone{1, 0, std::array<std::int16_t, 4>{0, 0, 0, 32767}});
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
    // Now: 15 sends a second, at most kMaxSendBones each.
    CoopPose::BonePicker picker;
    std::size_t now = 0;
    for (int f = 0; f < 60; f += 4) {
        const auto sent = picker.Pick(0x1234, Running(f), static_cast<std::uint64_t>(1000 + f * 1000 / 60));
        CHECK(sent.size() <= CoopPose::kMaxSendBones);
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
        CoopPose::Pose got = track.Fill(Unpacked(sent));
        track.Push(std::move(got), ms);
        ms += 1000 / CoopPose::kSendHz;
    };
    // The first pose: only kMaxSendBones of 300, the rest follow in later ones.
    send(Running(0));
    REQUIRE(track.At(ms));
    CHECK(track.At(ms)->bones.size() == CoopPose::kMaxSendBones);
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
    CHECK(picker.Pick(0x1234, still, ms).size() <= CoopPose::kMaxSendBones);
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
