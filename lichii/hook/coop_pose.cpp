// SPDX-License-Identifier: GPL-3.0-or-later
// The pose copy (coop_pose.h). No emulator header, so the unit tests build it.
#include "coop_pose.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "coop/net/client.hpp"
#include "coop/protocol/messages.hpp"
#include "coop_puppet.h"

namespace CoopPose {

namespace proto = coop::protocol;

// ------------------------------------------------------------------ math

namespace {

Quat Normalised(Quat q) {
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(n > 1e-6f) || !std::isfinite(n)) {
        return {0, 0, 0, 1};
    }
    for (float& v : q) {
        v /= n;
    }
    return q;
}

std::int16_t ToS16(float v, float scale) {
    return static_cast<std::int16_t>(std::lround(std::clamp(v * scale, -32767.0f, 32767.0f)));
}

}  // namespace

// The column-vector convention (v' = M v). Which way the game means its rows
// does not matter: QuatToRows gives back exactly what RowsToQuat was given.
Quat RowsToQuat(const Rows& m) {
    const float m00 = m[0], m01 = m[1], m02 = m[2];
    const float m10 = m[3], m11 = m[4], m12 = m[5];
    const float m20 = m[6], m21 = m[7], m22 = m[8];
    Quat q{};
    const float trace = m00 + m11 + m22;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
    q = Normalised(q);
    if (q[3] < 0.0f) {
        for (float& v : q) {
            v = -v;
        }
    }
    return q;
}

Rows QuatToRows(const Quat& in) {
    const Quat q = Normalised(in);
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    return {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
            2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
            2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)};
}

std::array<std::int16_t, 4> Pack(const Quat& in) {
    const Quat q = Normalised(in);
    return {ToS16(q[0], 32767.0f), ToS16(q[1], 32767.0f), ToS16(q[2], 32767.0f),
            ToS16(q[3], 32767.0f)};
}

Quat Unpack(const std::array<std::int16_t, 4>& in) {
    return Normalised({in[0] / 32767.0f, in[1] / 32767.0f, in[2] / 32767.0f, in[3] / 32767.0f});
}

std::array<std::int16_t, 3> PackUnit(const Vec3& v) {
    return {ToS16(v[0], proto::kPoseUnit), ToS16(v[1], proto::kPoseUnit),
            ToS16(v[2], proto::kPoseUnit)};
}

Vec3 UnpackUnit(const std::array<std::int16_t, 3>& v) {
    return {v[0] / proto::kPoseUnit, v[1] / proto::kPoseUnit, v[2] / proto::kPoseUnit};
}

Quat Nlerp(const Quat& a, const Quat& b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    const float sign = dot < 0.0f ? -1.0f : 1.0f;
    Quat q{};
    for (std::size_t i = 0; i < 4; ++i) {
        q[i] = a[i] * (1.0f - t) + sign * b[i] * t;
    }
    return Normalised(q);
}

std::uint32_t SkeletonId(std::uint32_t bone_count) {
    return bone_count;
}

namespace {

bool SameBones(const Pose& a, const Pose& b) {
    if (a.skeleton != b.skeleton || a.bones.size() != b.bones.size()) return false;
    for (std::size_t i = 0; i < a.bones.size(); ++i) {
        const BonePose& x = a.bones[i];
        const BonePose& y = b.bones[i];
        if (x.bone != y.bone || x.rotation.has_value() != y.rotation.has_value() ||
            x.translation.has_value() != y.translation.has_value() ||
            x.scale.has_value() != y.scale.has_value()) {
            return false;
        }
    }
    return true;
}

Vec3 Lerp(const Vec3& a, const Vec3& b, float t) {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}

}  // namespace

void Track::Push(Pose pose, std::uint64_t now_ms) {
    if (newest_ && SameBones(*newest_, pose)) {
        older_ = std::move(newest_);
        older_ms_ = newest_ms_;
    } else {
        older_.reset();
    }
    newest_ = std::move(pose);
    newest_ms_ = now_ms;
}

