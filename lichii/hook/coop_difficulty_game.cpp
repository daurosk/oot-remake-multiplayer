// SPDX-License-Identifier: GPL-3.0-or-later
//
// Difficulty, the session's side (coop_difficulty.h has the rules). The
// server's DIFFICULTY is what is in force; the host's choice from the pause
// menu (Set) is sent to the server and kept in coop-difficulty.ini beside
// coop.ini for this player's next session as host.

#include "coop_difficulty.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>
#include <utility>
#include <variant>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "coop_every.h"
#include "coop_ngplus.h"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"

namespace CoopDifficulty {
namespace {

namespace proto = coop::protocol;
namespace fs = std::filesystem;

struct Glue {
    std::mutex mutex;
    State state;                    // network thread writes, under mutex
    // state.values for any thread, all five (PackValues). Issue 55: the boss percents were left out of
    // this word, so the Life step read boss life and damage as 100% whatever the host's difficulty said.
    std::atomic<std::uint64_t> now{PackValues(Values{})};
    std::atomic<bool> told{false};  // (Navi) the server said the values (they hold New Game+ then)
    std::optional<Choice> wish;     // Set -> Frame
    std::optional<Choice> kept;     // the file's, read once
    bool kept_read = false;
    bool offered = false;           // the kept choice was sent on this connection
    std::vector<std::string> lines; // any thread -> chat box

    void Line(std::string text) {
        if (lines.size() < 32) {
            lines.push_back(std::move(text));
        }
    }
};

Glue& TheGlue() {
    static Glue glue;
    return glue;
}

fs::path File() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "coop-difficulty.ini";
}

// Under the mutex. A missing or broken file is no choice.
const std::optional<Choice>& Kept(Glue& g) {
    if (!g.kept_read) {
        g.kept_read = true;
        std::ifstream f(File(), std::ios::binary);
        if (f) {
            std::ostringstream text;
            text << f.rdbuf();
            g.kept = ParseChoice(text.str());
            if (!g.kept) {
                LOG_ERROR(Core, "coop: difficulty: {} is not a difficulty file; ignored", File().string());
            }
        }
    }
    return g.kept;
}

// Written whole: a new file beside it, then moved over the old one.
void Keep(const Choice& choice) {
    const fs::path file = File();
    fs::path tmp = file;
    tmp += ".new";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f || !(f << FormatChoice(choice)) || !f.flush()) {
            LOG_ERROR(Core, "coop: difficulty: could not write {}", tmp.string());
            return;
        }
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        LOG_ERROR(Core, "coop: difficulty: could not write {}", file.string());
    }
}

proto::Difficulty Message(const Choice& choice) {
    proto::Difficulty m;
    m.auto_scale = choice.auto_scale;
    m.preset = choice.preset;
    m.values = proto::clamp_difficulty(choice.custom);
    return m;
}

} // namespace

State Get() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    return g.state;
}

Values Now() {
    Glue& g = TheGlue();
    const Values v = UnpackValues(g.now.load());
    // (Navi) New Game+ alone: our own level on the game as it is. In a session the server's values hold the host's.
    return g.told.load() ? v : proto::ngplus_difficulty(v, CoopNgPlus::Level());
}

bool Set(const Choice& choice) {
    Glue& g = TheGlue();
    {
        std::lock_guard lock(g.mutex);
        if (!g.state.connected || !g.state.can_edit) {
            return false;
        }
        g.wish = choice;
        g.kept = choice;
        g.kept_read = true;
    }
    Keep(choice);
    return true;
}

void OnEvent(const coop::net::Event& event, coop::net::Client& client) {
    using coop::net::EventKind;
    Glue& g = TheGlue();
    if (event.kind == EventKind::welcomed || event.kind == EventKind::reconnected ||
        event.kind == EventKind::disconnected) {
        // Each connection starts with the game as it is; the server says the rest.
        std::lock_guard lock(g.mutex);
        g.state = State{};
        g.now.store(PackValues(Values{}));
        g.told.store(false);
        g.wish.reset();
        g.offered = false;
        return;
    }
    if (event.kind == EventKind::host_changed) {
        std::lock_guard lock(g.mutex);
        g.state.can_edit = client.is_host();
        return;
    }
    if (event.kind != EventKind::message) {
        return;
    }
    const auto* m = std::get_if<proto::Difficulty>(&event.message);
    if (m == nullptr) {
        return;
    }
    std::lock_guard lock(g.mutex);
    const bool first = !g.state.connected;
    // Worth a chat line: anything but the game as it is at the join, and
    // every change after it.
    const bool changed = first ? m->set_by != 0 || m->values != Values{}
                               : m->values != g.state.values ||
                                     m->auto_scale != g.state.choice.auto_scale ||
                                     m->preset != g.state.choice.preset;
    g.state.connected = true;
    g.state.can_edit = client.is_host();
    g.state.choice.auto_scale = m->auto_scale;
    g.state.choice.preset = m->preset;
    if (!m->auto_scale && m->preset == Preset::custom) {
        g.state.choice.custom = m->values;  // the host's sliders
    }
    g.state.values = m->values;
    g.state.set_by = m->set_by;
    g.now.store(PackValues(m->values));
    g.told.store(true);
    const std::size_t players = client.players().size();  // (this client included)
    if (changed) {
        g.Line("Difficulty: " + Describe(g.state.choice, m->values, players));
    }
    LOG_INFO(Core, "coop: difficulty: {} (set by {})", Describe(g.state.choice, m->values, players), m->set_by);
}

