// SPDX-License-Identifier: GPL-3.0-or-later
// --- The item-get moment (item_get.hpp): included by upstream.cpp inside its anonymous namespace ---
// Read from the lifted code by a research sub-agent (not by a run): an item actor's PouchContent component is
// [[actor+0x270]+0x380], its event component [[actor+0x270]+0x88]; main+0xd79f10(request) makes a 0x1b20-byte event
// request, main+0xfe40f0(component, request) fills it in for `Dm_SY_Get` with the actor as its performer (key
// `ActorName`), main+0xf61460(event component, request, 0) asks for it. [component+0x60] = 3 makes the scene's
// pouch add (EventTriggerAddOwnToPouch -> main+0xc6f620) do nothing, and also its delete at the end: we delete it.
// The fanfare alone: main+0x3356b00([main+0x4dd39e8], type, 0), type 0 small, 1 medium, 2 large.
struct ItemBox {
    std::mutex mutex;
    std::optional<std::pair<std::string, std::array<float, 3>>> scene;  // asked: actor, our Link's place
    std::optional<int> fanfare;                                        // asked: its type
    // the calc thread
    enum class Step { idle, allocating, creating, polling, taking, settling, requesting, filling, asking, playing,
                      deleting, fanfaring, skipping };
    Step step = Step::idle;
    std::uint64_t block = 0;
    std::uint64_t request = 0;
    std::uint64_t actor = 0;
    std::uint64_t since_ms = 0;
    int frames = 0;
    bool event_seen = false;
    bool skipped = false;
    std::uint64_t quiet_ms = 0;
    std::string name;
    std::array<float, 3> at{};
};
ItemBox g_item;
std::atomic<int> g_item_done{0};        // 1 played, 2 failed (TakeItemDone)
std::atomic<bool> g_item_event{false};  // our Link is in an event (the present thread's reading)
constexpr int kItemOrder = 59;
constexpr std::uint64_t kItemArg = 0x000, kItemName = 0x100, kItemHandle = 0x110, kItemText = 0x140,
                        kItemRequest = 0x200, kItemBlock = 0x2000;