std::optional<Pose> Track::At(std::uint64_t now_ms) const {
    if (!newest_) {
        return std::nullopt;
    }
    if (!older_ || newest_ms_ <= older_ms_) {
        return newest_;
    }
    // older shows at newest_ms, newest one interval later.
    const float span = static_cast<float>(newest_ms_ - older_ms_);
    const float t = static_cast<float>(now_ms - std::min(now_ms, newest_ms_)) / span;
    if (t >= 1.0f) {
        return newest_;
    }
    Pose out = *newest_;
    for (std::size_t i = 0; i < out.bones.size(); ++i) {
        BonePose& b = out.bones[i];
        const BonePose& o = older_->bones[i];
        if (b.rotation) b.rotation = Nlerp(*o.rotation, *b.rotation, t);
        if (b.translation) b.translation = Lerp(*o.translation, *b.translation, t);
        if (b.scale) b.scale = Lerp(*o.scale, *b.scale, t);
        if (t < 0.5f) b.flags = o.flags;
    }
    return out;
}

// ------------------------------------------------------------------ the game side

namespace {

template <typename T>
bool ReadValue(CoopPuppet::Guest& guest, std::uint64_t at, T& out) {
    return at != 0 && guest.Read(at, &out, sizeof(T));
}

struct LocalMtx {
    std::uint32_t flags;
    std::uint32_t unused[3];
    float scale[4];
    float rows[12];  // three rows of 4, the 4th of each 0
    float translation[4];
};
static_assert(sizeof(LocalMtx) == addr::kLocalStride);

Rows RowsOf(const LocalMtx& b) {
    return {b.rows[0], b.rows[1], b.rows[2], b.rows[4], b.rows[5],
            b.rows[6], b.rows[8], b.rows[9], b.rows[10]};
}

struct Skeleton {
    std::uint64_t local = 0;
    std::uint32_t count = 0;
};

// Actor -> its skeleton's local transform array, checking the model is one.
std::optional<Skeleton> FindSkeleton(CoopPuppet::Guest& guest, std::uint64_t actor) {
    std::uint64_t at = actor;
    std::uint64_t holder = 0;  // the struct the model object sits in
    for (std::size_t i = 0; i < addr::kSkeletonPath.size(); ++i) {
        if (i + 1 == addr::kSkeletonPath.size()) holder = at;
        if (!ReadValue(guest, at + addr::kSkeletonPath[i], at) || at == 0) {
            return std::nullopt;
        }
    }
    std::uint64_t resource = 0;
    char magic[4]{};
    if (!ReadValue(guest, holder + addr::kModelResource, resource) ||
        !guest.Read(resource, magic, sizeof(magic)) || std::memcmp(magic, "FMDL", 4) != 0) {
        return std::nullopt;
    }
    Skeleton s;
    std::uint16_t count = 0;
    if (!ReadValue(guest, at + addr::kBoneCount, count) ||
        !ReadValue(guest, at + addr::kLocalArray, s.local) || s.local == 0 || count == 0 ||
        count > kMaxBones) {
        return std::nullopt;
    }
    s.count = count;
    return s;
}

// The local player's actor, as coop::host::PlayerReader finds it (addresses.hpp).
std::uint64_t FindPlayer(CoopPuppet::Guest& guest, std::uint64_t main_base) {
    std::uint64_t scene = 0, table = 0, array = 0, manager = 0, must = 0;
    std::uint32_t count = 0;
    if (!ReadValue(guest, main_base + 0x4e97648, scene) || !ReadValue(guest, scene + 0x1f0, table) ||
        !ReadValue(guest, table + 0x50, count) || count < 0x10 ||
        !ReadValue(guest, table + 0x58, array) || !ReadValue(guest, array + 0x78, manager) ||
        !ReadValue(guest, manager + 0x30, must) || must == 0) {
        return 0;
    }
    for (const std::uint64_t link_at : {0x40, 0x50}) {
        std::uint64_t link = 0, holder = 0, actor = 0;
        std::uint32_t id = 0, holder_id = 0, state = 0;
        if (ReadValue(guest, manager + link_at, link) && ReadValue(guest, link + 0x10, id) &&
            id != 0xffffffffu && ReadValue(guest, link + 0x8, holder) &&
            ReadValue(guest, holder + 0x48, holder_id) && holder_id == id &&
            ReadValue(guest, holder + 0x40, actor) && ReadValue(guest, actor + 0x2c, state) &&
            state == 4) {
            return actor;
        }
    }
    return 0;
}

bool Moved(const float* a, const float* b, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (std::fabs(a[i] - b[i]) > kMoved) return true;
    }
    return false;
}

