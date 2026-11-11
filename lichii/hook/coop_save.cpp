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
#include "coop_settings.h"

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
unsigned g_frames = 0;
std::string g_notice;

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

bool Loopback(std::string_view server) {
    return server.starts_with("127.") || server == "localhost" || server == "::1" ||
           server == "[::1]";
}

void Decide() {
    if (const auto env = EnvChoice()) {
        g_use = *env;
        LOG_INFO(Core, "coop: save: LICHII_COOP_HOST_SAVE={}: {}", *env ? 1 : 0,
                 *env ? "this game uses the co-op save folder" : "this game uses its own saves");
        return;
    }
    std::vector<std::string> errors;
    const auto ini = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "coop.ini";
    const auto settings = CoopHook::LoadSettings(ini, errors);
    g_use = settings.enabled() && errors.empty() && !Loopback(settings.server);
    LOG_INFO(Core, "coop: save: {}", g_use
                 ? "joining another PC's server: this game uses the co-op save folder (the host's save)"
                 : "hosting or single player: this game uses its own saves");
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
    fs::path dir = fs::path(real_path).lexically_normal();
    if (!dir.has_filename()) {
        dir = dir.parent_path();
    }
    if (dir == g_dir) {
        return;
    }
    g_dir = dir;
    LOG_INFO(Core, "coop: save: the game's save folder is {}", g_dir.string());
    if (g_unwritten) {
        WriteSlot(*g_unwritten);
        g_unwritten.reset();
    }
}

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
            LOG_INFO(Core, "coop: save: player {} asked for our save", ask->player_number);
            g_planner.on_ask(ask->player_number);
        }
        return;
    }
    const auto* part = std::get_if<proto::SavePart>(&event.message);
    if (part == nullptr) {
        return;
    }
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
    ++g_frames;
    if (client.state() != coop::net::State::connected || client.host_number() == 0) {
        g_outbox.clear();
        return;
    }
    if (!client.is_host()) {
        g_outbox.clear();
        if (!g_asked && UseCoopFolder()) {
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
    if (!g_dir.empty() && !g_planner.idle() && g_frames % 30 == 0) {
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
