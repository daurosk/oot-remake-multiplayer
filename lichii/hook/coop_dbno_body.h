// SPDX-License-Identifier: GPL-3.0-or-later
//
// Down but not out: our downed Link's body, through the game's own systems
// (docs/play-online.md "Down but not out"; coop_dbno.h has the rules).
//
// - Crawl. The player's scheduler AI (Player.scheduler.*.module.ainb) enters a
//   crawlspace by asking its BSA for action verb 234 ("Crawl"); that runs
//   Player.action.crawl (ExecutePlayerCrawl) with its own animation and the
//   PlayerCamera's "Crawl" camera. A downed Link is asked for that verb through
//   the game's own "request action verb" (main+0xc08f0, the wrap of
//   hos_coop_bosses.h), with the scheduler's own asking node. Once the game has
//   taken it, the other actions the game asks for our Link are skipped at that
//   wrap (a hit's reaction no longer stands it up, row 34), except the ones the
//   world forces (kPassVerbs: falling, landing, sliding, swimming, diving, the
//   void, an event, the game's death): those run, and the crawl is asked again
//   once our Link is back on the ground (its next Walk or Wait).
//   The game takes ONE request per slot a frame (row 190; the lifted
//   main+0xc08f0): the BSA keeps a record per slot ([bsa + 0x1c0 + 8 * slot],
//   slots 0-4); a request finds bit 1 of its record's byte +0x7c set and
//   answers 3 (main+0xc1238), else the accept path sets it (|= 0xa, 0 at
//   main+0xc10f4) and the BSA update (main+0xcb9c0) clears it, once a slot.
//   (2 = the slot has no record.) So 3 is never about the verb, the node,
//   the flags or the ground: it says "this slot took a request this frame".
//   The scheduler asks our Link's main slot (Walk / Wait) nearly every frame
//   before our caller runs, so a Crawl asked on top always got 3. While a
//   crawl round is on (lying, no event, not in the air, not really refused,
//   asks left) the game's own requests ON THAT SLOT (except kPassVerbs) are
//   skipped, leaving it free for our Crawl; and in a frame where the game's
//   own request was let through on it (a kPassVerbs verb), nothing is asked:
//   the slot is known busy. A 3 all the same is asked again next frame. A
//   real refusal (any other result) skips nothing. A round is at most
//   kMaxAsks asks; the next one starts on the ground after the world moved
//   our Link (a kPassVerbs verb, or any other move of the game's that ran on
//   the slot: a ladder, an item), else kRetryMs after the last ask and at
//   most kSpotRounds times in a row: a crawl the game does not take where
//   our Link stands is not asked for ever.
//   Up again: the game's "Wait" (40) is asked once (answered 3 when the
//   game's own Walk or Wait took the slot first: that one stands our Link up).
//   The verb values are read from the scheduler modules' selector cases.
// - The adult's pose (row 190, adult). Crawl's clips (Tunnel_*) are in the
//   child's banks only, so an adult in Crawl stood upright and his puppet
//   copied that. The game's own adult AS has the lying start of its own
//   stand-up: the AS command DamageLStand (the adult AS root's command 61; its
//   tree plays Damage_Down_{F,B,L,R,Half}_StandUp per weapon and slope, the
//   clips the game's LDamageStandUp (85) and the fairy's revival
//   (Player.event.action.revivalfairystandup) play). While our adult Link is
//   in our Crawl, that command is played on his AS slot 0 at rate 0 through
//   the game's own play call (main+0x2022a0, the puppets' kPlayAs): frame 0
//   is him lying, held. Slot 0 is read every frame and played again when the
//   game played another command on it. Getting up: the held command is played
//   again at rate 1 (the game's stand-up from that pose), and the game's own
//   LDamageStandUp (85) is asked, the game's getting up from lying (the
//   game's own requests on our slot are skipped until it is taken, as for the
//   crawl, at most kMaxAsks frames; refused: the game's own Wait stands him
//   up). The others see it through the existing paths: Link puppets copy his
//   bones (coop_pose.h), Character Select puppets play the AS command, frame
//   and rate PLAYER_SAMPLE reads from slot 0 (rate 0: held there too). A
//   child keeps the crawl's own clips: nothing is played on a child.
//   Riding (adult only: slot 0's command a horse one): no crawl is asked and
//   nothing skipped while our Link is on a horse (the game's ride goes on,
//   shielded and unseen); the round starts once he is off it.
// - Shield. The game's own named invincibility, as the AI node
//   OneShotLifeSetInvincible gives it (run main+0x8d7520; inputs Bool,
//   InvincibleFlagName, IsNoReaction). IsNoReaction picks the list and so the
//   function: main+0x8d75e0 keeps the no-reaction keys (Life+0x1c60),
//   main+0xb34800 the others (Life+0x1c28); each takes (x0 the Life component,
//   w1 Bool: 1 adds the key, 0 takes it out, x2 -> the name's u32 key). Ours:
//   our own key (kOurKey) in the no-reaction list: a hit takes nothing and
//   plays no reaction. We never touch the other list.
// - Unseen. The game's own perception switch, as the AI node
//   OneShotPerceptionSetEnableOrDisable does it with ApplyToEmitter (run
//   main+0x974a60): main+0x1278e0 (x0 the PerceptionEmitter component
//   [[actor+0x270]+0x328] + 0x20, x1 -> a u32 key, w2 IsEnable) keeps a set of
//   keys that turn the emitter off. Ours goes in while downed, so no enemy or
//   boss in our game perceives our Link (the same perception search picks
//   every enemy's and boss's target) and they go for someone else.
//   Shield and unseen go on while downed and off when up, each only on the
//   component it was put on; the game's own keys are never touched.
// - A downed teammate unseen here too (and any teammate not really here). With the puppets perceived (issue 150:
//   the pack gives each puppet the player's PerceptionEmitter), our game's
//   enemies and bosses would pick a downed (or spectating) teammate's lying
//   puppet: in the enemy's owner's game that is who it goes for, so the downed
//   player's own unseen key alone did not keep enemies off them. The same key
//   goes into that puppet's emitter ([[puppet+0x270]+0x328] + 0x20) while its
//   player is downed or spectating, out when they are up; only on the
//   component it was put on (a remade puppet has a new one). A pack without
//   perception: the puppet has no emitter (0), nothing is called.
//   The same for a teammate who is not really here (PLAYER_STATUS away: paused,
//   in a menu, loading, in their own event or dead; review of perception-on):
//   the enemy's owner would stick to that puppet (the game keeps a target while
//   it is perceived) or attack a talking player mid-event, which the game never
//   does to its own Link in an event. Out again when the bit clears.
// - Heal. The revive's life goes in through the game's own Life step (patch
//   0007) as a negative sum, the game's heal (as LifeWrite::heal for bosses);
//   written directly only when the step has not taken it in kHealWaitUs.
//
// The crawl needs Hookshot's action verb wrap (patch 0013) to learn our
// Link's own node: elsewhere no crawl is asked. Solo play never gets here:
// downed exists only in a session.
#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop_boss_native.h"
#include "coop_puppet.h"

