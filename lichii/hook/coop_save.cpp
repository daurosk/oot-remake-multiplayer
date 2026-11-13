// The host's save for the guests (coop_save.h; issue 167).

#include "coop_save.h"

#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"

#include "coop/host/save_transfer.hpp"
#include "coop/net/client.hpp"
#include "coop/protocol/messages.hpp"
#include "coop_every.h"

namespace CoopSave {

namespace {

namespace fs = std::filesystem;
namespace save = coop::host::save;
namespace proto = coop::protocol;

// The co-op save decision is made once, before the game opens its save.
std::once_flag g_use_once;
bool g_use = false;

std::mutex g_mutex;  // everything below; the game's file service and the network thread
fs::path g_dir;      // the folder the game opened for its account save
save::Planner g_planner;
save::Assembler g_assembler;
std::optional<save::SaveSlot> g_unwritten;  // a guest's slot that came before the game opened its save
std::deque<proto::SavePart> g_outbox;       // the host's parts, a frame each
std::uint32_t g_next_id = 0;
bool g_asked = false;  // this run's ask went out (once a run: later saves come as host_load)
bool g_said_own = false;
// Issue 106: on the clock, not presented frames (high refresh presents up to
// 2.75 times as many).
std::uint64_t g_last_part_us = 0;  // a guest's last SAVE_PART
CoopEvery::Every g_folder_every{CoopEvery::kHalfSecondUs};
bool g_said_stuck = false;
std::string g_notice;
std::function<std::string()> g_root;                    // SetSwitch (Hookshot)
std::function<bool(const std::string&)> g_remount;
bool g_in_game = false;
bool g_decided = false;   // the run-time choice is made (for this session: save::decide_again)
const void* g_decided_client = nullptr;   // the session's client it was made for
bool g_ready = false;     // the host's save written into our co-op folder

// Unset, or "1"/"0": the environment's LICHII_COOP_HOST_SAVE.
std::optional<bool> EnvChoice() {
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, "LICHII_COOP_HOST_SAVE") != 0 || value == nullptr) {
        return std::nullopt;
    }
    const std::string v(value);
    std::free(value);
#else
    const char* raw = std::getenv("LICHII_COOP_HOST_SAVE");
    if (raw == nullptr) {
        return std::nullopt;
    }
    const std::string v(raw);
#endif
    if (v.empty()) {
        return std::nullopt;
    }
    return v != "0";
}

// Where the folder is fixed at the start (the emulator): the environment only (save::folder_choice).
void Decide() {
    const auto env = EnvChoice();
    g_use = save::folder_choice(env, false, false, false, false) == save::FolderChoice::coop;
    LOG_INFO(Core, "coop: save: {}", env ? (g_use ? "LICHII_COOP_HOST_SAVE=1 (a join): this game uses the co-op "
                                                   "save folder (the host's save)"
                                                 : "LICHII_COOP_HOST_SAVE=0 (a host): this game uses its own saves")
                                         : "this game uses its own saves (no LICHII_COOP_HOST_SAVE)");
}

void NoteSaveDirLocked(const fs::path& dir);

// Under g_mutex, each frame where the folder can be switched (Hookshot): the folder as mounted, and the choice.
void SwitchFrame(coop::net::Client& client) {
    if (!g_root || !g_remount) return;
    if (g_dir.empty()) {
        if (const auto root = g_root(); !root.empty()) NoteSaveDirLocked(fs::path(root));
    }
    {
        const auto st = client.state();
        const bool over = &client != g_decided_client || st == coop::net::State::idle ||
                          st == coop::net::State::stopped || st == coop::net::State::refused;
        if (save::decide_again(g_decided, over, g_in_game)) {
            g_decided = false;   // the next session (the title menu's Host after a Join) decides again
            g_ready = false;
            LOG_INFO(Core, "coop: save: the session is over: the next one chooses the save folder again");
        }
    }
    if (g_decided || g_dir.empty()) return;
    const bool connected = client.state() == coop::net::State::connected && client.host_number() != 0;
    const auto choice = save::folder_choice(std::nullopt, true, connected, connected && client.is_host(), g_in_game);
    if (choice == save::FolderChoice::wait) return;
    g_decided = true;
    g_decided_client = &client;
    const bool coop_now = save::is_coop_dir(g_dir);
    const fs::path want = choice == save::FolderChoice::coop
                              ? (coop_now ? g_dir : fs::path(g_dir.string() + std::string(save::kCoopSuffix)))
                              : (coop_now ? save::own_dir_of(g_dir) : g_dir);
    if (choice == save::FolderChoice::coop) {
        // Empty until the host's save comes: an older host's save there must not be the one loaded.
        if (const int n = save::clear_coop_slots(want); n > 0) {
            LOG_INFO(Core, "coop: save: {} older slot(s) taken out of {}", n, want.string());
        }
    }
    if (want != g_dir) {
        if (!g_remount(want.string())) {
            LOG_WARNING(Core, "coop: save: could not switch the save folder to {}: this game keeps {}",
                        want.string(), g_dir.string());
            return;
        }
        NoteSaveDirLocked(want);
    }
    g_use = choice == save::FolderChoice::coop;
    LOG_INFO(Core, "coop: save: {}", g_use ? "a guest: this game plays from the co-op save folder (the host's save) "
                                             "in " + g_dir.string()
                                           : (connected && client.is_host()
                                                  ? std::string("the host: this game uses its own saves")
                                                  : std::string("a save is loaded already: this game keeps its own saves")));
}

