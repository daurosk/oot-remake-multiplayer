// The host's save for the guests (coop_save.h; issue 167).

#include "coop_save.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
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
bool g_save_requests = false;   // SetSaveRequests: Hookshot asks our game for its own autosave (TakeSaveRequest)
std::atomic<bool> g_save_request_due{false};   // set under g_mutex when an ask opens a round; TakeSaveRequest's
                                               // lock-free check (the calc thread, every frame)
// Player report 120: the slot our game plays (patch 0025's opens, patch 0024's load windows), sent to the guests.
save::PlayedSlot g_played;
std::atomic<SlotOpenedFn> g_slot_opened{nullptr};   // SetSlotOpens (Hookshot's patch 0025)
struct SlotOpens {
    unsigned opens = 0;   // the count of opens so far (0: none)
    fs::path dir;         // the last one's slot folder
    bool write = false;
};
unsigned g_slot_opens_seen = 0;
bool g_title_seen = false;   // the title's menu was up in this run (PlayedSlot::new_game)
bool g_resync = false;        // RequestResync: taken by the next OnFrame
bool g_guest_live = false;    // a connected guest of this session (CanResync), set each OnFrame
// The title's own ask (save::title_resync_due): a guest of this session played a file (NoteInGame), and the title has
// come up since (NoteTitle); the next OnFrame decides, quietly (TitleResync). Both cleared with each new session.
bool g_guest_played = false;
bool g_title_ask = false;
// This session's guest save state (save::GuestSession): fresh for each session (decide_again).
save::GuestSession g_guest;
std::deque<proto::SavePart> g_outbox;       // the host's parts, a frame each
std::uint32_t g_next_id = 0;
bool g_said_own = false;
// Issue 106: on the clock, not presented frames (high refresh presents up to
// 2.75 times as many).
CoopEvery::Every g_folder_every{CoopEvery::kHalfSecondUs};
std::vector<std::string> g_notices;   // chat lines, oldest first (TakeNotice)
save::GuestNotices g_lines;           // the guest's lines about the host's save (the owner's wording)

void Say(std::string line) {
    if (!line.empty()) g_notices.push_back(std::move(line));
}
std::function<std::string()> g_root;                    // SetSwitch (Hookshot)
std::function<bool(const std::string&)> g_remount;
bool g_in_game = false;
bool g_decided = false;   // the run-time choice is made (for this session: save::decide_again)
// Hookshot, a guest: the co-op folder it moves to once the host's save is written there. Until then the game stays
// on its own folder, so the title never offers an empty or half-written co-op folder (that crashed the game).
fs::path g_pending;
bool g_title_up = false;   // the title's menu is up (NoteTitle, Hookshot's title module)
// The player picked a file on the title (CancelPendingSwitch): the switch waits while their file loads. It comes
// back when they return to the title without one (ResumePendingSwitch), and once Link is in the game (their file)
// the hold ends: a save that comes in during play is kept and switched to back on the title (PendingFrame).
bool g_suspended = false;
const void* g_decided_client = nullptr;   // the session's client it was made for
// The session the choice was made for was seen over while a file was loaded (the choice is kept then: no switch under
// a game being played). A new session connected in that state chooses again (save::rejoined_in_game).
bool g_ended = false;
bool g_live = false;   // decided and the session not over (SessionLive)
// (Navi) Solo or multiplayer saves (ChooseSaveSet): the choice kept in coop-saves.ini, read once; put in force on the
// first title's menu.
bool g_mp_saves = false;
bool g_set_read = false;
bool g_set_applied = false;
// Player report 124: the save folder was switched (RemountLocked) and the game's own save list (its caption manager,
// which the title's Continue and Load read) still shows the old folder's saves. Taken by the title's step
// (TakeSaveListStale), which has the game re-read it.
bool g_list_stale = false;

// An environment variable's text ("" when unset).
std::string EnvText(const char* name) {
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return {};
    }
    const std::string v(value);
    std::free(value);
    return v;
#else
    const char* raw = std::getenv(name);
    return raw != nullptr ? std::string(raw) : std::string();
#endif
}

// Unset, or "1"/"0": the environment's LICHII_COOP_HOST_SAVE.
std::optional<bool> EnvChoice() {
    const std::string v = EnvText("LICHII_COOP_HOST_SAVE");
    if (v.empty()) {
        return std::nullopt;
    }
    return v != "0";
}