namespace CoopDbno::Body {

namespace addr {
inline constexpr std::uint64_t kComponentBsa = CoopBossNative::addr::kComponentBsa;  // [actor+0x270]+0x188
inline constexpr std::uint64_t kComponentEmitter = 0x328;  // [actor+0x270]+0x328, PerceptionEmitter
inline constexpr std::uint64_t kEmitterKeys = 0x20;        // its set of "off" keys
inline constexpr std::uint64_t kInvincibleNoReaction = 0x8d75e0;  // main+, (life, add, &key): the no-reaction list
inline constexpr std::uint64_t kInvincibleList = 0xb34800;  // main+, the same for the other list (not used: the test's check)
inline constexpr std::uint64_t kEmitterSwitch = 0x1278e0;  // main+, (emitter + 0x20, &key, enable)
inline constexpr std::uint64_t kAsSlotRate = 0xd4;         // an AS slot's rate (f32), as PLAYER_SAMPLE reads it
// Our key for both (the game keys each by a name's hash; any value no game name has).
inline constexpr std::uint32_t kOurKey = 0xC0DB0D01u;
// Our block in guest memory: the action request, the key, the AS play
// request (0x30, as the puppets' kPlayAs one), then the command's name.
inline constexpr std::size_t kBlockKey = CoopBossNative::addr::kRequestSize;
inline constexpr std::size_t kBlockPlay = kBlockKey + 8;
inline constexpr std::size_t kPlaySize = 0x30;
inline constexpr std::size_t kBlockText = kBlockPlay + kPlaySize;
inline constexpr std::size_t kTextSize = 0x20;
inline constexpr std::size_t kBlockSize = kBlockText + kTextSize;
}  // namespace addr

// Player action verbs (Player.scheduler.*.module.ainb, ExecuteBSASetActionVerb).
namespace verb {
inline constexpr std::int32_t kWalk = 1;
inline constexpr std::int32_t kWaterDive = 18;
inline constexpr std::int32_t kLand = 32;
inline constexpr std::int32_t kFall = 33;
inline constexpr std::int32_t kSlippingDown = 34;
inline constexpr std::int32_t kSlide = 35;
inline constexpr std::int32_t kSlideLand = 36;
inline constexpr std::int32_t kWait = 40;
inline constexpr std::int32_t kLDamageStandUp = 85;  // getting up from lying (Player.scheduler.ldamagestandup)
inline constexpr std::int32_t kSwim = 84;
inline constexpr std::int32_t kPreDie = 141;
inline constexpr std::int32_t kPreAbyss = 156;
inline constexpr std::int32_t kPreEvent = 159;
inline constexpr std::int32_t kCrawl = 234;
}  // namespace verb
// What the world forces on a downed Link: never skipped.
inline constexpr std::array<std::int32_t, 10> kPassVerbs{verb::kFall,     verb::kLand,      verb::kSlippingDown,
                                                          verb::kSlide,    verb::kSlideLand, verb::kSwim,
                                                          verb::kWaterDive, verb::kPreAbyss, verb::kPreEvent,
                                                          verb::kPreDie};
// The lying verb: Crawl for both ages. Both run the same crawl AI (identical
// Player.action/scheduler.crawl modules in the Player_Child and Player_Adult
// packs) and their AS roots have the same commands; the crawl's clips
// (Tunnel_St / Tunnel_Wait / Tunnel_Move_F / Tunnel_Ed / Tunnel_Ed_B) are in
// the child's banks only (Player_Child_03_Animation), so an adult is laid
// down by kLyingPose on top (see the top). An adult was once given the
// game's HugeHit (136) instead: in its proof run (hs-dbno-adult, 2026-11-16)
// the knock-down threw our Link each time it was asked and he ended up away
// from the spot, so it is not used.
// The getting-up verb: the game's Wait for a child (the crawl ends), its
// LDamageStandUp for an adult laid down by kLyingPose (`lying_pose`).
constexpr std::int32_t DownVerb(bool /*adult*/) { return verb::kCrawl; }
constexpr std::int32_t UpVerb(bool lying_pose) { return lying_pose ? verb::kLDamageStandUp : verb::kWait; }
constexpr const char* VerbName(std::int32_t v) {
    return v == verb::kCrawl ? "Crawl" : v == verb::kLDamageStandUp ? "LDamageStandUp" : "Wait";
}
// The adult's lying pose: the game's own AS command, held at frame 0.
inline constexpr std::string_view kLyingPose = "DamageLStand";
inline constexpr float kHeldRate = 0.0f;
inline constexpr float kPlayRate = 1.0f;
// Plays at kPlayRate on getting up (the first, and again if the game's own
// LDamageStandUp asked the command with skip-if-same and kept it held).
inline constexpr int kMaxReleases = 3;
// Whether our Link is laid down by kLyingPose now: an adult, lying, not in
// an event, in our Crawl (taken), not on a horse.
constexpr bool PoseWanted(bool adult, bool lying, bool event, bool crawling, bool riding) {
    return adult && lying && !event && crawling && !riding;
}
// Whether an AS command of our Link's is one of riding a horse (the adult
// AS root's Horse*, AimHorse*, Rideon*, RideOn*, DismountHorse).
bool Riding(std::string_view as_command);
// An actor's AS slot 0 (the command, its frame and rate), as the puppets and
// PLAYER_SAMPLE read it (CoopPuppet::addr::kControllerPath, docs/puppet.md).
struct AsSlot {
    std::string name;
    float frame = 0.0f;
    float rate = 1.0f;
};
std::optional<AsSlot> ReadSlot0(CoopPuppet::Guest& guest, std::uint64_t actor);
constexpr bool Passes(std::int32_t v) {
    for (const std::int32_t p : kPassVerbs) {
        if (p == v) {
            return true;
        }
    }
    return false;
}

// A raise of our Link's life (a revive, a respawn) for the Life step: the
// step takes it as a negative sum; the frame writes it directly when the step
// has not within kHealWaitUs (it skips frames with no life value).
inline constexpr std::uint64_t kHealWaitUs = 250'000;

constexpr std::int32_t Raised(std::int32_t life, std::int32_t sum, std::int32_t to) {
    return to >= 0 && life - sum < to ? life - to : sum;
}

class Raising {
public:
    // The frame: raise to `to` from now (a new ask replaces an old one).
    void Start(std::int32_t to, std::uint64_t now_us) noexcept;
    // The frame, each frame: the life to write directly now (the step did not
    // take it in time), once; nothing while waiting or when done.
    std::optional<std::int32_t> Frame(std::optional<std::int32_t> life, std::uint64_t now_us) noexcept;
    void Drop() noexcept;
    // The Life step (any game thread): the life to raise to, or -1.
    std::int32_t Peek() const noexcept { return to_.load(std::memory_order_acquire); }

private:
    std::atomic<std::int32_t> to_{-1};
    std::uint64_t since_us_ = 0;
};

// Our Link's crawl, shield and unseen. Want() from the frame; Skip() from the
// action verb wrap (a game thread); Next() from the calc-thread caller.
class Crawl {
public:
    struct Wanted {
        bool lying = false;          // downed (or spectating): crawl, shielded, unseen
        bool event = false;          // our Link in an event: the event drives it
        bool adult = false;          // our Link is Player_Adult (the same Crawl, laid down by kLyingPose)
        std::uint64_t actor = 0;     // our Link (the AS pose; 0: none)
        std::uint64_t bsa = 0;       // our Link's BSA component (0: none)
        std::uint64_t life = 0;      // our Link's Life component (0: none)
        std::uint64_t emitter = 0;   // our Link's PerceptionEmitter component (0: none)
        // Teammates downed, spectating or away (UnseenWhy): their puppets in
        // our game are unseen too. `why` goes into the log line.
        struct Unseen {
            std::uint8_t number = 0;
            std::string why;
        };
        std::vector<Unseen> unseen_puppets;
    };
    void Want(const Wanted& w);
    // Player n's live puppet actor in our game (0: none). The calc thread, as
    // Next (the module: the puppet director's slots).
    using PuppetActor = std::function<std::uint64_t(std::uint8_t number)>;
    void SetPuppetActor(PuppetActor find);
    // Any thread (row 177): player n's puppet is unseen in our game now (Wanted::unseen_puppets).
    bool PuppetUnseen(std::uint8_t number) const;