// Under g_mutex.
void WriteSlot(const save::SaveSlot& slot) {
    if (!save::is_coop_dir(g_dir)) {
        if (!g_said_own) {
            g_said_own = true;
            LOG_INFO(Core, "coop: save: the host's save not written: this game uses its own saves ({})",
                     g_dir.string());
        }
        return;
    }
    const auto own = save::own_dir_of(g_dir);
    switch (save::write_coop_save(g_dir, slot, own)) {
    case save::WriteResult::written:
        g_ready = true;
        LOG_INFO(Core, "coop: save: the host's save written to {}", g_dir.string());
        g_notice = g_assembler.reason() == proto::SaveReason::host_load
                       ? "The host loaded a save. Load it from your co-op save to stand where it starts "
                         "(your progress already follows the host)."
                       : "The host's save is ready: load it (Continue, or Load Game, newest save).";
        break;
    case save::WriteResult::not_coop:
        LOG_WARNING(Core, "coop: save: refused to write the host's save into {}", g_dir.string());
        break;
    case save::WriteResult::failed:
        LOG_ERROR(Core, "coop: save: could not write the host's save into {}", g_dir.string());
        g_notice = "The host's save could not be written (see the log).";
        break;
    }
}

}  // namespace

bool UseCoopFolder() {
    std::call_once(g_use_once, Decide);
    return g_use;
}

void SetSwitch(std::function<std::string()> root, std::function<bool(const std::string&)> remount) {
    std::call_once(g_use_once, [] {});   // (the start-up decision is not used where the folder can be switched)
    std::lock_guard lock(g_mutex);
    g_root = std::move(root);
    g_remount = std::move(remount);
    g_use = false;
}

void NoteInGame(bool in_game) {
    std::lock_guard lock(g_mutex);
    g_in_game = in_game;
}

bool HostSaveReady() {
    std::lock_guard lock(g_mutex);
    return g_ready;
}

std::string SaveDirectory(const std::string& path, bool account_save) {
    if (!account_save || !UseCoopFolder() || path.empty()) {
        return path;
    }
    std::string out = path;
    while (!out.empty() && (out.back() == '/' || out.back() == '\\')) {
        out.pop_back();
    }
    if (save::is_coop_dir(out)) {
        return out;
    }
    return out + std::string(save::kCoopSuffix);
}

void NoteSaveDir(const std::string& real_path) {
    std::lock_guard lock(g_mutex);
    NoteSaveDirLocked(fs::path(real_path));
}

namespace {
void NoteSaveDirLocked(const fs::path& real_path) {
    fs::path dir = real_path.lexically_normal();
    if (!dir.has_filename()) {
        dir = dir.parent_path();
    }
    if (dir == g_dir) {
        return;
    }
    g_dir = dir;
    LOG_INFO(Core, "coop: save: the game's save folder is {}", g_dir.string());
    if (g_unwritten && save::is_coop_dir(g_dir)) {
        WriteSlot(*g_unwritten);
        g_unwritten.reset();
    }
}
}  // namespace

