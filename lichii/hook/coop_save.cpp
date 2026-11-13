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
// This session's guest save state (save::GuestSession): fresh for each session (decide_again).
save::GuestSession g_guest;
std::deque<proto::SavePart> g_outbox;       // the host's parts, a frame each
std::uint32_t g_next_id = 0;
bool g_said_own = false;
// Issue 106: on the clock, not presented frames (high refresh presents up to
// 2.75 times as many).
CoopEvery::Every g_folder_every{CoopEvery::kHalfSecondUs};
std::string g_notice;
std::function<std::string()> g_root;                    // SetSwitch (Hookshot)
std::function<bool(const std::string&)> g_remount;
bool g_in_game = false;
bool g_decided = false;   // the run-time choice is made (for this session: save::decide_again)
// Hookshot, a guest: the co-op folder it moves to once the host's save is written there. Until then the game stays
// on its own folder, so the title never offers an empty or half-written co-op folder (that crashed the game).
fs::path g_pending;
bool g_title_up = false;   // the title's menu is up (NoteTitle, Hookshot's title module)
const void* g_decided_client = nullptr;   // the session's client it was made for

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
save::WriteResult WriteSlotTo(const fs::path& dir, const save::SaveSlot& slot);
void PendingFrame();

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
            g_decided = false;   // the next session (the title menu's Host after a Join, a rejoin) decides again
            g_guest = save::GuestSession{};   // and asks for the host's save again: everything of the last one goes
            g_pending.clear();
            LOG_INFO(Core, "coop: save: the session is over: the next one chooses the save folder again");
        }
    }
    if (!g_decided && !g_dir.empty()) {
        const bool connected = client.state() == coop::net::State::connected && client.host_number() != 0;
        const auto choice =
            save::folder_choice(std::nullopt, true, connected, connected && client.is_host(), g_in_game);
        if (choice == save::FolderChoice::wait) return;
        g_decided = true;
        g_decided_client = &client;
        // Both sides start on the game's own folder: a guest moves to the co-op one only once the host's save is
        // whole in it (PendingFrame), never onto an empty one.
        const fs::path own = save::is_coop_dir(g_dir) ? save::own_dir_of(g_dir) : g_dir;
        if (own != g_dir) {
            if (!g_remount(own.string())) {
                LOG_WARNING(Core, "coop: save: could not switch the save folder back to {}: this game keeps {}",
                            own.string(), g_dir.string());
                g_use = false;
                return;
            }
            NoteSaveDirLocked(own);
        }
        g_use = choice == save::FolderChoice::coop;
        if (g_use) {
            g_pending = save::coop_dir_of(own);
            // Empty until the host's save comes: an older host's save there must not be the one loaded.
            if (const int n = save::clear_coop_slots(g_pending); n > 0) {
                LOG_INFO(Core, "coop: save: {} older slot(s) taken out of {}", n, g_pending.string());
            }
            LOG_INFO(Core, "coop: save: a guest: this game plays from the co-op save folder (the host's save) in {} "
                           "once the host's save is written there; until then it stays on {}",
                     g_pending.string(), g_dir.string());
        } else {
            LOG_INFO(Core, "coop: save: {}", connected && client.is_host()
                                                 ? "the host: this game uses its own saves"
                                                 : "a save is loaded already: this game keeps its own saves");
        }
    }
    PendingFrame();
}

// Under g_mutex, each frame (Hookshot): the host's save that came is written whole into the pending co-op folder,
// and only then does the game move to it. Link already in the game (the player chose their own file or a new game
// meanwhile): no switch, the save stays unused.
void PendingFrame() {
    if (g_pending.empty()) return;
    switch (save::pending_step(g_guest.unwritten.has_value(), g_title_up, g_in_game)) {
    case save::PendingStep::keep:
        return;
    case save::PendingStep::drop:
        LOG_INFO(Core, "coop: save: a file is loaded: this game keeps it; the host's save is not used");
        g_pending.clear();
        g_guest.unwritten.reset();
        return;
    case save::PendingStep::go:
        break;
    }
    auto slot = std::move(*g_guest.unwritten);
    g_guest.unwritten.reset();
    const fs::path to = std::exchange(g_pending, {});
    if (WriteSlotTo(to, slot) != save::WriteResult::written) return;
    if (!g_remount(to.string())) {
        LOG_WARNING(Core, "coop: save: could not switch the save folder to {}: this game keeps {}", to.string(),
                    g_dir.string());
        g_guest.ready = false;
        g_notice = "The host's save came, but this game could not switch to it (see the log).";
        return;
    }
    NoteSaveDirLocked(to);
    LOG_INFO(Core, "coop: save: now playing from the co-op save folder {}", to.string());
}

