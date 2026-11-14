// coop_rando.h: the rules first, then Rando, which acts on them with one
// worker thread at a time.
#include "coop_rando.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <thread>

extern char** environ;
#endif

namespace CoopRando {

namespace fs = std::filesystem;

namespace {

std::string ReadLine(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::string line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
    }
    return line;
}

bool WriteText(const fs::path& file, const std::string& text) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    return static_cast<bool>(out << text) && static_cast<bool>(out.flush());
}

bool IsSeedFolder(std::string_view name) {
    return name.size() > std::string_view(kSeedPrefix).size() && name.rfind(kSeedPrefix, 0) == 0 &&
           name.find('/') == std::string_view::npos;
}

const CoopMods::Row* RowOf(const std::vector<CoopMods::Row>& rows, std::string_view name) {
    for (const CoopMods::Row& r : rows) {
        if (CoopMods::NameSame(r.name, name)) return &r;
    }
    return nullptr;
}

std::string Quote(const std::string& arg) {
    // CommandLineToArgvW: backslashes count only before a quote.
    if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) {
        return arg;
    }
    std::string out = "\"";
    std::size_t slashes = 0;
    for (const char c : arg) {
        if (c == '\\') {
            ++slashes;
            continue;
        }
        if (c == '"') {
            out.append(slashes * 2 + 1, '\\');
        } else {
            out.append(slashes, '\\');
        }
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, '\\');
    out += '"';
    return out;
}

}  // namespace

std::string ManifestText() {
    std::string presets;
    for (const char* p : kPresets) {
        presets += std::string(presets.empty() ? "" : ", ") + "\"" + p + "\"";
    }
    return std::string(R"({
  "id": "oot-remake-randomizer",
  "name": "OoT Remake Randomizer",
  "version": "1",
  "author": "OOTRR by REALAZK (MIT), run by the co-op mod loader",
  "description": "Shuffles chests, dungeon items and rewards (OOTRR). In co-op everyone plays the host's seed and every pickup is shared. A new seed needs a restart of the game, never a game build.",
  "coop": "everyone",
  "settings": [
    {"key": "on", "name": "Randomizer", "type": "toggle", "shared": true, "restart": true,
     "help": "On: the game plays a randomized seed (start a new save for it)."},
    {"key": "seed", "name": "Seed", "type": "text", "max_length": 63, "shared": true, "restart": true,
     "help": "Letters and digits. Empty: a new one is made. In co-op the host's seed is everyone's."},
    {"key": "preset", "name": "Settings", "type": "choice", "choices": [)") +
           presets + R"(], "shared": true, "restart": true,
     "help": "OOTRR's presets (default, easy, open world, keysanity, full shuffle)."},
    {"key": "game_folder", "name": "Game folder", "type": "text", "max_length": 260,
     "help": "Only for other game files (1.1.1, with Pack, Banc, Event and System). Empty: the kit's own (romfs beside the game)."},
    {"key": "ootrr", "name": "OOTRR program", "type": "text", "max_length": 260,
     "help": "Only for another OOTRR. Empty: the one the kit brings (tools\\ootrr beside the game)."}
  ]
}
)";
}

std::string SeedWord(std::string_view seed) {
    std::string out;
    for (const char c : seed) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '-';
        if (ok && out.size() < 63) out += c;
    }
    return out.empty() ? "seed" : out;
}

std::string FolderFor(std::string_view seed, std::string_view preset) {
    std::string out = std::string(kSeedPrefix) + SeedWord(seed);
    if (!preset.empty() && preset != "default") {
        out += "-" + std::string(preset);
    }
    return out;
}