void OnEvent(const coop::net::Event& event, coop::net::Client& client) {
    using coop::net::EventKind;
    std::lock_guard lock(g_mutex);
    if (event.kind == EventKind::player_left) {
        g_planner.forget(event.player);
        return;
    }
    if (event.kind != EventKind::message) {
        return;
    }
    if (const auto* ask = std::get_if<proto::SaveAsk>(&event.message)) {
        if (client.is_host()) {
            if (g_planner.on_ask(ask->player_number)) {
                LOG_INFO(Core, "coop: save: player {} asked for our save", ask->player_number);
            } else {
                LOG_WARNING(Core, "coop: save: player {} asked for our save again: ignored (once a "
                                  "player)", ask->player_number);
            }
        }
        return;
    }
    const auto* part = std::get_if<proto::SavePart>(&event.message);
    if (part == nullptr) {
        return;
    }
    g_last_part_us = CoopEvery::NowUs();
    g_said_stuck = false;
    switch (g_assembler.add(*part, client.host_number())) {
    case save::Assembler::Result::partial:
        return;
    case save::Assembler::Result::refused:
        LOG_WARNING(Core, "coop: save: a save part refused (from player {}, transfer {}, part {} of {})",
                    part->player_number, part->transfer_id, part->index + 1, part->count);
        return;
    case save::Assembler::Result::bad_crc:
        LOG_WARNING(Core, "coop: save: the host's save (transfer {}) came damaged (CRC); waiting for "
                          "the next one", part->transfer_id);
        g_notice = "The host's save came damaged; it is sent again after the host's next save.";
        return;
    case save::Assembler::Result::complete:
        break;
    }
    const auto packed = g_assembler.take();
    const auto slot = save::unpack(packed);
    if (!slot) {
        LOG_WARNING(Core, "coop: save: the host's save (transfer {}) is not a save slot", part->transfer_id);
        return;
    }
    LOG_INFO(Core, "coop: save: the host's save is in ({} bytes, {}, transfer {})", packed.size(),
             part->reason == proto::SaveReason::host_load ? "the host loaded a save" : "asked on joining",
             part->transfer_id);
    if (g_dir.empty()) {
        g_unwritten = *slot;  // written when the game opens its save folder
        return;
    }
    WriteSlot(*slot);
}

void OnFrame(coop::net::Client& client, bool host_reset) {
    std::lock_guard lock(g_mutex);
    SwitchFrame(client);
    if (client.state() != coop::net::State::connected || client.host_number() == 0) {
        g_outbox.clear();
        return;
    }
    if (!client.is_host()) {
        g_outbox.clear();
        // A transfer that stopped part way (a part lost): said once; the next
        // save the host sends starts again.
        if (!g_said_stuck && g_assembler.received() != 0 &&
            g_assembler.received() < g_assembler.count() &&
            CoopEvery::NowUs() - g_last_part_us > CoopEvery::kSaveStuckUs) {
            g_said_stuck = true;
            LOG_WARNING(Core, "coop: save: the host's save (transfer {}) stopped at part {} of {} "
                              "for 10 s; waiting for the next one",
                        g_assembler.transfer_id(), g_assembler.received(), g_assembler.count());
        }
        if (!g_asked && (g_root ? g_use : UseCoopFolder())) {
            g_asked = client.send(proto::Message{proto::SaveAsk{}});
            if (g_asked) {
                LOG_INFO(Core, "coop: save: asked the host (player {}) for its save", client.host_number());
            }
        }
        return;
    }
    if (host_reset) {
        g_planner.on_host_load(fs::file_time_type::clock::now());
        LOG_INFO(Core, "coop: save: we loaded a save: the guests get it after the game's next save "
                       "(a door or a save point)");
    }
    // Twice a second: what the host owes (it reads the folder's file times).
    if (!g_dir.empty() && !g_planner.idle() && g_folder_every.Due(CoopEvery::NowUs())) {
        for (const auto& send : g_planner.due(save::newest_write(g_dir))) {
            const auto slot_dir = save::newest_slot(g_dir);
            const auto slot = slot_dir ? save::read_slot(*slot_dir) : std::nullopt;
            const auto packed = slot ? save::pack(*slot) : std::vector<std::uint8_t>{};
            if (packed.empty()) {
                LOG_WARNING(Core, "coop: save: could not read our newest save in {}", g_dir.string());
                continue;
            }
            if (g_next_id == 0) {
                g_next_id = static_cast<std::uint32_t>(
                    std::chrono::steady_clock::now().time_since_epoch().count()) | 1u;
            }
            auto parts = save::split(packed, g_next_id++, send.reason, send.to_player);
            if (g_outbox.size() + parts.size() > save::kMaxOutboxParts) {
                LOG_WARNING(Core, "coop: save: {} parts already waiting: this send dropped",
                            g_outbox.size());
                continue;
            }
            LOG_INFO(Core, "coop: save: sending {} ({} bytes, {} parts) to {}",
                     slot_dir->filename().string(), packed.size(), parts.size(),
                     send.to_player == 0 ? std::string("every guest")
                                         : fmt::format("player {}", send.to_player));
            for (auto& p : parts) {
                g_outbox.push_back(std::move(p));
            }
        }
    }
    if (!g_outbox.empty()) {
        if (client.send(proto::Message{g_outbox.front()})) {
            g_outbox.pop_front();
        }
    }
}

std::string TakeNotice() {
    std::lock_guard lock(g_mutex);
    return std::exchange(g_notice, {});
}

}  // namespace CoopSave
