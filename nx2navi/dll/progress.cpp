// SPDX-License-Identifier: GPL-3.0-or-later
// Shared progress and world state (progress.hpp): the emulator hook's own
// wiring (lichii/hook/coop_hook.cpp, Coop::Begin and Coop::Frame), kept in its
// order, with this port's memory and log. What is left out is said where it
// would be: the item cheats and the flag debug (emulator windows and switches).
#include "progress.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <format>
#include <mutex>
#include <string_view>
#include <utility>

#include "coop/host/flags.hpp"
#include "coop/host/game_flags.hpp"
#include "coop/host/game_items.hpp"
#include "coop/protocol/messages.hpp"
#include "coop_boss.h"
#include "coop_callers.h"
#include "coop_chests.h"
#include "coop_dungeon.h"
#include "coop_every.h"
#include "coop_live_events.h"
#include "coop_objects.h"
#include "coop_pickups.h"
#include "coop_puppet.h"
#include "coop_quest.h"
#include "coop_quests.h"
#include "coop_scrub_talk.h"
#include "coop_shared_scenes.h"
#include "coop_story.h"
#include "coop_time.h"
#include "coop_world.h"

namespace CoopNx2navi {
namespace {

using Clock = std::chrono::steady_clock;

std::string EnvString(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

std::uint64_t SteadyMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
}

bool ReadGuest(void* context, std::uint64_t address, void* out, std::size_t size) {
    return static_cast<coop::host::WritableGameMemory*>(context)->read(
        address, std::span<std::uint8_t>(static_cast<std::uint8_t*>(out), size));
}

}  // namespace

struct Progress::State {
    LogFn log;
    template <typename... Args>
    void Say(std::format_string<Args...> text, Args&&... args) {
        if (log) {
            log(std::format(text, std::forward<Args>(args)...));
        }
    }

    bool sharing = false;
    bool pickups = false;
    std::size_t pickups_given = 0;
    bool queue_sizes_logged = false;
    coop::host::LoadMend load_mend;
    CoopEvery::Every quest_catch_up{CoopEvery::kSecondUs};
    bool ocarina_mend_failed_logged = false;

    // The stores of one frame (Before builds them, the session's frame writes
    // through them, After reads what they held).
    std::optional<coop::host::GameFlagStore> game_store;
    std::optional<coop::host::ItemFlagStore> store;
    std::optional<CoopQuests::Store> quests;
    std::optional<coop::host::HoldFlagStore> hold;

    static constexpr std::chrono::milliseconds kHoldRelease{500};
    std::optional<Clock::time_point> no_link_since;
    std::string hold_scene;
    bool holding_logged = false;
    CoopScrubTalk::Hold scrub_talk;
    bool scrub_talk_on = EnvString("LICHII_COOP_SCRUB_TALK_HOLD") != "0";

    bool live_events = false;
    CoopLiveEvents::Decider live_decider;
    std::mutex live_mutex;  // the requester runs on the calc thread (a frame step caller)
    CoopLiveEvents::Requester live_requester;
    CoopLiveEvents::StartWatch start_watch;
    std::uint64_t main_base = 0;  // for the requester's caller

    bool shared_scenes = false;
    CoopSharedScenes::Joiner shared_joiner;
    CoopLiveEvents::StartWatch story_watch;
    std::mutex dungeon_events_mutex;  // DUNGEON_EVENT from the network thread
    std::vector<coop::protocol::DungeonEvent> story_events_in;
    std::vector<coop::protocol::DungeonEvent> dungeon_events_in;

    CoopBoss::StaleBossFlags stale_boss;
    std::uint64_t doubled_since_ms = 0;
    bool doubled_event_seen = false;
    static constexpr std::uint64_t kDoubledNoEventMs = 1000;

    std::uint64_t written = 0;  // values other players' changes wrote here

