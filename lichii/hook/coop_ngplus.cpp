// SPDX-License-Identifier: GPL-3.0-or-later
//
// (Navi) New Game+ (coop_ngplus.h has the rules).

#include "coop_ngplus.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "coop/host/game_flags.hpp"
#include "coop/protocol/difficulty.hpp"
#include "coop_every.h"
#include "coop_flags.h"
#include "coop_live_events.h"
#include "coop_save.h"

namespace CoopNgPlus {
namespace {

namespace fs = std::filesystem;
namespace proto = coop::protocol;

constexpr std::uint32_t kMaxLifeHash = 0xfbe01da1u;  // PlayerStatus.MaxLife (docs/items.md), quarter hearts
constexpr std::uint64_t kBeatenAgainUs = 600'000'000;  // one Ganon death a 10 minutes (the event is asked more than once)

struct Glue {
    std::mutex mutex;
    Saved saved;
    std::once_flag read;                   // ReadOnce, once (any thread)
    std::atomic<std::uint32_t> packed{0};  // level | unlocked << 8 | hearts << 16, for any thread
    std::uint64_t cursor = 0;              // the game's event requests read so far (the hook's thread)
    std::uint64_t beaten_at = 0;
    std::int32_t hearts_said = -1;
    bool test_done = false;
    bool new_file = false;    // this file began with the game's opening (IsNewFileOpening) since the title
    bool gap_written = false; // the hearts were written in this load (no Link): once per load
    std::int32_t wrote = -1;  // MaxLife written in the last load, to check once Link is made (-1: none)
    bool held = false;        // the kept hearts hold in this file: nothing more read or written
    std::uint64_t loads_seen = 0;  // CoopSave::SaveLoadStarts at the last frame
    bool was_in_game = false;
    std::uint64_t link_at_us = 0;  // when our Link was last made (the end of a load)
    std::vector<std::string> lines;
    // ngplus-hearts: the opening's and Ganon's played flags (kOpeningPlayed, kGanonPlayed), read a few times a second
    // while our Link is in the scene.
    Played opening[std::size(kOpeningPlayed)];
    Played ganon[std::size(kGanonPlayed)];
    CoopEvery::Every played_poll{CoopEvery::kQuarterUs};
    bool played_said = false;
};

Glue& TheGlue() {
    static Glue glue;
    return glue;
}

fs::path File() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "coop-ngplus.ini";
}

void Publish(Glue& g) {
    g.packed.store(static_cast<std::uint32_t>(g.saved.level) | static_cast<std::uint32_t>(g.saved.unlocked) << 8 |
                   static_cast<std::uint32_t>(std::clamp(g.saved.hearts, 0, kMostQuarters)) << 16);
}

// Once (Ready's call_once), under the mutex.
void ReadOnce(Glue& g) {
    std::ifstream f(File(), std::ios::binary);
    if (f) {
        std::ostringstream text;
        text << f.rdbuf();
        g.saved = Parse(text.str());
    }
    Publish(g);
    LOG_INFO(Core, "coop: New Game+: level {}, unlocked {}, hearts {} (quarters)", g.saved.level, g.saved.unlocked,
             g.saved.hearts);
}

// Under the mutex. Written whole: a new file, then moved over the old one.
void Keep(Glue& g) {
    Publish(g);
    const fs::path file = File();
    fs::path tmp = file;
    tmp += ".new";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f || !(f << Format(g.saved)) || !f.flush()) {
            LOG_ERROR(Core, "coop: New Game+: could not write {}", tmp.string());
            return;
        }
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        LOG_ERROR(Core, "coop: New Game+: could not write {}", file.string());
    }
}

Glue& Ready() {
    // Any thread (the hook's, the host menu's RunHost, the pause menu): call_once, so no thread reads a half-read
    // file or races the first read (review of PR 236).
    Glue& g = TheGlue();
    std::call_once(g.read, [&g] {
        std::lock_guard lock(g.mutex);
        ReadOnce(g);
    });
    return g;
}

std::string Hearts(std::int32_t quarters) {
    const std::int32_t whole = quarters / 4, part = quarters % 4;
    return std::to_string(whole) + (part ? " " + std::to_string(part) + "/4" : std::string()) + " hearts";
}

