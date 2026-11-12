// SPDX-License-Identifier: GPL-3.0-or-later
// The pose copy (coop_pose.h). No emulator header, so the unit tests build it.
#include "coop_pose.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
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

std::uint32_t SkeletonId(std::uint32_t bone_count, std::string_view model_name) {
    std::uint32_t hash = 2166136261u;  // FNV-1a
    for (const char c : model_name) {
        hash = (hash ^ static_cast<std::uint8_t>(c)) * 16777619u;
    }
    return (bone_count & 0x1ffu) | (hash << 9);
}

std::uint32_t BoneCountOf(std::uint32_t skeleton) {
    return skeleton & 0x1ffu;
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

Pose Track::Fill(Pose pose) const {
    if (!newest_ || newest_->skeleton != pose.skeleton) return pose;
    std::vector<BonePose> merged;
    merged.reserve(std::max(pose.bones.size(), newest_->bones.size()));
    std::size_t i = 0, j = 0;
    const auto& old = newest_->bones;
    while (i < pose.bones.size() || j < old.size()) {
        if (j == old.size() || (i < pose.bones.size() && pose.bones[i].bone <= old[j].bone)) {
            if (j < old.size() && pose.bones[i].bone == old[j].bone) ++j;
            merged.push_back(std::move(pose.bones[i++]));
        } else {
            merged.push_back(old[j++]);
        }
    }
    pose.bones = std::move(merged);
    return pose;
}

int BonePicker::Change(const proto::PoseBone& then, const proto::PoseBone& now) {
    constexpr int kFar = 1 << 20;
    int most = 0;
    auto part = [&](const auto& a, const auto& b) {
        if (a.has_value() != b.has_value()) {
            most = kFar;
            return;
        }
        if (!a) return;
        for (std::size_t k = 0; k < a->size(); ++k) {
            most = std::max(most, std::abs(int{(*a)[k]} - int{(*b)[k]}));
        }
    };
    part(then.rotation, now.rotation);
    part(then.translation, now.translation);
    part(then.scale, now.scale);
    if (then.flags != now.flags) most = std::max(most, kBoneStep + 1);
    return most;
}

std::vector<proto::PoseBone> BonePicker::Pick(std::uint32_t skeleton,
                                              const std::vector<proto::PoseBone>& full,
                                              std::uint64_t now_ms, std::size_t budget) {
    if (skeleton != skeleton_) {
        skeleton_ = skeleton;
        sent_.clear();
    }
    // Priority: the change in packed units plus a unit per millisecond of
    // age, so a small change waits but is never starved; a bone never sent
    // first. A bone unsent for kBoneRefreshMs goes even unchanged (a lost
    // packet, a late joiner).
    struct Wanted {
        std::int64_t priority;
        std::size_t index;
    };
    std::vector<Wanted> wanted;
    wanted.reserve(full.size());
    for (std::size_t i = 0; i < full.size(); ++i) {
        const auto it = sent_.find(full[i].bone);
        if (it == sent_.end()) {
            wanted.push_back({std::int64_t{1} << 40, i});
            continue;
        }
        const std::uint64_t age = now_ms - std::min(now_ms, it->second.ms);
        const int change = Change(it->second.bone, full[i]);
        if (age >= kBoneRefreshMs || change > kBoneStep) {
            wanted.push_back({change + static_cast<std::int64_t>(age), i});
        }
    }
    if (wanted.size() > budget) {
        std::nth_element(wanted.begin(), wanted.begin() + static_cast<std::ptrdiff_t>(budget),
                         wanted.end(), [](const Wanted& a, const Wanted& b) {
                             return a.priority != b.priority ? a.priority > b.priority
                                                             : a.index < b.index;
                         });
        wanted.resize(budget);
    }
    std::sort(wanted.begin(), wanted.end(),
              [](const Wanted& a, const Wanted& b) { return a.index < b.index; });
    std::vector<proto::PoseBone> out;
    out.reserve(wanted.size());
    for (const Wanted& w : wanted) {
        out.push_back(full[w.index]);
        sent_[full[w.index].bone] = {full[w.index], now_ms};
    }
    // Bones that stopped moving on our side are no longer in `full`: forget them.
    std::erase_if(sent_, [&](const auto& kv) {
        return !std::binary_search(full.begin(), full.end(), kv.second.bone,
                                   [](const proto::PoseBone& a, const proto::PoseBone& b) {
                                       return a.bone < b.bone;
                                   });
    });
    return out;
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
    std::string model;  // "Player_Child", "Player_Adult"
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
    std::uint64_t name = 0;
    std::uint16_t length = 0;
    char text[64]{};
    if (ReadValue(guest, resource + addr::kModelName, name) && ReadValue(guest, name, length) &&
        length > 0 && length < sizeof(text) && guest.Read(name + 2, text, length)) {
        s.model.assign(text, length);
    }
    return s;
}

}  // namespace

