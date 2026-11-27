// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_dbno_body.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
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

bool CrawlCommand(std::string_view c) {
    // Read from AS/Player_Child.root.asb (471 commands): none is named Tunnel* or Crawl*. The crawl's clips
    // are played by three commands: `Crouch` (its root node is the Tunnel_Wait clip itself),
    // `CrouchLockOnMove` (Tunnel_St, Tunnel_Move_F) and `CrouchLockOnMoveEnd` (Tunnel_Ed, Tunnel_Ed_B).
    // (Issue 337, 0.11.13: Tunnel* / Crawl* were guessed here, so a crawl that ran was never seen and its
    // slot was given back to the game's Wait 30 frames after it started.)
    return c == "Crouch" || c == "CrouchLockOnMove" || c == "CrouchLockOnMoveEnd";
}

std::optional<AnchorSpot> AnchorFor(bool lying, bool event, bool adult, const std::array<float, 3>& link,
                                    const CoopPuppet::Mat3& rows) {
    if (!lying || event || adult) {
        return std::nullopt;
    }
    // His front on the ground: the rotation's +Z column, flat.
    float fx = rows[2], fz = rows[8];
    const float len = std::sqrt(fx * fx + fz * fz);
    if (!std::isfinite(len) || len < 1e-4f || !std::isfinite(link[0]) || !std::isfinite(link[1]) ||
        !std::isfinite(link[2])) {
        return std::nullopt;
    }
    fx /= len;
    fz /= len;
    AnchorSpot spot;
    spot.position = {link[0] + fx * kAnchorAhead, link[1], link[2] + fz * kAnchorAhead};
    // Facing him: +Z = -front. A turn about Y by yaw maps +Z to (sin, 0, cos).
    const float s = -fx, c = -fz;
    spot.rotation = {c, 0, s, 0, 1, 0, -s, 0, c};
    return spot;
}

namespace {
std::mutex g_anchor_mutex;
std::optional<AnchorSpot> g_anchor;
std::uint32_t g_anchor_downs = 0;
std::uint32_t g_anchor_live = 0;  // the down whose anchor the copy director made a live actor
}  // namespace

void SetAnchor(std::optional<AnchorSpot> spot) {
    std::lock_guard lock(g_anchor_mutex);
    if (spot) {
        spot->down = g_anchor ? g_anchor->down : ++g_anchor_downs;
    }
    g_anchor = spot;
}

std::optional<AnchorSpot> Anchor() {
    std::lock_guard lock(g_anchor_mutex);
    return g_anchor;
}

void AnchorLive(std::uint32_t down) {
    std::lock_guard lock(g_anchor_mutex);
    g_anchor_live = down;
}