// ngplus-hearts (coop_ngplus.h, kOpeningPlayed): the game plays the opening and Ganon's death from its own flows,
// never through the event request wrap, so their played flags tell them. Only for a game that may give (alone or the
// host): a guest's flags come from the host's save, so a guest that joins a host past Ganon would see them rise.
// Forgotten at the title and at every save load the game starts (a loaded save's flags are its own, not a rise);
// where the save loads are not seen (the emulator) whenever our Link is not made, so a file switch never rises.
void PlayedFlags(Glue& g, CoopHook::EmulatorMemory& memory, bool in_game, bool may_give, FileMark& mark,
                 bool& beaten) {
    if (mark.title || mark.load_started || !may_give || (!mark.native_loads && !in_game)) {
        for (auto& p : g.opening) Reset(p);
        for (auto& p : g.ganon) Reset(p);
        g.played_said = false;
        return;
    }
    if (!in_game || !g.played_poll.Due(CoopEvery::NowUs())) {
        return;
    }
    coop::host::GameFlagStore reader(memory);
    auto read = [&](std::uint32_t hash) -> std::optional<bool> {
        const auto e = reader.read(proto::FlagKey{hash, 0});
        if (!e || e->kind != proto::FlagKind::boolean) {
            return std::nullopt;
        }
        return e->value != 0;
    };
    // The opening only matters while hearts may still go into this file (a level on, hearts kept, not yet new).
    const std::uint32_t packed = g.packed.load();
    const bool want_opening = (packed & 0xff) != 0 && (packed >> 16) != 0 && !g.new_file;
    std::string says;
    for (std::size_t i = 0; want_opening && i < std::size(kOpeningPlayed); ++i) {
        const auto now = read(kOpeningPlayed[i]);
        if (!g.played_said && now) {
            says += (says.empty() ? "" : ", ") + std::string(*now ? "1" : "0");
        }
        if (Rose(g.opening[i], now)) {
            mark.opening = true;
            LOG_INFO(Core, "coop: New Game+: the opening played in this file (HavePlayedEvent {:#010x} 0 -> 1)",
                     kOpeningPlayed[i]);
        }
    }
    if (!says.empty()) {
        g.played_said = true;
        LOG_INFO(Core, "coop: New Game+: the opening's played flags in this file: {}", says);
    }
    for (std::size_t i = 0; i < std::size(kGanonPlayed); ++i) {
        if (Rose(g.ganon[i], read(kGanonPlayed[i]))) {
            beaten = true;
            LOG_INFO(Core, "coop: New Game+: Ganon's death seen (HavePlayedEvent {:#010x} 0 -> 1)", kGanonPlayed[i]);
        }
    }
}

}  // namespace

std::uint8_t Level() {
    return static_cast<std::uint8_t>(Ready().packed.load() & 0xff);
}

std::uint8_t Unlocked() {
    return static_cast<std::uint8_t>((Ready().packed.load() >> 8) & 0xff);
}

std::int32_t Hearts() {
    return static_cast<std::int32_t>(Ready().packed.load() >> 16);
}

bool SetLevel(int level) {
    Glue& g = Ready();
    std::lock_guard lock(g.mutex);
    const Saved now = Choose(g.saved, level);
    if (now == g.saved) {
        return false;
    }
    g.saved = now;
    Keep(g);
    const proto::DifficultyValues v = proto::ngplus_difficulty({}, now.level);
    LOG_INFO(Core, "coop: New Game+: level {} chosen", now.level);
    if (g.lines.size() < 16) {
        g.lines.push_back(now.level == 0 ? std::string("New Game+ off")
                                         : "New Game+ " + std::to_string(now.level) + ": enemies " +
                                               std::to_string(v.hp_pct) + "% life, " + std::to_string(v.damage_pct) +
                                               "% damage; you start with " + Hearts(now.hearts));
    }
    return true;
}

