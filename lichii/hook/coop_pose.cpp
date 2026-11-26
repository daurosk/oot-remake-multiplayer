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
#include "coop_show_delay.h"

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

// `now` has every bone and part of `old` (same skeleton), and maybe more.
bool Grows(const Pose& old, const Pose& now) {
    if (old.skeleton != now.skeleton || old.bones.size() > now.bones.size()) return false;
    std::size_t j = 0;
    for (const BonePose& x : old.bones) {
        while (j < now.bones.size() && now.bones[j].bone < x.bone) ++j;
        if (j == now.bones.size() || now.bones[j].bone != x.bone) return false;
        const BonePose& y = now.bones[j];
        if ((x.rotation && !y.rotation) || (x.translation && !y.translation) || (x.scale && !y.scale)) {
            return false;
        }
    }
    return true;
}

// `old` with the bones and parts only `now` has, as `now` has them (nothing earlier is known),
// so it blends with `now` (SameBones).
Pose Grown(Pose old, const Pose& now) {
    Pose out = now;
    std::size_t i = 0;
    for (BonePose& y : out.bones) {
        while (i < old.bones.size() && old.bones[i].bone < y.bone) ++i;
        if (i == old.bones.size() || old.bones[i].bone != y.bone) continue;
        BonePose& x = old.bones[i];
        if (x.rotation) y.rotation = x.rotation;
        if (x.translation) y.translation = x.translation;
        if (x.scale) y.scale = x.scale;
        y.flags = x.flags;
    }
    return out;
}

Vec3 Lerp(const Vec3& a, const Vec3& b, float t) {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}

// `b` with each part taken `t` of the way from `o` (same bones).
Pose Blend(const Pose& o, const Pose& b, float t) {
    Pose out = b;
    for (std::size_t i = 0; i < out.bones.size(); ++i) {
        BonePose& x = out.bones[i];
        const BonePose& y = o.bones[i];
        if (x.rotation) x.rotation = Nlerp(*y.rotation, *x.rotation, t);
        if (x.translation) x.translation = Lerp(*y.translation, *x.translation, t);
        if (x.scale) x.scale = Lerp(*y.scale, *x.scale, t);
        if (t < 0.5f) x.flags = y.flags;
    }
    return out;
}

}  // namespace

void Track::Push(Pose pose, std::uint64_t now_ms, std::uint64_t made_ms) {
    // jitter-3: timed poses by their made time; another bone list or an
    // untimed pose starts them over; one not newer (out of order) is left out.
    // puppet-smooth-a-2: a pose that only adds bones or parts (a bone that moved for the first
    // time, the rest of a first pose the bone budget split) grows the kept ones instead: starting
    // over held the whole puppet kShowDelayMs, then jumped it.
    if (made_ms != 0 && !timed_.empty() && !SameBones(timed_.back().second, pose) &&
        Grows(timed_.back().second, pose)) {
        for (auto& kept : timed_) kept.second = Grown(std::move(kept.second), pose);
    }
    if (newest_ && !SameBones(*newest_, pose) && Grows(*newest_, pose)) {
        newest_ = Grown(std::move(*newest_), pose);
    }
    if (made_ms == 0 || (!timed_.empty() && !SameBones(timed_.back().second, pose))) {
        timed_.clear();
    }
    if (made_ms != 0 && (timed_.empty() || made_ms > timed_.back().first)) {
        if (timed_.size() >= kKeepPoses) timed_.erase(timed_.begin());
        timed_.emplace_back(made_ms, pose);
    }
    if (newest_ && SameBones(*newest_, pose)) {
        older_ = std::move(newest_);
        older_ms_ = newest_ms_;
    } else {
        older_.reset();
    }
    newest_ = std::move(pose);
    newest_ms_ = now_ms;
}