std::optional<std::string> ModelNameOf(CoopPuppet::Guest& guest, std::uint64_t actor) {
    if (actor == 0) {
        return std::nullopt;
    }
    const auto s = FindSkeleton(guest, actor);
    if (!s || s->model.empty()) {
        return std::nullopt;
    }
    return s->model;
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

bool CopySkeleton(CoopPuppet::Guest& guest, std::uint64_t from, std::uint64_t to,
                  std::string* note) {
    const auto a = FindSkeleton(guest, from);
    const auto b = FindSkeleton(guest, to);
    if (!a || !b) {
        if (note) *note = !a ? "no skeleton on the source" : "no skeleton on the target";
        return false;
    }
    if (note) {
        *note = a->model + " " + std::to_string(a->count) + " bones -> " + b->model + " " +
                std::to_string(b->count) + " bones";
    }
    if (a->count != b->count) return false;
    std::vector<LocalMtx> bones(a->count);
    const std::size_t size = bones.size() * sizeof(LocalMtx);
    if (!guest.Read(a->local, bones.data(), size)) return false;
    for (const LocalMtx& m : bones) {
        if (!std::isfinite(m.rows[0]) || !std::isfinite(m.translation[0])) return false;
    }
    return guest.Write(b->local, bones.data(), size);
}

namespace {

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
    std::string local_model;  // our skeleton's model, for the debug line
    // The local skeleton: the first read and which parts have moved since.
    std::vector<LocalMtx> first;
    std::vector<std::uint8_t> moving;  // pose_part bits, sticky
    // Local pose, from the calc thread, for the network thread.
    std::optional<Pose> outbox;
    // The pieces in our hands (kPieceSlots), every bone each: the slingshot's
    // band and seed, the bow's string.
    std::vector<Pose> pieces_outbox;
    std::uint32_t outbox_serial = 0;
    std::uint32_t sent_serial = 0;
    std::uint64_t last_send_ms = 0;
    std::uint32_t sequence = 0;
    std::size_t last_size = 0;
    // What each piece skeleton was last sent as, and when: an unchanged
    // piece is sent again only after kPieceRefreshMs.
    struct PieceSent {
        std::vector<proto::PoseBone> bones;
        std::uint64_t ms = 0;
    };
    std::map<std::uint32_t, PieceSent> pieces_sent;
    // Which of Link's bones go in the next pose (kMaxSendBones).
    BonePicker picker;
    // Received, per player 1 to kMaxPlayers, one track per skeleton: Link's
    // body and each held piece arrive as poses of their own skeleton.
    std::array<std::map<std::uint32_t, Track>, std::size_t{proto::kMaxPlayers} + 1> tracks;
    // What was last written to each puppet, to see whether the game keeps it.
    std::vector<Written> written_last;
    // Counters for the debug line.
    std::uint64_t log_ms = 0;
    unsigned sent = 0, received = 0, written = 0, refused = 0, kept = 0, overwritten = 0;
    unsigned pieces_sent_n = 0, pieces_written = 0;
};

// What was last written: kept for this many actors (puppets and pieces).
constexpr std::size_t kWrittenKeep = 32;

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
    if (s.first.size() != bones.size() || s.local_model != skel->model) {  // a new skeleton (age)
        s.first = bones;
        s.moving.assign(bones.size(), 0);
    }
    Pose pose;
    pose.skeleton = SkeletonId(skel->count, skel->model);
    s.local_model = skel->model;
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

// Calc thread, under the lock: every bone of a held piece, as a pose. A
// piece is small, so all its bones go, not only the ones that moved since a
// first read: the puppet's copy of the piece may start in another state
// (pulled back) than ours did.
std::optional<Pose> ReadPiece(CoopPuppet::Guest& guest, const Skeleton& skel) {
    if (skel.count > kMaxPieceBones) return std::nullopt;
    std::vector<LocalMtx> bones(skel.count);
    if (!guest.Read(skel.local, bones.data(), bones.size() * sizeof(LocalMtx))) {
        return std::nullopt;
    }
    Pose pose;
    pose.skeleton = SkeletonId(skel.count, skel.model);
    for (std::size_t i = 0; i < bones.size(); ++i) {
        const LocalMtx& b = bones[i];
        if (!std::isfinite(b.rows[0]) || !std::isfinite(b.translation[0])) return std::nullopt;
        BonePose out;
        out.bone = static_cast<std::uint16_t>(i);
        out.flags = static_cast<std::uint8_t>((b.flags & addr::kFlagMask) >> addr::kFlagShift);
        out.rotation = RowsToQuat(RowsOf(b));
        out.translation = Vec3{b.translation[0], b.translation[1], b.translation[2]};
        out.scale = Vec3{b.scale[0], b.scale[1], b.scale[2]};
        pose.bones.push_back(out);
    }
    return pose;
}

// The pieces in an actor's hands (kPieceSlots) that are out and drawn now.
std::vector<std::uint64_t> HeldPieces(CoopPuppet::Guest& guest, std::uint64_t actor) {
    namespace pa = CoopPuppet::addr;
    std::vector<std::uint64_t> out;
    std::uint64_t parts = 0, eu = 0;
    if (!ReadValue(guest, actor + pa::kActorParts, parts) || parts == 0 ||
        !ReadValue(guest, parts + pa::kPartsEquipmentUser, eu) || eu == 0) {
        return out;
    }
    for (const std::uint32_t slot : kPieceSlots) {
        const std::uint64_t link = eu + pa::kEquipSlots + pa::kEquipSlotStride * slot;
        std::uint32_t id = 0xFFFFFFFFu, state = 0;
        std::uint64_t holder = 0, piece = 0;
        if (ReadValue(guest, link + pa::kEquipSlotId, id) && id != 0xFFFFFFFFu &&
            ReadValue(guest, link + pa::kEquipSlotHolder, holder) && holder != 0 &&
            ReadValue(guest, holder + pa::kHolderActor, piece) && piece != 0 && piece != actor &&
            ReadValue(guest, piece + pa::kActorState, state) && state == pa::kStatePresent) {
            out.push_back(piece);
        }
    }
    return out;
}

// Calc thread, under the lock: the pose onto a puppet's skeleton.
bool WritePose(State& s, CoopPuppet::Guest& guest, std::uint64_t actor, const Pose& pose) {
    const auto skel = FindSkeleton(guest, actor);
    if (!skel || SkeletonId(skel->count, skel->model) != pose.skeleton) return false;
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
    std::uint64_t last = 0;
    for (const auto& [skeleton, track] : s.tracks[number]) last = std::max(last, track.last_ms());
    return last != 0 && now_ms - std::min(now_ms, last) < kFreshMs;
}

namespace {

// Calc thread, under the lock: the pose for `actor` (a puppet or a piece in
// its hands) from `number`'s poses of that skeleton, written. Nothing when
// no pose of its skeleton has come.
enum class Wrote { none, written, refused };
Wrote WriteFor(State& s, CoopPuppet::Guest& guest, std::uint8_t number, std::uint64_t actor,
               std::uint64_t now_ms) {
    const auto skel = FindSkeleton(guest, actor);
    if (!skel) return Wrote::none;
    const std::uint32_t id = SkeletonId(skel->count, skel->model);
    std::optional<Pose> pose;
    if (number == 0) {
        if (!s.debug) return Wrote::none;
        if (s.outbox && s.outbox->skeleton == id) {
            pose = s.outbox;  // the debug puppet mirrors us
        } else {
            for (const Pose& p : s.pieces_outbox) {
                if (p.skeleton == id) pose = p;
            }
        }
    } else {
        const auto& tracks = s.tracks[number];
        if (const auto it = tracks.find(id); it != tracks.end()) pose = it->second.At(now_ms);
    }
    if (!pose) return Wrote::none;
    return WritePose(s, guest, actor, *pose) ? Wrote::written : Wrote::refused;
}

}  // namespace

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
            s.pieces_outbox.clear();
            for (const std::uint64_t piece : HeldPieces(guest, player)) {
                if (const auto skel = FindSkeleton(guest, piece)) {
                    if (auto pose = ReadPiece(guest, *skel)) {
                        s.pieces_outbox.push_back(std::move(*pose));
                    }
                }
            }
            ++s.outbox_serial;
        }
    }
    for (const auto& [number, actor] : puppets) {
        if (actor == 0 || number >= s.tracks.size()) continue;
        const Wrote body = WriteFor(s, guest, number, actor, now_ms);
        if (body == Wrote::written) {
            ++s.written;
        } else if (body == Wrote::refused ||
                   (number != 0 && !s.tracks[number].empty())) {
            ++s.refused;  // poses come, none of this puppet's skeleton (age)
        }
        for (const std::uint64_t piece : HeldPieces(guest, actor)) {
            const Wrote w = WriteFor(s, guest, number, piece, now_ms);
            if (w == Wrote::written) {
                ++s.pieces_written;
            }
        }
    }
    if (s.written_last.size() > kWrittenKeep) {
        s.written_last.erase(s.written_last.begin(),
                             s.written_last.end() - static_cast<std::ptrdiff_t>(kWrittenKeep));
    }
    if (s.debug && now_ms - s.log_ms >= 1000) {
        s.log_ms = now_ms;
        // Facing, for checking that a puppet turns with its player: ours and
        // each puppet's, from the actor's rotation (+0x328).
        std::string facing = "; yaw ours " + YawText(guest, player);
        for (const auto& [number, actor] : puppets) {
            facing += ", P" + std::to_string(number) + " " + YawText(guest, actor);
        }
        Say(s, (s.local_model.empty() ? std::string("?") : s.local_model) + " " +
                   std::to_string(s.first.size()) + " bones, " + std::to_string(s.last_size) +
                   " moving; " + std::to_string(s.sent) + " sent/s, " +
                   std::to_string(s.received) + " received/s, " + std::to_string(s.written) +
                   " written/s, " + std::to_string(s.refused) + " refused/s; held pieces " +
                   std::to_string(s.pieces_outbox.size()) + " ours, " +
                   std::to_string(s.pieces_sent_n) + " sent/s, " + std::to_string(s.pieces_written) +
                   " written/s; last write kept " +
                   std::to_string(s.kept) + ", overwritten " + std::to_string(s.overwritten) +
                   facing);
        s.sent = s.received = s.written = s.refused = s.kept = s.overwritten = 0;
        s.pieces_sent_n = s.pieces_written = 0;
    }
}

