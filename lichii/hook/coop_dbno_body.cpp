// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_dbno_body.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

namespace CoopDbno::Body {

namespace {
// "Crawl (234)" for the log (the offline graders match these).
std::string Named(std::int32_t v) {
    return std::string(VerbName(v)) + " (" + std::to_string(v) + ")";
}
}  // namespace

// --- Raising ---

void Raising::Start(std::int32_t to, std::uint64_t now_us) noexcept {
    to_.store(to, std::memory_order_release);
    since_us_ = now_us == 0 ? 1 : now_us;
}

std::optional<std::int32_t> Raising::Frame(std::optional<std::int32_t> life, std::uint64_t now_us) noexcept {
    const std::int32_t to = to_.load(std::memory_order_acquire);
    if (to < 0 || since_us_ == 0) {
        return std::nullopt;
    }
    if (life && *life >= to) {
        Drop();  // the step took it (or something raised it further)
        return std::nullopt;
    }
    if (now_us < since_us_ + kHealWaitUs) {
        return std::nullopt;
    }
    Drop();
    return to;
}

void Raising::Drop() noexcept {
    to_.store(-1, std::memory_order_release);
    since_us_ = 0;
}

// --- The adult's lying pose ---

bool Riding(std::string_view c) {
    const auto starts = [c](std::string_view p) { return c.substr(0, p.size()) == p; };
    return starts("Horse") || starts("AimHorse") || starts("Rideon") || starts("RideOn") || c == "DismountHorse";
}

std::optional<AsSlot> ReadSlot0(CoopPuppet::Guest& guest, std::uint64_t actor) {
    namespace pa = CoopPuppet::addr;
    std::uint64_t at = actor;
    for (const std::uint64_t step : pa::kControllerPath) {
        std::uint64_t next = 0;
        if (at == 0 || !guest.Read(at + step, &next, sizeof next) || next == 0) {
            return std::nullopt;
        }
        at = next;
    }
    std::int32_t count = 0;
    std::uint64_t slots = 0, slot = 0, text = 0;
    if (!guest.Read(at + pa::kSlotCount, &count, sizeof count) || count < 1 ||
        !guest.Read(at + pa::kSlots, &slots, sizeof slots) || slots == 0 ||
        !guest.Read(slots, &slot, sizeof slot) || slot == 0 ||
        !guest.Read(slot + pa::kSlotName, &text, sizeof text) || text == 0) {
        return std::nullopt;
    }
    AsSlot out;
    std::array<char, 64> name{};
    if (!guest.Read(text, name.data(), name.size() - 1) || !guest.Read(slot + pa::kSlotFrame, &out.frame, sizeof out.frame) ||
        !guest.Read(slot + addr::kAsSlotRate, &out.rate, sizeof out.rate) || !std::isfinite(out.frame) ||
        !std::isfinite(out.rate)) {
        return std::nullopt;
    }
    out.name.assign(name.data(), strnlen(name.data(), name.size()));
    return out;
}

// --- Crawl ---

void Crawl::Want(const Wanted& w) {
    std::lock_guard lock(mutex_);
    // An adult laid down by kLyingPose gets up through the game's
    // LDamageStandUp: the game's own requests on our slot are skipped from
    // now (Skip), before its own Walk / Wait would take the slot this frame.
    // Only on the Link the pose was held on (a new Link after a reload, a
    // Game Over or a scene's re-entry was never laid down).
    if (want_.lying && !w.lying && !w.event && w.adult && crawling_ && posed_actor_ != 0 &&
        posed_actor_ == w.actor) {
        standing_ = true;
        up_asks_ = 0;
        releases_left_ = kMaxReleases;
    }
    if (w.lying || w.event || w.actor != posed_actor_) {
        standing_ = false;
    }
    want_ = w;
    bsa_.store(w.bsa, std::memory_order_release);
}

std::optional<std::string> UnseenWhy(bool downed_or_spectating, std::uint8_t status) {
    namespace st = coop::protocol::status;
    if (downed_or_spectating) {
        return std::string("downed");
    }
    if ((status & st::dead) != 0) {
        return std::string("dead");
    }
    if ((status & st::loading) != 0) {
        return std::string("loading");
    }
    if ((status & st::in_cutscene) != 0) {
        return std::string("in their event");
    }
    if ((status & st::in_menu) != 0) {
        return std::string("in a menu");
    }
    if ((status & st::paused) != 0) {
        return std::string("paused");
    }
    static_assert(st::away == (st::paused | st::loading | st::in_cutscene | st::in_menu | st::dead),
                  "UnseenWhy names every away bit");
    return std::nullopt;
}

void Crawl::SetPuppetActor(PuppetActor find) {
    std::lock_guard lock(mutex_);
    puppet_actor_ = std::move(find);
}

bool Crawl::Skip(std::uint64_t bsa, const CoopBossNative::Request& request, bool own) {
    if (bsa == 0 || own || bsa != bsa_.load(std::memory_order_acquire)) {
        return false;  // (every other actor's request: no lock)
    }
    std::lock_guard lock(mutex_);
    if (bsa != want_.bsa) {
        return false;
    }
    const auto a = CoopBossNative::ActionFromRequest(request);
    if (!a) {
        return false;
    }
    std::uint64_t node = 0;
    std::memcpy(&node, request.data() + CoopBossNative::addr::kReqNode, sizeof node);
    const bool ground = a->verb == verb::kWalk || a->verb == verb::kWait;
    if (ground && node != 0) {
        node_ = node;  // the scheduler's own node, on its main slot
        slot_ = a->slot;
        flags_ = a->flags;
        learnt_ = true;
    }
    // Our ask's slot: a request the game takes on it now keeps it busy
    // until the BSA's next update (ours would get kSlotBusy).
    const bool our_slot = learnt_ && a->slot == slot_;
    if (standing_ && our_slot && !want_.lying && !want_.event && !Passes(a->verb) &&
        a->verb != verb::kLDamageStandUp) {
        // Our adult gets up from the pose: the slot is left to our LDamageStandUp.
        ++counts_.cleared;
        return true;
    }
    if (!want_.lying || want_.event || a->verb == DownVerb(want_.adult)) {
        slot_taken_ = slot_taken_ || our_slot;  // (the frame our Link goes down or gets up, too)
        return false;
    }
    if (!crawling_) {
        if (Passes(a->verb)) {
            airborne_ = true;  // the world moves our Link: its own runs, the crawl waits for the ground
            moved_ = true;
            slot_taken_ = slot_taken_ || our_slot;
            return false;
        }
        if (ground) {
            airborne_ = false;
            ground_seen_ = true;  // (Next starts a new round when one is due)
        }
        if (our_slot && Trying()) {
            // A crawl round is on: the game takes one request per slot a
            // frame, so its own (asked before our caller runs) would take
            // the slot and ours would get kSlotBusy. (Its requests on other
            // slots have their own records: they run.)
            ++counts_.cleared;
            if (!std::exchange(cleared_noted_, true)) {
                Note("our downed Link: the game's own action verb " + std::to_string(a->verb) +
                     " skipped so our " + VerbName(DownVerb(want_.adult)) + " gets the slot");
            }
            return true;
        }
        // Not in our crawl and no round on (really refused, or out of asks),
        // or another slot's: the game's own runs. On our slot it takes the
        // slot for this frame, and anything but Walk / Wait is a move of our
        // Link (a ladder, an item, a reaction): a new round after it.
        if (our_slot) {
            slot_taken_ = true;
            moved_ = moved_ || !ground;
        }
        return false;
    }
    if (Passes(a->verb)) {
        crawling_ = false;  // the world moves our Link (a fall, water, the void): asked again after it
        airborne_ = true;
        moved_ = true;
        slot_taken_ = slot_taken_ || our_slot;
        ++counts_.passed;
        Note("our downed Link: the game's action verb " + std::to_string(a->verb) + " let through; crawl again after");
        return false;
    }
    ++counts_.skipped;
    if (counts_.skipped_verbs.size() < 16 &&
        std::find(counts_.skipped_verbs.begin(), counts_.skipped_verbs.end(), a->verb) ==
            counts_.skipped_verbs.end()) {
        counts_.skipped_verbs.push_back(a->verb);
        Note("our downed Link: the game's action verb " + std::to_string(a->verb) + " (slot " +
             std::to_string(a->slot) + ") skipped");
    }
    return true;
}

std::optional<CoopPuppet::Call> Crawl::Ask(CoopPuppet::Guest& guest, std::int32_t v, std::uint64_t main_base) {
    // (the lock is held; the block exists)
    coop::protocol::BossAction action;
    action.slot = slot_;
    action.verb = v;
    action.flags = flags_;
    action.changed = true;
    const CoopBossNative::Request r = CoopBossNative::RequestFromAction(action, node_);
    if (!guest.Write(block_, r.data(), r.size())) {
        return std::nullopt;
    }
    CoopPuppet::Call c;
    c.function = main_base + CoopBossNative::addr::kRequestActionVerb;
    c.x[0] = want_.bsa;
    c.x[1] = block_;
    c.x[2] = 1;
    pending_ = v == verb::kCrawl ? Pending::crawl : Pending::stand;
    asked_verb_ = v;
    ++counts_.asked;
    return c;
}

std::optional<CoopPuppet::Call> Crawl::Play(CoopPuppet::Guest& guest, float rate, std::uint64_t main_base) {
    // (the lock is held; the block exists) kLyingPose on our Link's slot 0
    // through the game's own play call, as the puppets play a command
    // (coop_puppet.cpp): +0 actor, +8 the name, +0x10 index 0, +0x14 rate,
    // +0x18 / +0x1c -1 (unset), +0x20 skip-if-same 0 (a held command is
    // played again on at rate 1 to get up).
    std::array<char, addr::kTextSize> name{};
    static_assert(kLyingPose.size() < addr::kTextSize);
    std::memcpy(name.data(), kLyingPose.data(), kLyingPose.size());
    const std::uint64_t text = block_ + addr::kBlockText;
    if (!guest.Write(text, name.data(), name.size())) {
        return std::nullopt;
    }
    std::array<std::uint8_t, addr::kPlaySize> request{};
    const float unset = -1.0f;
    const std::int32_t index = 0;
    std::memcpy(request.data() + 0x00, &want_.actor, 8);
    std::memcpy(request.data() + 0x08, &text, 8);
    std::memcpy(request.data() + 0x10, &index, 4);
    std::memcpy(request.data() + 0x14, &rate, 4);
    std::memcpy(request.data() + 0x18, &unset, 4);
    std::memcpy(request.data() + 0x1c, &unset, 4);
    if (!guest.Write(block_ + addr::kBlockPlay, request.data(), request.size())) {
        return std::nullopt;
    }
    CoopPuppet::Call c;
    c.function = main_base + CoopPuppet::addr::kPlayAs;
    c.x[0] = block_ + addr::kBlockPlay;
    pending_ = Pending::pose;
    pending_rate_ = rate;
    return c;
}

std::optional<CoopPuppet::Call> Crawl::StepPose(CoopPuppet::Guest& guest, std::uint64_t main_base) {
    // (the lock is held; the block exists) Adults only: a child's crawl has
    // its own clips, and nothing is read or played on a child.
    if (pose_tried_) {
        return std::nullopt;
    }
    pose_tried_ = true;
    if (!want_.adult || want_.actor == 0 || (!want_.lying && releases_left_ == 0)) {
        riding_ = false;
        return std::nullopt;
    }
    const std::optional<AsSlot> slot = ReadSlot0(guest, want_.actor);
    const bool was_riding = riding_;
    riding_ = want_.lying && slot && Riding(slot->name);
    if (riding_) {
        ++counts_.riding;
        if (!was_riding) {
            Note("our downed Link rides a horse ('" + slot->name + "'): no crawl asked while he is on it");
        }
        return std::nullopt;
    }
    const bool held = slot && slot->name == kLyingPose && slot->rate == kHeldRate;
    if (PoseWanted(want_.adult, want_.lying, want_.event, crawling_, riding_)) {
        if (held) {
            return std::nullopt;
        }
        pose_seen_ = slot ? slot->name : std::string("?");
        return Play(guest, kHeldRate, main_base);
    }
    if (!want_.lying && releases_left_ > 0) {
        if (want_.actor != posed_actor_ || !slot || slot->name != kLyingPose) {
            releases_left_ = 0;  // the game plays its own now (or another Link)
        } else if (held) {
            --releases_left_;
            return Play(guest, kPlayRate, main_base);
        }
    }
    return std::nullopt;
}

std::optional<CoopPuppet::Call> Crawl::Step(CoopPuppet::Guest& guest, Hold& h, Pending kind, std::uint64_t now_at,
                                            std::uint64_t main_base, bool want) {
    // (the lock is held; the block exists) One hold's step this frame: on
    // while lying, on the component now; off when up, only on the component
    // it was put on and only while that is still our Link's (a new scene's
    // Link has a new one: the old key went with the old component).
    h.tried = true;
    const bool want_on = want && now_at != 0;
    if (want_on && h.on_at == now_at) {
        return std::nullopt;
    }
    if (!want_on && h.on_at == 0) {
        return std::nullopt;
    }
    if (!want_on && h.on_at != now_at) {
        h.on_at = 0;  // (that component is gone)
        return std::nullopt;
    }
    const std::uint32_t key = addr::kOurKey;
    if (!guest.Write(block_ + addr::kBlockKey, &key, sizeof key)) {
        return std::nullopt;
    }
    CoopPuppet::Call c;
    if (kind == Pending::shield) {
        c.function = main_base + addr::kInvincibleNoReaction;
        c.x[0] = now_at;
        c.x[1] = want_on ? 1 : 0;  // Bool: add our key / take it out
        c.x[2] = block_ + addr::kBlockKey;
    } else {
        c.function = main_base + addr::kEmitterSwitch;
        c.x[0] = now_at + addr::kEmitterKeys;
        c.x[1] = block_ + addr::kBlockKey;
        c.x[2] = want_on ? 0 : 1;  // IsEnable
    }
    pending_ = kind;
    pending_on_ = want_on;
    pending_at_ = now_at;
    return c;
}

std::optional<CoopPuppet::Call> Crawl::StepPuppets(CoopPuppet::Guest& guest, std::uint64_t main_base) {
    // (the lock is held; the block exists) Every teammate downed now, and
    // every puppet still holding our key (they got up, or left: their puppet
    // is gone and so is its component).
    for (const auto& u : want_.unseen_puppets) {
        puppets_.try_emplace(u.number);
    }
    for (auto it = puppets_.begin(); it != puppets_.end();) {
        const std::uint8_t n = it->first;
        Hold& h = it->second;
        const auto wanted = std::find_if(want_.unseen_puppets.begin(), want_.unseen_puppets.end(),
                                         [n](const Wanted::Unseen& u) { return u.number == n; });
        const bool want = wanted != want_.unseen_puppets.end();
        if (!want && h.on_at == 0 && !(pending_ == Pending::puppet && pending_number_ == n)) {
            it = puppets_.erase(it);
            continue;
        }
        ++it;
        if (h.tried) {
            continue;
        }
        // Their puppet's PerceptionEmitter now: [[actor + 0x270] + 0x328] (0:
        // no puppet, or a pack without perception).
        std::uint64_t component = 0;
        if (const std::uint64_t actor = puppet_actor_ ? puppet_actor_(n) : 0; actor != 0) {
            std::uint64_t table = 0;
            if (guest.Read(actor + CoopBossNative::addr::kActorComponents, &table, sizeof table) && table != 0 &&
                !guest.Read(table + addr::kComponentEmitter, &component, sizeof component)) {
                component = 0;
            }
        }
        if (auto c = Step(guest, h, Pending::puppet, component, main_base, want)) {
            pending_number_ = n;
            pending_why_ = want ? wanted->why : std::string();
            return c;
        }
    }
    return std::nullopt;
}

std::optional<CoopPuppet::Call> Crawl::Next(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                            std::uint64_t now_ms, std::uint64_t main_base) {
    std::lock_guard lock(mutex_);
    if (!after_call) {
        held_back_ = std::exchange(slot_taken_, false);
        shield_.tried = false;
        pose_tried_ = false;
        unseen_.tried = false;
        for (auto& [n, h] : puppets_) {
            h.tried = false;
        }
    } else {
        const Pending was = std::exchange(pending_, Pending::none);
        switch (was) {
        case Pending::malloc:
            block_ = result;
            break;
        case Pending::shield:
        case Pending::unseen: {
            Hold& h = was == Pending::shield ? shield_ : unseen_;
            h.on_at = pending_on_ ? pending_at_ : 0;
            if (pending_on_) {
                ++(was == Pending::shield ? counts_.shield_on : counts_.unseen_on);
            }
            Note(std::string(was == Pending::shield ? "the game's invincibility (no reaction)" : "the game's perception emitter") +
                 (pending_on_ ? (was == Pending::shield ? " on for our downed Link" : " off for our downed Link")
                              : (was == Pending::shield ? " off: our Link is up" : " on again: our Link is up")));
            break;
        }
        case Pending::puppet: {
            Hold& h = puppets_[pending_number_];
            h.on_at = pending_on_ ? pending_at_ : 0;
            if (pending_on_) {
                ++counts_.puppets_unseen_on;
            }
            Note("the game's perception emitter " + std::string(pending_on_ ? "off" : "on again") + " for player " +
                 std::to_string(pending_number_) + "'s puppet: " + (pending_on_ ? "they are " + pending_why_ : "they are back"));
            break;
        }
        case Pending::pose:
            if (pending_rate_ == kHeldRate) {
                ++counts_.poses;
                const bool first = posed_actor_ != want_.actor;
                posed_actor_ = want_.actor;
                if (first) {
                    Note("our downed adult Link: the game's AS '" + std::string(kLyingPose) +
                         "' held at frame 0 (lying; the crawl's own clips are the child's)");
                } else if (counts_.poses <= 4 || counts_.poses % 100 == 0) {
                    Note("our downed adult Link: '" + std::string(kLyingPose) + "' held again (the game played '" +
                         pose_seen_ + "'; " + std::to_string(counts_.poses) + " so far)");
                }
            } else {
                ++counts_.releases;
                Note("our Link up: the game's AS '" + std::string(kLyingPose) +
                     "' played on from the held pose (the game's stand-up from lying)");
            }
            break;
        case Pending::crawl:
        case Pending::stand: {
            const bool taken = result == CoopBossNative::addr::kActionTaken;
            if (result == kSlotBusy) {
                ++counts_.busy;
            } else if (!taken) {
                ++counts_.refused;
            }
            if (was == Pending::crawl) {
                crawling_ = taken;
                if (taken) {
                    asks_ = 0;  // (a later fall or water may need a fresh round)
                    spot_rounds_ = 0;
                    moved_ = false;
                    Note("our downed Link: " + Named(asked_verb_) + " asked and taken");
                } else if (result == kSlotBusy) {
                    // Not a refusal of the crawl: the slot took another
                    // request this frame. Skipping is on now: next frame.
                    next_ask_ms_ = 0;
                    if (++refusals_noted_ <= 3) {   // (as the refusals below: a boss fight's events start many rounds)
                        Note("our downed Link: " + Named(asked_verb_) +
                             " waits: the slot took a request this frame (3); asked next frame" +
                             (refusals_noted_ == 3 ? "; more while down are not noted" : ""));
                    }
                } else {
                    blocked_ = true;  // a real refusal: the game's own runs
                    // A refusal repeats while the game keeps our Link busy (60 lines in a boss fight's
                    // log): the first three are noted, the rest only counted (counts_.refused).
                    if (++refusals_noted_ <= 3) {
                        Note("our downed Link: " + Named(asked_verb_) + " refused (" + std::to_string(result) + ")" +
                             (refusals_noted_ == 3 ? "; more refusals while down are not noted" : ""));
                    }
                }
            } else {
                if (standing_ && !taken && result == kSlotBusy && up_asks_ < kMaxAsks) {
                    Note("our Link up: " + Named(asked_verb_) +
                         " waits: the slot took a request this frame (3); asked next frame");
                    return std::nullopt;
                }
                if (standing_ && !taken && result != kSlotBusy) {
                    Note("our Link up: " + Named(asked_verb_) + " refused (" + std::to_string(result) +
                         "): the game's own Wait stands our Link up");
                    standing_ = false;
                    return std::nullopt;
                }
                standing_ = false;  // taken, or out of asks
                // (kSlotBusy: the game's own Walk or Wait, no longer skipped, took the slot first this frame.)
                Note("our Link up: " + Named(asked_verb_) +
                     (taken                 ? " asked and taken"
                      : result == kSlotBusy ? " not needed: the game's own request took the slot this frame (3) and stands our Link up"
                                            : " refused (" + std::to_string(result) + ")"));
            }
            return std::nullopt;  // (one action ask a frame)
        }
        case Pending::none:
            break;
        }
    }

    bool puppets_held = !want_.unseen_puppets.empty();
    for (const auto& [n, h] : puppets_) {
        puppets_held = puppets_held || h.on_at != 0;
    }
    const bool need_block = want_.lying || shield_.on_at != 0 || unseen_.on_at != 0 || stand_due_ ||
                            want_.lying != was_lying_ || puppets_held || standing_ || releases_left_ > 0;
    if (block_ == 0) {
        if (!need_block || pending_ != Pending::none || (after_call && result == 0)) {
            return std::nullopt;  // (nothing to do, or the game had no memory: next frame)
        }
        CoopPuppet::Call c;
        c.function = main_base + CoopBossNative::addr::kMalloc;
        c.x[0] = addr::kBlockSize;
        pending_ = Pending::malloc;
        return c;
    }

    // Shield and unseen (any time, events too).
    if (!shield_.tried) {
        if (auto c = Step(guest, shield_, Pending::shield, want_.life, main_base, want_.lying)) {
            return c;
        }
    }
    if (!unseen_.tried) {
        if (auto c = Step(guest, unseen_, Pending::unseen, want_.emitter, main_base, want_.lying)) {
            return c;
        }
    }
    if (auto c = StepPuppets(guest, main_base)) {
        return c;
    }
    if (auto c = StepPose(guest, main_base)) {
        return c;
    }

    // The crawl.
    const auto new_round = [this] {
        ++counts_.rounds;
        asks_ = 0;
        next_ask_ms_ = 0;
        blocked_ = false;
        moved_ = false;
        cleared_noted_ = false;
    };
    if (want_.lying != was_lying_) {
        was_lying_ = want_.lying;
        refusals_noted_ = 0;  // (the first few misses noted again each time our Link goes down, not each round)
        new_round();
        spot_rounds_ = 0;
        airborne_ = false;
        ground_seen_ = false;
        stand_due_ = !want_.lying && crawling_ && !standing_;
        crawling_ = false;
    }
    if (want_.event) {
        crawling_ = false;  // the event moves our Link; asked again after it
        if (asks_ != 0 || blocked_ || moved_) {
            new_round();
        }
        spot_rounds_ = 0;
        airborne_ = false;
        return std::nullopt;
    }
    if (std::exchange(ground_seen_, false) && !crawling_ && !airborne_) {
        // Back on the ground. After a move of our Link: a new round (never
        // more than one each kAskEveryMs: a slope that slides our Link off
        // every frame). A round that ended on the spot: only kRetryMs after
        // its last ask, kSpotRounds times; then our Link has to move first.
        const bool over = blocked_ || asks_ >= kMaxAsks;
        if (moved_) {
            if (asks_ == 0 && !blocked_) {
                moved_ = false;  // (this round asked nothing yet: it is the new one)
                spot_rounds_ = 0;
            } else if (now_ms >= last_ask_ms_ + kAskEveryMs) {
                spot_rounds_ = 0;
                new_round();
            } else {
                ground_seen_ = true;  // (looked at again next frame)
            }
        } else if (over && spot_rounds_ < kSpotRounds && now_ms >= last_ask_ms_ + kRetryMs) {
            ++spot_rounds_;
            new_round();
        }
    }
    if (want_.bsa == 0 || !learnt_) {
        return std::nullopt;
    }
    if (standing_) {
        // Our adult up from the pose: the game's LDamageStandUp, its getting
        // up from lying (the game's own requests on the slot are skipped).
        if (held_back_) {
            // A kPassVerbs verb of the game's ran on the slot (a fall, water):
            // it moves our Link; asked next frame, at most kMaxAsks frames.
            if (++up_asks_ >= kMaxAsks) {
                standing_ = false;
            }
            return std::nullopt;
        }
        ++up_asks_;
        return Ask(guest, UpVerb(true), main_base);
    }
    if (stand_due_) {
        stand_due_ = false;
        if (held_back_) {
            // The game's own Walk or Wait (skipped while down, asked every
            // frame) ran on the slot this frame: that stands our Link up,
            // and ours on top would only get kSlotBusy.
            Note("our Link up: the game's own request stands it up (no " + Named(UpVerb(false)) + " of ours needed)");
            return std::nullopt;
        }
        return Ask(guest, UpVerb(false), main_base);
    }
    if (want_.lying && !crawling_ && !airborne_ && !blocked_ && !riding_ && asks_ < kMaxAsks && now_ms >= next_ask_ms_) {
        if (held_back_) {
            // The game's own request ran on our slot this frame (a round
            // that starts now, or a kPassVerbs verb): the slot is busy.
            // Not an ask: the next frame's is skipped for ours.
            ++counts_.held_back;
            return std::nullopt;
        }
        ++asks_;
        next_ask_ms_ = now_ms + kAskEveryMs;
        last_ask_ms_ = now_ms;
        return Ask(guest, DownVerb(want_.adult), main_base);
    }
    return std::nullopt;
}

bool Crawl::PuppetUnseen(std::uint8_t number) const {
    std::lock_guard lock(mutex_);
    return std::any_of(want_.unseen_puppets.begin(), want_.unseen_puppets.end(),
                       [number](const Wanted::Unseen& u) { return u.number == number; });
}

Crawl::Counts Crawl::Totals() const {
    std::lock_guard lock(mutex_);
    return counts_;
}

std::vector<std::string> Crawl::TakeNotes() {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    out.swap(notes_);
    return out;
}

void Crawl::Note(std::string line) {
    if (notes_.size() < 32) {
        notes_.push_back(std::move(line));
    }
}

Crawl& TheCrawl() {
    static Crawl crawl;
    return crawl;
}

Raising& TheRaising() {
    static Raising raising;
    return raising;
}

}  // namespace CoopDbno::Body
