// SPDX-License-Identifier: GPL-3.0-or-later
// The Scrub Brothers' hits shared (coop_scrubs.h, issue 149).
#include "coop_scrubs.h"

#include <algorithm>
#include <array>
#include <cstdio>

namespace CoopScrubs {

namespace {

namespace proto = coop::protocol;

// Read from the Banc files (romfs, build e0f3ccfaf50065ae). The managers'
// ScrubDekuLink lists these three brothers each.
constexpr std::uint64_t kDekuTreeManager = 0xeb15dea71838d33dULL;
constexpr std::uint64_t kKokiriChildManager = 0xb6df77f3871bf915ULL;
constexpr std::uint64_t kKokiriAdultManager = 0xdb89f80f885aa368ULL;

constexpr std::array<Brother, 9> kBrothers{{
    {0x927ab536d2e86500ULL, kDekuTreeManager, 1, "Two"},
    {0x079a5f22c7ad91c1ULL, kDekuTreeManager, 2, "Three"},
    {0x9b04ae25f50c7fcaULL, kDekuTreeManager, 3, "One"},
    {0x1ecd1043f21dc0b2ULL, kKokiriChildManager, 1, "Two"},
    {0x8af1fb9d49d1185cULL, kKokiriChildManager, 2, "Three"},
    {0xa46d124b198abdc1ULL, kKokiriChildManager, 3, "One"},
    {0xf5f713563d5db41fULL, kKokiriAdultManager, 1, "Two"},
    {0x783b8992ce40cc03ULL, kKokiriAdultManager, 2, "Three"},
    {0x7e432fa8180b1382ULL, kKokiriAdultManager, 3, "One"},
}};

std::string Hex(std::uint64_t v) {
    char text[24];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(v));
    return text;
}

std::string Label(const Brother& b) {
    return "brother " + std::string(b.name) + " " + Hex(b.hash);
}

} // namespace

std::span<const Brother> Brothers() {
    return kBrothers;
}

const Brother* FindBrother(std::uint64_t hash) {
    for (const Brother& b : kBrothers) {
        if (b.hash == hash) {
            return &b;
        }
    }
    return nullptr;
}

bool IsManager(std::uint64_t hash) {
    return hash == kDekuTreeManager || hash == kKokiriChildManager || hash == kKokiriAdultManager;
}