std::optional<CoopPuppet::Call> ItemStep(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                         std::uint64_t now_ms) {
    ItemBox& v = g_item;
    const std::uint64_t main = g_main.load(std::memory_order_relaxed);
    auto word = [&guest](std::uint64_t at) {
        std::uint64_t w = 0;
        return at >= 0x10000 && guest.Read(at, &w, 8) ? w : 0;
    };
    auto fail = [&](const char* why) {
        CoopPose::FreezeOwnPose(false);
        LOG_INFO(Core, "coop: item-get: {}: {} (its fanfare and line instead)", v.name, why);
        v.step = ItemBox::Step::idle;
        g_item_done = 2;
    };
    using S = ItemBox::Step;
    if (after_call) {
        switch (v.step) {
        case S::allocating:
            v.block = result;
            v.step = S::idle;
            return std::nullopt;
        case S::creating:
            v.step = S::polling;
            v.since_ms = now_ms;
            return std::nullopt;
        case S::taking:
            v.actor = result;
            if (v.actor < 0x10000) {
                fail("the game made no actor");
                return std::nullopt;
            }
            v.step = S::settling;
            v.frames = 0;
            return std::nullopt;
        case S::requesting: {
            CoopPuppet::Call c;
            c.function = main + 0xfe40f0;
            c.x[0] = word(word(v.actor + 0x270) + 0x380);
            c.x[1] = v.request;
            v.step = S::filling;
            return c;
        }
        case S::filling: {
            CoopPuppet::Call c;
            c.function = main + 0xf61460;
            c.x[0] = word(word(v.actor + 0x270) + 0x88);
            c.x[1] = v.request;
            c.x[2] = 0;
            v.step = S::asking;
            return c;
        }
        case S::asking:
            CoopPose::FreezeOwnPose(true);  // the others keep our last pose (no empty raised hand on our puppet)
            LOG_INFO(Core, "coop: item-get: {}: the game's get scene asked for (answer {:#x})", v.name, result);
            v.step = S::playing;
            v.since_ms = now_ms;
            v.event_seen = false;
            v.skipped = false;
            v.quiet_ms = 0;
            return std::nullopt;
        case S::deleting:
            CoopPose::FreezeOwnPose(false);
            v.step = S::idle;
            g_item_done = v.event_seen ? 1 : 2;
            LOG_INFO(Core, "coop: item-get: {}: {}; its actor deleted", v.name,
                     v.event_seen ? "the get scene played" : "no get scene came (its fanfare and line instead)");
            return std::nullopt;
        case S::fanfaring:
        case S::skipping:
            v.step = v.step == S::skipping ? S::playing : S::idle;
            return std::nullopt;
        default:
            return std::nullopt;
        }
    }
    switch (v.step) {
    case S::idle: {
        std::optional<std::pair<std::string, std::array<float, 3>>> scene;
        std::optional<int> fanfare;
        {
            std::lock_guard lock(v.mutex);
            if (v.block == 0 && v.scene) {
                CoopPuppet::Call c;
                c.function = main + CoopPuppet::addr::kMalloc;
                c.x[0] = kItemBlock;
                v.step = S::allocating;
                return c;
            }
            scene = std::exchange(v.scene, std::nullopt);
            fanfare = std::exchange(v.fanfare, std::nullopt);
        }
        if (fanfare) {
            const std::uint64_t mgr = word(main + 0x4dd39e8);
            if (mgr == 0) {
                LOG_INFO(Core, "coop: item-get: the fanfare's sound manager is not there yet: no fanfare");
                return std::nullopt;
            }
            CoopPuppet::Call c;
            c.function = main + 0x3356b00;
            c.x[0] = mgr;
            c.x[1] = static_cast<std::uint64_t>(*fanfare);
            c.x[2] = 0;
            v.step = S::fanfaring;
            LOG_INFO(Core, "coop: item-get: the game's item fanfare (type {})", *fanfare);
            return c;
        }
        if (!scene) {
            return std::nullopt;
        }
        v.name = scene->first;
        v.at = scene->second;
        const std::uint64_t manager = word(main + CoopPuppet::addr::kActorManagerPtr);
        const std::uint64_t create_default = word(main + CoopPuppet::addr::kGotCreateDefault);
        if (manager == 0) {
            fail("no actor manager");
            return std::nullopt;
        }
        std::array<std::uint8_t, 0x100> arg{};
        const std::array<float, 3> place{v.at[0], v.at[1] + 1.0f, v.at[2]};
        const std::array<float, 9> turn{1, 0, 0, 0, 1, 0, 0, 0, 1};
        const std::array<float, 3> scale{1, 1, 1};
        std::memcpy(arg.data(), place.data(), 12);
        std::memcpy(arg.data() + 0x0c, turn.data(), 36);
        std::memcpy(arg.data() + 0x30, scale.data(), 12);
        std::memcpy(arg.data() + 0x40, &create_default, 8);
        arg[0x98] = 1;
        std::array<char, 0x40> text{};
        std::memcpy(text.data(), v.name.data(), std::min<std::size_t>(v.name.size(), text.size() - 1));
        const std::uint64_t name_ptr[2] = {v.block + kItemText, 0x40};
        const std::uint64_t zero = 0;
        guest.Write(v.block + kItemArg, arg.data(), arg.size());
        guest.Write(v.block + kItemText, text.data(), text.size());
        guest.Write(v.block + kItemName, name_ptr, 16);
        guest.Write(v.block + kItemHandle, &zero, 8);
        CoopPuppet::Call c;
        c.function = main + CoopPuppet::addr::kCreate;
        c.x[0] = manager;
        c.x[1] = v.block + kItemName;
        c.x[2] = v.block + kItemArg;
        c.x[3] = v.block + kItemHandle;
        c.x[4] = 1;
        v.step = S::creating;
        LOG_INFO(Core, "coop: item-get: {}: made beside our Link for the game's get scene", v.name);
        return c;
    }
    case S::polling: {
        const std::uint64_t request = word(v.block + kItemHandle);
        std::uint32_t state = 0;
        if (request != 0 && guest.Read(request, &state, 4)) {
            state &= 0xffff;
            if (state == CoopPuppet::addr::kRequestReady) {
                CoopPuppet::Call c;
                c.function = main + CoopPuppet::addr::kTakeCreated;
                c.x[0] = request;
                c.x[1] = 1;
                c.x[2] = 0;
                v.step = S::taking;
                return c;
            }
            if (state == 5 || state == 6 || state == CoopPuppet::addr::kRequestDrop) {
                fail("the game refused to make it (no such actor here)");
                return std::nullopt;
            }
        }
        if (now_ms - v.since_ms > 6000) {
            fail("the game did not make it in 6 s");
        }
        return std::nullopt;
    }
    case S::settling: {
        if (++v.frames < 4) {
            return std::nullopt;
        }
        const std::uint64_t comp = word(word(v.actor + 0x270) + 0x380);
        const std::uint64_t events = word(word(v.actor + 0x270) + 0x88);
        if (comp == 0 || events == 0) {
            if (v.frames > 90) {
                fail("it has no pouch or event component");
            }
            return std::nullopt;
        }
        if (std::getenv("OOT_COOP_ITEM_GET_ADD") == nullptr) {
            const std::uint32_t no_add = 3;
            guest.Write(comp + 0x60, &no_add, 4);  // not added to the pouch again: the item came by the flags
        }
        v.request = v.block + kItemRequest;
        CoopPuppet::Call c;
        c.function = main + 0xd79f10;
        c.x[0] = v.request;
        v.step = S::requesting;
        return c;
    }
    case S::playing: {
        const bool event = g_item_event.load();
        if (event) {
            v.event_seen = true;
            v.quiet_ms = 0;
        } else if (v.event_seen && v.quiet_ms == 0) {
            v.quiet_ms = now_ms;
        }
        // A player who never presses A would keep the box open and every follow waiting: after 25 s the event is
        // ended with the game's own force skip (SeqOneShotEventRequestForceSkip's call, as coop_boss_game.cpp does).
        // Run ig2: the force skip was made and the box stayed open, so it is only tried with OOT_COOP_ITEM_GET_SKIP=1.
        static const bool try_skip = std::getenv("OOT_COOP_ITEM_GET_SKIP") != nullptr;
        if (try_skip && event && !v.skipped && now_ms - v.since_ms > 25000) {
            v.skipped = true;
            std::uint64_t manager = word(main + 0x4e94840), session = manager ? word(manager + 0x68) : 0;
            std::uint64_t events = session ? word(session + 0xeb8) : 0;
            std::uint32_t count = 0, current = 0;
            if (events != 0 && guest.Read(session + 0xeb0, &count, 4) && guest.Read(session + 0xf18, &current, 4) &&
                current < count) {
                if (const std::uint64_t ev = word(events + 8ull * current); ev != 0) {
                    LOG_INFO(Core, "coop: item-get: {}: its text box was open for 25 s: the scene is ended", v.name);
                    CoopPuppet::Call c;
                    c.function = main + 0x3b3af30;
                    c.x[0] = ev;
                    v.step = S::skipping;
                    return c;
                }
            }
        }
        const bool over = v.event_seen && v.quiet_ms != 0 && now_ms - v.quiet_ms > 500;
        // The scene ends with its text box, which waits for the player's A: the actor stays until then (run ig1:
        // deleted under an open text box after 30 s, nothing broke, but the box stayed).
        if (over || now_ms - v.since_ms > (v.event_seen ? 180000u : 4000u)) {
            CoopPuppet::Call c;
            c.function = main + CoopPuppet::addr::kRequestDelete;
            c.x[0] = v.actor;
            c.x[1] = 0;
            v.step = S::deleting;
            return c;
        }
        return std::nullopt;
    }
    default:
        return std::nullopt;
    }
}
