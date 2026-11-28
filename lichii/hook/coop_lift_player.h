// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native puppets phase 3: one player lifts, carries, throws or sets down another (docs/native-puppets.md,
// "Lifting another player"; docs/protocol.md, "Player lift"). The rules only, without any game or network
// code (tests/lichii/lift_player_test.cpp); coop_lift_player_game.cpp is the glue.
//
// - Owner rule (2026-11-19): only a YOUNG (child) Link can be lifted, carried as Link carries Ruto in Jabu-Jabu;
//   any lifter age may lift him (the game's own lift rules decide; no lifter-age code here). Gated on every side:
//   the pack gives only the child copies (CoopPuppet_Child*) a LiftableRef, the Lifter asks only for a copy whose
//   player presence says child (CopyView::young, re-checked every frame), the server refuses an ask to a player
//   whose last sample is not child (not_young), and the lifted game refuses (Local::young) and, lifted, frees
//   itself the moment its Link is not child any more (not_young): its Link is its own again and falls by its own
//   character controller, the game's own drop.
// - The lift itself is the game's own: the child copy built with the pack switch --puppet-liftable has a
//   LiftableRef, Ruto's (Npc_Zora_Ruto_Child_001's LiftableParam: her LiftableType, offsets and weight), so our
//   Link's own lifter picks it up, carries it and throws or sets it down with the game's Ruto set (Player AS:
//   Lift_Ready_Ruto, Lift_Up_Ruto, Pose_Ruto / _Run / _Caution, Lift_Put_Ruto, Lift_Throw_Ruto).
// - The carried Link's pose: Ruto's own carried pose (her clip Act_LiftRest_Wait). While lifted, the lifted game
//   sends kCarriedPose ("Eaten") as its Link's animation (CarriedAnimation), so every copy of it plays that
//   command. On a young copy built with --puppet-liftable, "Eaten" plays Ruto's clip retargeted on Link's skeleton
//   (the copy's own AS root, tools/puppet_pack/asb_carry.py); where that AS could not be made, the copy plays
//   Link's own Eaten (a Like Like's hold), the fallback. Our own Link on our own screen plays what its own
//   action AI picks (the game's AS, unchanged: solo play is untouched); CarriedPose logs it (UNKNOWN 7).
// - The lifter's game sees the lift on the copy: the game raised it kRiseM over the place the director drew
//   it, within kReachM of our Link, kRiseFrames frames running, while our own Link's main animation slot plays a
//   lift (OurLinkLifts: a copy raised by anything else, a rising platform under a --puppet-body copy, a step, our
//   Link pushing under it, is not our lift; our Link's animation not read is no lift either). It sends LIFT_ASK and stops placing the copy
//   (the director leaves it to the game: CoopPuppet::SetGameHolds) while it waits for the answer.
// - The lifted player's game decides (Victim::Ask): no while its Link is in an event, a load or a menu, dead,
//   already lifted, lifting someone itself, or not near the lifter (the lifter's place not known, or in another
//   scene, is "not near": a stray ask never takes a player's input away).
// - Taken: the lifter's game sends the copy's place as the game has it, each frame (LIFT_PLACE held); when the
//   copy leaves our Link's hands (thrown or set down: it is kReleaseM further out or lower than it was carried)
//   the places go on as flying until the copy is at rest (kRestFrames still frames, or kFlightMaxUs), then one
//   landed place ends the lift. The lifted game puts its real Link on each place with the game's own
//   physics-aware setter and answers "input off" to its Link's own input check meanwhile; after the landed
//   place its Link is its own again (it falls and lands by its own character controller).
// - The lifted game ends a lift by itself (LIFT_ANSWER freed) when its Link goes into an event or dies, when
//   the places stop for kPlaceWaitUs (the lifter left or lost the copy), or after kMaxHeldUs (it wriggles free).
// - Break free (owner 2026-11-27: "players should be able to break free from being grabbed"): the lifted player
//   struggles as when an enemy holds Link (a Like Like, a ReDead): every new press of a face button and every new
//   push of the stick in a direction while lifted fills a meter (Struggle), which drains kStruggleDrainPerS a
//   second; at kStruggleFull the lifted game frees itself (LIFT_ANSWER freed, as every other free), its Link falls
//   by its own character controller, and for kFreeGraceUs a new ask from anyone is refused (busy), so a freed
//   player is not caught again the next frame. The counts are ours (the game's own grab escape count is not
//   read): about 1.5 s of mashing, which a held stick or a held button never fills.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace CoopLiftPlayer {