struct Written {
    std::uint64_t actor = 0;
    std::uint64_t local = 0;
    std::uint16_t bone = 0;
    float rows[12]{};
};

struct State {
    std::mutex mutex;
    std::function<void(const std::string&)> log;
    bool debug = false;
    bool no_as = false;
    // The local skeleton: the first read and which parts have moved since.
    std::vector<LocalMtx> first;
    std::vector<std::uint8_t> moving;  // pose_part bits, sticky
    // Local pose, from the calc thread, for the network thread.
    std::optional<Pose> outbox;
    std::uint32_t outbox_serial = 0;
    std::uint32_t sent_serial = 0;
    std::uint64_t last_send_ms = 0;
    std::uint32_t sequence = 0;
    std::size_t last_size = 0;
    // Received, per player 1 to 4.
    std::array<Track, 5> tracks;
    // What was last written to each puppet, to see whether the game keeps it.
    std::vector<Written> written_last;
    // Counters for the debug line.
    std::uint64_t log_ms = 0;
    unsigned sent = 0, received = 0, written = 0, refused = 0, kept = 0, overwritten = 0;
};

State& S() {
    static State state;
    return state;
}

void Say(State& s, const std::string& text) {
    if (s.log) s.log("pose: " + text);
}

// Calc thread, under the lock: the local skeleton as a pose of its moving bones.
std::optional<Pose> ReadLocal(State& s, CoopPuppet::Guest& guest, std::uint64_t actor) {
    const auto skel = FindSkeleton(guest, actor);
    if (!skel) return std::nullopt;
    std::vector<LocalMtx> bones(skel->count);
    if (!guest.Read(skel->local, bones.data(), bones.size() * sizeof(LocalMtx))) {
        return std::nullopt;
    }
    for (const LocalMtx& b : bones) {
        if (!std::isfinite(b.rows[0]) || !std::isfinite(b.translation[0])) return std::nullopt;
    }
    if (s.first.size() != bones.size()) {
        s.first = bones;
        s.moving.assign(bones.size(), 0);
    }
    Pose pose;
    pose.skeleton = SkeletonId(skel->count);
    for (std::size_t i = 0; i < bones.size(); ++i) {
        const LocalMtx& b = bones[i];
        const LocalMtx& f = s.first[i];
        std::uint8_t& m = s.moving[i];
        if (Moved(b.rows, f.rows, 12) || (b.flags & addr::kFlagMask) != (f.flags & addr::kFlagMask)) {
            m |= proto::pose_part::rotation;
        }
        if (Moved(b.translation, f.translation, 3)) m |= proto::pose_part::translation;
        if (Moved(b.scale, f.scale, 3)) m |= proto::pose_part::scale;
        if (m == 0) continue;
        BonePose out;
        out.bone = static_cast<std::uint16_t>(i);
        out.flags = static_cast<std::uint8_t>((b.flags & addr::kFlagMask) >> addr::kFlagShift);
        if (m & proto::pose_part::rotation) out.rotation = RowsToQuat(RowsOf(b));
        if (m & proto::pose_part::translation) {
            out.translation = Vec3{b.translation[0], b.translation[1], b.translation[2]};
        }
        if (m & proto::pose_part::scale) out.scale = Vec3{b.scale[0], b.scale[1], b.scale[2]};
        pose.bones.push_back(out);
    }
    return pose;
}