// The hits the game's Life step scaled (Kinds::Step), said at most every 5 s
// when there were new ones: the in-game proof that the values reach the game.
static void LogHits() {
    static CoopEvery::Every every{5'000'000};
    static std::uint64_t enemy_said = 0;
    static std::uint64_t link_said = 0;
    if (!every.Due(CoopEvery::NowUs())) {
        return;
    }
    // A full table (Kinds::kSlots live addresses at once: row 270, the gone ones give their slots back): what the
    // last walk could not mark goes unscaled. Said once each time it fills.
    static bool full_said = false;
    const std::uint64_t unmarked = TheKinds().Unmarked();
    if (unmarked == 0) {
        full_said = false;
    } else if (!full_said) {
        full_said = true;
        LOG_WARNING(Core, "coop: difficulty: the kinds table is full ({} of {} slots hold a live actor or Life "
                          "component): {} more are not scaled now",
                    TheKinds().Held(), Kinds::kSlots, unmarked);
    }
    // The proof that gone actors give their slots back (row 270): said at most once a minute when it changed.
    static CoopEvery::Every table_every{60'000'000};
    static std::uint64_t freed_said = 0;
    const std::uint64_t freed = TheKinds().Freed();
    if (freed != freed_said && table_every.Due(CoopEvery::NowUs())) {
        freed_said = freed;
        LOG_INFO(Core, "coop: difficulty: kinds table: {} of {} slots held, {} freed in this scene", TheKinds().Held(),
                 Kinds::kSlots, freed);
    }
    const std::uint64_t enemy = TheKinds().EnemyHits();
    const std::uint64_t link = TheKinds().LinkHits();
    static std::uint64_t boss_said = 0;
    static std::uint64_t boss_in_said = 0;
    static std::uint64_t boss_took_said = 0;
    const std::uint64_t boss = TheKinds().BossSteps();
    const std::uint64_t boss_in = TheKinds().BossDamageIn();
    const std::uint64_t boss_took = TheKinds().BossDamageTaken();
    if (enemy == enemy_said && link == link_said && boss == boss_said) {
        return;
    }
    const Values v = Now();
    LOG_INFO(Core, "coop: difficulty: the Life step scaled {} hits on enemies (life {}%) and {} on our Link (damage {}%)",
             enemy - enemy_said, v.hp_pct, link - link_said, v.damage_pct);
    if (boss != boss_said) {
        // Boss life scaling, traced apart (seed N2BTEYKM: "boss life not scaled"): a boss hit that never shows
        // here does not go through the game's Life step sum.
        LOG_INFO(Core, "coop: difficulty: bosses: {} Life steps with damage, {} damage in, {} taken (boss life {}%)",
                 boss - boss_said, boss_in - boss_in_said, boss_took - boss_took_said, v.boss_hp_pct);
    }
    enemy_said = enemy;
    link_said = link;
    boss_said = boss;
    boss_in_said = boss_in;
    boss_took_said = boss_took;
}

void Frame(coop::host::Session* session) {
    LogHits();
    if (session == nullptr) {
        return;
    }
    Glue& g = TheGlue();
    std::optional<Choice> send;
    {
        std::lock_guard lock(g.mutex);
        if (!g.state.connected) {
            return;
        }
        if (g.wish) {
            send = std::exchange(g.wish, std::nullopt);
            g.offered = true;  // the host chose here: the kept one is this one
        } else if (OffersKept(g.state.can_edit, g.state.set_by, Kept(g), g.offered)) {
            send = g.kept;
            g.offered = true;
        }
    }
    if (!send) {
        return;
    }
    const bool sent = session->with_client([&](coop::net::Client& client) {
        return client.state() == coop::net::State::connected && client.is_host() &&
               client.send(Message(*send));
    });
    if (!sent) {
        std::lock_guard lock(g.mutex);
        g.Line("Difficulty: could not be sent");
    }
}

bool OnChat(std::string_view text, coop::host::Session& session) {
    const auto command = ParseCommand(text);
    if (!command) {
        return false;
    }
    std::string line;
    std::optional<Choice> change;
    session.with_client([&](coop::net::Client& client) {
        if (client.state() != coop::net::State::connected) {
            line = "difficulty: not connected";
            return;
        }
        const State now = Get();
        if (!now.connected) {
            line = "difficulty: the server has not said it yet";
            return;
        }
        switch (command->kind) {
        case Command::Kind::show:
            // (the same line as a change's, so the chat's languages have it: coop_i18n.cpp's shapes)
            line = "Difficulty: " + Describe(now.choice, now.values, client.players().size());
            return;
        case Command::Kind::bad:
            line = "difficulty: /difficulty auto, easy, normal or hard, or /difficulty custom <life> <damage> "
                   "<enemies> (life 25-500, damage 25-400, enemies 100-300, steps of 25)";
            return;
        case Command::Kind::change:
            if (!client.is_host()) {
                line = "difficulty: only the host can change it";
                return;
            }
            change = Applied(now.choice, *command);
            return;
        }
    });
    if (change && !Set(*change)) {
        line = "difficulty: only the host can change it";
    }
    // (a change: the server's answer, DIFFICULTY to everyone, puts its line in the chat: OnEvent)
    if (!line.empty()) {
        Glue& g = TheGlue();
        std::lock_guard lock(g.mutex);
        g.Line(std::move(line));
    }
    return true;
}

std::vector<std::string> TakeLines() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    return std::exchange(g.lines, {});
}

} // namespace CoopDifficulty