std::string NewSeed(std::uint64_t entropy) {
    static constexpr char kChars[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // no 0/O, 1/I
    std::string out;
    for (int i = 0; i < 8; ++i) {
        out += kChars[entropy % 32];
        entropy /= 32;
        entropy ^= entropy << 13;  // stays varied once the first bits are used
    }
    return out;
}

std::vector<Built> Scan(const fs::path& mods_root) {
    std::vector<Built> out;
    std::error_code ec;
    for (fs::directory_iterator it(mods_root, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = CoopMods::ToUtf8(it->path().filename());
        if (!it->is_directory(ec) || !IsSeedFolder(name)) continue;
        const fs::path seed_file = it->path() / kSeedFile;
        if (!fs::is_regular_file(seed_file, ec)) continue;
        out.push_back({name, ReadLine(seed_file), ReadLine(it->path() / kPresetFile)});
    }
    std::sort(out.begin(), out.end(), [](const Built& a, const Built& b) { return CoopMods::NameLess(a.name, b.name); });
    return out;
}

Plan Decide(const Want& want, const std::vector<Built>& built, const std::vector<CoopMods::Row>& rows) {
    Plan plan;
    const auto switch_off_all_but = [&](std::string_view keep) {
        for (const CoopMods::Row& r : rows) {
            if (IsSeedFolder(r.name) && r.on && !CoopMods::NameSame(r.name, keep)) {
                plan.off.push_back(r.name);
            }
        }
    };
    if (!want.on) {
        switch_off_all_but("");
        if (!plan.off.empty()) {
            plan.what = Plan::Do::use;
            plan.why = "the randomizer is off: its seeds are switched off";
        }
        return plan;
    }
    if (want.seed.empty()) {
        plan.why = "the randomizer waits for a seed";
        return plan;
    }
    const std::string preset = want.preset.empty() ? "default" : want.preset;
    plan.folder = FolderFor(want.seed, preset);
    const auto have = std::find_if(built.begin(), built.end(), [&](const Built& b) {
        return CoopMods::NameSame(b.name, plan.folder) && b.seed == SeedWord(want.seed) &&
               (b.preset.empty() ? "default" : b.preset) == preset;
    });
    if (have == built.end()) {
        plan.what = Plan::Do::build;
        plan.why = "seed " + SeedWord(want.seed) + " (" + preset + ") is built now";
        return plan;
    }
    switch_off_all_but(plan.folder);
    // On, and the first row: its size table (which keeps the larger entry of every other mod) must win.
    const CoopMods::Row* row = RowOf(rows, plan.folder);
    if (row == nullptr || !row->on || rows.empty() || !CoopMods::NameSame(rows.front().name, plan.folder)) {
        plan.on.push_back(plan.folder);
    }
    plan.what = plan.on.empty() && plan.off.empty() ? Plan::Do::nothing : Plan::Do::use;
    plan.why = plan.what == Plan::Do::use ? "seed " + SeedWord(want.seed) + " (" + preset + ") is switched on" : "";
    return plan;
}

std::vector<std::string> GenerateArgs(const std::string& preset, const std::string& seed, const fs::path& seed_file) {
    return {"generate", preset.empty() ? "default" : preset, SeedWord(seed), CoopMods::ToUtf8(seed_file)};
}

std::vector<std::string> BuildArgs(const fs::path& seed_file, const fs::path& game_folder, const fs::path& mods_root,
                                   const std::string& folder) {
    return {"build", CoopMods::ToUtf8(seed_file), CoopMods::ToUtf8(game_folder), CoopMods::ToUtf8(mods_root), folder,
            "--host", "hookshot"};
}

std::wstring CommandLine(const fs::path& program, const std::vector<std::string>& args) {
    // ToUtf8 writes '/': Windows paths back to '\' before quoting (a trailing '\' is doubled by Quote).
    const auto windows = [](std::string s) {
        std::replace(s.begin(), s.end(), '/', '\\');
        return s;
    };
    std::string line = Quote(windows(CoopMods::ToUtf8(program)));
    for (const std::string& a : args) {
        line += " " + Quote(windows(a));
    }
    return CoopMods::FromUtf8(line).wstring();
}

const char* OotrrName() {
#ifdef _WIN32
    return "ootrr.exe";
#else
    return "ootrr";
#endif
}

std::optional<fs::path> FindOotrr(const std::string& setting, const fs::path& game_dir, const fs::path& mods_root) {
    std::error_code ec;
    if (!setting.empty()) {
        const fs::path p = CoopMods::FromUtf8(setting);
        return fs::is_regular_file(p, ec) ? std::optional<fs::path>(p) : std::nullopt;
    }
    for (const fs::path& p : {game_dir / "tools" / "ootrr" / OotrrName(), mods_root / kFolder / OotrrName()}) {
        if (fs::is_regular_file(p, ec)) return p;
    }
    return std::nullopt;
}

std::optional<fs::path> FindGameFolder(const std::string& setting, const fs::path& game_dir) {
    const auto game_files = [](const fs::path& p) {
        std::error_code ec;
        for (const char* part : {"Pack", "System"}) {
            if (!fs::is_directory(p / part, ec)) return false;
        }
        return true;
    };
    std::string where = setting;
    if (where.empty()) {
        const char* env = std::getenv("OOT_ROMFS");
        where = env != nullptr ? env : "";
    }
    if (!where.empty()) {
        // Named: that folder or none (a wrong setting is said, not passed over for another folder).
        const fs::path p = CoopMods::FromUtf8(where);
        return game_files(p) ? std::optional<fs::path>(p) : std::nullopt;
    }
    // The kit's own: Longshot's build unpacks the player's game beside the program (<game>\romfs).
    // (a builder's bin\ layout has the program one folder down: <game>\bin\longshot.exe, <game>\romfs)
    if (game_dir.empty()) return std::nullopt;
    for (const fs::path& p : {game_dir / "romfs", game_dir.parent_path() / "romfs"}) {
        if (game_files(p)) return p;
    }
    return std::nullopt;
}

std::string TellLine(std::string_view why) {
    std::string line = "[randomizer] I play without the host's seed: " + std::string(why);
    if (line.size() > kTellChars) {
        std::size_t cut = kTellChars;
        while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) --cut;  // a whole UTF-8 character
        line.resize(cut);
    }
    return line;
}

std::string BuildFailureHint(std::string_view out) {
    if (out.find("the game folder has no") != std::string_view::npos) {
        return "your game files are not version 1.1.1 (co-op's game is update v131072): dump the game with that "
               "update and build the kit's game again (Build-Hookshot-Coop)";
    }
    return {};
}

int RunProgram(const fs::path& program, const std::vector<std::string>& args, const fs::path& log_file,
               std::chrono::milliseconds limit, const std::atomic<bool>* stop) {
#ifdef _WIN32
    const std::wstring command_line = CommandLine(program, args);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};  // the child writes into it
    HANDLE log = CreateFileW(log_file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        return -1;
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = INVALID_HANDLE_VALUE;
    si.hStdOutput = log;
    si.hStdError = log;
    PROCESS_INFORMATION pi{};
    std::wstring line = command_line;  // CreateProcessW may write into it
    const BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                        nullptr, &si, &pi);
    CloseHandle(log);
    if (!started) {
        return -1;
    }
    const auto until = std::chrono::steady_clock::now() + limit;
    bool ended = false;
    for (;;) {
        ended = WaitForSingleObject(pi.hProcess, 200) != WAIT_TIMEOUT;
        if (ended || (stop != nullptr && stop->load()) || std::chrono::steady_clock::now() >= until) {
            break;
        }
    }
    DWORD code = 1;
    if (!ended) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    } else {
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ended ? static_cast<int>(code) : kStopped;
#else
    const int log = open(log_file.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (log < 0) {
        return -1;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, log, 1);
    posix_spawn_file_actions_adddup2(&actions, log, 2);
    std::string path = program.string();
    std::vector<std::string> owned = args;
    std::vector<char*> argv;
    argv.push_back(path.data());
    for (std::string& a : owned) argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, path.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(log);
    if (spawned != 0) {
        return -1;
    }
    const auto until = std::chrono::steady_clock::now() + limit;
    int status = 0;
    for (;;) {
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            return 1;  // ended by a signal
        }
        if (done < 0 || (stop != nullptr && stop->load()) || std::chrono::steady_clock::now() >= until) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return kStopped;
#endif
}

std::string SwitchLine(bool on) {
    return on ? "Randomizer on: restart the game and start a new save to play it"
              : "Randomizer off: restart the game to play without it";
}

// ---- Rando ----

Rando::Rando(CoopMods::Loader& loader, Hooks hooks) : loader_(loader), hooks_(std::move(hooks)) {}

Rando::~Rando() {
    Wait();
}

void Rando::Say(std::string line) {
    if (hooks_.log) hooks_.log("randomizer: " + line);
    const std::lock_guard lock(mutex_);
    status_ = std::move(line);
}

void Rando::Start() {
    const fs::path folder = loader_.Root() / kFolder;
    std::error_code ec;
    if (!fs::exists(folder / CoopMods::kManifestFile, ec)) {
        fs::create_directories(folder, ec);
        if (ec || !WriteText(folder / CoopMods::kManifestFile, ManifestText())) {
            Say("could not write " + CoopMods::ToUtf8(folder / CoopMods::kManifestFile));
            return;
        }
        loader_.Refresh();
    }
    {
        // Whether this run of the game plays a seed: one of ours was indexed at its start.
        bool on_now = false;
        for (const CoopMods::Row& r : loader_.Rows()) {
            if (IsSeedFolder(r.name) && r.on_now) on_now = true;
        }
        const std::lock_guard lock(mutex_);
        on_now_ = on_said_ = on_now;
    }
    loader_.OnSetting([this](const std::string& id, const std::string&, const std::string&) {
        if (id == kId) Check();
    });
    Check();
}

void Rando::TellOthers() {
    std::string line;
    {
        const std::lock_guard lock(mutex_);
        if (tell_.empty() || !hooks_.tell_others) return;
        line = tell_;
    }
    if (!hooks_.tell_others(line)) return;  // not connected: the next Recheck tries again
    const std::lock_guard lock(mutex_);
    if (tell_ == line) tell_.clear();
}

std::vector<std::string> Rando::TakeLines() {
    const std::lock_guard lock(mutex_);
    return std::exchange(lines_, {});
}

bool Rando::Building() const {
    const std::lock_guard lock(mutex_);
    return building_;
}

std::string Rando::Status() const {
    const std::lock_guard lock(mutex_);
    return status_;
}

void Rando::Wait() {
    // Not from the worker itself. A build started while waiting is waited for too.
    for (;;) {
        std::thread t;
        {
            const std::lock_guard lock(mutex_);
            t = std::move(worker_);
        }
        if (!t.joinable()) return;
        t.join();
    }
}

Want Rando::Wanted(bool* seed_locked) const {
    Want want;
    for (const CoopMods::SettingRow& s : loader_.Settings(kFolder)) {
        if (s.setting.key == "on") want.on = s.value == "1";
        if (s.setting.key == "seed") {
            want.seed = s.value;
            if (seed_locked != nullptr) *seed_locked = s.locked;
        }
        if (s.setting.key == "preset") want.preset = s.value;
    }
    // A guest plays the host's world: a host whose randomizer is off or missing sends no switch, so ours is off too
    // (our own value is kept for a game alone).
    if (want.on && loader_.HostValuesSet()) {
        bool host_sets = false;
        for (const CoopMods::SettingRow& s : loader_.Settings(kFolder)) {
            if (s.setting.key == "on") host_sets = s.locked;
        }
        if (!host_sets) want.on = false;
    }
    return want;
}

void Rando::Switch(const Plan& plan) {
    for (const std::string& name : plan.off) loader_.SetOn(name, false);
    for (const std::string& name : plan.on) {
        loader_.SetOn(name, true);
        loader_.Move(name, -100000);  // first row: its size table wins
    }
    if (!plan.on.empty() || !plan.off.empty()) {
        Say(plan.why + ": start the game again to play it");
    } else if (!plan.why.empty()) {
        Say(plan.why);
    }
}

void Rando::Check() {
    bool seed_locked = false;
    const Want want = Wanted(&seed_locked);
    {
        // Said once per change of the switch, in the tab until the restart and in the chat.
        const std::lock_guard lock(mutex_);
        // Back as this run started (a guest restarted on the host's seed, then joined): nothing to restart for.
        if (want.on != on_said_) {
            on_said_ = want.on;
            if (want.on != on_now_) {
                lines_.push_back(SwitchLine(want.on));
                if (hooks_.log) hooks_.log("randomizer: " + lines_.back());
            }
        }
    }
    loader_.NoteChanged(std::string(kFolder) + "/switch", want.on == on_now_ ? std::string() : SwitchLine(want.on));
    if (want.on && want.seed.empty() && !seed_locked) {
        // Our own seed (we host or play alone): made once, kept as our setting, sent to the guests.
        const std::string seed = NewSeed(hooks_.entropy ? hooks_.entropy() : 0x9E3779B97F4A7C15ull);
        if (loader_.Set(kFolder, "seed", seed)) {
            return;  // the change calls Check again
        }
    }
    {
        const std::lock_guard lock(mutex_);
        if (building_) {
            again_ = true;  // the worker looks again when its build ends
            return;
        }
    }
    const Plan plan = Decide(want, Scan(loader_.Root()), loader_.Rows());
    if (plan.what != Plan::Do::build) {
        Switch(plan);
        return;
    }
    std::thread old;
    {
        const std::lock_guard lock(mutex_);
        if (building_) {
            again_ = true;
            return;
        }
        if (plan.folder == last_failed_) {
            return;  // said already; a change of the settings (or a restart) tries again
        }
        building_ = true;
        again_ = false;
        old = std::move(worker_);  // a worker that has ended
    }
    if (old.joinable()) old.join();
    const std::lock_guard lock(mutex_);
    worker_ = std::thread([this, plan, want] { Work(plan, want); });
}

void Rando::Work(Plan plan, Want want) {
    for (;;) {
        if (plan.what == Plan::Do::build) {
            Build(plan, want);
            loader_.Refresh();
        } else {
            Switch(plan);
        }
        // What next: the settings as they are now. A change that came meanwhile set again_ (its Check saw a worker).
        for (;;) {
            want = Wanted(nullptr);
            plan = Decide(want, Scan(loader_.Root()), loader_.Rows());
            const std::lock_guard lock(mutex_);
            const bool failed = plan.what == Plan::Do::build && plan.folder == last_failed_;
            if (!failed && plan.what != Plan::Do::nothing) {
                again_ = false;
                break;
            }
            if (!again_) {
                building_ = false;
                return;
            }
            again_ = false;
        }
    }
}

// One seed: generate, build, check. A failure is said and remembered, so the same settings are not built again in a
// loop (a change of the settings tries again).
void Rando::Build(const Plan& plan, const Want& want) {
    const fs::path root = loader_.Root();
    const fs::path control = root / kFolder;
    const fs::path log = control / kLogFile;
    // The host's seed (a guest): a failure is also said in the chat and told to the others, so the host knows which
    // player plays without it and why (the chat line carries the player's name).
    bool hosts_seed = false;
    if (loader_.HostValuesSet()) {
        for (const CoopMods::SettingRow& s : loader_.Settings(kFolder)) {
            if (s.setting.key == "seed") hosts_seed = s.locked;
        }
    }
    const auto fail = [&](std::string line) {
        {
            const std::lock_guard lock(mutex_);
            last_failed_ = plan.folder;
            if (hosts_seed) {
                // (sent, the line shows in this game's chat too, as "you: ...")
                tell_ = TellLine(line);
                if (!hooks_.tell_others) lines_.push_back(tell_);
            }
        }
        Say(std::move(line));
        TellOthers();
    };
    const auto exit_text = [](int code) {
        return code == kStopped ? std::string("stopped: it ran too long or the game closed")
                                : "exit " + std::to_string(code);
    };
    std::string ootrr_setting, game_setting;
    for (const CoopMods::SettingRow& s : loader_.Settings(kFolder)) {
        if (s.setting.key == "ootrr") ootrr_setting = s.value;
        if (s.setting.key == "game_folder") game_setting = s.value;
    }
    const auto ootrr = FindOotrr(ootrr_setting, hooks_.game_dir, root);
    if (!ootrr) {
        return fail(ootrr_setting.empty()
                        ? std::string("OOTRR (the randomizer the kit brings) is not in tools/ootrr beside the game: "
                                      "start the game from the kit again (its setup downloads it)")
                        : "OOTRR not found at \"" + ootrr_setting + "\" (the OOTRR program setting): empty it to use "
                                                                    "the kit's own");
    }
    const auto game = FindGameFolder(game_setting, hooks_.game_dir);
    if (!game) {
        return fail(game_setting.empty()
                        ? std::string("the game's files (romfs beside the game, with Pack and System) are not there: "
                                      "build the kit's game again (Build-Hookshot-Coop)")
                        : "no game files (Pack and System) in \"" + game_setting +
                              "\" (the Game folder setting): empty it to use the kit's own");
    }
    for (const CoopMods::Row& r : loader_.Rows()) {
        if (CoopMods::NameSame(r.name, plan.folder) && r.on_now) {
            return fail(plan.folder + " is in use by this game and not a finished seed: start the game again first");
        }
    }
    std::error_code ec;
    const fs::path target = root / CoopMods::FromUtf8(plan.folder);
    fs::remove_all(target, ec);  // a build cut off before: not in use (checked above)
    fs::remove(log, ec);
    Say(plan.why + " (about a minute)");
    const fs::path seed_file = control / (plan.folder + ".ootrr");
    const int made = hooks_.run(*ootrr, GenerateArgs(want.preset, want.seed, seed_file), log);
    if (made != 0) {
        return fail("OOTRR could not make the seed (" + exit_text(made) + "): see " + CoopMods::ToUtf8(log));
    }
    const int built = hooks_.run(*ootrr, BuildArgs(seed_file, *game, root, plan.folder), log);
    if (built != 0 || !fs::is_regular_file(target / kSeedFile, ec)) {
        fs::remove_all(target, ec);
        std::ifstream in(log, std::ios::binary);
        std::ostringstream out;
        out << in.rdbuf();
        const std::string hint = BuildFailureHint(out.str());
        return fail("OOTRR could not build the seed (" + exit_text(built) + "): " +
                    (hint.empty() ? "see " + CoopMods::ToUtf8(log) : hint));
    }
    WriteText(target / kPresetFile, (want.preset.empty() ? std::string("default") : want.preset) + "\n");
    {
        const std::lock_guard lock(mutex_);
        last_failed_.clear();
    }
    loader_.NoteChanged(kFolder, "Randomizer seed " + SeedWord(want.seed) + " is ready: start the game again to play it.");
}

}  // namespace CoopRando
