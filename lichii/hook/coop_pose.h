// SPDX-License-Identifier: GPL-3.0-or-later
// Full-body pose copy for puppets (docs/puppet.md, "Pose").
//
// Each game frame, on the game's calc thread (the puppet cave's SVC), the
// local Link's skeleton is read: every bone's local transform as the game's
// animation left it. The network thread sends the bones that move as
// PLAYER_POSE at most kSendHz times a second. A received pose is kept per
// player; each frame the newest two are blended and written onto that
// player's puppet skeleton.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace coop::net {
class Client;
struct Event;
}  // namespace coop::net

namespace CoopPuppet {
class Guest;
}

namespace CoopPose {

using Quat = std::array<float, 4>;  // x, y, z, w
using Vec3 = std::array<float, 3>;
using Rows = std::array<float, 9>;  // 3x3 rotation as the game stores it, rows

// Read from the running game with the emulator's debug server (2026-11-09,
// Kakariko, child): see docs/puppet.md, Pose.
namespace addr {
// Actor -> skeleton object: read a pointer at each offset in turn. The model
// object sits inline at +0x120 of the fourth, its skeleton pointer at +0x20.
inline constexpr std::array<std::uint64_t, 5> kSkeletonPath{0x270, 0x8, 0x18, 0x18, 0x140};
inline constexpr std::uint64_t kModelResource = 0x120;  // of the fourth: "FMDL"
inline constexpr std::uint64_t kBoneCount = 0x48;       // u16 in the skeleton object
inline constexpr std::uint64_t kLocalArray = 0x18;      // pointer to the local transforms
inline constexpr std::uint64_t kLocalStride = 0x60;
// One local transform: u32 flags (bits 23 to 30 change with the pose),
// scale (3 floats at +0x10), rotation (3 rows of 4 floats at +0x20, the 4th
// of each 0) and translation (3 floats at +0x50).
inline constexpr std::uint32_t kFlagShift = 23;
inline constexpr std::uint32_t kFlagMask = 0xffu << kFlagShift;
}  // namespace addr

inline constexpr std::size_t kMaxBones = 512;
inline constexpr int kSendHz = 30;
// A part counts as moving once it differs from the first read by more than this.
inline constexpr float kMoved = 1e-4f;

// One bone of a pose: the parts that move.
struct BonePose {
    std::uint16_t bone = 0;
    std::uint8_t flags = 0;
    std::optional<Quat> rotation;
    std::optional<Vec3> translation;
    std::optional<Vec3> scale;
};

struct Pose {
    std::uint32_t skeleton = 0;
    std::vector<BonePose> bones;  // increasing bone order
};

Quat RowsToQuat(const Rows& m);  // normalised, w >= 0
Rows QuatToRows(const Quat& q);  // q normalised first; identity if it is 0
std::array<std::int16_t, 4> Pack(const Quat& q);
Quat Unpack(const std::array<std::int16_t, 4>& q);
std::array<std::int16_t, 3> PackUnit(const Vec3& v);  // scaled by protocol::kPoseUnit
Vec3 UnpackUnit(const std::array<std::int16_t, 3>& v);
// Shortest-path normalised lerp from a to b; t in [0, 1].
Quat Nlerp(const Quat& a, const Quat& b, float t);

// A skeleton's id: the bone count (the two skeletons the puppet can have,
// child and adult, differ in it).
std::uint32_t SkeletonId(std::uint32_t bone_count);

// The newest poses of one remote player, and the blend shown now.
class Track {
public:
    void Push(Pose pose, std::uint64_t now_ms);
    // The pose to show at now_ms: shown one interval behind, the last two
    // blended, so a 30 Hz stream plays smooth at 60 frames a second.
    std::optional<Pose> At(std::uint64_t now_ms) const;
    std::uint64_t last_ms() const { return newest_ms_; }

private:
    std::optional<Pose> older_;
    std::optional<Pose> newest_;
    std::uint64_t older_ms_ = 0;
    std::uint64_t newest_ms_ = 0;
};

// Calc thread, once a frame: read the local player's skeleton and write the
// shown pose onto each live puppet. puppets: (player number, actor); the
// debug puppet (number 0) copies the local player's own pose.
void OnCalcFrame(CoopPuppet::Guest& guest, std::uint64_t main_base,
                 const std::vector<std::pair<std::uint8_t, std::uint64_t>>& puppets,
                 std::uint64_t now_ms);
// Network thread: send the newest local pose when it is time.
void Send(coop::net::Client& client, std::uint64_t now_ms);
// Network thread: keep a received PLAYER_POSE.
void OnEvent(const coop::net::Event& event, std::uint64_t now_ms);
// LICHII_COOP_DEBUG_POSE=1: a line a second with bone counts and rates.
// LICHII_COOP_POSE_NO_AS=1 (no_as): a puppet whose poses arrive plays no AS
// command of its own (see Drives).
void SetLog(std::function<void(const std::string&)> log, bool debug, bool no_as = false);
// Calc thread: whether the pose copy drives this player's puppet now, so the
// director plays no AS command on it (with no_as only: poses arrived in the
// last kFreshMs).
bool Drives(std::uint8_t number, std::uint64_t now_ms);
inline constexpr std::uint64_t kFreshMs = 1000;

}  // namespace CoopPose
