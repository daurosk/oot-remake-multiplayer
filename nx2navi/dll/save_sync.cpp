// SPDX-License-Identifier: GPL-3.0-or-later
#include "save_sync.hpp"
#include "settings.hpp"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <set>
#include <variant>
#include <vector>

#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop/protocol/messages.hpp"
#include "mp_saves.hpp"

namespace CoopNx2navi::SaveSync {
namespace {

namespace fs = std::filesystem;
namespace save = coop::host::save;
namespace proto = coop::protocol;

std::atomic<void (*)(const char*)> g_log{nullptr};

#if defined(__GNUC__)
__attribute__((format(gnu_printf, 1, 2)))
#endif
void Log(const char* format, ...) {
    auto* log = g_log.load();
    if (log == nullptr) {
        return;
    }
    char line[768];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    log(line);
}

std::string Shown(const fs::path& path) {
    const std::u8string u = path.u8string();
    return std::string(u.begin(), u.end());
}

// --- The host ---

std::mutex g_mutex;  // everything below; taken inside the session's lock
save::Planner g_planner;
std::deque<proto::SavePart> g_outbox;
std::uint32_t g_next_id = 0;
unsigned g_frames = 0;
bool g_said_no_dir = false;
bool g_said_no_save = false;
std::set<std::uint8_t> g_fresh_asks;  // asks not answered yet: for "a new game" when we have no save

constexpr std::string_view kNameMagic = "OOTSNAM1";
constexpr std::string_view kNewMagic = "OOTSNEW1";

std::vector<std::uint8_t> Packet(std::string_view magic, std::string_view name) {
    std::vector<std::uint8_t> out(magic.begin(), magic.end());
    out.insert(out.end(), name.begin(), name.end());
    return out;
}

std::optional<std::string> PacketName(std::string_view magic, std::span<const std::uint8_t> packet) {
    if (packet.size() < magic.size() ||
        !std::equal(magic.begin(), magic.end(), packet.begin(),
                    [](char a, std::uint8_t b) { return static_cast<std::uint8_t>(a) == b; })) {
        return std::nullopt;
    }
    std::string name(packet.begin() + static_cast<std::ptrdiff_t>(magic.size()), packet.end());
    // What a host says is not trusted: only a name a save can have is one.
    return MpSaves::ValidName(name) ? name : std::string();
}

}  // namespace

void SetLog(void (*log)(const char* line)) { g_log = log; }

std::vector<std::uint8_t> NamePacket(std::string_view name) { return Packet(kNameMagic, name); }
std::optional<std::string> NameOf(std::span<const std::uint8_t> packet) {
    return PacketName(kNameMagic, packet);
}
std::vector<std::uint8_t> NewGamePacket(std::string_view name) { return Packet(kNewMagic, name); }
std::optional<std::string> NewGameOf(std::span<const std::uint8_t> packet) {
    return PacketName(kNewMagic, packet);
}

void OnEvent(const coop::net::Event& event, coop::net::Client& client) {
    using coop::net::EventKind;
    std::lock_guard lock(g_mutex);
    if (event.kind == EventKind::player_left) {
        g_planner.forget(event.player);
        g_fresh_asks.erase(event.player);
        return;
    }
    if (event.kind != EventKind::message) {
        return;
    }
    if (const auto* ask = std::get_if<proto::SaveAsk>(&event.message)) {
        if (!client.is_host()) {
            return;
        }
        if (g_planner.on_ask(ask->player_number)) {
            Log("save: player %u asked for our save", static_cast<unsigned>(ask->player_number));
            g_said_no_save = false;
            g_fresh_asks.insert(ask->player_number);
        } else {
            Log("save: player %u asked for our save again: ignored (once a player)",
                static_cast<unsigned>(ask->player_number));
        }
    }
}

void Frame(coop::host::Session& session, bool connected, const fs::path& save_dir) {
    session.with_client([&](coop::net::Client& client) {
        std::lock_guard lock(g_mutex);
        ++g_frames;
        if (!connected || !client.is_host()) {
            g_outbox.clear();
            return;
        }
        // Twice a second: what we owe (it reads the folder's file times).
        if (!g_planner.idle() && g_frames % 30 == 0) {
            if (save_dir.empty()) {
                if (!g_said_no_dir) {
                    g_said_no_dir = true;
                    Log("save: a player asked for our save, but this game does not tell its save "
                        "folder (plugin interface version 2 gives it): nothing is sent");
                }
                return;
            }
            const auto newest = save::newest_write(save_dir);
            if (!newest && !g_said_no_save) {
                g_said_no_save = true;
                Log("save: a player asked for our save, but there is none yet in %s",
                    Shown(save_dir).c_str());
            }
            if (g_next_id == 0) {
                g_next_id = static_cast<std::uint32_t>(
                                std::chrono::steady_clock::now().time_since_epoch().count()) |
                            1u;
            }
            // No save yet on a multiplayer save: we are beginning a new game on
            // it. The guest is told so (and the save's name), and begins one too
            // on an empty save folder of its own, instead of playing on its own
            // saves while it waits for ours.
            if (!newest) {
                const std::string name = MpSaves::NameOfSaveDir(save_dir);
                if (!name.empty()) {
                    for (const std::uint8_t player : g_fresh_asks) {
                        for (auto& p : save::split(NewGamePacket(name), g_next_id++,
                                                   proto::SaveReason::join, player)) {
                            g_outbox.push_back(std::move(p));
                        }
                        g_planner.forget(player);  // answered: nothing is owed to it
                        Log("save: player %u is told that this save, '%s', has nothing in it yet: "
                            "a new game to begin",
                            static_cast<unsigned>(player), name.c_str());
                    }
                    g_fresh_asks.clear();
                }
            }
            for (const auto& send : g_planner.due(newest)) {
                const auto slot_dir = save::newest_slot(save_dir);
                const auto slot = slot_dir ? save::read_slot(*slot_dir) : std::nullopt;
                const auto packed = slot ? save::pack(*slot) : std::vector<std::uint8_t>{};
                if (packed.empty()) {
                    Log("save: could not read our newest save in %s", Shown(save_dir).c_str());
                    continue;
                }
                g_fresh_asks.erase(send.to_player);
                auto parts = save::split(packed, g_next_id++, send.reason, send.to_player);
                if (g_outbox.size() + parts.size() > save::kMaxOutboxParts) {
                    Log("save: %zu parts already waiting: this send dropped", g_outbox.size());
                    continue;
                }
                Log("save: sending %s (%zu bytes, %zu parts) to player %u",
                    Shown(slot_dir->filename()).c_str(), packed.size(), parts.size(),
                    static_cast<unsigned>(send.to_player));
                for (auto& p : parts) {
                    g_outbox.push_back(std::move(p));
                }
                // The name of our multiplayer save, after the save: the guest
                // keeps its copy under it. A transfer of its own, so a guest
                // that does not know it (it has left once the save is in) and
                // the save's own packing are as they were.
                const std::string name = MpSaves::NameOfSaveDir(save_dir);
                if (!name.empty() && send.to_player != 0) {
                    for (auto& p : save::split(NamePacket(name), g_next_id++, send.reason, send.to_player)) {
                        g_outbox.push_back(std::move(p));
                    }
                    Log("save: player %u is told this save's name, '%s'",
                        static_cast<unsigned>(send.to_player), name.c_str());
                }
            }
        }
        if (!g_outbox.empty() && client.send(proto::Message{g_outbox.front()})) {
            g_outbox.pop_front();
        }
    });
}

// --- The joiner ---

Fetched Fetch(const FetchOptions& options, const std::atomic<bool>& stop,
              const std::function<void(bool welcomed)>& progress) {
    using Clock = std::chrono::steady_clock;
    using coop::net::EventKind;
    Fetched out;
    coop::net::ClientConfig config;
    config.host = options.server;
    config.port = options.port;
    config.hello.build_id = options.build_id;
    config.hello.game_build_id = options.game_build_id;
    config.hello.game_version = options.game_version;
    config.hello.nickname = options.name;
    config.join_password = Settings::JoinPassword(options.password);
    coop::net::Client client(std::move(config));
    if (!client.start()) {
        Log("save: ENet did not start");
        return out;
    }
    Log("save: asking the host at %s:%u for its save, as '%s'", options.server.c_str(),
        static_cast<unsigned>(options.port), options.name.c_str());
    save::Assembler assembler;
    const auto started = Clock::now();
    std::optional<Clock::time_point> welcomed;
    std::optional<Clock::time_point> asked;
    std::optional<Clock::time_point> name_until;  // the save is in: its name may follow
    bool done = false;
    while (!done) {
        if (stop) {
            out.result = FetchResult::stopped;
            break;
        }
        client.service(20);
        while (const auto event = client.next_event()) {
            if (event->kind == EventKind::welcomed || event->kind == EventKind::reconnected) {
                if (!welcomed) {
                    welcomed = Clock::now();
                    Log("save: welcomed as player %u",
                        static_cast<unsigned>(event->welcome.player_number));
                    if (progress) {
                        progress(true);
                    }
                }
            } else if (event->kind == EventKind::refused) {
                out.result = FetchResult::refused;
                out.reason = std::string(coop::net::refuse_reason_text(event->refuse_reason));
                Log("save: the server refused: %s", out.reason.c_str());
                done = true;
            } else if (event->kind == EventKind::message) {
                const auto* part = std::get_if<proto::SavePart>(&event->message);
                if (part == nullptr) {
                    continue;
                }
                switch (assembler.add(*part, client.host_number())) {
                case save::Assembler::Result::complete: {
                    const auto packed = assembler.take();
                    const auto host_name = [&client]() {
                        for (const auto& player : client.players()) {
                            if (player.number == client.host_number()) {
                                return player.nickname;
                            }
                        }
                        return std::string();
                    };
                    if (const auto fresh = NewGameOf(packed)) {
                        if (!out.slot) {
                            out.result = FetchResult::new_game;
                            out.save_name = *fresh;
                            out.host = client.host_number();
                            out.host_name = host_name();
                            Log("save: the host is beginning a new game on its save '%s': nothing to "
                                "take, a new game here too",
                                out.save_name.c_str());
                            done = true;
                        }
                        break;
                    }
                    if (const auto name = NameOf(packed)) {
                        if (out.slot) {
                            out.save_name = *name;
                            Log("save: the host's save is named '%s'", out.save_name.c_str());
                            done = true;
                        }
                        break;
                    }
                    out.slot = save::unpack(packed);
                    if (out.slot) {
                        out.result = FetchResult::fetched;
                        out.host = client.host_number();
                        out.host_name = host_name();
                        Log("save: the host's save is in (%zu bytes, %u parts)", packed.size(),
                            static_cast<unsigned>(part->count));
                        // Its name follows at once from a host that has one to tell.
                        name_until = Clock::now() + std::chrono::milliseconds(options.name_ms);
                    } else {
                        out.result = FetchResult::no_save;
                        Log("save: what the host sent is not a save slot");
                        done = true;
                    }
                    break;
                }
                case save::Assembler::Result::bad_crc:
                    out.result = FetchResult::no_save;
                    Log("save: the host's save came damaged (CRC)");
                    done = true;
                    break;
                case save::Assembler::Result::refused:
                    Log("save: a save part refused (from player %u)",
                        static_cast<unsigned>(part->player_number));
                    break;
                default: break;
                }
            }
        }
        if (done) {
            break;
        }
        const auto now = Clock::now();
        if (name_until) {
            if (now >= *name_until) {
                Log("save: the host told no name for its save (an older build, or not a "
                    "multiplayer save)");
                break;
            }
            continue;
        }
        if (!welcomed) {
            if (now - started > std::chrono::milliseconds(options.connect_ms)) {
                out.result = FetchResult::no_answer;
                Log("save: no answer from %s:%u in %u s", options.server.c_str(),
                    static_cast<unsigned>(options.port), options.connect_ms / 1000);
                break;
            }
            continue;
        }
        if (!asked) {
            if (client.host_number() != 0) {
                if (client.is_host()) {
                    out.result = FetchResult::no_save;
                    Log("save: we are the first on that server, so nobody has a save to give");
                    break;
                }
                if (client.send(proto::Message{proto::SaveAsk{}})) {
                    asked = now;
                    Log("save: asked the host (player %u)", static_cast<unsigned>(client.host_number()));
                }
            } else if (now - *welcomed > std::chrono::milliseconds(5000)) {
                out.result = FetchResult::no_save;
                Log("save: the server named no host in 5 s");
                break;
            }
        } else if (now - *asked > std::chrono::milliseconds(options.save_ms)) {
            out.result = FetchResult::no_save;
            Log("save: no save from the host in %u s (it has none yet, or its game cannot send "
                "one); %zu of %zu parts came",
                options.save_ms / 1000, assembler.received(), assembler.count());
            break;
        }
    }
    // The goodbye, so that the name is free again at once for the session.
    client.stop();
    for (int i = 0; i < 10; ++i) {
        client.service(10);
    }
    return out;
}

namespace {

bool WriteWhole(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out.good()) {
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool Same(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::exists(a, ec) && fs::exists(b, ec) && fs::equivalent(a, b, ec)) {
        return true;
    }
    return fs::weakly_canonical(a, ec).lexically_normal() == fs::weakly_canonical(b, ec).lexically_normal();
}

// `inner` is `outer` or lies in it.
bool Inside(const fs::path& inner, const fs::path& outer) {
    std::error_code ec;
    const fs::path in = fs::weakly_canonical(inner, ec).lexically_normal();
    const fs::path out = fs::weakly_canonical(outer, ec).lexically_normal();
    const auto rel = in.lexically_relative(out);
    return !rel.empty() && *rel.begin() != "..";
}

}  // namespace

namespace {

// The folder made ready for a slot: the checks, an earlier save kept once, the
// player's own settings copied. `target`: save\<account> in it.
bool PrepareCoopProfile(const fs::path& own_profile, const fs::path& own_save_dir,
                        const fs::path& coop_profile, fs::path& target, std::string& error) {
    std::error_code ec;
    if (own_profile.empty() || own_save_dir.empty() || coop_profile.empty()) {
        error = "the game's profile or save folder is not known";
        return false;
    }
    // Never the player's own saves: the co-op profile is another folder, and
    // neither lies inside the other's save.
    if (Same(own_profile, coop_profile) || Inside(coop_profile, own_save_dir) ||
        Inside(own_save_dir, coop_profile)) {
        error = "the co-op profile would be the player's own (" + Shown(coop_profile) + ")";
        return false;
    }
    const fs::path account = own_save_dir.filename();
    if (account.empty()) {
        error = "the save folder has no account name (" + Shown(own_save_dir) + ")";
        return false;
    }
    target = coop_profile / "save" / account;
    fs::create_directories(coop_profile / "save", ec);
    if (ec) {
        error = "cannot make " + Shown(coop_profile / "save");
        return false;
    }
    // An earlier session's co-op save is kept once.
    if (fs::exists(target, ec)) {
        const fs::path previous = coop_profile / "save-previous";
        fs::remove_all(previous, ec);
        fs::rename(target, previous, ec);
        if (ec) {
            fs::remove_all(target, ec);
            if (ec) {
                error = "cannot replace the earlier co-op save in " + Shown(target);
                return false;
            }
        }
    }
    // A finished save of an earlier run that the game had not swapped in yet
    // would win over ours at the next start (NX2navi's save-tx/ready-<n>).
    fs::remove_all(coop_profile / "save-tx", ec);
    fs::create_directories(target, ec);
    if (ec) {
        error = "cannot make " + Shown(target);
        return false;
    }
    // The player's own account and settings, never the host's.
    const auto copy = [&](const fs::path& from, const fs::path& to) {
        std::error_code cec;
        if (fs::is_regular_file(from, cec)) {
            fs::copy_file(from, to, fs::copy_options::overwrite_existing, cec);
        }
    };
    copy(own_profile / "account.uid", coop_profile / "account.uid");
    copy(own_save_dir / "account.sav", target / "account.sav");
    copy(own_save_dir / "option.sav", target / "option.sav");
    return true;
}

}  // namespace

bool WriteEmptyCoopProfile(const fs::path& own_profile, const fs::path& own_save_dir,
                           const fs::path& coop_profile, std::string& error) {
    fs::path target;
    return PrepareCoopProfile(own_profile, own_save_dir, coop_profile, target, error);
}

bool WriteCoopProfile(const fs::path& own_profile, const fs::path& own_save_dir,
                      const fs::path& coop_profile, const save::SaveSlot& slot, std::string& error) {
    if (slot.files.empty() || slot.files.front().name != save::kSlotFiles[0]) {
        error = "the host's slot has no progress.sav";
        return false;
    }
    fs::path target;
    if (!PrepareCoopProfile(own_profile, own_save_dir, coop_profile, target, error)) {
        return false;
    }
    std::error_code ec;
    const fs::path slot_dir = target / "slot_00";
    fs::create_directories(slot_dir, ec);
    if (ec) {
        error = "cannot make " + Shown(slot_dir);
        return false;
    }
    for (const save::SaveFile& file : slot.files) {
        if (!save::slot_file(file.name)) {
            continue;
        }
        if (!WriteWhole(slot_dir / file.name, file.bytes)) {
            error = "cannot write " + Shown(slot_dir / file.name);
            return false;
        }
    }
    return true;
}

}  // namespace CoopNx2navi::SaveSync
