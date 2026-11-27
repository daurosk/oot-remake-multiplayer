// SPDX-License-Identifier: GPL-3.0-or-later
//
// Row 307 (a character's puppet runs like a fast walk): where the game keeps Link's AS variables.
//
// The player's AS root (AS/Player_Child.root.asb, AS/Player_Adult.root.asb) chooses and blends its clips by AS
// variables. Its variable table (header word 14, u32 name offsets into the string pool) names them in groups: s32
// from 14 (CutComboPattern) to 57 (BGMLastPlayNote), f32 from 58 (LeftStickLength) to 157 (BGMPlayRate), bool from
// 158 (WeaponBurn) to 234, triggers 235 to 251, LinearVel 252 (read from the game's own file with
// tools/puppet_pack/asb_carry.py's pool reader, 2026-11-27). The 'Move' command's walk/run blend reads the stick and
// speed ones (LeftStickLength 58, LinearVelXZ 66, XZSpeed 112, IsDash 173). The player's own code writes them every
// frame; a puppet has no player code, so nothing writes them there. A Link puppet hides that under the pose copy
// (CoopPose); a Character Select character's puppet takes no pose copy and plays 'Move' at the sender's rate with the
// walk clip's weight: a walk played fast.
//
// Where those values sit in the AS controller was not found by reading `main` (no code reaches the names: the AS
// looks them up by index). This probe finds it in a run. It only reads, and is off unless
// LICHII_COOP_AS_VARS_PROBE=1. Every 100 ms it reads our own Link's AS controller ([[[[actor + 0x270] + 0x40] +
// 0x18] + 0x18], docs/puppet.md "Animation") and every block one or two pointers away from it, as floats, together
// with our Link's measured speed over the ground (its place, actor + 0x31c, between two reads). Every 5 s it logs the
// float words that follow the speed best (Pearson r over all reads so far), each by its path from the controller, so
// the same path can be read on a puppet; then each puppet's value at the best paths (a character's puppet should read
// 0 there while its player runs, if this is the cause). Log: `as-vars probe: ...`.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace CoopAsVars {

// A float word's place: from the controller, follow the pointer at hops[0] (and then at hops[1]), then + offset.
struct Spot {
    std::uint8_t depth = 0;  // 0: in the controller itself, 1 or 2: pointers followed
    std::array<std::uint16_t, 2> hops{};
    std::uint16_t offset = 0;
    bool operator==(const Spot&) const = default;
};

inline std::string SpotText(const Spot& s) {
    char buf[64];
    if (s.depth == 0) {
        std::snprintf(buf, sizeof(buf), "ctl:+0x%x", s.offset);
    } else if (s.depth == 1) {
        std::snprintf(buf, sizeof(buf), "ctl+0x%x>:+0x%x", s.hops[0], s.offset);
    } else {
        std::snprintf(buf, sizeof(buf), "ctl+0x%x>+0x%x>:+0x%x", s.hops[0], s.hops[1], s.offset);
    }
    return buf;
}

class Probe {
public:
    // Reads `size` bytes of guest memory at `address`; false when it is not mapped.
    using Read = std::function<bool(std::uint64_t address, void* out, std::size_t size)>;

    static constexpr std::size_t kControllerBytes = 0x800;
    static constexpr std::size_t kRegionBytes = 0x400;
    static constexpr std::size_t kHopScanBytes = 0x100;  // pointers taken from the first part of a depth-1 block
    static constexpr std::size_t kMaxRegions = 200;
    static constexpr std::uint64_t kEveryMs = 100;
    static constexpr std::uint64_t kReportMs = 5000;
    static constexpr std::size_t kTop = 12;
    static constexpr std::size_t kPuppetSpots = 6;
    static constexpr int kMaxLines = 400;
    static constexpr std::size_t kMinReads = 30;