namespace {

std::vector<proto::PoseBone> Encode(const Pose& pose) {
    std::vector<proto::PoseBone> bones;
    bones.reserve(pose.bones.size());
    for (const BonePose& b : pose.bones) {
        proto::PoseBone out;
        out.bone = b.bone;
        out.flags = b.flags;
        if (b.rotation) out.rotation = Pack(*b.rotation);
        if (b.translation) out.translation = PackUnit(*b.translation);
        if (b.scale) out.scale = PackUnit(*b.scale);
        bones.push_back(out);
    }
    return bones;
}

}  // namespace

std::vector<proto::PlayerPose> TakeOutgoing(std::uint64_t server_now_us, std::uint64_t now_ms) {
    std::vector<proto::PlayerPose> out;
    State& s = S();
    std::lock_guard lock(s.mutex);
    if (!s.outbox || s.outbox_serial == s.sent_serial ||
        now_ms - s.last_send_ms < 1000 / kSendHz) {
        return out;
    }
    s.sent_serial = s.outbox_serial;
    s.last_send_ms = now_ms;
    proto::PlayerPose body;
    body.time = {server_now_us, ++s.sequence};
    body.skeleton = s.outbox->skeleton;
    // Sent even with no bone changed: the receiver keeps its puppet's pose
    // fresh (Drives) and its blend interval right.
    body.bones = s.picker.Pick(body.skeleton, Encode(*s.outbox), now_ms);
    out.push_back(std::move(body));
    ++s.sent;
    // Each piece in our hands as a pose of its own skeleton: when it changed,
    // or now and then while it holds still (a late joiner, a lost packet).
    for (const Pose& piece : s.pieces_outbox) {
        std::vector<proto::PoseBone> bones = Encode(piece);
        auto& last = s.pieces_sent[piece.skeleton];
        if (last.ms != 0 && last.bones == bones && now_ms - last.ms < kPieceRefreshMs) continue;
        last.bones = bones;
        last.ms = now_ms;
        proto::PlayerPose m;
        m.time = {server_now_us, ++s.sequence};
        m.skeleton = piece.skeleton;
        m.bones = std::move(bones);
        out.push_back(std::move(m));
        ++s.pieces_sent_n;
    }
    std::erase_if(s.pieces_sent, [&](const auto& kv) {
        return now_ms - kv.second.ms > 10 * kPieceRefreshMs;  // put away long ago
    });
    return out;
}