// Under g_mutex: the host's slot written whole into the co-op folder `dir`.
save::WriteResult WriteSlotTo(const fs::path& dir, const save::SaveSlot& slot) {
    const auto result = save::write_coop_save(dir, slot, save::own_dir_of(dir));
    switch (result) {
    case save::WriteResult::written:
        g_guest.ready = true;
        LOG_INFO(Core, "coop: save: the host's save written to {}", dir.string());
        g_notice = g_guest.assembler.reason() == proto::SaveReason::host_load
                       ? "The host loaded a save. Load it from your co-op save to stand where it starts "
                         "(your progress already follows the host)."
                       : "The host's save is ready: load it (Continue, or Load Game, newest save).";
        break;
    case save::WriteResult::not_coop:
        LOG_WARNING(Core, "coop: save: refused to write the host's save into {}", dir.string());
        break;
    case save::WriteResult::failed:
        LOG_ERROR(Core, "coop: save: could not write the host's save into {}", dir.string());
        g_notice = "The host's save could not be written (see the log).";
        break;
    }
    return result;
}

// Under g_mutex: the host's slot for the folder the game has open (the emulator's fixed co-op folder).
void WriteSlot(const save::SaveSlot& slot) {
    if (!save::is_coop_dir(g_dir)) {
        if (!g_said_own) {
            g_said_own = true;
            LOG_INFO(Core, "coop: save: the host's save not written: this game uses its own saves ({})",
                     g_dir.string());
        }
        return;
    }
    WriteSlotTo(g_dir, slot);
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
    return g_guest.ready;
}

bool HostHasNoSave() {
    std::lock_guard lock(g_mutex);
    return g_guest.no_save;
}

void NoteTitle(bool title_up) {
    std::lock_guard lock(g_mutex);
    g_title_up = title_up;
}

void CancelPendingSwitch(std::string_view why) {
    std::lock_guard lock(g_mutex);
    if (g_pending.empty()) return;
    g_pending.clear();
    g_guest.unwritten.reset();
    LOG_INFO(Core, "coop: save: {}: this game stays on its own saves; the host's save is not used", why);
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
    if (g_guest.unwritten && save::is_coop_dir(g_dir)) {
        WriteSlot(*g_guest.unwritten);
        g_guest.unwritten.reset();
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
    g_guest.last_part_us = CoopEvery::NowUs();
    g_guest.said_stuck = false;
    switch (g_guest.assembler.add(*part, client.host_number())) {
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
    const auto packed = g_guest.assembler.take();
    if (save::is_no_save(packed)) {
        LOG_INFO(Core, "coop: save: the host has no save yet (a new game): this game keeps its own saves");
        g_guest.no_save = true;
        g_pending.clear();   // (nothing to switch to)
        g_notice = "The host has no save yet: New Game starts one, Continue plays your own.";
        return;
    }
    const auto slot = save::unpack(packed);
    if (!slot) {
        LOG_WARNING(Core, "coop: save: the host's save (transfer {}) is not a save slot", part->transfer_id);
        return;
    }
    LOG_INFO(Core, "coop: save: the host's save is in ({} bytes, {}, transfer {})", packed.size(),
             part->reason == proto::SaveReason::host_load ? "the host loaded a save" : "asked on joining",
             part->transfer_id);
    if (g_dir.empty() || !g_pending.empty()) {
        // Written when the game opens its save folder, or (Hookshot) into the co-op folder at the next frame and
        // only then switched to (PendingFrame).
        g_guest.unwritten = *slot;
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
        if (!g_guest.said_stuck && g_guest.assembler.received() != 0 &&
            g_guest.assembler.received() < g_guest.assembler.count() &&
            CoopEvery::NowUs() - g_guest.last_part_us > CoopEvery::kSaveStuckUs) {
            g_guest.said_stuck = true;
            LOG_WARNING(Core, "coop: save: the host's save (transfer {}) stopped at part {} of {} "
                              "for 10 s; waiting for the next one",
                        g_guest.assembler.transfer_id(), g_guest.assembler.received(), g_guest.assembler.count());
        }
        if (!g_guest.asked && (g_root ? g_use : UseCoopFolder())) {
            g_guest.asked = client.send(proto::Message{proto::SaveAsk{}});
            if (g_guest.asked) {
                LOG_INFO(Core, "coop: save: asked the host (player {}) for its save", client.host_number());
            }
        }
        return;
    }
    if (host_reset) {
        g_planner.on_host_load(fs::file_time_type::clock::now());
        LOG_INFO(Core, "coop: save: we loaded a save: a guest that asks gets our newest save at once, and every "
                       "guest gets this one after the game's next save (a door or a save point)");
    }
    // Twice a second: what the host owes (it reads the folder's file times).
    if (!g_dir.empty() && !g_planner.idle() && g_folder_every.Due(CoopEvery::NowUs())) {
        for (const auto& send : g_planner.due(save::newest_write(g_dir))) {
            const auto slot_dir = send.no_save ? std::nullopt : save::newest_slot(g_dir);
            const auto slot = slot_dir ? save::read_slot(*slot_dir) : std::nullopt;
            const auto packed =
                send.no_save ? save::pack_no_save() : (slot ? save::pack(*slot) : std::vector<std::uint8_t>{});
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
                LOG_WARNING(Core, "coop: save: {} parts already waiting: this send goes at a later look",
                            g_outbox.size());
                g_planner.retry(send);
                continue;
            }
            LOG_INFO(Core, "coop: save: sending {} ({} bytes, {} parts) to {}",
                     send.no_save ? std::string("'no save yet'") : slot_dir->filename().string(), packed.size(),
                     parts.size(),
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