bool IsHitAnimation(std::string_view anim, const std::set<std::string>& names) {
    if (anim.empty()) {
        return false;
    }
    for (const std::string& n : names) {
        if (!n.empty() && anim.find(n) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

std::set<std::string> DefaultHitAnimations() {
    // Run 2026-11-11 (autotest deku-scrubs-solo): a seed on a brother before the last plays
    // Paralysis_Start; the last right hit plays SmallHit, then Escape, Damage, Stop.
    return {"Paralysis_Start", "SmallHit"};
}

bool IsIdleAnimation(std::string_view anim) {
    // The brothers' own cycle, logged only when LICHII_COOP_DEBUG_OBJECTS asks (the cap).
    constexpr std::string_view kIdle[] = {"Dig", "Sleep", "GetUpBrothers", "WaitBrothers",
                                          "Turn", "Attack", "Wait", "Loading_Ed"};
    for (const std::string_view i : kIdle) {
        if (anim == i) {
            return true;
        }
    }
    return false;
}

std::set<std::string> ParseAnimations(std::string_view text) {
    std::set<std::string> out;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        std::string_view part = text.substr(0, comma);
        while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
        while (!part.empty() && part.back() == ' ') part.remove_suffix(1);
        if (!part.empty()) {
            out.emplace(part);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    return out;
}

void Puzzle::Look(const std::string& scene, const std::vector<Seen>& seen,
                  const std::optional<proto::Vec3>& player, std::uint64_t now_us, Out& out) {
    if (scene != scene_) {
        scene_ = scene;
        first_seen_us_ = 0;
        animation_.clear();
        echo_until_.clear();
        if (!pending_.empty()) {
            out.notes.push_back("scrubs: " + std::to_string(pending_.size()) +
                                " hits from other games dropped: we left the scene");
            pending_.clear();
        }
    }
    here_.clear();
    bool brothers = false;
    std::map<std::uint64_t, std::string> animation;
    for (const Seen& s : seen) {
        here_[s.hash] = Here{s.address, s.id};
        const Brother* b = FindBrother(s.hash);
        if (b == nullptr) {
            continue;
        }
        brothers = true;
        const auto it = animation_.find(s.address);
        const std::string before = it != animation_.end() ? it->second : std::string();
        if (!s.animation.empty() && s.animation != before) {
            if (anim_lines_ < kMaxAnimLines && !IsIdleAnimation(s.animation)) {
                ++anim_lines_;
                char at[64];
                std::snprintf(at, sizeof at, " at %.2f %.2f %.2f", s.position[0], s.position[1], s.position[2]);
                out.notes.push_back("scrubs: " + Label(*b) + " (order " + std::to_string(b->order) + ", id " +
                                    std::to_string(s.id) + ") anim '" + s.animation + "'" + at);
            }
            // The first look at a brother is its baseline, never a hit.
            if (it != animation_.end() && IsHitAnimation(s.animation, hit_animations_) &&
                !IsHitAnimation(before, hit_animations_)) {
                const auto echo = echo_until_.find(b->hash);
                if (echo != echo_until_.end() && now_us < echo->second) {
                    out.notes.push_back("scrubs: " + Label(*b) +
                                        " hit by another game's hit given here: not sent back");
                } else {
                    proto::WorldHit h;
                    h.scene = scene;
                    h.hash = b->hash;
                    h.at = s.position;
                    h.from = player.value_or(s.position);
                    out.send.push_back(std::move(h));
                    out.notes.push_back("scrubs: " + Label(*b) + " hit here (order " + std::to_string(b->order) +
                                        "): sending");
                }
            }
        }
        animation[s.address] = s.animation.empty() ? before : s.animation;
    }
    animation_ = std::move(animation);
    if (!brothers) {
        first_seen_us_ = 0;  // out of their room: a later visit settles again
    } else if (first_seen_us_ == 0) {
        first_seen_us_ = now_us;
    }
    look_us_ = now_us;
}

bool Puzzle::Receive(const proto::WorldHit& h, std::uint64_t now_us) {
    const Brother* b = FindBrother(h.hash);
    if (b == nullptr) {
        return false;
    }
    const std::string from = "P" + std::to_string(h.player_number) + "'s hit on " + Label(*b);
    if (h.scene != scene_) {
        notes_.push_back("scrubs: " + from + " in " + h.scene + " dropped: not our scene");
    } else if (first_seen_us_ == 0) {
        notes_.push_back("scrubs: " + from + " dropped: our brothers are not loaded");
    } else if (now_us < first_seen_us_ + kSettleUs) {
        notes_.push_back("scrubs: " + from + " dropped: we just came (a kept hit of an earlier try)");
    } else {
        pending_.push_back(Want{b->hash, h.player_number, now_us});
        echo_until_[b->hash] = now_us + kEchoUs;
        notes_.push_back("scrubs: " + from + " (order " + std::to_string(b->order) + ") to give here");
    }
    return true;
}

std::optional<CoopPuppet::Call> Puzzle::Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                             std::uint64_t main, std::uint64_t now_us) {
    if (after_call) {
        const Next done = next_;
        next_ = Next::none;
        if (done == Next::malloc) {
            if (result == 0) {
                malloc_failed_ = true;
                notes_.push_back("scrubs: the game's malloc gave nothing; hits from other games off");
                pending_.clear();
                return std::nullopt;
            }
            block_ = result;
        } else if (done == Next::send) {
            if (const Brother* b = FindBrother(current_.hash)) {
                notes_.push_back("scrubs: P" + std::to_string(current_.player) + "'s hit on " + Label(*b) +
                                 " given to our manager (order " + std::to_string(b->order) + ", send gave " +
                                 std::to_string(static_cast<std::int32_t>(result)) + ")");
            }
        }
    }
    while (!pending_.empty()) {
        if (malloc_failed_) {
            pending_.clear();
            return std::nullopt;
        }
        if (block_ == 0) {
            CoopPuppet::Call c;
            c.function = main + addr::kMalloc;
            c.x[0] = addr::kBlockSize;
            next_ = Next::malloc;
            return c;
        }
        const Want w = pending_.front();
        pending_.pop_front();
        const Brother* b = FindBrother(w.hash);
        if (b == nullptr) {
            continue;
        }
        const std::string what = "scrubs: P" + std::to_string(w.player) + "'s hit on " + Label(*b);
        if (now_us > w.got_us + kWaitUs) {
            notes_.push_back(what + " dropped: waited too long");
            continue;
        }
        const auto brother = here_.find(b->hash);
        const auto manager = here_.find(b->manager);
        if (brother == here_.end() || manager == here_.end() || now_us > look_us_ + kFreshUs) {
            notes_.push_back(what + " dropped: our brother or manager is gone");
            continue;
        }
        std::uint32_t brother_id = 0;
        std::uint32_t manager_id = 0;
        if (!guest.Read(brother->second.address + addr::kTransceiverId, &brother_id, 4) ||
            !guest.Read(manager->second.address + addr::kTransceiverId, &manager_id, 4) || brother_id == 0 ||
            manager_id == 0 || brother_id != brother->second.id || manager_id != manager->second.id) {
            notes_.push_back(what + " dropped: our brother or manager changed since the walk");
            continue;
        }
        std::array<std::uint8_t, addr::kPayload + addr::kPayloadSize> block{};
        const std::uint32_t type = addr::kDefeatedOrderMessage;
        std::copy_n(reinterpret_cast<const std::uint8_t*>(&manager_id), 4, block.data() + addr::kHeader);
        std::copy_n(reinterpret_cast<const std::uint8_t*>(&type), 4, block.data() + addr::kHeader + 4);
        std::copy_n(reinterpret_cast<const std::uint8_t*>(&b->order), 4, block.data() + addr::kPayload);
        if (!guest.Write(block_, block.data(), block.size())) {
            notes_.push_back(what + " dropped: the block was not written");
            continue;
        }
        CoopPuppet::Call c;
        c.function = main + addr::kSend;
        c.x[0] = brother->second.address + addr::kTransceiver;
        c.x[1] = 0;
        c.x[2] = block_ + addr::kHeader;
        c.x[3] = block_ + addr::kPayload;
        next_ = Next::send;
        current_ = w;
        return c;
    }
    return std::nullopt;
}

std::vector<std::string> Puzzle::TakeNotes() {
    std::vector<std::string> out;
    out.swap(notes_);
    return out;
}

void Puzzle::Reset() {
    scene_.clear();
    first_seen_us_ = 0;
    look_us_ = 0;
    here_.clear();
    animation_.clear();
    echo_until_.clear();
    pending_.clear();
    // block_ is the game's memory, kept for the run (a new game run: Reset
    // by the caller with a new Puzzle).
}

} // namespace CoopScrubs