// LICHII_COOP_TEST_OWN_SAVE=1 (the autotests, tools/autotest): a guest on Hookshot keeps its own save folder and
// asks for no host's save, so each test game plays the save its scenario put in. Players never set it.
bool TestOwnSave() {
    const std::string v = EnvText("LICHII_COOP_TEST_OWN_SAVE");
    return !v.empty() && v != "0";
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
bool RemountLocked(const fs::path& dir);
save::WriteResult WriteSlotTo(const fs::path& dir, const save::SaveSlot& slot, bool played_from = true);
void PendingFrame();
void AskAtTitle();

// Under g_mutex: a session's choice with a file loaded (a Join from the pause menu, a rejoin during play): nothing
// of the folder is touched under the game being played. A guest asks for the host's save as on any join; it is
// written and switched to on the title's menu (AskAtTitle), and the chat says so.
void DecideInGame(coop::net::Client& client) {
    g_decided = true;
    g_decided_client = &client;
    g_ended = false;
    if (client.is_host()) {
        g_use = false;
        g_guest = save::GuestSession{};   // (an earlier session's host's save, still waiting: not this game's)
        g_lines = save::GuestNotices{g_in_game};
        g_pending.clear();
        g_suspended = false;
        LOG_INFO(Core, "coop: save: the host (a file is loaded): this game keeps the saves it plays ({})",
                 g_dir.string());
        return;
    }
    if (TestOwnSave()) {
        g_use = false;
        g_guest = save::GuestSession{};   // (as for the host: nothing of an earlier session's ask stays)
        g_lines = save::GuestNotices{g_in_game};
        g_pending.clear();
        g_suspended = false;
        LOG_INFO(Core, "coop: save: LICHII_COOP_TEST_OWN_SAVE: this guest keeps its own saves (a test)");
        return;
    }
    AskAtTitle();
    LOG_INFO(Core, "coop: save: a guest that joined with a file loaded: asking the host for its save; it is written "
                   "to {} and switched to on the title's menu, the game played now keeps {}",
             g_pending.string(), g_dir.string());
    Say(std::string(save::GuestNotices::kJoinedPlaying));
}

// Under g_mutex, each frame where the folder can be switched (Hookshot): the folder as mounted, and the choice.
void SwitchFrame(coop::net::Client& client) {
    if (!g_root || !g_remount) return;
    if (g_dir.empty()) {
        if (const auto root = g_root(); !root.empty()) NoteSaveDirLocked(fs::path(root));
    }
    {
        const auto st = client.state();
        bool over = &client != g_decided_client || st == coop::net::State::idle ||
                    st == coop::net::State::stopped || st == coop::net::State::refused;
        if (save::decide_again(g_decided, over, g_in_game)) {
            g_decided = false;   // the next session (the title menu's Host after a Join, a rejoin) decides again
            g_guest = save::GuestSession{};   // and asks for the host's save again: everything of the last one goes
            g_lines = save::GuestNotices{};
            g_pending.clear();
            g_suspended = false;
            g_ended = false;
            g_guest_played = false;   // (the title's ask is this session's)
            g_title_ask = false;
            LOG_INFO(Core, "coop: save: the session is over: the next one chooses the save folder again");
        } else if (g_decided && over) {
            g_ended = true;   // (a file is loaded: the choice is kept for the title, or for a rejoin below)
        }
        const bool connected = st == coop::net::State::connected && client.host_number() != 0;
        if (!g_dir.empty() && save::rejoined_in_game(g_decided, g_ended, connected, g_in_game)) {
            LOG_INFO(Core, "coop: save: a new session with a file loaded: it chooses again");
            DecideInGame(client);
            over = false;
        }
        g_live = g_decided && !over;   // (decided below in this frame: on from the next one)
    }
    if (!g_decided && !g_dir.empty()) {
        const bool connected = client.state() == coop::net::State::connected && client.host_number() != 0;
        auto choice = save::folder_choice(std::nullopt, true, connected, connected && client.is_host(), g_in_game);
        if (choice == save::FolderChoice::wait) return;
        if (choice == save::FolderChoice::coop_later) {
            DecideInGame(client);
            PendingFrame();
            return;
        }
        if (choice == save::FolderChoice::coop && TestOwnSave()) {
            LOG_INFO(Core, "coop: save: LICHII_COOP_TEST_OWN_SAVE: this guest keeps its own saves (a test)");
            choice = save::FolderChoice::own;
        }
        g_decided = true;
        g_decided_client = &client;
        // Both sides start on the game's own folder: a guest moves to the co-op one only once the host's save is
        // whole in it (PendingFrame), never onto an empty one.
        const fs::path own = save::is_coop_dir(g_dir) ? save::own_dir_of(g_dir) : g_dir;
        if (own != g_dir) {
            if (!RemountLocked(own)) {
                LOG_WARNING(Core, "coop: save: could not switch the save folder back to {}: this game keeps {}",
                            own.string(), g_dir.string());
                g_use = false;
                return;
            }
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
        } else if (connected && client.is_host()) {
            LOG_INFO(Core, "coop: save: the host: this game uses its own saves");
        }
    }
    PendingFrame();
}

// Under g_mutex, each frame (Hookshot): the host's save that came is written whole into the pending co-op folder,
// and only then does the game move to it. Link already in the game (the player chose their own file or a new game
// meanwhile): no switch, the save stays unused.
void PendingFrame() {
    if (g_pending.empty()) return;
    if (g_in_game && g_suspended) {
        g_suspended = false;   // (the player's own pick has loaded: back on the title's menu, the switch goes)
        LOG_INFO(Core, "coop: save: a file is loaded: the host's save is switched to when the title is back");
    }
    if (save::old_coop_first(true, g_guest.unwritten.has_value(), g_title_up && !g_suspended, g_in_game,
                             g_dir == g_pending)) {
        // (AskAtTitle while the game played an earlier host's save: that one must not be the one the title offers)
        const fs::path own = save::own_dir_of(g_pending);
        if (!RemountLocked(own)) {
            LOG_WARNING(Core, "coop: save: could not switch the save folder back to {} before the host's save: the "
                              "title may offer an earlier host's save in {}", own.string(), g_dir.string());
            g_pending.clear();   // (no switch to it either: this game keeps the folder it has)
            g_use = false;
            return;
        }
        if (const int n = save::clear_coop_slots(g_pending); n > 0) {
            LOG_INFO(Core, "coop: save: {} older slot(s) taken out of {}", n, g_pending.string());
        }
        LOG_INFO(Core, "coop: save: on the title before the host's save came: back on {} until it is in", own.string());
    }
    switch (save::pending_step(g_guest.unwritten.has_value(), g_title_up && !g_suspended, g_in_game)) {
    case save::PendingStep::keep:
        return;   // (also while a file is played: switched on the title's menu, after it)
    case save::PendingStep::go:
        break;
    }
    auto slot = std::move(*g_guest.unwritten);
    g_guest.unwritten.reset();
    const fs::path to = std::exchange(g_pending, {});
    if (WriteSlotTo(to, slot, false) != save::WriteResult::written) return;
    if (!RemountLocked(to)) {
        LOG_WARNING(Core, "coop: save: could not switch the save folder to {}: this game keeps {}", to.string(),
                    g_dir.string());
        save::note_write_failed(g_guest);
        g_lines.on_failed();
        g_use = false;   // keeps its own saves from now on: the title asks no more (review of #278)
        Say("The host's save came, but this game could not switch to it (see the log): play your own save.");
        return;
    }
    LOG_INFO(Core, "coop: save: now playing from the co-op save folder {}", to.string());
    // The host's save just written covers this return to the title (a join with a file loaded writes it on the
    // frame the title comes up): the title asks nothing more for it (review of #278).
    g_title_ask = false;
    g_guest_played = false;
    Say(g_lines.on_ready(g_in_game));
}

// Under g_mutex: THE one way a guest asks for the host's save without touching the folder it plays from (a join
// with a file loaded, Resync from host): its save session starts again, so the next OnFrame asks the host, the parts
// come, and the slot waits for the title's menu (PendingFrame), where it is written into the co-op folder and
// switched to. Its own saves are never touched.
void AskAtTitle() {
    const fs::path own = save::is_coop_dir(g_dir) ? save::own_dir_of(g_dir) : g_dir;
    g_guest = save::GuestSession{};
    g_lines = save::GuestNotices{g_in_game};   // (the file played now was loaded before this ask: never "too early")
    g_use = true;
    g_pending = save::coop_dir_of(own);
    g_suspended = false;
}

// Under g_mutex: THE one way this game's save folder is switched (Hookshot): every switch (back to its own folder for
// a new session, to the co-op folder once the host's save is in it, which Resync from host also ends in) goes through
// here. After a switch the game's save list is stale (player report 124): the title has the game re-read it
// (TakeSaveListStale). False: not switched, the game keeps its folder and its list.
bool RemountLocked(const fs::path& dir) {
    if (!g_remount || !g_remount(dir.string())) return false;
    NoteSaveDirLocked(dir);
    g_list_stale = true;
    LOG_INFO(Core, "coop: save: the save folder is now {}: the game re-reads its save list on the title", dir.string());
    return true;
}

// Under g_mutex: the host's slot written whole into the co-op folder `dir`.
// `played_from`: the game already plays from `dir` (else Success is said once it is switched to: PendingFrame).
save::WriteResult WriteSlotTo(const fs::path& dir, const save::SaveSlot& slot, bool played_from) {
    const auto result = save::write_coop_save(dir, slot, save::own_dir_of(dir));
    switch (result) {
    case save::WriteResult::written:
        g_guest.ready = true;
        LOG_INFO(Core, "coop: save: the host's save written to {}", dir.string());
        if (played_from) Say(g_lines.on_ready(g_in_game));
        break;
    case save::WriteResult::not_coop:
        LOG_WARNING(Core, "coop: save: refused to write the host's save into {}", dir.string());
        save::note_write_failed(g_guest);
        g_lines.on_failed();
        break;
    case save::WriteResult::failed:
        LOG_ERROR(Core, "coop: save: could not write the host's save into {}", dir.string());
        save::note_write_failed(g_guest);
        g_lines.on_failed();
        Say("The host's save could not be written (see the log): play your own save.");
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
    if (in_game && g_guest_live) g_guest_played = true;   // (the title's ask, TitleResync)
    Say(g_lines.on_in_game(in_game));
}

bool HostSaveReady() {
    std::lock_guard lock(g_mutex);
    return g_guest.ready;
}

void SetSaveRequests(bool on) {
    std::lock_guard lock(g_mutex);
    g_save_requests = on;
}

bool TakeSaveRequest() {
    // The calc thread asks every frame: no lock unless a guest's ask opened a round (the host's game never waits on
    // g_mutex while OnFrame packs a slot, except on the one frame that takes the request).
    if (!g_save_request_due.exchange(false, std::memory_order_acq_rel)) return false;
    std::lock_guard lock(g_mutex);
    return g_in_game && g_planner.take_save_request();   // (back on the title: no file to save; the limit sends)
}

bool HostSaveComing() {
    std::lock_guard lock(g_mutex);
    if (g_guest.unwritten) return true;   // (in: written and switched to on the title's menu)
    const auto& a = g_guest.assembler;
    return a.received() != 0 && a.received() < a.count() &&
           CoopEvery::NowUs() - g_guest.last_part_us < CoopEvery::kSaveStuckUs;
}

bool HostHasNoSave() {
    std::lock_guard lock(g_mutex);
    return g_guest.no_save;
}

bool SessionLive() {
    std::lock_guard lock(g_mutex);
    return g_live;
}

namespace {
std::atomic<unsigned> g_load_starts{0};
std::atomic<std::uint64_t> g_load_starts_ever{0};   // (SaveLoadStarts: never taken back)
std::atomic<long long> g_load_start_ms{0};
std::atomic<unsigned> g_loads{0};
std::atomic<long long> g_game_over_ms{0};  // NoteGameOverSave; 0: none, or the title came since
std::atomic<bool> g_load_retry{false};     // the last save in came after a fresh Game Over mark
std::atomic<bool> g_keeper_load{false};
std::atomic<bool> g_native_loads{false};

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

fs::path SaveSetFile() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "coop-saves.ini";
}

// Under g_mutex: the kept choice, read once ("saves = multiplayer"; anything else: solo).
void ReadSaveSetLocked() {
    if (g_set_read) return;
    g_set_read = true;
    std::FILE* f = nullptr;
#ifdef _MSC_VER
    if (_wfopen_s(&f, SaveSetFile().c_str(), L"rb") != 0) f = nullptr;
#else
    f = std::fopen(SaveSetFile().c_str(), "rb");
#endif
    if (f == nullptr) return;
    char buf[256] = {};
    const std::size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    g_mp_saves = std::string_view(buf, n).find("saves = multiplayer") != std::string_view::npos;
}

void KeepSaveSetLocked() {
    std::FILE* f = nullptr;
#ifdef _MSC_VER
    if (_wfopen_s(&f, SaveSetFile().c_str(), L"wb") != 0) f = nullptr;
#else
    f = std::fopen(SaveSetFile().c_str(), "wb");
#endif
    if (f == nullptr) {
        LOG_WARNING(Core, "coop: save: could not write {}", SaveSetFile().string());
        return;
    }
    const std::string text = std::string("# Navi: the saves played, solo or multiplayer (Multiplayer tab, Settings, Saves)\n") +
                             "saves = " + (g_mp_saves ? "multiplayer" : "solo") + "\n";
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

// Under g_mutex: the solo folder of the folder mounted now (no -coop, no -mp).
fs::path SoloDirLocked(const fs::path& now) {
    fs::path p = save::own_dir_of(now);
    const std::string name = p.filename().string();
    if (name.size() > 3 && name.compare(name.size() - 3, 3, "-mp") == 0) {
        p = p.parent_path() / name.substr(0, name.size() - 3);
    }
    return p;
}

// Under g_mutex: the chosen set mounted, when that may be done now (the title's menu, no file, no session). The note.
std::string ApplySaveSetLocked() {
    if (!g_root || !g_remount) return "Saves can only be switched in this game's own menu.";
    if (g_in_game || !g_title_up) return "Go back to the title screen first: the saves switch there.";
    if (g_decided || !g_pending.empty()) return "Leave the multiplayer game first: the saves switch with no session.";
    const auto root = g_root();
    if (root.empty()) return "The game has not opened its saves yet.";
    const fs::path now = fs::path(root).lexically_normal();
    const fs::path solo = SoloDirLocked(now);
    fs::path want = solo;
    if (g_mp_saves) want += "-mp";
    if (want == now) return g_mp_saves ? "Multiplayer saves." : "Solo saves.";
    std::error_code ec;
    if (g_mp_saves && !fs::exists(want, ec)) {   // (the first time: the solo saves as they are, to start from)
        // Copied to a temporary folder and renamed once whole: a failed copy leaves no half folder (review of PR 236).
        // The solo saves are only read.
        std::string why;
        const bool made = save::copy_dir_whole(solo, want, &why);
        LOG_INFO(Core, "coop: save: the multiplayer saves made as a copy of the solo saves: {}{}", want.string(),
                 made ? std::string() : " (copy failed: " + why + ")");
        if (!made) return "The multiplayer saves could not be made (see the log).";
    }
    // (the one switch path: the title has the game re-read its save list, so the new set shows at once, report 124)
    if (!RemountLocked(want)) {
        LOG_WARNING(Core, "coop: save: could not switch the saves to {}", want.string());
        return "The saves could not be switched (see the log).";
    }
    LOG_INFO(Core, "coop: save: playing the {} saves: {}", g_mp_saves ? "multiplayer" : "solo", want.string());
    return g_mp_saves ? "Multiplayer saves: load a save to play them." : "Solo saves: load a save to play them.";
}

}  // namespace

void NoteTitle(bool title_up) {
    if (title_up) {
        g_game_over_ms.store(0, std::memory_order_relaxed);  // Game Over's Quit Game: the next load is the player's pick
    }
    std::lock_guard lock(g_mutex);
    if (title_up && !g_title_up) {
        g_played.on_title();   // (a new pick follows: a file, or New Game)
        g_title_seen = true;
        if (std::exchange(g_guest_played, false)) g_title_ask = true;   // (decided by the next OnFrame: TitleResync)
    }
    g_title_up = title_up;
    if (title_up && !g_set_applied && !g_in_game && g_root) {   // (Navi) the kept choice, on the first title
        ReadSaveSetLocked();
        g_set_applied = true;
        if (g_mp_saves) ApplySaveSetLocked();
    }
}

std::string ChooseSaveSet(bool multiplayer) {
    std::lock_guard lock(g_mutex);
    ReadSaveSetLocked();
    const bool before = g_mp_saves;
    g_mp_saves = multiplayer;
    const std::string note = ApplySaveSetLocked();
    const std::string root = g_root ? g_root() : std::string();
    const bool done = !root.empty() && (fs::path(root).lexically_normal().filename().string().ends_with("-mp") ||
                                        save::own_dir_of(fs::path(root)).filename().string().ends_with("-mp")) == multiplayer;
    if (!done) {
        g_mp_saves = before;   // (not switched: the choice stays as it was)
        return note;
    }
    KeepSaveSetLocked();
    return note;
}

bool MultiplayerSaves() {
    std::lock_guard lock(g_mutex);
    ReadSaveSetLocked();
    return g_mp_saves;
}

void SetSlotOpens(SlotOpenedFn opened) {
    g_slot_opened.store(opened, std::memory_order_release);
}

namespace {
const char* SourceName(save::PlayedSlot::Source s) {
    switch (s) {
    case save::PlayedSlot::Source::write: return "the game saved into it";
    case save::PlayedSlot::Source::load_read: return "the game loaded it";
    case save::PlayedSlot::Source::last_read: return "the last one the game read before its load";
    case save::PlayedSlot::Source::none: break;
    }
    return "not known";
}

// Under g_mutex: a change of the played slot, logged once, in a session only (solo writes no co-op save lines).
void LogPlayed(const std::optional<fs::path>& before) {
    if (g_played.slot() == before || !g_live) return;
    if (g_played.slot()) {
        LOG_INFO(Core, "coop: save: our game plays {} ({})", g_played.slot()->filename().string(),
                 SourceName(g_played.source()));
    }
}

// Patch 0025's opens now (no lock of ours: the exe's own lock, held for a copy).
SlotOpens ReadSlotOpens() {
    SlotOpens o;
    if (const auto opened = g_slot_opened.load(std::memory_order_acquire)) {
        char buf[1024];
        int w = 0;
        o.opens = opened(buf, sizeof buf, &w);
        if (o.opens != 0) o.dir = fs::path(buf).lexically_normal();
        o.write = w != 0;
    }
    return o;
}

// The game thread's load-window marks (NoteGameSaveLoad), each with patch 0025's opens at that moment, for the next
// PollSlotOpens. Their own small lock: only a push or a swap under it, never g_mutex or disk work (the game's save
// load never waits on the host's save reads).
std::mutex g_window_mutex;
std::vector<std::pair<bool, SlotOpens>> g_windows;

void NoteLoadWindowMark(bool start) {
    auto opens = ReadSlotOpens();
    std::lock_guard lock(g_window_mutex);
    if (g_windows.size() < 64) g_windows.emplace_back(start, std::move(opens));
}
}  // namespace

void PollSlotOpens() {
    std::vector<std::pair<bool, SlotOpens>> windows;
    {
        std::lock_guard lock(g_window_mutex);
        windows.swap(g_windows);
    }
    const SlotOpens now = ReadSlotOpens();
    std::lock_guard lock(g_mutex);
    const auto before = g_played.slot();
    // The window marks first, each with the opens as they were at it: a load's read is never lost, nor a read from
    // before the load taken as inside it, whatever the frame's timing (save::PlayedSlot counts the opens).
    for (const auto& [start, at] : windows) g_played.on_load_window(start, at.opens, at.dir, at.write);
    if (now.opens != g_slot_opens_seen && !now.dir.empty()) {
        g_slot_opens_seen = now.opens;   // (several opens in one frame: the last counts; one slot per load)
        g_played.on_open(now.dir, now.write, now.opens);
    }
    LogPlayed(before);
}

void RequestResync() {
    std::lock_guard lock(g_mutex);
    g_resync = true;
}

bool CanResync() {
    std::lock_guard lock(g_mutex);
    return g_guest_live && g_root && g_remount;
}

void NoteGameSaveLoad(bool start) {
    NoteLoadWindowMark(start);   // player report 120: the slot the game loads (save::PlayedSlot)
    const long long now = NowMs();
    if (start) {
        g_load_starts.fetch_add(1, std::memory_order_relaxed);
        g_load_starts_ever.fetch_add(1, std::memory_order_relaxed);
        g_load_start_ms.store(now, std::memory_order_relaxed);
        return;
    }
    const long long over = g_game_over_ms.exchange(0, std::memory_order_relaxed);
    g_load_retry.store(over != 0 && now - over <= kGameOverFreshMs, std::memory_order_relaxed);
    g_loads.fetch_add(1, std::memory_order_relaxed);
    g_keeper_load.store(true, std::memory_order_relaxed);
}

void NoteGameOverSave() {
    g_game_over_ms.store(NowMs(), std::memory_order_relaxed);
}

void SetNativeSaveLoads(bool on) {
    g_native_loads.store(on, std::memory_order_relaxed);
}

bool NativeSaveLoads() {
    return g_native_loads.load(std::memory_order_relaxed);
}

std::uint64_t SaveLoadStarts() {
    return g_load_starts_ever.load(std::memory_order_relaxed);
}

GameSaveLoads TakeGameSaveLoads() {
    GameSaveLoads out;
    out.starts = g_load_starts.exchange(0, std::memory_order_relaxed);
    if (out.starts != 0 && NowMs() - g_load_start_ms.load(std::memory_order_relaxed) > kGameSaveLoadFreshMs) {
        out.starts = 0;  // (the game's own start, before any session: no load of ours)
    }
    out.loads = g_loads.exchange(0, std::memory_order_relaxed);
    out.retry = out.loads != 0 && g_load_retry.load(std::memory_order_relaxed);
    return out;
}

bool TakeKeeperSaveLoad() {
    return g_keeper_load.exchange(false, std::memory_order_relaxed);
}

bool TakeSaveListStale() {
    std::lock_guard lock(g_mutex);
    return std::exchange(g_list_stale, false);
}

bool TitleUp() {
    std::lock_guard lock(g_mutex);
    return g_title_up;
}

void CancelPendingSwitch(std::string_view why) {
    std::lock_guard lock(g_mutex);
    if (g_pending.empty() || g_suspended) return;
    g_suspended = true;
    LOG_INFO(Core, "coop: save: {}: no switch to the host's save while that file loads (back on the title without "
                   "it, the host's save is offered again)", why);
}

void ResumePendingSwitch() {
    std::lock_guard lock(g_mutex);
    if (g_pending.empty() || !g_suspended || g_in_game) return;
    g_suspended = false;
    LOG_INFO(Core, "coop: save: back on the title with no file loaded: the host's save is offered again");
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
            // A file played (not the title, not a new game never saved) where the game can be asked for its own
            // autosave (Hookshot, SetSaveRequests): the ask waits for that save (save::Planner's round, case 1 of
            // the join's state: the file on disk is only as new as our last door).
            const bool fresh = g_save_requests && g_in_game && !g_played.new_game(g_in_game, g_title_seen);
            const bool open = g_planner.round_open();
            if (g_planner.on_ask(ask->player_number, CoopEvery::NowUs(),
                                 fresh ? std::optional(fs::file_time_type::clock::now()) : std::nullopt)) {
                if (fresh && !open) g_save_request_due.store(true, std::memory_order_release);   // (a new round)
                LOG_INFO(Core, "coop: save: player {} asked for our save{}", ask->player_number,
                         !fresh ? "" : open ? " (it waits for the save already asked of our game)"
                                            : " (our game is asked for its own autosave first)");
            } else {
                LOG_WARNING(Core, "coop: save: player {} asked for our save again within 10 s: ignored",
                            ask->player_number);
            }
        }
        return;
    }
    const auto* part = std::get_if<proto::SavePart>(&event.message);
    if (part == nullptr) {
        return;
    }
    save::note_part(g_guest, part->transfer_id, CoopEvery::NowUs());   // (a new transfer: gave_up ends)
    if (part->player_number == client.host_number() && !client.is_host()) {
        Say(g_lines.on_part(part->transfer_id, part->count));
    }
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
        g_lines.on_failed();
        Say("The host's save came damaged; it is sent again after the host's next save.");
        return;
    case save::Assembler::Result::complete:
        break;
    }
    const auto packed = g_guest.assembler.take();
    if (save::is_no_save(packed)) {
        LOG_INFO(Core, "coop: save: the host has no save yet (a new game): this game keeps its own saves");
        g_guest.no_save = true;
        g_pending.clear();   // (nothing to switch to)
        g_lines.on_no_save();
        Say("The host has no save yet: New Game starts one, Continue plays your own.");
        return;
    }
    const auto slot = save::unpack(packed);
    if (!slot) {
        LOG_WARNING(Core, "coop: save: the host's save (transfer {}) is not a save slot", part->transfer_id);
        save::note_write_failed(g_guest);   // nothing is on its way now (else Resync says "already on its way")
        g_lines.on_failed();
        Say("The host's save could not be read (see the log): play your own save, or join again.");
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

namespace {
// Under g_mutex, from OnFrame: the Lobby tab's Resync from host (save::resync_check). The guest's save session starts
// again as on joining: the next OnFrame asks the host, the parts come, the slot waits for the title's menu
// (PendingFrame) and is then written into the co-op folder and switched to. Its own saves are never touched.
// Under g_mutex: the host's save is on its way (whole and waiting to be written, parts coming, or asked and not yet
// answered).
bool SaveComingLocked() {
    return save::save_coming(g_guest, CoopEvery::NowUs(), CoopEvery::kSaveStuckUs);
}

// Under g_mutex, from OnFrame: a guest back on the title after playing a file asks for the host's save again, the
// same ask as Resync from host (AskAtTitle), so the next load is the host's save. Quiet when it does not go (the
// button explains itself; this was never pressed): an ask already waiting or a save coming, no folder switch, a
// guest that keeps its own saves (g_use off: LICHII_COOP_TEST_OWN_SAVE, a failed switch), the session over.
void TitleResync(bool connected_guest) {
    save::TitleResync t;
    t.title_came = true;
    t.connected_guest = connected_guest;
    t.played_in_session = true;
    t.on = EnvText("LICHII_COOP_TITLE_RESYNC") != "0";
    t.can_switch = g_root && g_remount && !g_dir.empty();
    t.plays_hosts_save = g_use;
    t.ask_waiting = !g_pending.empty() || SaveComingLocked();
    if (!save::title_resync_due(t)) return;
    AskAtTitle();   // asks at once (OnFrame), as on joining; written and switched to on the title's menu
    LOG_INFO(Core, "coop: save: back on the title as a guest: asking for the host's save again, so the next load is "
                   "the host's");
    Say(std::string(save::GuestNotices::kTitleAsked));
}

void Resync(bool connected_guest) {
    switch (save::resync_check(connected_guest, g_root && g_remount && !g_dir.empty(), SaveComingLocked())) {
    case save::ResyncCheck::not_guest:
        LOG_INFO(Core, "coop: save: resync from host: refused (not a guest in a session)");
        Say("Resync from host: only a guest in a session can ask for the host's save.");
        return;
    case save::ResyncCheck::no_switch:
        LOG_INFO(Core, "coop: save: resync from host: refused (this game's save folder cannot switch)");
        Say("Resync from host: this game cannot switch its save folder (see the log).");
        return;
    case save::ResyncCheck::coming:
        LOG_INFO(Core, "coop: save: resync from host: the host's save is already on its way");
        Say("Resync from host: the host's save is already on its way.");
        return;
    case save::ResyncCheck::go:
        break;
    }
    AskAtTitle();   // asks again at once (OnFrame), as on joining
    LOG_INFO(Core, "coop: save: resync from host: asking the host for its save again ({}; switched to on the "
                   "title's menu)", g_in_game ? "a file is loaded" : "no file loaded");
    Say(g_in_game ? "Resync from host: the host's save is coming. Go back to the title and load it once the chat "
                    "says Success."
                  : "Resync from host: the host's save is coming. Load a save once the chat says Success.");
}
}  // namespace

void OnFrame(coop::net::Client& client, bool host_reset) {
    std::lock_guard lock(g_mutex);
    SwitchFrame(client);
    const bool connected = client.state() == coop::net::State::connected && client.host_number() != 0;
    g_guest_live = connected && !client.is_host() && g_live;
    if (std::exchange(g_resync, false)) Resync(g_guest_live);
    if (std::exchange(g_title_ask, false)) TitleResync(g_guest_live);
    if (!connected) {
        g_outbox.clear();
        return;
    }
    if (!client.is_host()) {
        g_outbox.clear();
        // A transfer that stopped part way (a part lost): said once; the guest
        // asks again (save::reask_due), and the host's next send starts again.
        if (!g_guest.said_stuck && g_guest.assembler.received() != 0 &&
            g_guest.assembler.received() < g_guest.assembler.count() &&
            CoopEvery::NowUs() - g_guest.last_part_us > CoopEvery::kSaveStuckUs) {
            g_guest.said_stuck = true;
            LOG_WARNING(Core, "coop: save: the host's save (transfer {}) stopped at part {} of {} "
                              "for 10 s; asking again soon",
                        g_guest.assembler.transfer_id(), g_guest.assembler.received(), g_guest.assembler.count());
        }
        // (Hookshot: only while the switch to the host's save is still wanted.)
        const bool want = g_root ? g_use && !g_pending.empty() : UseCoopFolder();
        const std::uint64_t now = CoopEvery::NowUs();
        if (want && (!g_guest.asked || save::reask_due(g_guest, now))) {
            const bool again = g_guest.asked;
            const bool stalled = g_guest.assembler.received() != 0;
            if (client.send(proto::Message{proto::SaveAsk{}})) {
                g_guest.asked = true;
                g_guest.asked_us = now;
                ++g_guest.asks;
                LOG_INFO(Core, "coop: save: asked the host (player {}) for its save{}", client.host_number(),
                         again ? fmt::format(" again ({} in {} s; ask {} of {})",
                                             stalled ? "the transfer stopped: no part" : "nothing came",
                                             save::kGuestReaskUs / 1'000'000, g_guest.asks, save::kGuestMaxAsks)
                               : std::string());
            }
        } else if (want && save::give_up_due(g_guest, now)) {
            // The last ask got nothing whole either: stop waiting, so a later own load is not "too early".
            g_guest.gave_up = true;
            Say(g_lines.on_gave_up());
            LOG_WARNING(Core, "coop: save: the host's save did not come after {} asks; playing on without it",
                        g_guest.asks);
        }
        return;
    }
    if (host_reset) {
        const auto before = g_played.slot();
        g_played.on_load_seen();
        LogPlayed(before);
        const bool known = g_played.slot().has_value();
        g_planner.on_host_load(fs::file_time_type::clock::now(), known);
        if (known) {
            LOG_INFO(Core, "coop: save: we loaded {}: every guest gets it now", g_played.slot()->filename().string());
        } else {
            LOG_INFO(Core, "coop: save: we loaded a save (which slot is not known here): a guest that asks gets our "
                           "newest save at once, and every guest gets this one after the game's next save (a door "
                           "or a save point)");
        }
    }
    // A new game not saved yet: the guests hear "no save yet", never another slot of ours (player report 120).
    const bool new_game = g_played.new_game(g_in_game, g_title_seen);
    // Twice a second: what the host owes (it reads the folder's file times).
    if (!g_dir.empty() && !g_planner.idle() && g_folder_every.Due(CoopEvery::NowUs())) {
        const auto newest = new_game ? std::nullopt : save::newest_write(g_dir);
        const bool round = g_planner.round_open();
        const auto sends = g_planner.due(newest, CoopEvery::NowUs());
        if (round && !g_planner.round_open() && !sends.empty() && !sends.front().no_save &&
            sends.front().reason == proto::SaveReason::join) {
            if (sends.front().fresh) {
                LOG_INFO(Core, "coop: save: our game saved after the ask: the guests get the game as it is now");
            } else {
                LOG_WARNING(Core, "coop: save: our game did not save within {} s of the ask (it refused: a cutscene, "
                                  "a boss room, just saved at a door, or a state it does not save in): sending our "
                                  "last save on disk, not the game as it is now",
                            save::Planner::kFreshSaveUs / 1'000'000);
            }
        }
        for (const auto& send : sends) {
            bool played = send.fresh;   // (a save written after the ask: the newest slot is the game now)
            const auto slot_dir = send.no_save ? std::nullopt
                                  : send.fresh ? save::newest_slot(g_dir)
                                               : save::slot_to_send(g_dir, g_played.slot(), &played);
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
            LOG_INFO(Core, "coop: save: sending {} ({}; {} bytes, {} parts) to {}",
                     send.no_save ? std::string("'no save yet'") : slot_dir->filename().string(),
                     send.no_save ? (new_game ? "a new game, not saved yet" : "nothing on disk")
                                  : send.fresh ? "saved by our game after the ask: the game now"
                                  : (played ? "the slot our game plays"
                                            : "the newest on disk: the slot played is not known"),
                     packed.size(),
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
    if (g_notices.empty()) return {};
    std::string line = std::move(g_notices.front());
    g_notices.erase(g_notices.begin());
    return line;
}

}  // namespace CoopSave