namespace proto = coop::protocol;
using Vec3 = std::array<float, 3>;

inline constexpr float kRiseM = 0.30f;     // the game raised the copy this far over where we drew it ...
inline constexpr float kReachM = 1.6f;     // ... within this of our Link ...
inline constexpr int kRiseFrames = 2;      // ... this many frames running: our Link lifted it
inline constexpr std::uint64_t kAnswerWaitUs = 1'500'000;  // no answer: let go
inline constexpr float kReleaseM = 0.6f;   // further out or lower than carried by this: thrown or set down
inline constexpr float kRestM = 0.02f;     // a flying copy moving less than this a frame ...
inline constexpr int kRestFrames = 8;      // ... this many frames running is at rest
inline constexpr std::uint64_t kFlightMaxUs = 2'000'000;   // a flight ends by this anyway
inline constexpr std::uint64_t kPlaceWaitUs = 1'000'000;   // lifted, no place this long: free
inline constexpr std::uint64_t kMaxHeldUs = 15'000'000;    // lifted this long: wriggles free
inline constexpr float kStruggleFull = 10.0f;              // break free: this many presses/pushes ...
inline constexpr float kStruggleDrainPerS = 2.0f;          // ... less this much a second of the hold
inline constexpr std::uint64_t kFreeGraceUs = 2'000'000;   // struggled free: no new lift this long

// The new presses in a pad read: bits of `now` that were not set in `before` (the caller gives only the
// struggle bits: face buttons and the stick's four directions).
inline int NewPresses(std::uint64_t before, std::uint64_t now) {
    std::uint64_t edges = now & ~before;
    int n = 0;
    for (; edges != 0; edges &= edges - 1) ++n;
    return n;
}
inline constexpr float kFarM = 3.0f;       // the lifter's Link must be this near ours
// The input-off answer holds only while the lift's frame keeps renewing it: a stamp older than this is "on"
// (the hook's frame stopped being called while lifted: our Link's input comes back by itself).
inline constexpr std::uint64_t kInputOffStaleUs = 500'000;

// Whether our Link's own main animation (AS slot 0, PlayerReading::animation) is a lift or a carry: the
// lifter's own state, so only our Link's own lift can start an ask. Unset (not read): no. The game's exact
// command names are UNKNOWN (docs/native-puppets.md, phase 3 probes): every name with "lift" or "carry" in it,
// any case; the glue logs the name it saw whenever a copy rose without it, so the proof run settles the list.
inline bool OurLinkLifts(const std::optional<std::string_view>& animation) {
    if (!animation) return false;
    auto has = [&](std::string_view word) {
        const std::string_view n = *animation;
        for (std::size_t i = 0; i + word.size() <= n.size(); ++i) {
            bool same = true;
            for (std::size_t k = 0; k < word.size() && same; ++k) {
                char c = n[i + k];
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                same = c == word[k];
            }
            if (same) return true;
        }
        return false;
    };
    return has("lift") || has("carry") || has("pose_ruto");  // Pose_Ruto: Link's own carry of Ruto
}

// The carried Link's pose on every copy (see the top): the AS command "Eaten" from frame 0 at rate 1 (the director
// sets the frame once and plays on at the rate, so the loop runs, not frame 0 held). On a young copy's own carry
// AS its clips are Ruto's Act_LiftRest_Wait; else Link's own Eaten_F / Eaten_B (Player_Child_01_Animation).
// UNKNOWN in game: that it plays so (docs/native-puppets.md, phase 3 probes, UNKNOWN 7).
inline constexpr std::string_view kCarriedPose = "Eaten";
inline constexpr float kCarriedRate = 1.0f;  // played on (looped)
inline proto::Animation CarriedAnimation() {
    return proto::Animation{std::string(kCarriedPose), 0.0f, kCarriedRate};
}