    void CatchUpQuests(coop::host::WritableGameMemory& memory);
    void MendLiftGrade(coop::host::WritableGameMemory& memory);
    void MendOcarina(coop::host::WritableGameMemory& memory);
    void CatchUpQuest(coop::host::WritableGameMemory& memory);
    void SharedScenes(coop::host::WritableGameMemory* memory, coop::host::Session& session,
                      const std::optional<coop::host::PlayerReading>& reading, std::uint64_t server_now_us);
};

Progress::Progress() : s_(std::make_unique<State>()) {}
Progress::~Progress() = default;

bool Progress::Sharing() const { return s_->sharing; }

std::optional<std::string> Progress::ExtraNotice(coop::host::FlagKey key, const coop::protocol::FlagEntry&) {
    if (key.kind == coop::protocol::FlagKeyKind::named && CoopChests::KeyOfOpened(key.hash)) {
        return std::string("opened a chest");
    }
    return std::nullopt;
}

void Progress::Configure(coop::host::SessionSettings& session, bool on, LogFn log) {
    State& s = *s_;
    s.log = std::move(log);
    s.load_mend.set_clock([] { return CoopEvery::NowUs(); });
    if (!on) {
        s.sharing = false;
        s.pickups = false;
        session.learn_pickups = false;
        return;
    }
    // coop_flags.cpp LoadShared without the list file: the player's items
    // (docs/items.md), unless LICHII_COOP_ITEMS=0.
    if (EnvString("LICHII_COOP_ITEMS") != "0") {
        session.shared = coop::host::item_shared_flags();
        s.Say("flags: sharing {} item flags", session.shared->size());
    }
    CoopDungeon::AddShared(session.shared);  // docs/dungeon.md
    CoopStory::AddShared(session.shared);    // docs/flags.md, "Story and cutscenes"
    CoopQuests::AddShared(session.shared);   // docs/flags.md, "Threads of Time"
    CoopChests::AddShared(session.shared);   // docs/items.md, "Chests"
    s.sharing = session.shared.has_value();
    // One-time pickups (docs/items.md): keys the object walk notes are shared
    // as they are seen, and another game's taken key is taken here too.
    s.pickups = s.sharing && EnvString("LICHII_COOP_PICKUPS") != "0";
    session.learn_pickups = s.pickups;
    s.pickups_given = 0;
    s.Say("flags: shared progress on ({} flags: items, songs, keys, chests, dungeons, story{})",
          session.shared ? session.shared->size() : 0, s.pickups ? ", one-time pickups" : "");
}

void Progress::Started(coop::host::Session& session) {
    State& s = *s_;
    if (!s.sharing) {
        return;
    }
    if (s.pickups && CoopDungeon::On()) {  // chests, silver rupees, cleared rooms stay done
        for (const auto& k : CoopDungeon::Keys()) {
            session.share_pickup(coop::protocol::FlagKey::key64_flag(k.key));
        }
    }
    // Another player's gimmick event plays here too when our Link comes near
    // it (dungeon switches): on unless LICHII_COOP_LIVE_EVENTS=0.
    s.live_events = EnvString("LICHII_COOP_LIVE_EVENTS") != "0";
    if (s.live_events) {
        const std::string names = EnvString("LICHII_COOP_LIVE_EVENT_NAMES");
        if (!names.empty()) {
            if (auto parsed = CoopLiveEvents::ParseNames(names)) {
                s.live_decider = CoopLiveEvents::Decider(std::move(*parsed));
            } else {
                s.live_events = false;
                s.Say("dungeon: LICHII_COOP_LIVE_EVENT_NAMES names no event: live events off");
            }
        }
    }
    if (s.live_events) {
        // The frame step's callers (objects/emu_port.cpp keeps the list and
        // objects.cpp asks it on the game's calc thread; with objects = 0
        // nothing asks, and a held event then waits for the room's load).
        State* state = s_.get();
        CoopCallers::Add(60, [state](CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                     std::uint64_t) {
            std::lock_guard lock(state->live_mutex);
            auto call = state->live_requester.Step(guest, after_call, result, state->main_base);
            for (const auto& line : state->live_requester.TakeNotes()) {
                state->Say("dungeon: {}", line);
            }
            return call;
        });
        s.Say("dungeon: live events on (off with LICHII_COOP_LIVE_EVENTS=0)");
    }
    s.shared_scenes = s.live_events && CoopSharedScenes::OnFromEnv(EnvString("LICHII_COOP_SHARED_CUTSCENES"));
    s.Say("cutscene: shared story cutscenes {}",
          s.shared_scenes ? "on (off with LICHII_COOP_SHARED_CUTSCENES=0)" : "off");
    if (CoopDungeon::On()) {  // room switch flags the game resets itself: one way, no load write-back
        for (const auto& f : CoopDungeon::LiveFlags()) {
            session.share_one_way(coop::protocol::FlagKey(f.hash, std::uint16_t{0}));
        }
    }
    if (s.pickups && CoopChests::On()) {  // one way: an opened chest never closes again
        for (const auto& k : CoopChests::Keys()) {
            session.share_pickup(coop::protocol::FlagKey::key64_flag(k.key));
        }
        // What a real opening sets (issue 87): the chest's own Bool.
        for (const auto& o : CoopChests::OpenedFlags()) {
            session.share_pickup_named(coop::protocol::FlagKey(o.flag, std::uint16_t{0}));
        }
    }
    if (s.pickups) {  // issue 89: every Gold Skulltula's key, and the token count guarded by them
        std::vector<coop::protocol::FlagKey> skulltulas;
        for (const std::uint64_t key : coop::host::gold_skulltula_keys()) {
            skulltulas.push_back(coop::protocol::FlagKey::key64_flag(key));
        }
        session.guard_count(coop::protocol::FlagKey(coop::host::kGoldTokenCount, std::uint16_t{0}), skulltulas);
    }
}

void Progress::OnEvent(const coop::net::Event& event) {
    State& s = *s_;
    if (!s.sharing || event.kind != coop::net::EventKind::message) {
        return;
    }
    if (const auto* e = std::get_if<coop::protocol::DungeonEvent>(&event.message)) {
        std::lock_guard lock(s.dungeon_events_mutex);
        // A story cutscene's start (coop_shared_scenes.h) has its own queue.
        auto& in = CoopSharedScenes::IsShared(e->name) ? s.story_events_in : s.dungeon_events_in;
        if (in.size() < 16) {
            in.push_back(*e);
        }
    }
}

coop::host::FlagStore* Progress::Before(coop::host::WritableGameMemory* memory, coop::host::Session&,
                                        const std::optional<coop::host::PlayerReading>& reading) {
    State& s = *s_;
    s.hold.reset();
    s.quests.reset();
    s.store.reset();
    s.game_store.reset();
    if (!s.sharing) {
        return nullptr;
    }
    const auto at = Clock::now();
    if (memory != nullptr) {
        s.main_base = memory->main_base();
        s.game_store.emplace(*memory);
        if (!s.queue_sizes_logged) {  // once: half of each is ours (issue 175)
            if (const auto q = coop::host::GameFlags(*memory).queue_sizes()) {
                s.queue_sizes_logged = true;
                const std::uint32_t least = std::min({q->boolean, q->int32, q->uint32, q->enumeration, q->key64});
                s.Say("flags: the game's write queues hold Bool {}, Int {}, UInt {}, Enum {}, 64-bit keys {} "
                      "slots; ours is half of each{}",
                      q->boolean, q->int32, q->uint32, q->enumeration, q->key64,
                      least < 2 ? " (a queue under 2 slots gets no write of ours)" : "");
            }
        }
        if (s.load_mend.ready() && CoopStory::On() && s.quest_catch_up.Due(CoopEvery::NowUs())) {
            s.CatchUpQuests(*memory);  // issue 173: Impa comes for every player
        }
        // A small key or token we and another player took at once: ours goes
        // back. A token taken elsewhere: its Skulltula's key here (docs/world.md).
        for (const std::string& count : CoopWorld::TakeGiveBacks()) {
            s.Say("{}", CoopWorld::GiveBack(*s.game_store, count));
        }
        for (const std::uint64_t key : CoopWorld::TakeTakenKeys()) {
            s.Say("{}", CoopWorld::SetTakenKey(*s.game_store, key));
        }
        // Issue 138: grants wait for a loaded save, and the capacities of held
        // items are mended for a while after each load.
        const bool mend = s.load_mend.frame(reading.has_value());
        s.store.emplace(*s.game_store, &s.load_mend);
        if (mend) {
            if (const auto raised = s.store->mend(); raised != 0) {
                s.Say("items: {} capacities mended after a load", raised);
            }
        }
        if (s.load_mend.ready()) {
            s.MendLiftGrade(*memory);  // issue 125
            s.MendOcarina(*memory);    // issue 190
            if (CoopStory::On()) {
                s.CatchUpQuest(*memory);  // issue 130: the Kokiri Shop opens
            }
        }
    }
    if (!s.store) {
        return nullptr;
    }
    // Issue 129 (docs/dungeon.md "Held until the room loads") and issue 193
    // (the Threads of Time quests, ordered).
    s.quests.emplace(
        *s.store, [memory](std::uint32_t hash) { return coop::host::GameFlags(*memory).get_enum(hash); },
        [memory](std::uint32_t hash, std::uint32_t value) {
            return coop::host::GameFlags::set_enum(*memory, hash, value);
        });
    if (reading) {
        s.no_link_since.reset();
    } else if (!s.no_link_since) {
        s.no_link_since = at;
    }
    if (reading && reading->scene) {
        s.hold_scene = *reading->scene;
    }
    const bool link = !s.no_link_since || at - *s.no_link_since < State::kHoldRelease;
    const std::optional<std::string> scene =
        s.hold_scene.empty() ? std::nullopt : std::optional<std::string>(s.hold_scene);
    // Issue 144: a scrub's destroy flag waits while our Link is in an event.
    if (s.scrub_talk_on &&
        s.scrub_talk.Frame(reading && reading->event_running.value_or(false), CoopEvery::NowUs())) {
        s.Say("dungeon: {}", s.scrub_talk.Holding()
                                 ? "our Link is in an event: Deku Scrub destroy flags from others wait"
                                 : "the event ended: Deku Scrub destroy flags from others go through");
    }
    const CoopScrubTalk::Hold talk = s.scrub_talk;
    s.hold.emplace(*s.quests, [link, scene, talk](coop::protocol::FlagKey key) {
        if (key.kind != coop::protocol::FlagKeyKind::named || key.index != 0) {
            return false;
        }
        if (talk.Holds(key.hash)) {
            return true;
        }
        if (!link || !scene) {
            return false;
        }
        if (CoopQuests::Find(key.hash) != nullptr) {
            return CoopDungeon::IsDungeonScene(*scene);  // a quest step waits outside dungeons
        }
        const auto in = CoopDungeon::HeldIn(key.hash);
        return in && (in->empty() ? CoopDungeon::IsDungeonScene(*scene) : *scene == *in);
    });
    return &*s.hold;
}

void Progress::After(coop::host::WritableGameMemory* memory, coop::host::Session& session,
                     const coop::host::FrameReport& report,
                     const std::optional<coop::host::PlayerReading>& reading) {
    State& s = *s_;
    if (!s.sharing) {
        return;
    }
    namespace proto = coop::protocol;
    s.written += report.received.size();
    if (s.pickups) {
        const auto keys = CoopPickups::Since(s.pickups_given);
        for (const std::uint64_t key : keys) {
            session.share_pickup(proto::FlagKey::key64_flag(key));
            s.Say("flags: one-time pickup {:#x} is shared from now on", key);
        }
        s.pickups_given += keys.size();
    }
    if (s.quests) {
        for (const auto& r : s.quests->take_raised()) {
            s.Say("quests: {} {} -> {} (shared Threads of Time; issue 193)", r.quest, r.from, r.to);
        }
    }
    if (report.save_load_flags != 0) {
        s.Say("flags: save load seen ({} flags back, {} frames without a scene)", report.save_load_flags,
              report.save_load_frames);
        CoopTime::SaveLoaded(report.server_now_us);  // issue 159
    }
    const bool running_now = reading && reading->event_running.value_or(false);
    std::optional<std::string> view_scene = reading ? reading->scene : std::nullopt;
    // DUNGEON_EVENT (issues 176 and 196): a gimmick event our game starts goes
    // out at once, and another player's start is offered like a held flag.
    if (s.hold && s.live_events) {
        std::vector<proto::DungeonEvent> got;
        {
            std::lock_guard lock(s.dungeon_events_mutex);
            got.swap(s.dungeon_events_in);
        }
        for (const auto& e : got) {
            if (e.scene == s.hold_scene && CoopDungeon::HeldIn(coop::host::flag_hash("HavePlayedEvent." + e.name))) {
                s.Say("dungeon: player {} started the event {} in this scene", e.player_number, e.name);
                s.live_decider.Started(e.name, e.scene);
            }
        }
        if (memory != nullptr && !s.hold_scene.empty()) {
            std::vector<std::string_view> names;
            for (const std::string_view e : CoopLiveEvents::EventsIn(s.hold_scene)) {
                if (CoopDungeon::HeldIn(coop::host::flag_hash("HavePlayedEvent." + std::string(e)))) {
                    names.push_back(e);
                }
            }
            const auto found = s.start_watch.Frame(running_now, [&] {
                return CoopLiveEvents::FindRunningEvent(&ReadGuest, memory, s.main_base, names);
            });
            if (found) {
                s.live_decider.Ours(*found, s.hold_scene);
                proto::DungeonEvent e;
                e.scene = s.hold_scene;
                e.name = *found;
                session.with_client([&](coop::net::Client& c) {
                    const bool sent = c.send(proto::Message{e});
                    s.Say("dungeon: our game started the event {}: {}", *found,
                          sent ? "sent to the others" : "not sent");
                    return 0;
                });
            }
        }
    }
    if (!(s.hold && s.live_events)) {
        std::lock_guard lock(s.dungeon_events_mutex);
        s.dungeon_events_in.clear();  // nothing takes them now: none applied late
    }
    if (s.shared_scenes) {
        s.SharedScenes(memory, session, reading, report.server_now_us);
    }
    if (s.live_events && memory != nullptr) {
        std::lock_guard lock(s.live_mutex);
        s.live_requester.Expire(view_scene.value_or(std::string()), CoopEvery::NowUs(), [&](const std::string& n) {
            return coop::host::GameFlags(*memory)
                .get_bool(coop::host::flag_hash("HavePlayedEvent." + n))
                .value_or(false);
        });
    }
    // Issue 129: a held gimmick event is started here too when our Link is
    // near the place that asks for it, so its result happens live.
    if (s.hold && s.live_events) {
        std::vector<std::string> held_events;
        for (const auto& k : s.hold->held_keys()) {
            if (const auto e = CoopDungeon::EventOf(k.hash); !e.empty()) {
                held_events.emplace_back(e);
            }
        }
        std::optional<std::array<float, 3>> link;
        if (reading) {
            link = std::array<float, 3>{reading->position[0], reading->position[1], reading->position[2]};
        }
        if (const auto name = s.live_decider.Next(held_events, s.hold_scene, running_now, link)) {
            s.Say("dungeon: starting the event {} here (another player played it)", *name);
            std::lock_guard lock(s.live_mutex);
            s.live_requester.Want(*name, s.hold_scene);
        }
    }
    const bool gimmick_held =
        s.hold && std::any_of(s.hold->held_keys().begin(), s.hold->held_keys().end(),
                              [](const proto::FlagKey& k) { return !CoopScrubTalk::IsTalkFlag(k.hash); });
    if (s.hold && gimmick_held != s.holding_logged) {
        s.holding_logged = gimmick_held;
        s.Say("dungeon: {}", s.holding_logged
                                 ? "room gimmick flags from another player held until we leave this dungeon or load"
                                 : "no room gimmick flags held");
    }
    // Issue 146: a map or compass the game took back, put back.
    for (const auto& k : session.take_one_way_restores()) {
        s.Say("flags: {:#010x} (dungeon map or compass) went back to 0 here: put back to 1 ({} put back this "
              "session)",
              k.hash, session.one_way_restores());
    }
    for (const auto& g : session.take_counts_guarded()) {  // issue 89 (docs/items.md)
        s.Say("flags: count {:#010x} +{} here was a pickup another player had taken: put back, not sent",
              g.count.hash, g.amount);
    }
    {  // issue 195: a chest another player had opened, opened again here
        for (unsigned n = CoopObjects::TakeChestsOpenedTwice(); n > 0; --n) {
            if (session.expect_doubled_gain()) {
                s.Say("flags: a chest another player had opened was opened here too: this game's count gain "
                      "in its get scene is taken back");
                s.doubled_since_ms = SteadyMs();
                s.doubled_event_seen = running_now;
            }
        }
        if (s.doubled_since_ms != 0) {
            s.doubled_event_seen = s.doubled_event_seen || running_now;
            if (!running_now &&
                (s.doubled_event_seen || SteadyMs() - s.doubled_since_ms > State::kDoubledNoEventMs)) {
                session.end_doubled_gain();
                s.doubled_since_ms = 0;
            }
        }
    }
    // Chests opened and pickups taken in another game: what is loaded of them
    // goes here too (docs/items.md, "Chests").
    if (const auto taken = session.take_written_for_others(); !taken.empty()) {
        std::vector<std::uint64_t> keys;
        for (const auto& k : taken) {
            if (k.kind == proto::FlagKeyKind::key64) {
                keys.push_back(k.key);
            } else if (const auto chest = CoopChests::KeyOfOpened(k.hash)) {
                keys.push_back(*chest);  // a chest's opened flag: that chest (issue 87)
            }
        }
        s.Say("flags: {} one-way flags written here for another player ({} of them loaded things to open "
              "or take away)",
              taken.size(), keys.size());
        CoopObjects::TakenForOthers(keys);
    }
    // Issue 148: a boss door left shut in this save by an older build.
    if (s.game_store) {
        std::optional<CoopBoss::Vec> at;
        if (reading) {
            at = CoopBoss::Vec{reading->position[0], reading->position[1], reading->position[2]};
        }
        const auto key = [](std::string_view name) {
            return proto::FlagKey{coop::host::flag_hash(std::string(name)), 0};
        };
        for (const std::string_view name : s.stale_boss.Check(view_scene, at, [&](std::string_view n) {
                 const auto v = s.game_store->read(key(n));
                 return v && v->value != 0;
             })) {
            const bool ok = s.game_store->write(proto::FlagEntry{key(name), proto::FlagKind::boolean, 0});
            s.Say("boss: {} was left set in this save (an older co-op build shared it): {}", name,
                  ok ? "cleared" : "could not clear");
        }
    }
    {  // our own key spends, for the locks' give-back (docs/dungeon.md, "Locked doors")
        std::vector<std::uint32_t> drops;
        for (const auto& k : session.take_own_count_drops()) {
            drops.push_back(k.hash);
        }
        CoopObjects::OwnCountDrops(drops);
    }
    if (s.game_store) {  // a lock opened twice: our second key back (docs/dungeon.md)
        for (const std::uint32_t key : CoopObjects::TakeKeyRefunds()) {
            const auto now = s.game_store->read(proto::FlagKey{key, 0});
            if (now && now->kind == proto::FlagKind::int32) {
                auto back = *now;
                back.value = static_cast<std::uint32_t>(static_cast<std::int32_t>(now->value) + 1);
                const bool ok = s.game_store->write(back);
                s.Say("doors: small key {:#x} given back ({} -> {}){}", key, static_cast<std::int32_t>(now->value),
                      static_cast<std::int32_t>(back.value), ok ? "" : ": the write failed");
            }
        }
    }
    if (s.game_store) {  // issue 88: what the session left in MaxLife (docs/items.md)
        const auto max_life = s.game_store->read(proto::FlagKey{0xfbe01da1u, 0});
        CoopObjects::SetMaxLife(max_life && max_life->kind == proto::FlagKind::int32
                                    ? std::optional<std::int32_t>(static_cast<std::int32_t>(max_life->value))
                                    : std::nullopt);
    }
}

std::string Progress::StatusText() const {
    if (!s_->sharing) {
        return {};
    }
    return std::format("; progress: {} values written here for other players' changes", s_->written);
}

// Shared story cutscenes (coop_shared_scenes.h): a story cutscene our game
// starts goes to the others in our scene; one another player started near us
// plays here too, through the live-event requester. Once a frame.
void Progress::State::SharedScenes(coop::host::WritableGameMemory* memory, coop::host::Session& session,
                                   const std::optional<coop::host::PlayerReading>& reading,
                                   std::uint64_t server_now_us) {
    namespace proto = coop::protocol;
    std::vector<proto::DungeonEvent> got;
    {
        std::lock_guard lock(dungeon_events_mutex);
        got.swap(story_events_in);
    }
    const std::uint64_t now_us = CoopEvery::NowUs();
    if (!got.empty()) {
        const auto remotes = CoopPuppet::Collect(&session, server_now_us);
        for (const auto& e : got) {
            std::optional<CoopSharedScenes::Vec3> starter;
            for (const auto& r : remotes) {
                if (r.number == e.player_number && r.scene == e.scene) {
                    starter = r.position;
                }
            }
            shared_joiner.Started(e.player_number, e.name, e.scene, starter, now_us);
        }
    }
    const bool running = reading && reading->event_running.value_or(false);
    std::uint8_t status = 0;
    session.with_client([&](coop::net::Client& c) {
        if (const auto* me = c.find_player(c.player_number())) {
            status = me->status.value_or(0);
        }
        return 0;
    });
    CoopSharedScenes::Here here;
    here.scene = reading && reading->scene ? *reading->scene : std::string();
    if (reading) {
        here.link = reading->position;
    }
    here.can_start = reading && !running &&
                     (status & (proto::status::paused | proto::status::in_menu | proto::status::loading)) == 0;
    here.now_us = now_us;
    if (memory != nullptr && !here.scene.empty()) {
        static const std::vector<std::string_view> names = [] {
            const auto all = CoopSharedScenes::Names();
            return std::vector<std::string_view>(all.begin(), all.end());
        }();
        const auto found = story_watch.Frame(running, [&] {
            return CoopLiveEvents::FindRunningEvent(&ReadGuest, memory, main_base, names);
        });
        if (found && shared_joiner.Ours(*found, here.scene)) {
            proto::DungeonEvent e;
            e.scene = here.scene;
            e.name = *found;
            session.with_client([&](coop::net::Client& c) {
                const bool sent = c.send(proto::Message{e});
                Say("cutscene: our game started the story cutscene {}: {}", *found,
                    sent ? "sent to the others in this scene" : "not sent");
                return 0;
            });
        }
    }
    const auto name = shared_joiner.Next(here, [&](const std::string& n) {
        return memory != nullptr && coop::host::GameFlags(*memory)
                                        .get_bool(coop::host::flag_hash("HavePlayedEvent." + n))
                                        .value_or(false);
    });
    for (const auto& line : shared_joiner.TakeNotes()) {
        Say("cutscene: {}", line);
    }
    if (name) {
        std::lock_guard lock(live_mutex);
        live_requester.Want(*name, here.scene, now_us, CoopSharedScenes::kJoinUs);
    }
}

// Issue 173 (coop_quest.h): raises our own quests to what the shared story
// scenes say. Only ever raised.
void Progress::State::CatchUpQuests(coop::host::WritableGameMemory& memory) {
    coop::host::GameFlags flags(memory);
    const CoopQuest::ReadBool flag = [&](std::uint32_t h) { return flags.get_bool(h); };
    const CoopQuest::ReadEnum value = [&](std::uint32_t h) { return flags.get_enum(h); };
    for (const auto& quest : CoopQuest::Quests()) {
        const auto was = flags.get_enum(quest.hash);
        const auto raise = CoopQuest::CatchUp(quest, flag, value);
        if (raise && was && coop::host::GameFlags::set_enum(memory, quest.hash, *raise)) {
            Say("story: quest {} {:#x} -> {:#x} (shared story; issue 173)", quest.name, *was, *raise);
        }
    }
}

// Issue 125 (docs/items.md, "Strength").
void Progress::State::MendLiftGrade(coop::host::WritableGameMemory& memory) {
    namespace lift = coop::host::lift;
    coop::host::GameFlags flags(memory);
    const auto grade = flags.get_enum(lift::kGrade);
    if (!grade) {
        return;
    }
    const auto stock = [&](std::uint32_t hash) { return flags.get_int(hash).value_or(0); };
    const auto raise =
        coop::host::lift_grade_raise(stock(lift::kGoron), stock(lift::kSilver), stock(lift::kGold), *grade);
    if (raise && coop::host::GameFlags::set_enum(memory, lift::kGrade, *raise)) {
        Say("items: lift grade {:#x} -> {:#x} for the held strength item", *grade, *raise);
    }
}

// Issue 190 (docs/items.md, "The ocarina").
void Progress::State::MendOcarina(coop::host::WritableGameMemory& memory) {
    namespace oc = coop::host::ocarina;
    coop::host::GameFlags flags(memory);
    const auto fairy = flags.get_int(oc::kFairy).value_or(0);
    const auto time = flags.get_int(oc::kTime).value_or(0);
    for (const std::uint32_t hash :
         coop::host::ocarina_bools_mend(fairy, time, [&](std::uint32_t h) { return flags.get_bool(h); })) {
        if (coop::host::GameFlags::set_bool(memory, hash, true)) {
            Say("items: ocarina bool {:#010x} 0 -> 1 for the held ocarina", hash);
        }
    }
    const auto current = flags.get_string64(oc::kEquip);
    if (!current) {
        return;
    }
    const auto want = coop::host::ocarina_equip_mend(flags.get_int(oc::kFairy).value_or(0),
                                                     flags.get_int(oc::kTime).value_or(0), *current);
    if (!want) {
        return;
    }
    if (coop::host::GameFlags::set_string64_now(memory, oc::kEquip, *want)) {
        Say("items: ocarina '{}' -> '{}' for the held ocarina", *current, *want);
    } else if (!ocarina_mend_failed_logged) {
        ocarina_mend_failed_logged = true;
        Say("items: could not equip the held ocarina '{}'", *want);
    }
}

// Issue 130 (docs/flags.md, "Story and cutscenes").
void Progress::State::CatchUpQuest(coop::host::WritableGameMemory& memory) {
    namespace quest = CoopStory::quest;
    coop::host::GameFlags flags(memory);
    const auto step = flags.get_enum(quest::kMeetDekuTree);
    const auto raise =
        CoopStory::QuestCatchUp(step, flags.get_bool(quest::kMidoBlocker), flags.get_bool(quest::kMetDekuTree));
    if (raise && coop::host::GameFlags::set_enum(memory, quest::kMeetDekuTree, *raise)) {
        Say("story: quest Meet_DekuTree {:#x} -> {:#x} (shared story; issue 130)", *step, *raise);
    }
}

}  // namespace CoopNx2navi