namespace {

const BonePose* FindBone(const Pose& pose, std::uint16_t bone) {
    const auto it = std::lower_bound(pose.bones.begin(), pose.bones.end(), bone,
                                     [](const BonePose& b, std::uint16_t n) { return b.bone < n; });
    return it != pose.bones.end() && it->bone == bone ? &*it : nullptr;
}

float MostOf(const std::optional<Vec3>& a, const std::optional<Vec3>& b) {
    if (!a || !b) return 0.0f;
    return std::max({std::fabs((*a)[0] - (*b)[0]), std::fabs((*a)[1] - (*b)[1]), std::fabs((*a)[2] - (*b)[2])});
}

// idle-twitch: a bone's step slow enough over `seconds` to be eased (the same parts on both).
// The same parts and flags on both: one can be blended into the other.
bool SameParts(const BonePose& a, const BonePose& b) {
    return a.rotation.has_value() == b.rotation.has_value() &&
           a.translation.has_value() == b.translation.has_value() &&
           a.scale.has_value() == b.scale.has_value() && a.flags == b.flags;
}

bool SlowStep(const BonePose& from, const BonePose& to, float seconds) {
    if (!SameParts(from, to)) return false;
    if (from.rotation) {
        const Quat& a = *from.rotation;
        const Quat& b = *to.rotation;
        const float d = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
        if (2.0f * std::acos(std::min(d, 1.0f)) > kRampRad * seconds) return false;
    }
    return MostOf(from.translation, to.translation) <= kRampUnit * seconds &&
           MostOf(from.scale, to.scale) <= kRampUnit * seconds;
}

BonePose Between(const BonePose& from, const BonePose& to, float t) {
    BonePose out = to;
    if (out.rotation) out.rotation = Nlerp(*from.rotation, *to.rotation, t);
    if (out.translation) out.translation = Lerp(*from.translation, *to.translation, t);
    if (out.scale) out.scale = Lerp(*from.scale, *to.scale, t);
    return out;
}

}  // namespace

std::size_t Track::Add(Pose sent, std::uint64_t now_ms, std::uint64_t made_ms) {
    // Eased only between timed poses of the same skeleton, on in time.
    const bool timed = made_ms != 0 && !timed_.empty() && timed_.back().second.skeleton == sent.skeleton &&
                       made_ms > timed_.back().first;
    if (!timed) {
        ramps_.clear();
        if (made_ms == 0 || (!timed_.empty() && timed_.back().second.skeleton != sent.skeleton)) {
            sent_ms_.clear();
        }
    } else {
        const auto& [shown_ms, shown] = timed_.back();  // each bone as it is shown at shown_ms
        std::vector<std::uint16_t> in_sent;
        in_sent.reserve(sent.bones.size());
        for (BonePose& b : sent.bones) {
            in_sent.push_back(b.bone);
            const auto last = sent_ms_.find(b.bone);
            const BonePose* was = FindBone(shown, b.bone);
            const std::uint64_t gap = last != sent_ms_.end() && made_ms > last->second ? made_ms - last->second : 0;
            if (was == nullptr || gap <= kRampAfterMs ||
                !SlowStep(*was, b, static_cast<float>(gap) / 1000.0f)) {
                ramps_.erase(b.bone);  // a real move (or a first): as sent
                continue;
            }
            Ramp r{*was, b, shown_ms, std::min(gap, kRampMaxMs)};
            ramps_[b.bone] = r;
        }
        // Each eased bone (sent now or still on its way) at this pose's time.
        for (auto it = ramps_.begin(); it != ramps_.end();) {
            const Ramp& r = it->second;
            const float t = static_cast<float>(made_ms - r.start_ms) / static_cast<float>(r.span_ms);
            const BonePose at = t >= 1.0f ? r.to : Between(r.from, r.to, t);
            auto pos = std::lower_bound(sent.bones.begin(), sent.bones.end(), at.bone,
                                        [](const BonePose& b, std::uint16_t n) { return b.bone < n; });
            if (pos != sent.bones.end() && pos->bone == at.bone) {
                *pos = at;
            } else {
                sent.bones.insert(pos, at);
            }
            it = t >= 1.0f ? ramps_.erase(it) : std::next(it);
        }
        for (const std::uint16_t n : in_sent) sent_ms_[n] = made_ms;
    }
    if (!timed && made_ms != 0) {
        for (const BonePose& b : sent.bones) sent_ms_[b.bone] = made_ms;
    }
    Push(Fill(std::move(sent)), now_ms, made_ms);
    return ramps_.size();
}