    // A request the game makes on any BSA; true: skip it (our downed Link in
    // the game's Crawl, any verb but Crawl and kPassVerbs, not our own call).
    // Learns our Link's own asking node.
    bool Skip(std::uint64_t bsa, const CoopBossNative::Request& request, bool own);
    // Whether `bsa` is our Link's (no lock; the wrap reads a request only then).
    bool IsOurs(std::uint64_t bsa) const noexcept {
        return bsa != 0 && bsa == bsa_.load(std::memory_order_acquire);
    }

    // The next game call (CoopCallers::Caller).
    std::optional<CoopPuppet::Call> Next(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                         std::uint64_t now_ms, std::uint64_t main_base);

    struct Counts {
        std::uint64_t skipped = 0;     // the game's own requests skipped while crawling
        std::uint64_t passed = 0;      // kPassVerbs let through while crawling
        std::uint64_t asked = 0;       // our Crawl / Wait requests made
        std::uint64_t refused = 0;     // ... that the game really refused (result not 0 and not kSlotBusy)
        std::uint64_t busy = 0;        // ... answered kSlotBusy: the slot had taken a request this frame
        std::uint64_t held_back = 0;   // frames nothing was asked: the game's own request ran on the slot
        std::uint64_t rounds = 0;      // crawl rounds started
        std::uint64_t cleared = 0;     // the game's own requests skipped to free the slot for our Crawl
        std::uint64_t shield_on = 0;   // our invincibility put on
        std::uint64_t unseen_on = 0;   // our emitter key put in
        std::uint64_t puppets_unseen_on = 0;  // ... into a downed teammate's puppet's emitter
        std::uint64_t poses = 0;       // kLyingPose played held on our adult Link (again: the game played another)
        std::uint64_t releases = 0;    // ... played on at kPlayRate (getting up)
        std::uint64_t riding = 0;      // frames lying on a horse (no crawl asked)
        std::vector<std::int32_t> skipped_verbs;  // the first distinct ones (the log)
    };
    Counts Totals() const;
    // Log lines since the last call.
    std::vector<std::string> TakeNotes();