// Calc thread, under the lock: the pose onto a puppet's skeleton.
bool WritePose(State& s, CoopPuppet::Guest& guest, std::uint64_t actor, const Pose& pose) {
    const auto skel = FindSkeleton(guest, actor);
    if (!skel || SkeletonId(skel->count) != pose.skeleton) return false;
    std::vector<LocalMtx> bones(skel->count);
    if (!guest.Read(skel->local, bones.data(), bones.size() * sizeof(LocalMtx))) return false;

    // Did the game keep what we wrote last frame?
    auto last = std::find_if(s.written_last.begin(), s.written_last.end(),
                             [actor](const Written& w) { return w.actor == actor; });
    if (last != s.written_last.end() && last->local == skel->local &&
        last->bone < bones.size()) {
        if (Moved(bones[last->bone].rows, last->rows, 12)) {
            ++s.overwritten;
        } else {
            ++s.kept;
        }
    }

    std::optional<std::uint16_t> probe;
    for (const BonePose& b : pose.bones) {
        if (b.bone >= bones.size()) continue;
        LocalMtx& m = bones[b.bone];
        m.flags = (m.flags & ~addr::kFlagMask) | (std::uint32_t{b.flags} << addr::kFlagShift);
        if (b.rotation) {
            const Rows r = QuatToRows(*b.rotation);
            for (std::size_t row = 0; row < 3; ++row) {
                for (std::size_t col = 0; col < 3; ++col) {
                    m.rows[row * 4 + col] = r[row * 3 + col];
                }
            }
            if (!probe) probe = b.bone;
        }
        if (b.translation) std::copy_n(b.translation->data(), 3, m.translation);
        if (b.scale) std::copy_n(b.scale->data(), 3, m.scale);
    }
    if (!guest.Write(skel->local, bones.data(), bones.size() * sizeof(LocalMtx))) return false;
    if (probe) {
        if (last == s.written_last.end()) {
            s.written_last.push_back({});
            last = s.written_last.end() - 1;
        }
        last->actor = actor;
        last->local = skel->local;
        last->bone = *probe;
        std::copy_n(bones[*probe].rows, 12, last->rows);
    }
    return true;
}

// The actor's facing in degrees: its forward axis (rotation column 2) on the
// ground plane, atan2(x, z). "-" when it cannot be read.
std::string YawText(CoopPuppet::Guest& guest, std::uint64_t actor) {
    std::array<float, 9> m{};
    if (actor == 0 || !guest.Read(actor + 0x328, m.data(), sizeof(m))) return "-";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.0f", std::atan2(m[2], m[8]) * 57.29578f);
    return buf;
}

}  // namespace

void SetLog(std::function<void(const std::string&)> log, bool debug, bool as_too) {
    State& s = S();
    std::lock_guard lock(s.mutex);
    s.log = std::move(log);
    s.debug = debug;
    s.no_as = !as_too;
}

bool Drives(std::uint8_t number, std::uint64_t now_ms) {
    State& s = S();
    std::lock_guard lock(s.mutex);
    if (!s.no_as || number == 0 || number >= s.tracks.size()) return false;
    const std::uint64_t last = s.tracks[number].last_ms();
    return last != 0 && now_ms - std::min(now_ms, last) < kFreshMs;
}