bool AnchorReady() {
    std::lock_guard lock(g_anchor_mutex);
    return g_anchor && g_anchor->down != 0 && g_anchor->down == g_anchor_live;
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

std::optional<std::uint32_t> ReadDoAction(CoopPuppet::Guest& guest, std::uint64_t actor) {
    std::uint64_t parts = 0, work = 0;
    std::uint32_t action = 0;
    if (actor == 0 || !guest.Read(actor + addr::kActorComponents, &parts, sizeof parts) || parts == 0 ||
        !guest.Read(parts + addr::kComponentWork, &work, sizeof work) || work == 0 ||
        !guest.Read(work + addr::kWorkDoAction, &action, sizeof action)) {
        return std::nullopt;
    }
    return action;
}

// --- Crawl ---

void Crawl::StepOffer(CoopPuppet::Guest& guest) {
    // (the lock is held) Issue 337. The game's crawl starts only when the game itself offers it: the
    // attention receiver has taken the CrawlAnchor as its target and the do action reads Crawl. A live anchor
    // is not enough (asked in the frame it appeared, the Crawl was taken and nothing crawled).
    const bool ours = want_.lying && !want_.event && !want_.adult && want_.actor != 0 && Anchor().has_value();
    if (!want_.lying) {
        offers_seen_.clear();
        false_starts_ = 0;
        start_reads_ = 0;
        lie_ = false;
        crawl_ran_ = false;
        crawl_frames_ = 0;
        offer_waits_ = 0;
    }
    if (!ours || crawling_) {
        awaits_offer_ = false;  // (no anchor wanted: asked as before; taken: StepPose watches slot 0)
        return;
    }
    if (!lie_ && false_starts_ >= kMaxFalseStarts) {
        LieDown("the game's crawl did not start " + std::to_string(kMaxFalseStarts) + " times");
    }
    if (lie_) {
        awaits_offer_ = false;  // part 2: Crawl is asked as for an adult; StepPose lays him down
        return;
    }
    const std::optional<std::uint32_t> action = AnchorReady() ? ReadDoAction(guest, want_.actor) : std::nullopt;
    const bool offered = action && *action == addr::kDoActionCrawl;
    if (offered && awaits_offer_) {
        Note("our downed Link: the game offers Crawl (its do action is " + std::to_string(*action) + "): Crawl asked");
    } else if (!offered && action && offers_seen_.size() < 8 &&
               std::find(offers_seen_.begin(), offers_seen_.end(), *action) == offers_seen_.end()) {
        offers_seen_.push_back(*action);
        Note("our downed Link: the game's do action is " + std::to_string(*action) + ", not Crawl (" +
             std::to_string(addr::kDoActionCrawl) + "): Crawl waits for the game's offer");
    }
    awaits_offer_ = !offered;
    if (offered) {
        offer_waits_ = 0;
    } else if (++offer_waits_ >= kOfferWaitReads) {
        LieDown("the game offered no Crawl in " + std::to_string(kOfferWaitReads) + " frames");
        awaits_offer_ = false;
    }
}

void Crawl::LieDown(std::string why) {
    if (std::exchange(lie_, true)) {
        return;
    }
    ++counts_.lie_downs;
    Note("our downed Link: " + std::move(why) + ": the game's AS '" + std::string(kChildLyingPose) +
         "' (Tunnel_Wait, its lying wait) is held on slot 0 for the rest of this down");
}

bool Crawl::LaidDown() const {
    std::lock_guard lock(mutex_);
    return lie_;
}

void Crawl::Want(const Wanted& w) {
    std::lock_guard lock(mutex_);
    if (w.lying && !want_.lying) {
        lie_ = false;  // (a new down: the game's crawl is tried first)
        crawl_ran_ = false;
        crawl_frames_ = 0;
        offer_waits_ = 0;
    }
    if (w.lying && !want_.lying && !w.adult && Anchor().has_value()) {
        awaits_offer_ = true;  // (until this frame's StepOffer: the game's own verbs are not skipped before it)
    }
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
    if (ground && our_slot && (want_.adult || lie_)) {
        // 0.12.1 downed-move: a Link held in a lying pose (an adult's kLyingPose, a child's kChildLyingPose once the
        // game's crawl could not go on) has no crawl to move him: with the game's Walk / Wait skipped too, nothing
        // read the stick and he lay still. The game's own ground move runs (the pad's slow stick); StepPose plays
        // the pose again whenever the game plays another command on slot 0, so he slides while lying.
        slot_taken_ = true;
        ++counts_.ground_moves;
        if (!std::exchange(ground_noted_, true)) {
            Note("our downed Link: the game's action verb " + std::to_string(a->verb) +
                 " let through: he moves lying, in the held pose");
        }
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

bool Crawl::NoRoom(std::uint64_t actor) {
    std::lock_guard lock(mutex_);
    if (actor == 0 || actor != want_.actor || !want_.lying || want_.event || want_.adult || !crawling_) {
        return false;
    }
    ++counts_.kept_crawling;
    if (!std::exchange(kept_noted_, true)) {  // (once a down)
        Note("our downed Link: the game's crawl asks for room to stand up: answered no room, the crawl goes on");
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

std::optional<CoopPuppet::Call> Crawl::Play(CoopPuppet::Guest& guest, std::string_view command, float rate,
                                            std::uint64_t main_base) {
    // (the lock is held; the block exists) The lying pose (kLyingPose, a child's kChildLyingPose) on our Link's slot 0
    // through the game's own play call, as the puppets play a command
    // (coop_puppet.cpp): +0 actor, +8 the name, +0x10 index 0, +0x14 rate,
    // +0x18 / +0x1c -1 (unset), +0x20 skip-if-same 0 (a held command is
    // played again on at rate 1 to get up).
    std::array<char, addr::kTextSize> name{};
    static_assert(kLyingPose.size() < addr::kTextSize && kChildLyingPose.size() < addr::kTextSize);
    std::memcpy(name.data(), command.data(), command.size());
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
    pending_child_ = command == kChildLyingPose;
    return c;
}

std::optional<CoopPuppet::Call> Crawl::StepPose(CoopPuppet::Guest& guest, std::uint64_t main_base) {
    // (the lock is held; the block exists) An adult is held in kLyingPose. A child's crawl is the game's
    // own; once it ended (or could not start) in this down he is held in kChildLyingPose (lie_).
    if (pose_tried_) {
        return std::nullopt;
    }
    pose_tried_ = true;
    if (!want_.adult) {
        // Issue 337: a child's crawl is the game's own; slot 0 tells whether it runs.
        riding_ = false;
        const std::optional<AsSlot> slot =
            want_.lying && want_.actor != 0 ? ReadSlot0(guest, want_.actor) : std::nullopt;
        // (Only under our taken Crawl: `Crouch` is also a standing child's own crouch.)
        const bool crawl = crawling_ && slot && CrawlCommand(slot->name);
        // The crawl's get-up is over when its clip's frame stands still (its node then ends).
        bool end_over = false;
        if (crawl && slot->name == kCrawlEnd) {
            if (slot->frame == end_frame_) {
                end_over = ++end_stalls_ >= kEndStallReads;
            } else {
                end_frame_ = slot->frame;
                end_stalls_ = 0;
            }
        } else {
            end_frame_ = -1.0f;
            end_stalls_ = 0;
        }
        // Laid down by us: the lying wait in slot 0 is ours, not the game's crawl (the pad's slow stick).
        const bool held = lie_ && crawl && slot->name == kChildLyingPose;
        const bool now = crawl && !end_over && !held;
        const bool down = want_.lying && !want_.event && crawling_;
        if (now) {
            ++counts_.game_crawl;
            ++crawl_frames_;
            crawl_ran_ = true;
            start_reads_ = 0;
            false_starts_ = 0;
        } else if (down && !lie_ && crawl_ran_) {
            // Part 2: it ran and ended by itself (room to stand up). The slot stays ours.
            ++counts_.crawl_ended;
            LieDown("the game's crawl ended by itself after " + std::to_string(crawl_frames_) + " frames ('" +
                    (slot ? slot->name : std::string("-")) + "')");
        } else if (down && !lie_ && Anchor() && ++start_reads_ >= kCrawlStartReads) {
            // Taken, and the game's crawl is not in slot 0 (its enter failed, or the crawl ended): while
            // crawling_ every verb of the game's is skipped, so our Link would stay as he is for the whole
            // down. The slot is the game's again; StepOffer waits for its next offer.
            start_reads_ = 0;
            crawling_ = false;
            awaits_offer_ = true;
            ++false_starts_;
            Note("our downed Link: the game's crawl is not running ('" + (slot ? slot->name : std::string("-")) +
                 "'): the game's own verbs run again, Crawl waits for its next offer (" +
                 std::to_string(false_starts_) + " of " + std::to_string(kMaxFalseStarts) + ")");
        }
        if (now != game_crawl_.load(std::memory_order_relaxed)) {
            game_crawl_.store(now, std::memory_order_release);
            crawl_seen_ = slot ? slot->name : std::string("-");
            if (now) {
                Note("our downed Link: the game's own crawl runs ('" + crawl_seen_ + "')");
            } else if (want_.lying && crawling_ && !lie_) {
                Note("our downed Link: Crawl taken but not crawling ('" + crawl_seen_ + "')");
            }
        } else if (want_.lying && crawling_ && !now && !held && crawl_seen_.empty()) {
            crawl_seen_ = slot ? slot->name : std::string("-");  // once a down (the log)
            Note("our downed Link: Crawl taken but not crawling ('" + crawl_seen_ + "')");
        }
        if (!want_.lying) {
            crawl_seen_.clear();
        }
        if (lie_ && down && !now && !held) {
            pose_seen_ = slot ? slot->name : std::string("?");
            return Play(guest, kChildLyingPose, kPlayRate, main_base);
        }
        return std::nullopt;
    }
    game_crawl_.store(false, std::memory_order_release);
    if (want_.actor == 0 || (!want_.lying && releases_left_ == 0)) {
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
        return Play(guest, kLyingPose, kHeldRate, main_base);
    }
    if (!want_.lying && releases_left_ > 0) {
        if (want_.actor != posed_actor_ || !slot || slot->name != kLyingPose) {
            releases_left_ = 0;  // the game plays its own now (or another Link)
        } else if (held) {
            --releases_left_;
            return Play(guest, kLyingPose, kPlayRate, main_base);
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
        StepOffer(guest);
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
            if (std::exchange(pending_child_, false)) {
                if (++counts_.child_poses <= 4 || counts_.child_poses % 100 == 0) {
                    Note("our downed child Link: the game's AS '" + std::string(kChildLyingPose) +
                         "' played on slot 0 (lying; the game played '" + pose_seen_ + "'; " +
                         std::to_string(counts_.child_poses) + " so far)");
                }
            } else if (pending_rate_ == kHeldRate) {
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
        anchor_wait_noted_ = false;
        kept_noted_ = false;
        ground_noted_ = false;
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
        if (!want_.adult && !lie_ && Anchor() && !AnchorReady()) {  // (laid down: asked as for an adult)
            // Issue 337: the game reads verb 234 against the CrawlAnchor's attention; asked before the anchor
            // (made async by the copy director) is live, it is taken and nothing crawls (slot 0 stays DamageS).
            if (!std::exchange(anchor_wait_noted_, true)) {
                Note("our downed Link: Crawl waits for this down's CrawlAnchor to be a live actor");
            }
            return std::nullopt;
        }
        if (!want_.adult && AnchorReady() && std::exchange(anchor_wait_noted_, false)) {
            Note("our downed Link: this down's CrawlAnchor is live");  // (never live: no ask; the director
        }                                                             // logs its request)
        if (awaits_offer_) {
            return std::nullopt;  // (StepOffer: the game does not offer Crawl yet; its own verbs run)
        }
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