    // Ask again at most this often while lying and not known to crawl.
    static constexpr std::uint64_t kAskEveryMs = 500;
    static constexpr int kMaxAsks = 6;  // then left until our Link's next Walk/Wait (see kRetryMs)
    // A round that ended refused (or out of asks) on the spot starts again on
    // the ground only this long after its last ask (moved by the world: at once).
    static constexpr std::uint64_t kRetryMs = 5000;
    // ... and at most this many rounds in a row with no move between (then
    // only a move of our Link, an event or getting up starts another).
    static constexpr int kSpotRounds = 3;
    // The request's result when the slot already took one this frame.
    static constexpr std::uint64_t kSlotBusy = CoopBossNative::addr::kActionSlotBusy;

private:
    enum class Pending : std::uint8_t { none, malloc, crawl, stand, shield, unseen, puppet, pose };
    // A keyed state of ours on one component: on while lying.
    struct Hold {
        std::uint64_t on_at = 0;  // the component it is on (0: off)
        bool tried = false;       // this frame's step done
    };
    std::optional<CoopPuppet::Call> Ask(CoopPuppet::Guest& guest, std::int32_t verb, std::uint64_t main_base);
    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, Hold& h, Pending kind, std::uint64_t now_at,
                                         std::uint64_t main_base, bool want);
    // A downed teammate's puppet's emitter, or none this frame.
    std::optional<CoopPuppet::Call> StepPuppets(CoopPuppet::Guest& guest, std::uint64_t main_base);
    void Note(std::string line);
    // The adult's lying pose this frame: play it held, or on to get up.
    std::optional<CoopPuppet::Call> StepPose(CoopPuppet::Guest& guest, std::uint64_t main_base);
    std::optional<CoopPuppet::Call> Play(CoopPuppet::Guest& guest, float rate, std::uint64_t main_base);