void OnCalcFrame(CoopPuppet::Guest& guest, std::uint64_t main_base,
                 const std::vector<std::pair<std::uint8_t, std::uint64_t>>& puppets,
                 std::uint64_t now_ms) {
    State& s = S();
    std::lock_guard lock(s.mutex);
    const std::uint64_t player = main_base != 0 ? FindPlayer(guest, main_base) : 0;
    if (player != 0) {
        if (auto local = ReadLocal(s, guest, player)) {
            s.last_size = local->bones.size();
            s.outbox = std::move(local);
            ++s.outbox_serial;
        }
    }
    for (const auto& [number, actor] : puppets) {
        if (actor == 0 || number >= s.tracks.size()) continue;
        std::optional<Pose> pose;
        if (number == 0) {
            if (s.debug) pose = s.outbox;  // the debug puppet mirrors us
        } else {
            pose = s.tracks[number].At(now_ms);
        }
        if (!pose) continue;
        if (WritePose(s, guest, actor, *pose)) {
            ++s.written;
        } else {
            ++s.refused;
        }
    }
    if (s.debug && now_ms - s.log_ms >= 1000) {
        s.log_ms = now_ms;
        // Facing, for checking that a puppet turns with its player: ours and
        // each puppet's, from the actor's rotation (+0x328).
        std::string facing = "; yaw ours " + YawText(guest, player);
        for (const auto& [number, actor] : puppets) {
            facing += ", P" + std::to_string(number) + " " + YawText(guest, actor);
        }
        Say(s, std::to_string(s.first.size()) + " bones, " + std::to_string(s.last_size) +
                   " moving; " + std::to_string(s.sent) + " sent/s, " +
                   std::to_string(s.received) + " received/s, " + std::to_string(s.written) +
                   " written/s, " + std::to_string(s.refused) + " refused/s; last write kept " +
                   std::to_string(s.kept) + ", overwritten " + std::to_string(s.overwritten) +
                   facing);
        s.sent = s.received = s.written = s.refused = s.kept = s.overwritten = 0;
    }
}

void Send(coop::net::Client& client, std::uint64_t now_ms) {
    proto::PlayerPose message;
    {
        State& s = S();
        std::lock_guard lock(s.mutex);
        if (!s.outbox || s.outbox_serial == s.sent_serial ||
            now_ms - s.last_send_ms < 1000 / kSendHz) {
            return;
        }
        s.sent_serial = s.outbox_serial;
        s.last_send_ms = now_ms;
        message.time = {client.server_now_us(), ++s.sequence};
        message.skeleton = s.outbox->skeleton;
        message.bones.reserve(s.outbox->bones.size());
        for (const BonePose& b : s.outbox->bones) {
            proto::PoseBone out;
            out.bone = b.bone;
            out.flags = b.flags;
            if (b.rotation) out.rotation = Pack(*b.rotation);
            if (b.translation) out.translation = PackUnit(*b.translation);
            if (b.scale) out.scale = PackUnit(*b.scale);
            message.bones.push_back(out);
        }
        ++s.sent;
    }
    client.send(message);
}

void OnEvent(const coop::net::Event& event, std::uint64_t now_ms) {
    if (event.kind != coop::net::EventKind::message) return;
    const auto* m = std::get_if<proto::PlayerPose>(&event.message);
    if (m == nullptr) return;
    Pose pose;
    pose.skeleton = m->skeleton;
    pose.bones.reserve(m->bones.size());
    for (const proto::PoseBone& b : m->bones) {
        BonePose out;
        out.bone = b.bone;
        out.flags = b.flags;
        if (b.rotation) out.rotation = Unpack(*b.rotation);
        if (b.translation) out.translation = UnpackUnit(*b.translation);
        if (b.scale) out.scale = UnpackUnit(*b.scale);
        pose.bones.push_back(out);
    }
    State& s = S();
    std::lock_guard lock(s.mutex);
    if (m->player_number == 0 || m->player_number >= s.tracks.size()) return;
    s.tracks[m->player_number].Push(std::move(pose), now_ms);
    ++s.received;
}

}  // namespace CoopPose