Pose Track::Fill(Pose pose) const {
    if (!newest_ || newest_->skeleton != pose.skeleton) return pose;
    std::vector<BonePose> merged;
    merged.reserve(std::max(pose.bones.size(), newest_->bones.size()));
    std::size_t i = 0, j = 0;
    const auto& old = newest_->bones;
    while (i < pose.bones.size() || j < old.size()) {
        if (j == old.size() || (i < pose.bones.size() && pose.bones[i].bone <= old[j].bone)) {
            BonePose b = std::move(pose.bones[i++]);
            if (j < old.size() && b.bone == old[j].bone) {
                // A part this pose lacks keeps its last value (a still part
                // went whole earlier, StillPicker): never the puppet's own.
                const BonePose& o = old[j++];
                if (!b.rotation) b.rotation = o.rotation;
                if (!b.translation) b.translation = o.translation;
                if (!b.scale) b.scale = o.scale;
            }
            merged.push_back(std::move(b));
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

std::size_t WireBytes(const proto::PoseBone& bone) {
    return 4 + (bone.rotation ? 8 : 0) + (bone.translation ? 6 : 0) + (bone.scale ? 6 : 0);
}

std::size_t WireBytes(const proto::PlayerPose& pose) {
    std::size_t n = kPoseHeaderBytes;
    for (const proto::PoseBone& b : pose.bones) n += WireBytes(b);
    return n;
}

std::size_t ShareFor(std::size_t watchers) {
    const std::size_t w = std::max<std::size_t>(watchers, 1);
    return kPoseRelayBytes / ((w + 1) * w);
}

std::size_t BudgetFor(std::size_t watchers) {
    const std::size_t floor_ms = static_cast<std::size_t>(StreamPlan{kSendHz, kFloorSendBytes}.IntervalMs());
    return std::max(ShareFor(watchers), (kFloorSendBytes * 1000 + floor_ms - 1) / floor_ms);
}

StreamPlan PlanFor(std::size_t watchers) {
    const std::size_t share = ShareFor(watchers);  // bytes a second, our stream
    StreamPlan plan;
    if (share >= static_cast<std::size_t>(kFastHz) * kFullBodyBones * kBoneBytes) {
        plan.hz = kFastHz;
    }
    plan.bytes = std::max(share * static_cast<std::size_t>(plan.IntervalMs()) / 1000, kFloorSendBytes);
    return plan;
}

std::vector<proto::PoseBone> BonePicker::Pick(std::uint32_t skeleton,
                                              const std::vector<proto::PoseBone>& full,
                                              std::uint64_t now_ms, std::size_t bytes,
                                              const std::vector<std::uint16_t>& going) {
    if (skeleton != skeleton_) {
        skeleton_ = skeleton;
        sent_.clear();
    }
    // Two tiers. Refresh: up to kRefreshBones of the bones unsent for
    // kBoneRefreshMs, oldest first, whatever they changed: a big limb motion
    // never starves them. Then the rest by the change in packed units plus a
    // unit per millisecond of age; a bone never sent before any change.
    // puppet-smooth-a-2: a bone that did not change (an aged still face or finger bone) goes in
    // the change tier only after every bone that changed: by age alone it outranked a slow limb
    // near its turning point, and still bones age together (sent together), so a clump of them
    // every kBoneRefreshMs pushed moving bones out of a pose or two (held, then a jump). The
    // refresh tier still gets every bone across within its bound.
    struct Wanted {
        std::int64_t priority;
        std::size_t index;
        bool changed = true;
    };
    std::vector<Wanted> wanted, aged;
    wanted.reserve(full.size());
    for (std::size_t i = 0; i < full.size(); ++i) {
        if (std::binary_search(going.begin(), going.end(), full[i].bone)) {
            sent_[full[i].bone] = {full[i], now_ms};  // whole in this pose already
            continue;
        }
        const auto it = sent_.find(full[i].bone);
        if (it == sent_.end()) {
            wanted.push_back({std::int64_t{1} << 40, i});
            continue;
        }
        const std::uint64_t age = now_ms - std::min(now_ms, it->second.ms);
        const int change = Change(it->second.bone, full[i]);
        const std::int64_t priority = change + static_cast<std::int64_t>(age);
        if (age >= refresh_ms_) {
            aged.push_back({static_cast<std::int64_t>(age), i});
            wanted.push_back({priority, i, change > kBoneStep});  // also in the change tier, below
        } else if (change > kBoneStep) {
            wanted.push_back({priority, i});
        }
    }
    const auto first = [](const Wanted& a, const Wanted& b) {
        if (a.changed != b.changed) return a.changed;
        return a.priority != b.priority ? a.priority > b.priority : a.index < b.index;
    };
    // puppet-budget: each tier takes bones in its order while their wire bytes fit; one that
    // does not fit is passed over for a smaller one after it.
    std::vector<bool> taken(full.size(), false);
    std::vector<Wanted> picked;
    const auto take = [&](std::vector<Wanted>& tier, std::size_t most) {
        std::sort(tier.begin(), tier.end(), first);
        std::size_t n = 0;
        for (const Wanted& w : tier) {
            if (n == most) break;
            if (taken[w.index]) continue;
            const std::size_t cost = WireBytes(full[w.index]);
            if (cost > bytes) continue;
            bytes -= cost;
            taken[w.index] = true;
            picked.push_back(w);
            ++n;
        }
    };
    take(aged, kRefreshBones);
    take(wanted, SIZE_MAX);
    wanted = std::move(picked);
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

std::vector<proto::PoseBone> StillPicker::Pick(std::uint32_t skeleton, const std::vector<proto::PoseBone>& all,
                                               std::uint64_t now_ms, std::size_t budget, std::size_t bytes) {
    if (skeleton != skeleton_) {
        skeleton_ = skeleton;
        sent_ms_.clear();
    }
    struct Wanted {
        std::uint64_t age;
        std::size_t index;
    };
    std::vector<Wanted> wanted;
    for (std::size_t i = 0; i < all.size(); ++i) {
        const auto it = sent_ms_.find(all[i].bone);
        if (it == sent_ms_.end()) {
            wanted.push_back({~std::uint64_t{0}, i});  // never sent: first, in bone order
        } else if (now_ms - std::min(now_ms, it->second) >= kStillRefreshMs) {
            wanted.push_back({now_ms - it->second, i});
        }
    }
    std::sort(wanted.begin(), wanted.end(),
              [](const Wanted& a, const Wanted& b) { return a.age != b.age ? a.age > b.age : a.index < b.index; });
    std::size_t n = 0;
    for (std::size_t k = 0; k < wanted.size() && n < budget; ++k) {
        const std::size_t cost = WireBytes(all[wanted[k].index]);
        if (cost > bytes) continue;
        bytes -= cost;
        wanted[n++] = wanted[k];
    }
    wanted.resize(n);
    std::sort(wanted.begin(), wanted.end(), [](const Wanted& a, const Wanted& b) { return a.index < b.index; });
    std::vector<proto::PoseBone> out;
    out.reserve(wanted.size());
    for (const Wanted& w : wanted) {
        out.push_back(all[w.index]);
        sent_ms_[all[w.index].bone] = now_ms;
    }
    return out;
}

std::vector<proto::PoseBone> MergeBones(const std::vector<proto::PoseBone>& moving,
                                        const std::vector<proto::PoseBone>& whole) {
    std::vector<proto::PoseBone> out;
    out.reserve(moving.size() + whole.size());
    std::size_t i = 0, j = 0;
    while (i < moving.size() || j < whole.size()) {
        if (j < whole.size() && (i == moving.size() || whole[j].bone <= moving[i].bone)) {
            if (i < moving.size() && moving[i].bone == whole[j].bone) ++i;
            out.push_back(whole[j++]);
        } else {
            out.push_back(moving[i++]);
        }
    }
    return out;
}

std::optional<Pose> Track::At(std::uint64_t now_ms, std::uint64_t delay_ms) const {
    if (!timed_.empty()) {
        const std::uint64_t show = now_ms - std::min(now_ms, delay_ms);
        if (show <= timed_.front().first) return timed_.front().second;
        for (std::size_t i = 0; i + 1 < timed_.size(); ++i) {
            const auto& [a_ms, a] = timed_[i];
            const auto& [b_ms, b] = timed_[i + 1];
            if (show < b_ms) {
                return Blend(a, b, static_cast<float>(show - a_ms) / static_cast<float>(b_ms - a_ms));
            }
        }
        return timed_.back().second;  // past the newest: held (as presence holds the place)
    }
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
    return Blend(*older_, *newest_, t);
}

std::optional<float> Evenness::Step(const Pose& shown, std::uint64_t now_ms, int* moving) {
    if (start_ms_ == 0 || now_ms < start_ms_) start_ms_ = now_ms;
    const std::uint64_t dt = last_ms_ != 0 && now_ms > last_ms_ ? now_ms - last_ms_ : 0;
    if (last_ && SameBones(*last_, shown) && dt > 0) {
        held_.resize(shown.bones.size(), static_cast<std::uint8_t>(kNotMoved));
        float change = 0.0f;
        for (std::size_t i = 0; i < shown.bones.size(); ++i) {
            const auto& a = shown.bones[i].rotation;
            const auto& b = last_->bones[i].rotation;
            if (a && b) {
                // The turn from the chord |a - b| (b on a's side): acos of the dot product reads
                // every turn under ~0.7 mrad as 0 in float, a moving bone as a held one.
                const float sign = (*a)[0] * (*b)[0] + (*a)[1] * (*b)[1] + (*a)[2] * (*b)[2] + (*a)[3] * (*b)[3] < 0.0f
                                       ? -1.0f
                                       : 1.0f;
                float chord = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    const float d = (*a)[k] - sign * (*b)[k];
                    chord += d * d;
                }
                const float c = std::sqrt(chord);
                const float turn = 4.0f * std::asin(std::min(c / 2.0f, 1.0f));
                change += turn;
                const bool same = c < kHeldChord;  // a held copy (Fill), to float rounding of its blend
                // Only a bone that moved, was held, then moved: the first move after a (re)start
                // (a puppet coming into view) is not a stair-step.
                if (!same && held_[i] != kNotMoved && held_[i] >= kJumpHeldFrames && turn > kJumpRad) ++jumps_;
                if (!same) {
                    held_[i] = 0;
                } else if (held_[i] != kNotMoved) {
                    held_[i] = static_cast<std::uint8_t>(std::min(held_[i] + 1, kNotMoved - 1));
                }
            }
        }
        const float rate = change / static_cast<float>(dt);
        if (change > kEvenMovingRad && last_change_ > kEvenMovingRad) {
            sum_ += std::fabs(1.0f - rate / last_rate_);
            raw_sum_ += std::fabs(1.0f - change / last_change_);
            ++moving_;
        }
        last_change_ = change;
        last_rate_ = rate;
    } else {
        last_change_ = 0.0f;
        last_rate_ = 0.0f;
        held_.assign(shown.bones.size(), static_cast<std::uint8_t>(kNotMoved));
    }
    last_ = shown;
    last_ms_ = now_ms;
    if (now_ms - start_ms_ < kEvenReportMs) return std::nullopt;
    const float seconds = static_cast<float>(now_ms - start_ms_) / 1000.0f;
    report_.uneven = moving_ ? sum_ / static_cast<float>(moving_) : 0.0f;
    report_.raw = moving_ ? raw_sum_ / static_cast<float>(moving_) : 0.0f;
    report_.moving = moving_;
    report_.jumps_per_s = static_cast<float>(jumps_) / seconds;
    if (moving != nullptr) *moving = moving_;
    sum_ = 0.0f;
    raw_sum_ = 0.0f;
    moving_ = 0;
    jumps_ = 0;
    start_ms_ = now_ms;
    return report_.uneven;
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
    // Each piece skeleton's bones within its share of the send (puppet-budget): a changed bone
    // goes at once, an unchanged one again after kPieceRefreshMs. `out_ms`: last seen in a hand.
    struct PieceSent {
        BonePicker picker{kPieceRefreshMs};
        std::uint64_t out_ms = 0;
    };
    std::map<std::uint32_t, PieceSent> pieces_sent;
    std::size_t piece_turn = 0;
    // Which of Link's moving bones go in the next pose (what plan.bytes leaves them).
    BonePicker picker;
    // Every bone whole (StillPicker), from the same read as outbox.
    std::optional<Pose> still_outbox;
    StillPicker still_picker;
    // puppet-smooth: our stream's rate and byte budget, by who watches it (PlanFor).
    StreamPlan plan;
    std::size_t last_bones = 0, last_bytes = 0;  // our newest send: Link's bones, every pose's bytes
    // Received, per player 1 to kMaxPlayers, one track per skeleton: Link's
    // body and each held piece arrive as poses of their own skeleton.
    std::array<std::map<std::uint32_t, Track>, std::size_t{proto::kMaxPlayers} + 1> tracks;
    // What was last written to each puppet, to see whether the game keeps it.
    std::vector<Written> written_last;
    // puppet-anim: when each player's body pose was last written on its
    // puppet (Writes).
    std::array<std::uint64_t, std::size_t{proto::kMaxPlayers} + 1> body_written_ms{};
    // Counters for the debug line.
    std::uint64_t log_ms = 0;
    // jitter-3: server ms minus our steady ms (from the client, at each Send):
    // a received pose's sample time on our clock. And the frame our outbox
    // pose was read on, so it is stamped with that frame's time.
    std::optional<std::int64_t> offset_ms;
    std::uint64_t outbox_ms = 0;
    std::map<std::uint8_t, Evenness> evenness;  // the puppets' shown body pose
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
    Pose whole;
    whole.skeleton = pose.skeleton;
    whole.bones.reserve(bones.size());
    for (std::size_t i = 0; i < bones.size(); ++i) {
        const LocalMtx& b = bones[i];
        BonePose out;
        out.bone = static_cast<std::uint16_t>(i);
        out.flags = static_cast<std::uint8_t>((b.flags & addr::kFlagMask) >> addr::kFlagShift);
        out.rotation = RowsToQuat(RowsOf(b));
        out.translation = Vec3{b.translation[0], b.translation[1], b.translation[2]};
        out.scale = Vec3{b.scale[0], b.scale[1], b.scale[2]};
        whole.bones.push_back(out);
    }
    s.still_outbox = std::move(whole);
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

bool Writes(std::uint8_t number, std::uint64_t now_ms) {
    State& s = S();
    std::lock_guard lock(s.mutex);
    if (number == 0 || number >= s.body_written_ms.size()) return false;
    const std::uint64_t at = s.body_written_ms[number];
    return at != 0 && now_ms - std::min(now_ms, at) < kFreshMs;
}

namespace {

// Calc thread, under the lock: the pose for `actor` (a puppet or a piece in
// its hands) from `number`'s poses of that skeleton, written. Nothing when
// no pose of its skeleton has come.
enum class Wrote { none, written, refused };
Wrote WriteFor(State& s, CoopPuppet::Guest& guest, std::uint8_t number, std::uint64_t actor,
               std::uint64_t now_ms, bool body) {
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
        const auto it = tracks.find(id);
        // puppet-anim (#36/#41): a track whose poses stopped is not written on: its last
        // pose held for ever was the puppet stuck in one animation. Its AS commands drive it.
        if (it != tracks.end() && now_ms - std::min(now_ms, it->second.last_ms()) < kFreshMs) {
            pose = it->second.At(now_ms, CoopShowDelay::Us(number) / 1000);
        }
        int moving = 0;
        const std::optional<float> u = pose && body ? s.evenness[number].Step(*pose, now_ms, &moving) : std::nullopt;
        if (u) {
            // jitter-3's proof line: how even the limbs move (0 even), and by which clock.
            // idle-twitch: and how many slow (idle) bones are being eased over their send gap.
            // puppet-smooth: by frame time, the old per-frame number, the held-then-jumped bones,
            // and the stream we send (the other side's is in its own log).
            const EvenReport& r = s.evenness[number].Last();
            char buf[288];
            std::snprintf(buf, sizeof(buf),
                          "P%u limbs: uneven %.2f (%d moving frames, %s, %zu bones eased); raw %.2f, "
                          "jumps %.1f/s; we send %d/s x %zu bones, %zu of %zu bytes",
                          static_cast<unsigned>(number), *u, moving,
                          it->second.Timed() ? "at the place's time" : "by arrival", it->second.Eased(),
                          r.raw, r.jumps_per_s, s.plan.hz, s.last_bones, s.last_bytes, s.plan.bytes);
            Say(s, buf);
        }
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
            s.outbox_ms = now_ms;
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
        const Wrote body = WriteFor(s, guest, number, actor, now_ms, true);
        if (body == Wrote::written) {
            ++s.written;
            s.body_written_ms[number] = now_ms;
        } else if (body == Wrote::refused ||
                   (number != 0 && !s.tracks[number].empty())) {
            ++s.refused;  // poses come, none of this puppet's skeleton (age)
        }
        for (const std::uint64_t piece : HeldPieces(guest, actor)) {
            const Wrote w = WriteFor(s, guest, number, piece, now_ms, false);
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
    // puppet-smooth: who watches our stream = the other players whose body poses reach us (the
    // server forwards poses both ways by the same interest), each counted once.
    std::size_t watchers = 0;
    for (std::size_t n = 1; n < s.tracks.size(); ++n) {
        for (const auto& [skeleton, track] : s.tracks[n]) {
            if (BoneCountOf(skeleton) > kMaxPieceBones &&
                now_ms - std::min(now_ms, track.last_ms()) < kFreshMs) {
                ++watchers;
                break;
            }
        }
    }
    s.plan = PlanFor(watchers);
    if (!s.outbox || s.outbox_serial == s.sent_serial ||
        now_ms - s.last_send_ms < s.plan.IntervalMs()) {
        return out;
    }
    s.sent_serial = s.outbox_serial;
    // puppet-smooth: on the rate's own beat, not "an interval after the network thread's tick
    // that passed it" (that rounded every interval up to the next tick: fewer sends a second
    // than the rate, unevenly spaced). A late send (a stall) starts the beat again from now.
    const std::uint64_t interval = s.plan.IntervalMs();
    s.last_send_ms = now_ms - s.last_send_ms < 2 * interval ? s.last_send_ms + interval : now_ms;
    proto::PlayerPose body;
    // jitter-3: stamped with the frame it was read on, not with this send.
    const std::uint64_t age_us = s.outbox_ms != 0 && s.outbox_ms <= now_ms && now_ms - s.outbox_ms < 1000
                                     ? (now_ms - s.outbox_ms) * 1000
                                     : 0;
    body.time = {server_now_us - std::min(server_now_us, age_us), ++s.sequence};
    body.skeleton = s.outbox->skeleton;
    // Sent even with no bone changed: the receiver keeps its puppet's pose
    // fresh (Drives) and its blend interval right.
    // puppet-budget: everything this send carries fits plan.bytes on the wire (headers included):
    // first up to kStillBones whole bones (issue 152: a bone held still at its own value), then
    // the held pieces (issue 158) in at most half of what is left, then Link's moving bones in the
    // rest. A bone that goes whole is not sent again as a moving one.
    std::size_t left = s.plan.bytes - kPoseHeaderBytes;
    std::vector<proto::PoseBone> whole;
    if (s.still_outbox && s.still_outbox->skeleton == body.skeleton) {
        whole = s.still_picker.Pick(body.skeleton, Encode(*s.still_outbox), now_ms, kStillBones, left);
    }
    std::vector<std::uint16_t> going;
    going.reserve(whole.size());
    for (const proto::PoseBone& b : whole) {
        going.push_back(b.bone);
        left -= WireBytes(b);
    }
    // Each piece in our hands as a pose of its own skeleton: its changed bones, and the rest
    // now and then while it holds still (a late joiner, a lost packet). The first piece to
    // pick turns each send, so one piece moving all the time never starves another.
    std::size_t room = left / 2;
    std::vector<proto::PlayerPose> pieces;
    const std::size_t n_pieces = s.pieces_outbox.size();
    const std::size_t turn = n_pieces == 0 ? 0 : s.piece_turn++ % n_pieces;
    for (std::size_t k = 0; k < n_pieces; ++k) {
        const Pose& piece = s.pieces_outbox[(turn + k) % n_pieces];
        auto& p = s.pieces_sent[piece.skeleton];
        p.out_ms = now_ms;
        if (room <= kPoseHeaderBytes) continue;
        proto::PlayerPose m;
        m.time = {server_now_us, 0};
        m.skeleton = piece.skeleton;
        m.bones = p.picker.Pick(piece.skeleton, Encode(piece), now_ms, room - kPoseHeaderBytes);
        if (m.bones.empty()) continue;
        room -= WireBytes(m);
        left -= WireBytes(m);
        pieces.push_back(std::move(m));
    }
    body.bones = MergeBones(s.picker.Pick(body.skeleton, Encode(*s.outbox), now_ms, left, going), whole);
    s.last_bones = body.bones.size();
    s.last_bytes = WireBytes(body);
    out.push_back(std::move(body));
    ++s.sent;
    for (proto::PlayerPose& m : pieces) {
        m.time.sequence = ++s.sequence;
        s.last_bytes += WireBytes(m);
        out.push_back(std::move(m));
        ++s.pieces_sent_n;
    }
    std::erase_if(s.pieces_sent, [&](const auto& kv) {
        return now_ms - kv.second.out_ms > 10 * kPieceRefreshMs;  // put away long ago
    });
    return out;
}

void Send(coop::net::Client& client, std::uint64_t now_ms) {
    const std::uint64_t server_now_us = client.server_now_us();
    SetClock(server_now_us, now_ms);
    for (const proto::PlayerPose& m : TakeOutgoing(server_now_us, now_ms)) {
        client.send(m);
    }
}

void OnEvent(const coop::net::Event& event, std::uint64_t now_ms) {
    if (event.kind != coop::net::EventKind::message) return;
    const auto* m = std::get_if<proto::PlayerPose>(&event.message);
    if (m == nullptr) return;
    Receive(*m, now_ms);
}

void SetClock(std::uint64_t server_now_us, std::uint64_t now_ms) {
    if (server_now_us == 0) return;
    State& s = S();
    std::lock_guard lock(s.mutex);
    s.offset_ms = static_cast<std::int64_t>(server_now_us / 1000) - static_cast<std::int64_t>(now_ms);
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
    // jitter-3: its sample time on our clock (sane: not ahead, not stale).
    std::uint64_t made_ms = 0;
    if (s.offset_ms && m.time.server_time_us != 0) {
        const std::int64_t made = static_cast<std::int64_t>(m.time.server_time_us / 1000) - *s.offset_ms;
        if (made > 0 && static_cast<std::uint64_t>(made) <= now_ms + 50 &&
            now_ms - std::min(now_ms, static_cast<std::uint64_t>(made)) < 2000) {
            made_ms = static_cast<std::uint64_t>(made);
        }
    }
    track.Add(std::move(pose), now_ms, made_ms);  // idle-twitch: slow bones eased
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