    mutable std::mutex mutex_;
    Wanted want_;
    std::atomic<std::uint64_t> bsa_{0};  // want_.bsa, read without the lock by Skip
    // Learnt from our Link's own scheduler requests (Walk / Wait: its main slot).
    std::uint64_t node_ = 0;
    std::int32_t slot_ = 0;
    std::uint16_t flags_ = 0;
    bool learnt_ = false;
    bool crawling_ = false;      // our Crawl was taken (until up, an event or a pass verb)
    // The caller's (calc thread).
    bool was_lying_ = false;
    std::int32_t asked_verb_ = 0;  // the verb of the action ask in flight (the log)
    bool stand_due_ = false;     // up again after a crawl: ask Wait once
    int asks_ = 0;
    int refusals_noted_ = 0;  // crawl refusals and slot-busy waits logged since our Link went down (the first few only)
    std::uint64_t next_ask_ms_ = 0;
    std::uint64_t last_ask_ms_ = 0;
    bool blocked_ = false;      // this round's crawl was really refused (not kSlotBusy)
    bool airborne_ = false;     // a kPassVerbs verb let through, no Walk/Wait since
    bool moved_ = false;        // the world moved our Link since this round started
    bool ground_seen_ = false;  // a Walk/Wait of the game's while not crawling (Next may start a round)
    bool cleared_noted_ = false;
    int spot_rounds_ = 0;       // rounds since our Link last moved (kSpotRounds)
    // The game's own request ran on our slot since the caller's last frame
    // (Skip let it through): the slot is busy until the BSA's next update.
    bool slot_taken_ = false;
    bool held_back_ = false;    // ... as the caller read it at this frame's start
    // The adult's lying pose (kLyingPose).
    bool riding_ = false;              // slot 0 plays a horse command (read each frame, adults)
    bool pose_tried_ = false;          // this frame's pose step done
    float pending_rate_ = 0.0f;        // the play call in flight
    std::uint64_t posed_actor_ = 0;    // the actor the pose was last held on (0: none)
    int releases_left_ = 0;            // plays at kPlayRate still allowed (getting up)
    bool standing_ = false;            // an adult up from the pose: LDamageStandUp asked, the game's own skipped
    int up_asks_ = 0;
    std::string pose_seen_;            // what slot 0 played when the pose was played again (the log)
    // A crawl round is on: the game's own requests are skipped to free the slot.
    bool Trying() const noexcept {
        return want_.lying && !want_.event && learnt_ && !crawling_ && !airborne_ && !blocked_ &&
               !riding_ && asks_ < kMaxAsks;
    }
    Hold shield_;
    Hold unseen_;
    std::map<std::uint8_t, Hold> puppets_;  // by player number: their puppet's emitter
    PuppetActor puppet_actor_;
    std::uint8_t pending_number_ = 0;  // the puppet hold call in flight
    std::string pending_why_;
    bool pending_on_ = false;    // the hold call in flight turns on (else off)
    std::uint64_t pending_at_ = 0;
    std::uint64_t block_ = 0;    // our request block in guest memory
    Pending pending_ = Pending::none;
    Counts counts_;
    std::vector<std::string> notes_;
};

// Why a teammate's puppet is unseen in our game, or nothing (they can be
// fought): downed or spectating (dbno), else their PLAYER_STATUS away bits.
std::optional<std::string> UnseenWhy(bool downed_or_spectating, std::uint8_t status);

Crawl& TheCrawl();
Raising& TheRaising();

}  // namespace CoopDbno::Body