    // Once a frame with our own Link's AS controller (0 when none) and place; `puppets` are (label, AS controller)
    // of the live puppets. Returns log lines (without the "as-vars probe: " head).
    std::vector<std::string> Look(const Read& read, std::uint64_t controller, const std::array<float, 3>& place,
                                  std::uint64_t now_ms,
                                  const std::vector<std::pair<std::string, std::uint64_t>>& puppets) {
        std::vector<std::string> out;
        if (controller == 0 || lines_ >= kMaxLines || (last_ms_ != 0 && now_ms - last_ms_ < kEveryMs)) {
            return out;
        }
        if (controller != controller_) {
            controller_ = controller;
            Map(read);
            stats_.assign(regions_.size() * kFloats, Stat{});
            have_place_ = false;
            Say(out, "our controller 0x" + Hex(controller) + ": " + std::to_string(regions_.size()) +
                         " blocks to read");
        }
        const std::uint64_t dt = last_ms_ == 0 ? 0 : now_ms - last_ms_;
        last_ms_ = now_ms;
        const bool moved = have_place_ && dt > 0 && dt < 1000;
        const float speed = moved ? std::hypot(place[0] - place_[0], place[2] - place_[2]) * 1000.0f /
                                        static_cast<float>(dt)
                                  : 0.0f;
        place_ = place;
        have_place_ = true;
        if (!moved || !std::isfinite(speed)) {
            return out;
        }
        ++reads_;
        speed_min_ = reads_ == 1 ? speed : std::min(speed_min_, speed);
        speed_max_ = reads_ == 1 ? speed : std::max(speed_max_, speed);
        speed_now_ = speed;
        std::array<float, kFloats> block{};
        for (std::size_t r = 0; r < regions_.size(); ++r) {
            const auto at = Resolve(read, controller, regions_[r]);
            if (!at || !read(*at, block.data(), kRegionBytes)) continue;
            for (std::size_t i = 0; i < kFloats; ++i) {
                const float x = block[i];
                if (!std::isfinite(x) || std::fabs(x) > 1e5f) continue;
                Stat& s = stats_[r * kFloats + i];
                const double dx = x;
                const double dy = speed;
                if (s.n == 0) s.min = s.max = x;
                s.min = std::min(s.min, x);
                s.max = std::max(s.max, x);
                s.now = x;
                ++s.n;
                s.sx += dx;
                s.sy += dy;
                s.sxx += dx * dx;
                s.syy += dy * dy;
                s.sxy += dx * dy;
            }
        }
        if (reads_ >= kMinReads && now_ms - report_ms_ >= kReportMs && speed_max_ - speed_min_ > 0.5f) {
            report_ms_ = now_ms;
            Report(read, out, puppets);
        }
        return out;
    }

    // For tests: the best spots of the last report.
    const std::vector<Spot>& Best() const { return best_; }

private:
    static constexpr std::size_t kFloats = kRegionBytes / 4;
    struct Stat {
        std::size_t n = 0;
        double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
        float min = 0, max = 0, now = 0;
    };