void Frame(CoopHook::EmulatorMemory& memory, bool in_game, bool may_give, bool title) {
    Glue& g = Ready();
    const std::uint64_t now_us = CoopEvery::NowUs();
    bool beaten = false;
    FileMark mark;
    for (const auto& r : CoopLiveEvents::TheGameRequests().Since(g.cursor)) {
        if (r.taken && IsGanonDeath(r.name)) {
            beaten = true;
            LOG_INFO(Core, "coop: New Game+: Ganon's death seen ({})", r.name);
        }
        mark.opening = mark.opening || (r.taken && IsNewFileOpening(r.name));
    }
    // Which file this is: the title or any save load the game starts ends a new file (a loaded save gets nothing,
    // however it was loaded); New Game's opening begins one. Not a load between scenes: the hearts go in there.
    const std::uint64_t loads = CoopSave::SaveLoadStarts();
    mark.was = g.new_file;
    mark.native_loads = CoopSave::NativeSaveLoads();
    mark.title = title;
    mark.load_started = loads != g.loads_seen;
    g.loads_seen = loads;
    PlayedFlags(g, memory, in_game, may_give, mark, beaten);
    const bool new_file = NewFileNow(mark);
    if (new_file != g.new_file) {
        if (new_file && !g.new_file) {
            LOG_INFO(Core, "coop: New Game+: a new file (its opening): the kept hearts may go into it");
        }
        g.held = false;
        g.wrote = -1;
    }
    g.new_file = new_file;
    // Tests only (OOT_COOP_TEST=1, as the server's test switches): Ganon counted beaten once Link is in a save.
    if (!g.test_done && in_game) {
        g.test_done = true;
        const char* test = std::getenv("OOT_COOP_TEST");
        const char* t = std::getenv("OOT_COOP_TEST_NGPLUS_BEAT");
        if (test != nullptr && std::string_view(test) == "1" && t != nullptr && t[0] == '1') {
            beaten = true;
            LOG_INFO(Core, "coop: New Game+: Ganon counted beaten (OOT_COOP_TEST_NGPLUS_BEAT, tests only)");
        }
    }
    const proto::FlagKey max_key{kMaxLifeHash, 0};
    // The save's MaxLife, read only when it is needed (Ganon's death, or a level on and a new file): New Game+ off
    // reads nothing, so the game alone runs as without it.
    auto read_max = [&]() -> std::optional<proto::FlagEntry> {
        coop::host::GameFlagStore reader(memory);
        auto e = reader.read(max_key);
        return e && e->kind == proto::FlagKind::int32 ? e : std::nullopt;
    };
    if (beaten && (g.beaten_at == 0 || now_us - g.beaten_at >= kBeatenAgainUs)) {
        g.beaten_at = now_us;
        const auto max_life = read_max();
        std::lock_guard lock(g.mutex);
        const Saved was = g.saved;
        g.saved = Beaten(g.saved, max_life ? static_cast<std::int32_t>(max_life->value) : 0);
        Keep(g);
        LOG_INFO(Core, "coop: New Game+: Ganon beaten at level {}: unlocked {}, hearts {}", was.level, g.saved.unlocked,
                 g.saved.hearts);
        if (g.saved.unlocked != was.unlocked && g.lines.size() < 16) {
            g.lines.push_back("New Game+ " + std::to_string(g.saved.unlocked) + " unlocked, with " +
                              Hearts(g.saved.hearts) + ": Multiplayer, Settings, New Game+, then start a new file");
        }
    }
    if (in_game) {
        g.gap_written = false;   // (the next load is a new gap)
        if (!g.was_in_game) {
            g.link_at_us = now_us;   // our Link made (a load ended)
        }
    }
    g.was_in_game = in_game;
    const std::uint32_t packed = g.packed.load();
    Give give;
    give.level = static_cast<std::uint8_t>(packed & 0xff);
    give.hearts = static_cast<std::int32_t>(packed >> 16);
    give.new_file = g.new_file;
    give.may_give = may_give;
    give.link_made = in_game;
    if (give.level == 0 || give.hearts <= 0 || !give.new_file || !give.may_give || g.held) {
        return;   // (nothing to give: MaxLife is not read)
    }
    if (in_game) {
        // After a write in a load: our Link was made from the flag; a second later his Life component has written
        // its max back into it (within a quarter second), so the flag says whether the hearts held. Read once per
        // write.
        if (g.wrote < 0 || now_us - g.link_at_us < 1'000'000) {
            return;
        }
        const auto max_life = read_max();
        if (!max_life) {
            return;
        }
        const auto now = static_cast<std::int32_t>(max_life->value);
        std::lock_guard lock(g.mutex);
        if (now >= g.wrote) {
            g.held = true;
            LOG_INFO(Core, "coop: New Game+: our Link was made with MaxLife {}: the kept hearts hold", now);
            if (g.lines.size() < 16) {
                g.lines.push_back("New Game+: your " + Hearts(now) + " carried over");
            }
        } else {
            LOG_INFO(Core, "coop: New Game+: the game kept MaxLife {} (not {}): written again at the next load", now,
                     g.wrote);
        }
        g.wrote = -1;
        return;
    }
    // A load between scenes (no Link): once per load. The game makes Link from PlayerStatus.MaxLife, as for a
    // loaded save, so the hearts hold alone too (with Link there, his Life component writes its own max back).
    if (g.gap_written) {
        return;
    }
    g.gap_written = true;
    const auto max_life = read_max();
    give.max_life = max_life ? std::optional<std::int32_t>(static_cast<std::int32_t>(max_life->value)) : std::nullopt;
    const auto to = HeartsToGive(give);
    if (!to) {
        if (give.max_life && *give.max_life >= std::min(give.hearts, kMostQuarters)) {
            g.held = true;   // (already that high: nothing more for this file)
        }
        return;
    }
    // The save's MaxLife through the game's own Int setter (GameFlags::use_calls: CoopFlagNative on the calc thread),
    // the path the boss's one heart container takes (coop_hook.cpp, CoopBoss::TakeMaxLifeWrite). In a session the
    // max rule shares it, so it reaches the guests as the host's save does.
    coop::host::GameFlagStore store(memory);
    auto set = *max_life;
    set.value = static_cast<std::uint32_t>(*to);
    const bool ok = store.write(set);
    LOG_INFO(Core, "coop: New Game+: MaxLife {} -> {} (a new file, in a load: our Link is made with it){}",
             *give.max_life, *to, ok ? "" : ": the write failed");
    if (ok) {
        g.wrote = *to;
    }
}

std::vector<std::string> TakeLines() {
    Glue& g = Ready();
    std::lock_guard lock(g.mutex);
    return std::exchange(g.lines, {});
}

}  // namespace CoopNgPlus