void Send(coop::net::Client& client, std::uint64_t now_ms) {
    for (const proto::PlayerPose& m : TakeOutgoing(client.server_now_us(), now_ms)) {
        client.send(m);
    }
}

void OnEvent(const coop::net::Event& event, std::uint64_t now_ms) {
    if (event.kind != coop::net::EventKind::message) return;
    const auto* m = std::get_if<proto::PlayerPose>(&event.message);
    if (m == nullptr) return;
    Receive(*m, now_ms);
}

void Receive(const proto::PlayerPose& m, std::uint64_t now_ms) {
    Pose pose;
    pose.skeleton = m.skeleton;
    pose.bones.reserve(m.bones.size());
    for (const proto::PoseBone& b : m.bones) {
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
    if (m.player_number == 0 || m.player_number >= s.tracks.size()) return;
    auto& tracks = s.tracks[m.player_number];
    auto& track = tracks[m.skeleton];
    track.Push(track.Fill(std::move(pose)), now_ms);
    // Skeletons not heard from for a while (a piece put away, the other age).
    std::erase_if(tracks, [&](const auto& kv) {
        return now_ms - std::min(now_ms, kv.second.last_ms()) > kTrackForgetMs;
    });
    while (tracks.size() > kMaxTracks) {
        auto oldest = tracks.begin();
        for (auto it = tracks.begin(); it != tracks.end(); ++it) {
            if (it->second.last_ms() < oldest->second.last_ms()) oldest = it;
        }
        tracks.erase(oldest);
    }
    ++s.received;
}

}  // namespace CoopPose