    static std::string Hex(std::uint64_t v) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(v));
        return buf;
    }

    void Say(std::vector<std::string>& out, std::string line) {
        if (lines_ < kMaxLines) {
            ++lines_;
            out.push_back(std::move(line));
        }
    }

    static bool Pointer(const Read& read, std::uint64_t w) {
        std::uint64_t probe = 0;
        return w % 8 == 0 && w >= 0x10000000ull && read(w, &probe, sizeof(probe));
    }

    static std::optional<std::uint64_t> Resolve(const Read& read, std::uint64_t controller, const Spot& s) {
        std::uint64_t at = controller;
        for (std::uint8_t d = 0; d < s.depth; ++d) {
            std::uint64_t next = 0;
            if (!read(at + s.hops[d], &next, sizeof(next)) || !Pointer(read, next)) return std::nullopt;
            at = next;
        }
        return at;
    }

    void Map(const Read& read) {
        regions_.clear();
        regions_.push_back(Spot{});  // the controller's own first kRegionBytes
        std::vector<std::uint64_t> words(kControllerBytes / 8);
        if (!read(controller_, words.data(), kControllerBytes)) return;
        std::vector<Spot> first;
        for (std::size_t i = 0; i < words.size() && regions_.size() < kMaxRegions; ++i) {
            if (!Pointer(read, words[i])) continue;
            Spot s;
            s.depth = 1;
            s.hops[0] = static_cast<std::uint16_t>(i * 8);
            regions_.push_back(s);
            first.push_back(s);
        }
        for (const Spot& f : first) {
            const auto at = Resolve(read, controller_, f);
            std::vector<std::uint64_t> inner(kHopScanBytes / 8);
            if (!at || !read(*at, inner.data(), kHopScanBytes)) continue;
            for (std::size_t i = 0; i < inner.size() && regions_.size() < kMaxRegions; ++i) {
                if (!Pointer(read, inner[i])) continue;
                Spot s = f;
                s.depth = 2;
                s.hops[1] = static_cast<std::uint16_t>(i * 8);
                regions_.push_back(s);
            }
        }
    }

    void Report(const Read& read, std::vector<std::string>& out,
                const std::vector<std::pair<std::string, std::uint64_t>>& puppets) {
        struct Hit {
            double r;
            std::size_t at;
        };
        std::vector<Hit> hits;
        for (std::size_t k = 0; k < stats_.size(); ++k) {
            const Stat& s = stats_[k];
            if (s.n < kMinReads || s.max - s.min < 1e-3f) continue;
            const double n = static_cast<double>(s.n);
            const double vx = s.sxx - s.sx * s.sx / n;
            const double vy = s.syy - s.sy * s.sy / n;
            if (vx <= 1e-9 || vy <= 1e-9) continue;
            const double r = (s.sxy - s.sx * s.sy / n) / std::sqrt(vx * vy);
            if (std::fabs(r) >= 0.6) hits.push_back({r, k});
        }
        std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return std::fabs(a.r) > std::fabs(b.r); });
        char buf[200];
        std::snprintf(buf, sizeof(buf), "%zu reads, our speed now %.2f (%.2f to %.2f), %zu words with |r| >= 0.6",
                      reads_, speed_now_, speed_min_, speed_max_, hits.size());
        Say(out, buf);
        best_.clear();
        for (std::size_t j = 0; j < hits.size() && j < kTop; ++j) {
            const Stat& s = stats_[hits[j].at];
            Spot spot = regions_[hits[j].at / kFloats];
            spot.offset = static_cast<std::uint16_t>((hits[j].at % kFloats) * 4);
            best_.push_back(spot);
            std::snprintf(buf, sizeof(buf), "r=%.3f %s now %.3f (%.3f to %.3f)", hits[j].r, SpotText(spot).c_str(),
                          s.now, s.min, s.max);
            Say(out, buf);
        }
        for (const auto& [label, controller] : puppets) {
            if (controller == 0) continue;
            std::string line = label + ":";
            for (std::size_t j = 0; j < best_.size() && j < kPuppetSpots; ++j) {
                Spot base = best_[j];
                const std::uint16_t offset = base.offset;
                base.offset = 0;
                const auto at = Resolve(read, controller, base);
                float v = 0.0f;
                if (at && read(*at + offset, &v, sizeof(v))) {
                    std::snprintf(buf, sizeof(buf), " %s %.3f", SpotText(best_[j]).c_str(), v);
                } else {
                    std::snprintf(buf, sizeof(buf), " %s unread", SpotText(best_[j]).c_str());
                }
                line += buf;
            }
            Say(out, line);
        }
    }

    std::uint64_t controller_ = 0;
    std::vector<Spot> regions_;
    std::vector<Stat> stats_;
    std::vector<Spot> best_;
    std::array<float, 3> place_{};
    bool have_place_ = false;
    std::uint64_t last_ms_ = 0;
    std::uint64_t report_ms_ = 0;
    std::size_t reads_ = 0;
    float speed_min_ = 0, speed_max_ = 0, speed_now_ = 0;
    int lines_ = 0;
};

}  // namespace CoopAsVars