// The input-off answer at `now_us` for the stamp the lift's frame left (0: off).
inline bool InputOffFresh(std::uint64_t stamp_us, std::uint64_t now_us) {
    return stamp_us != 0 && (now_us < stamp_us || now_us - stamp_us <= kInputOffStaleUs);
}

inline float Distance(const Vec3& a, const Vec3& b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
inline float Flat(const Vec3& a, const Vec3& b) {
    return std::hypot(a[0] - b[0], a[2] - b[2]);
}

// One copy of another player's Link as the director left it this frame: where it drew the copy last, and
// where the game had it when the director came back (before its write), if read.
struct CopyView {
    std::uint8_t number = 0;
    Vec3 drawn{};
    std::optional<Vec3> game{};
    bool young = false;  // its player's presence says child now; not known: no (only a young Link is lifted)
};

// The lifter's side: at most one lift at a time.
class Lifter {
public:
    enum class State { idle, asking, holding, flying };
    struct Out {
        std::optional<proto::LiftAsk> ask;
        std::optional<proto::LiftPlace> place;
        // A copy rose as a lift would but our Link's own animation said no lift: not asked (the probe logs it).
        std::uint8_t rose_alone = 0;
    };

    // Our Link's own lift state this frame (OurLinkLifts), set before Frame. Not set: no lift.
    void SetOurLinkLifts(bool lifts) { our_lifts_ = lifts; }

    Out Frame(const Vec3& our_link, const std::vector<CopyView>& copies, std::uint64_t now_us) {
        Out out;
        const CopyView* mine = nullptr;
        for (const auto& c : copies) {
            if (c.number == who_ && who_ != 0) mine = &c;
        }
        switch (state_) {
        case State::idle:
            Watch(our_link, copies, now_us, out);
            break;
        case State::asking:
            if (now_us - since_us_ >= kAnswerWaitUs || mine == nullptr || !mine->young) {
                Drop();  // no answer, the copy is gone, or it is not young now: the director places it again
            }
            break;
        case State::holding:
        case State::flying:
            if (mine == nullptr || !mine->game || !mine->young) {
                // Not young any more (grown up while carried; the lifted game frees itself too): let go there.
                // The copy is gone (they left, a load): the last place we had ends the lift.
                if (last_) out.place = Place(proto::LiftPhase::landed, *last_);
                Drop();
                break;
            }
            Carry(our_link, *mine->game, now_us, out);
            break;
        }
        // A refused or ended copy is asked again only once the game lets it down.
        for (auto it = blocked_.begin(); it != blocked_.end();) {
            const CopyView* c = nullptr;
            for (const auto& v : copies) {
                if (v.number == *it) c = &v;
            }
            if (c == nullptr || !c->game || (*c->game)[1] - c->drawn[1] < kRiseM / 2) {
                it = blocked_.erase(it);
            } else {
                ++it;
            }
        }
        return out;
    }

    // The lifted game's answer (or the server's not_sharing).
    void Answer(const proto::LiftAnswer& a, std::uint64_t now_us) {
        if (a.player_number != who_ || who_ == 0) return;
        if (state_ == State::asking && a.refusal == proto::LiftRefusal::none) {
            state_ = State::holding;
            since_us_ = now_us;
            carried_.reset();
            return;
        }
        if (a.refusal != proto::LiftRefusal::none) {
            last_refusal_ = a.refusal;
            Drop();
        }
    }

    // The copy the director leaves to the game (asking, holding or flying); 0 none.
    std::uint8_t Held() const { return state_ == State::idle ? 0 : who_; }
    State Now() const { return state_; }
    std::optional<proto::LiftRefusal> LastRefusal() const { return last_refusal_; }
    bool Blocked(std::uint8_t number) const {
        for (auto b : blocked_) {
            if (b == number) return true;
        }
        return false;
    }
    void Reset() { *this = Lifter{}; }

private:
    struct Carried {
        float flat = 0.0f;    // from our Link, across
        float height = 0.0f;  // over our Link
    };

    void Watch(const Vec3& our_link, const std::vector<CopyView>& copies, std::uint64_t now_us, Out& out) {
        for (const auto& c : copies) {
            int& n = rising_[c.number];
            if (!c.young) {
                n = 0;  // an adult (or age not known) is never lifted: no ask, no probe line
                continue;
            }
            const bool rose = c.game && (*c.game)[1] - c.drawn[1] >= kRiseM && Distance(*c.game, our_link) <= kReachM;
            if (rose && !our_lifts_) {
                // Raised, but not by our Link's own lift (a platform, a step, a push): never an ask.
                if (n >= 0 && c.number != 0) out.rose_alone = c.number;
                n = -1;  // logged once per rise
                continue;
            }
            n = rose ? std::max(n, 0) + 1 : 0;
            if (n >= kRiseFrames && !Blocked(c.number) && c.number != 0) {
                state_ = State::asking;
                who_ = c.number;
                since_us_ = now_us;
                last_ = c.game;
                out.ask = proto::LiftAsk{proto::kPlayerSelf, c.number};
                rising_.clear();
                return;
            }
        }
    }

    void Carry(const Vec3& our_link, const Vec3& at, std::uint64_t now_us, Out& out) {
        const Carried now{Flat(at, our_link), at[1] - our_link[1]};
        if (state_ == State::holding) {
            if (!carried_) carried_ = now;
            if (now.flat > carried_->flat + kReleaseM || now.height < carried_->height - kReleaseM) {
                state_ = State::flying;  // out of our Link's hands: thrown, or set down
                since_us_ = now_us;
                still_ = 0;
            } else {
                out.place = Place(proto::LiftPhase::held, at);
                last_ = at;
                return;
            }
        }
        // Flying: on until it rests.
        still_ = last_ && Distance(at, *last_) < kRestM ? still_ + 1 : 0;
        last_ = at;
        if (still_ >= kRestFrames || now_us - since_us_ >= kFlightMaxUs) {
            out.place = Place(proto::LiftPhase::landed, at);
            Drop();
            return;
        }
        out.place = Place(proto::LiftPhase::flying, at);
    }

    proto::LiftPlace Place(proto::LiftPhase phase, const Vec3& at) const {
        return proto::LiftPlace{proto::kPlayerSelf, who_, phase, at};
    }

    void Drop() {
        if (who_ != 0 && !Blocked(who_)) blocked_.push_back(who_);
        state_ = State::idle;
        who_ = 0;
        carried_.reset();
        last_.reset();
        still_ = 0;
        rising_.clear();
    }

    State state_ = State::idle;
    std::uint8_t who_ = 0;
    std::uint64_t since_us_ = 0;
    std::optional<Carried> carried_;
    std::optional<Vec3> last_;
    int still_ = 0;
    std::map<std::uint8_t, int> rising_;
    std::vector<std::uint8_t> blocked_;
    std::optional<proto::LiftRefusal> last_refusal_;
    bool our_lifts_ = false;
};

// Our own game, as the lifted side sees it.
struct Local {
    std::uint8_t self = 0;
    bool event = false;     // our Link in an event, a load or a menu (PLAYER_STATUS away bits but dead)
    bool down = false;      // dead, life 0, out or watching (spectating): never lifted. Row 317: NOT a Link
                            // down but not out (`downed`): that one may be lifted and carried to safety
    bool downed = false;    // down but not out (row 317): lifted like any young Link, never struggles free
    bool lifting = false;   // our Link holds another player's copy (Lifter::Held)
    bool young = false;     // our Link is child now (PlayerReading::age); not read: no
    std::optional<Vec3> our_link;
};

// The lifted player's side: our game decides.
class Victim {
public:
    struct Step {
        std::optional<Vec3> put;                   // our Link's place this frame
        bool input_off = false;                    // our Link's own input check answers "off"
        std::optional<proto::LiftAnswer> freed;    // the lift ended here: tell the lifter
        bool struggled = false;                    // freed by the player's own struggle
    };

    // A LIFT_ASK; `lifter_at` is the lifter's Link as presence has it in our place, either era (unset: not known, or
    // another place: refused too_far).
    proto::LiftAnswer Ask(const proto::LiftAsk& m, const Local& local, const std::optional<Vec3>& lifter_at,
                          std::uint64_t now_us) {
        proto::LiftAnswer a{proto::kPlayerSelf, m.player_number, proto::LiftRefusal::none};
        if (m.lifted != local.self || m.player_number == local.self || m.player_number == 0) {
            a.refusal = proto::LiftRefusal::busy;  // not for us (the server names us; kept for safety)
        } else if (!local.young) {
            a.refusal = proto::LiftRefusal::not_young;  // only a young Link is lifted (owner rule)
        } else if (by_ != 0 && by_ != m.player_number) {
            a.refusal = proto::LiftRefusal::busy;
        } else if (by_ == 0 && free_us_ != 0 && now_us >= free_us_ && now_us - free_us_ < kFreeGraceUs) {
            a.refusal = proto::LiftRefusal::busy;  // just struggled free: not caught again at once
        } else if (local.lifting) {
            a.refusal = proto::LiftRefusal::busy;
        } else if (local.down) {
            a.refusal = proto::LiftRefusal::down;
        } else if (local.event || !local.our_link) {
            a.refusal = proto::LiftRefusal::event;
        } else if (!lifter_at || Distance(*lifter_at, *local.our_link) > kFarM) {
            a.refusal = proto::LiftRefusal::too_far;
        }
        if (a.refusal == proto::LiftRefusal::none) {
            by_ = m.player_number;
            since_us_ = now_us;
            place_us_ = now_us;
            at_.reset();
            landed_ = false;
            meter_ = 0.0f;
            meter_us_ = now_us;
        }
        return a;
    }

    // A LIFT_PLACE: only the lifter's, only while lifted.
    void Place(const proto::LiftPlace& m, std::uint64_t now_us) {
        if (by_ == 0 || m.player_number != by_ || m.lifted == 0) return;
        at_ = m.position;
        place_us_ = now_us;
        landed_ = m.phase == proto::LiftPhase::landed;
    }

    // The new presses and pushes our player made since the last call (while lifted: else nothing).
    void Struggle(int presses, std::uint64_t now_us) {
        if (by_ == 0 || landed_) return;
        if (now_us > meter_us_) {
            meter_ -= kStruggleDrainPerS * static_cast<float>(now_us - meter_us_) / 1e6f;
        }
        meter_us_ = std::max(meter_us_, now_us);
        meter_ = std::max(0.0f, meter_ + static_cast<float>(std::max(presses, 0)));
    }
    float Meter() const { return meter_; }

    Step Frame(const Local& local, std::uint64_t now_us) {
        Step s;
        if (by_ == 0) return s;
        if (landed_) {
            s.put = at_;  // the last place, then our Link is its own
            End();
            return s;
        }
        proto::LiftRefusal why = proto::LiftRefusal::none;
        if (!local.young) {
            why = proto::LiftRefusal::not_young;  // grown up while lifted: free, our Link falls by its own
        } else if (local.down || local.event || !local.our_link) {
            why = proto::LiftRefusal::freed;
        } else if (now_us - place_us_ >= kPlaceWaitUs || now_us - since_us_ >= kMaxHeldUs) {
            why = proto::LiftRefusal::freed;
        } else if (meter_ >= kStruggleFull) {
            why = proto::LiftRefusal::freed;  // struggled free
            s.struggled = true;
            free_us_ = now_us;
        }
        if (why != proto::LiftRefusal::none) {
            s.freed = proto::LiftAnswer{proto::kPlayerSelf, by_, why};
            End();
            return s;
        }
        s.put = at_;
        s.input_off = true;
        return s;
    }

    std::uint8_t By() const { return by_; }
    // Lifted now (asked yes, not ended): our Link's samples carry CarriedAnimation.
    bool Carried() const { return by_ != 0; }
    void Reset() { *this = Victim{}; }

private:
    void End() {
        by_ = 0;
        at_.reset();
        landed_ = false;
        meter_ = 0.0f;
    }

    std::uint8_t by_ = 0;
    std::uint64_t since_us_ = 0;
    std::uint64_t place_us_ = 0;
    std::optional<Vec3> at_;
    bool landed_ = false;
    float meter_ = 0.0f;           // the struggle (Struggle)
    std::uint64_t meter_us_ = 0;
    std::uint64_t free_us_ = 0;    // when our Link last struggled free (0 never)
};

} // namespace CoopLiftPlayer
