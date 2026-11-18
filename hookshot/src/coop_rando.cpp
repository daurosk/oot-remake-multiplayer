// coop_rando.h: the rules first, then Rando, which acts on them with one
// worker thread at a time.
#include "coop_rando.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
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

std::string ReadAll(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

bool WriteText(const fs::path& file, const std::string& text) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    return static_cast<bool>(out << text) && static_cast<bool>(out.flush());
}

// The file's text, or "" when it cannot be read (a missing file: the caller writes it).
std::string ReadFileText(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
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

// Each choice is {"value", "name", "name_zh-Hans", "name_zh-Hant"}: the value is
// what OOTRR is given and what the folder is named after; the names are what the Mods
// tab shows, the loader picking the one for the player's language (TextIn in
// coop_mod_library.cpp). A preset the installed OOTRR cannot build is not offered, so
// its line is left out here and the table's line for it is simply unused.
std::string PresetNames(const std::string& value) {
    static constexpr const char* kValues[] = {"default", "easy", "open_world", "keysanity", "full_shuffle"};
    static constexpr const char* kNames[] = {"Default", "Easy", "Open world", "Keysanity", "Full shuffle"};
    static constexpr const char* kNamesHans[] = {"默认", "简单", "开放世界", "钥匙乱序", "完全乱序"};
    static constexpr const char* kNamesHant[] = {"預設", "簡單", "開放世界", "鑰匙亂序", "完全亂序"};
    for (std::size_t i = 0; i < sizeof kValues / sizeof kValues[0]; ++i) {
        if (value == kValues[i]) {
            std::string out = "{\"value\": \"" + value + "\", \"name\": \"" + kNames[i];
            out += "\", \"name_zh-Hans\": \"" + std::string(kNamesHans[i]);
            out += "\", \"name_zh-Hant\": \"" + std::string(kNamesHant[i]) + "\"}";
            return out;
        }
    }
    return "\"" + value + "\"";
}

std::string ManifestText() { return ManifestText(std::vector<std::string>(std::begin(kPresets), std::end(kPresets))); }

std::string ManifestText(const std::vector<std::string>& offered) {
    std::string presets = PresetNames("default");
    for (const std::string& p : offered) {
        if (p != "default") presets += ", " + PresetNames(p);
    }
    return std::string(R"({
  "id": "oot-remake-randomizer",
  "name": "OoT Remake Randomizer",
  "name_zh-Hans": "OoT 重制版随机模式",
  "name_zh-Hant": "OoT 重製版隨機模式",
  "version": "1",
  "author": "OOTRR by REALAZK (GPL-3.0-or-later), run by the co-op mod loader",
  "author_zh-Hans": "OOTRR by REALAZK（GPL-3.0-or-later），由联机模组加载器运行",
  "author_zh-Hant": "OOTRR by REALAZK（GPL-3.0-or-later），由連線模組載入器執行",
  "description": "Shuffles chests, dungeon items and rewards (OOTRR). In co-op everyone plays the host's seed and every pickup is shared. A new seed needs a restart of the game, never a game build.",
  "description_zh-Hans": "打乱宝箱、迷宫道具和奖励（OOTRR）。联机时所有人玩房主的种子，拾取共享。换种子只需重启游戏，不用重新构建。",
  "description_zh-Hant": "打亂寶箱、迷宮道具和獎勵（OOTRR）。連線時所有人玩房主的種子，拾取共享。換種子只需重新啟動遊戲，不用重新建置。",
  "coop": "everyone",
  "settings": [
    {"key": "on", "name": "Randomizer", "type": "toggle", "shared": true, "restart": true,
     "name_zh-Hans": "随机模式", "name_zh-Hant": "隨機模式",
     "help": "On: the game plays a randomized seed (start a new save for it).",
     "help_zh-Hans": "开启：游戏按随机种子运行（需要开一个新存档）。",
     "help_zh-Hant": "開啟：遊戲按隨機種子執行（需要開一個新存檔）。"},
    {"key": "seed", "name": "Seed", "type": "text", "max_length": 63, "shared": true, "restart": true,
     "name_zh-Hans": "种子", "name_zh-Hant": "種子",
     "help": "Letters and digits. Empty: a new one is made. In co-op the host's seed is everyone's.",
     "help_zh-Hans": "字母和数字。留空：自动生成。联机时所有人用房主的种子。",
     "help_zh-Hant": "字母和數字。留空：自動產生。連線時所有人用房主的種子。"},
    {"key": "preset", "name": "Settings", "type": "choice", "choices": [)") +
           presets + R"(], "shared": true, "restart": true,
     "name_zh-Hans": "设定", "name_zh-Hant": "設定",
     "help": "OOTRR's presets: only the ones the installed OOTRR can build are offered.",
     "help_zh-Hans": "OOTRR 的预设：只列出已安装的 OOTRR 能构建的那些。",
     "help_zh-Hant": "OOTRR 的預設：只列出已安裝的 OOTRR 能建置的那些。"},
    {"key": "settings", "name": "Settings string", "type": "text", "max_length": 400, "shared": true, "restart": true,
     "name_zh-Hans": "设定字符串", "name_zh-Hant": "設定字串",
     "help": "Optional: a settings string from OOTRR's own page (OOTRR1-...), used in place of Settings. In co-op the host's is everyone's.",
     "help_zh-Hans": "可选：OOTRR 官网的设定字符串（OOTRR1-...），代替上面的设定。联机时所有人用房主的。",
     "help_zh-Hant": "選填：OOTRR 官網的設定字串（OOTRR1-...），取代上面的設定。連線時所有人用房主的。"},
    {"key": "game_folder", "name": "Game folder", "type": "text", "max_length": 260,
     "name_zh-Hans": "游戏文件夹", "name_zh-Hant": "遊戲資料夾",
     "help": "Only for other game files (1.1.1, with Pack, Banc, Event and System). Empty: the kit's own (romfs beside the game).",
     "help_zh-Hans": "只在用别的游戏文件时填（1.1.1，含 Pack、Banc、Event 和 System）。留空：用工具包自带的（游戏旁边的 romfs）。",
     "help_zh-Hant": "只在用別的遊戲檔案時填（1.1.1，含 Pack、Banc、Event 和 System）。留空：用工具包自帶的（遊戲旁邊的 romfs）。"},
    {"key": "ootrr", "name": "OOTRR program", "type": "text", "max_length": 260,
     "name_zh-Hans": "OOTRR 程序", "name_zh-Hant": "OOTRR 程式",
     "help": "Only for another OOTRR. Empty: the one the kit brings (tools\\ootrr beside the game).",
     "help_zh-Hans": "只在用别的 OOTRR 时填。留空：用工具包自带的（游戏旁边的 tools\\ootrr）。",
     "help_zh-Hant": "只在用別的 OOTRR 時填。留空：用工具包自帶的（遊戲旁邊的 tools\\ootrr）。"}
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

bool IsSettingsString(std::string_view text) {
    if (text.size() <= 5 || text.size() > kSettingsMax || text.substr(0, 5) != "OOTRR") return false;
    return std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

std::string SettingsString(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    text = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    return IsSettingsString(text) ? std::string(text) : std::string();
}

std::string SettingsName(std::string_view preset) {
    if (!IsSettingsString(preset)) return std::string(preset.empty() ? "default" : preset);
    return "settings " + std::string(preset.substr(0, 16)) + (preset.size() > 16 ? "..." : "");
}

std::string FolderFor(std::string_view seed, std::string_view preset) {
    std::string out = std::string(kSeedPrefix) + SeedWord(seed);
    if (IsSettingsString(preset)) {
        std::uint32_t hash = 0x811c9dc5u;  // FNV-1a
        for (const char c : preset) hash = (hash ^ static_cast<unsigned char>(c)) * 0x01000193u;
        char hex[9];
        std::snprintf(hex, sizeof hex, "%08x", static_cast<unsigned>(hash));
        out += "-s" + std::string(hex);
    } else if (!preset.empty() && preset != "default") {
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
        out.push_back({name, ReadLine(seed_file), ReadLine(it->path() / kPresetFile), ReadAll(it->path() / kTablesFile),
                       fs::is_regular_file(it->path() / kSettingsFile, ec)});
    }
    std::sort(out.begin(), out.end(), [](const Built& a, const Built& b) { return CoopMods::NameLess(a.name, b.name); });
    return out;
}

Plan Decide(const Want& want, const std::vector<Built>& built, const std::vector<CoopMods::Row>& rows,
            const std::optional<std::string>& tables) {
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
        plan.why = "seed " + SeedWord(want.seed) + " (" + SettingsName(preset) + ") is built now";
        return plan;
    }
    if (tables && have->tables != *tables) {
        plan.what = Plan::Do::refresh;
        plan.why = "seed " + SeedWord(want.seed) + " (" + SettingsName(preset) +
                   ") is made again for the other mods' size tables (the co-op puppets')";
        return plan;
    }
    if (!have->shared_files) {
        // Built before seeds carried spoiler.txt and settings.txt: made again once (the same seed: OOTRR is
        // deterministic), which writes both. settings.txt is written by every build, so this does not repeat.
        plan.what = Plan::Do::refresh;
        plan.for_tables = false;
        plan.why =
            "seed " + SeedWord(want.seed) + " (" + SettingsName(preset) + ") gets its spoiler.txt and settings.txt";
        return plan;
    }
    switch_off_all_but(plan.folder);
    // On, and the first row: its size table (which keeps the larger entry of every other mod) must win.
    const CoopMods::Row* row = RowOf(rows, plan.folder);
    if (row == nullptr || !row->on || rows.empty() || !CoopMods::NameSame(rows.front().name, plan.folder)) {
        plan.on.push_back(plan.folder);
    }
    plan.what = plan.on.empty() && plan.off.empty() ? Plan::Do::nothing : Plan::Do::use;
    plan.why = plan.what == Plan::Do::use ? "seed " + SeedWord(want.seed) + " (" + SettingsName(preset) + ") is switched on" : "";
    return plan;
}

std::vector<std::string> GenerateArgs(const std::string& preset, const std::string& seed, const fs::path& seed_file,
                                      const fs::path& spoiler) {
    std::vector<std::string> args = {"generate", preset.empty() ? "default" : preset, SeedWord(seed),
                                     CoopMods::ToUtf8(seed_file)};
    if (!spoiler.empty()) {
        args.push_back("--spoiler");
        args.push_back(CoopMods::ToUtf8(spoiler));
    }
    return args;
}

std::string SeedSettings(std::string_view text) {
    // {"format": "ootrr-seed", ..., "settings": "OOTRR1-...", ...}: the key at the top level, before "placements".
    const auto key = text.find("\"settings\"");
    if (key == std::string_view::npos) return {};
    auto at = text.find_first_not_of(" \t\r\n", key + 10);
    if (at == std::string_view::npos || text[at] != ':') return {};
    at = text.find_first_not_of(" \t\r\n", at + 1);
    if (at == std::string_view::npos || text[at] != '"') return {};
    const auto end = text.find('"', at + 1);
    if (end == std::string_view::npos) return {};
    return SettingsString(text.substr(at + 1, end - at - 1));
}

std::vector<std::string> BuildArgs(const fs::path& seed_file, const fs::path& game_folder, const fs::path& mods_root,
                                   const std::string& folder) {
    return {"build", CoopMods::ToUtf8(seed_file), CoopMods::ToUtf8(game_folder), CoopMods::ToUtf8(mods_root), folder,
            "--host", "hookshot"};
}

std::vector<fs::path> OtherSizeTables(const fs::path& mods_root, const std::vector<CoopMods::Row>& rows) {
    std::vector<fs::path> out;
    std::error_code ec;
    fs::recursive_directory_iterator it(mods_root, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (std::error_code dec; !it->is_directory(dec)) continue;
        const std::string name = CoopMods::ToUtf8(it->path().filename());
        const bool top = it.depth() == 0;
        if (name.empty() || name[0] == '-' || name[0] == '_' || it.depth() > 3 ||
            (top && (CoopMods::NameSame(name, kFolder) || IsSeedFolder(name)))) {
            it.disable_recursion_pending();
            continue;
        }
        if (top) {
            const CoopMods::Row* row = RowOf(rows, name);
            if (row != nullptr && !row->on) {
                it.disable_recursion_pending();
                continue;
            }
        }
        if (name != "romfs") continue;
        it.disable_recursion_pending();  // (a mod's own files: no mod inside)
        std::error_code fec;
        for (fs::directory_iterator files(it->path() / "System" / "Resource", fec), last; !fec && files != last;
             files.increment(fec)) {
            const fs::directory_entry& f = *files;
            const std::string file = CoopMods::ToUtf8(f.path().filename());
            const std::string_view tail = ".rsizetable.zs";
            if (f.is_regular_file(fec) && file.size() > tail.size() &&
                file.compare(file.size() - tail.size(), tail.size(), tail) == 0) {
                out.push_back(f.path());
            }
        }
    }
    return out;
}

int StageSizeTables(const fs::path& mods_root, const std::vector<CoopMods::Row>& rows, const fs::path& stage) {
    int count = 0;
    for (const fs::path& table : OtherSizeTables(mods_root, rows)) {
        const fs::path to = stage / std::to_string(count) / "romfs" / "System" / "Resource" / table.filename();
        std::error_code cec;
        fs::create_directories(to.parent_path(), cec);
        if (cec || !fs::copy_file(table, to, fs::copy_options::overwrite_existing, cec)) return -1;
        ++count;
    }
    return count;
}

namespace {
bool HasSizeTable(const fs::path& mod) {
    std::error_code ec;
    for (fs::directory_iterator files(mod / "romfs" / "System" / "Resource", ec), last; !ec && files != last;
         files.increment(ec)) {
        const std::string file = CoopMods::ToUtf8(files->path().filename());
        const std::string_view tail = ".rsizetable.zs";
        if (std::error_code fec; files->is_regular_file(fec) && file.size() > tail.size() &&
                                 file.compare(file.size() - tail.size(), tail.size(), tail) == 0) {
            return true;
        }
    }
    return false;
}

// A mod name cut for a chat line (a folder name can be long).
std::string ShortName(std::string_view mod) {
    constexpr std::size_t kMost = 48;
    if (mod.size() <= kMost) return std::string(mod);
    std::size_t cut = kMost;
    while (cut > 0 && (static_cast<unsigned char>(mod[cut]) & 0xC0) == 0x80) --cut;  // a whole UTF-8 character
    return std::string(mod.substr(0, cut)) + "...";
}
}  // namespace

std::string Fnv64Hex(std::string_view bytes) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const char c : bytes) h = (h ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
    char out[17];
    std::snprintf(out, sizeof out, "%016llx", static_cast<unsigned long long>(h));
    return out;
}

bool MergedByKit(const fs::path& mods_root, std::string_view mod, std::string* rule) {
    const fs::path folder = mods_root / CoopMods::FromUtf8(std::string(mod));
    std::string version, table, table_hash, pack_hash, why = "merged";  // (no rule line: a kit before 0.11.3)
    for (const std::string& line : LastLines(ReadAll(folder / kMergedFile), static_cast<std::size_t>(-1))) {
        std::istringstream words(line);
        std::string key;
        words >> key;
        if (key == "coop-merged") words >> version;
        else if (key == "table") words >> table >> table_hash;
        else if (key == "pack") words >> pack_hash;
        else if (key == "rule") words >> why;
    }
    if (why != "merged" && why != "contains") return false;
    if (version != "1" || table.empty() || table_hash.size() != 16 || pack_hash.size() != 16) return false;
    if (table.find_first_of("/\\") != std::string::npos || table == "." || table == "..") return false;
    const fs::path in_mod = folder / "romfs" / "System" / "Resource" / CoopMods::FromUtf8(table);
    const fs::path in_pack =
        mods_root / CoopMods::FromUtf8(kPackFolder) / "romfs" / "System" / "Resource" / CoopMods::FromUtf8(table);
    std::error_code ec;
    if (!fs::is_regular_file(in_mod, ec) || !fs::is_regular_file(in_pack, ec)) return false;
    if (Fnv64Hex(ReadAll(in_mod)) != table_hash || Fnv64Hex(ReadAll(in_pack)) != pack_hash) return false;
    if (rule != nullptr) *rule = why;
    return true;
}

namespace {
// The game writes a nested folder's name with '\' (patch 0011 counts it as '/'; the pack is one).
std::string Slashed(std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    return name;
}

// The mod whose copy of a file the game reads, among the mods `has_file` holds for; "" when none sorts over the pack.
template <typename Has>
std::string WinnerOf(const std::vector<CoopMods::Row>& rows, const std::vector<std::string>* indexed, Has has_file) {
    std::string winner;
    if (indexed != nullptr) {
        // The game's own indexing order (coop-mods.applied): the Mods list's rows from the last to the first, under
        // them the mods it does not name (a mod dropped in since; the pack, never listed) by name. The last one wins.
        for (auto it = indexed->rbegin(); it != indexed->rend() && winner.empty(); ++it) {
            if (has_file(*it)) winner = *it;
        }
    } else {
        // Hookshot's order: by name, case folded, the last wins; the pack is a mod of its own in it.
        const auto folded = [](std::string_view s) {
            std::string out(s);
            for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        };
        std::string best = "contents/0400486024dec000/coop-puppet";
        for (const CoopMods::Row& r : rows) {
            if (!r.on_now || !has_file(r.name)) continue;
            if (const std::string f = folded(r.name); f > best) {
                best = f;
                winner = r.name;
            }
        }
    }
    return winner;
}
}  // namespace

std::optional<std::string> OutsideSizeTable(const fs::path& mods_root, const std::vector<CoopMods::Row>& rows,
                                            const std::vector<std::string>* indexed, std::string* merged, std::string* rule) {
    const std::string winner = WinnerOf(rows, indexed, [&](const std::string& name) {
        return HasSizeTable(mods_root / CoopMods::FromUtf8(Slashed(name)));
    });
    if (winner.empty() || CoopMods::NameSame(Slashed(winner), kPackFolder) || IsSeedFolder(winner) ||
        CoopMods::NameSame(winner, kFolder)) {
        return std::nullopt;
    }
    if (MergedByKit(mods_root, winner, rule)) {
        if (merged != nullptr) *merged = winner;
        return std::nullopt;
    }
    return winner;
}

std::vector<std::string> PackActorTables(const fs::path& mods_root) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator files(mods_root / CoopMods::FromUtf8(kPackFolder) / "romfs" / "RSDB", ec), last;
         !ec && files != last; files.increment(ec)) {
        const std::string file = CoopMods::ToUtf8(files->path().filename());
        const std::string_view tail = ".rstbl.byml.zs";
        if (std::error_code fec; files->is_regular_file(fec) && file.size() > tail.size() &&
                                 file.compare(file.size() - tail.size(), tail.size(), tail) == 0) {
            out.push_back(file);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool RowsMergedByKit(const fs::path& mods_root, std::string_view mod, std::string_view table) {
    const fs::path folder = mods_root / CoopMods::FromUtf8(Slashed(std::string(mod)));
    const std::vector<std::string> lines = LastLines(ReadAll(folder / kRowsFile), static_cast<std::size_t>(-1));
    if (lines.empty() || lines.front() != "coop-rows 1") return false;
    for (const std::string& line : lines) {
        std::istringstream words(line);
        std::string key, name, table_hash, pack_hash;
        words >> key >> name >> table_hash >> pack_hash;
        if (key != "table" || name != table || table_hash.size() != 16 || pack_hash.size() != 16) continue;
        const fs::path in_mod = folder / "romfs" / "RSDB" / CoopMods::FromUtf8(name);
        const fs::path in_pack =
            mods_root / CoopMods::FromUtf8(kPackFolder) / "romfs" / "RSDB" / CoopMods::FromUtf8(name);
        std::error_code ec;
        if (!fs::is_regular_file(in_mod, ec) || !fs::is_regular_file(in_pack, ec)) return false;
        return Fnv64Hex(ReadAll(in_mod)) == table_hash && Fnv64Hex(ReadAll(in_pack)) == pack_hash;
    }
    return false;
}

std::optional<std::pair<std::string, std::string>> OutsideActorRows(const fs::path& mods_root,
                                                                    const std::vector<CoopMods::Row>& rows,
                                                                    const std::vector<std::string>* indexed) {
    for (const std::string& table : PackActorTables(mods_root)) {
        const std::string winner = WinnerOf(rows, indexed, [&](const std::string& name) {
            std::error_code ec;
            return fs::is_regular_file(
                mods_root / CoopMods::FromUtf8(Slashed(name)) / "romfs" / "RSDB" / CoopMods::FromUtf8(table), ec);
        });
        if (winner.empty() || CoopMods::NameSame(Slashed(winner), kPackFolder)) continue;
        if (RowsMergedByKit(mods_root, winner, table)) continue;
        return std::make_pair(winner, table);
    }
    return std::nullopt;
}

int PutBackKeptRows(const fs::path& folder) {
    int back = 0;
    const fs::path keep = folder / "_coop-rows";  // (merge_tables.py ROWS_KEEP)
    std::error_code ec;
    if (!fs::is_directory(keep, ec)) return 0;
    for (fs::directory_iterator f(keep, ec), last; !ec && f != last; f.increment(ec)) {
        const std::string name = CoopMods::ToUtf8(f->path().filename());
        const std::string_view tail = ".rstbl.byml.zs";
        if (name.size() <= tail.size() || name.compare(name.size() - tail.size(), tail.size(), tail) != 0) continue;
        std::error_code cec;
        if (!fs::copy_file(f->path(), folder / "romfs" / "RSDB" / f->path().filename(),
                           fs::copy_options::overwrite_existing, cec)) {
            return -1;  // (the kept folder stays: the kit's next start sorts it out)
        }
        ++back;
    }
    fs::remove(folder / kRowsFile, ec);
    fs::remove_all(keep, ec);
    return back;
}

std::string RowsLine(std::string_view mod, std::string_view table) {
    const std::string_view name = table.substr(0, std::min(table.find(".Product"), table.size()));
    return "Co-op: the mod \"" + ShortName(mod) + "\" has its own " + std::string(name) +
           " table, without the puppet pack's rows, so the game can't make the other players' bodies (they show as "
           "fairies). Start the game with Start Squadala (it merges the rows in), or turn the mod off.";
}

std::string OutsideLine(std::string_view mod) {
    return "Randomizer: \"" + ShortName(mod) +
           "\" is a mod with its own size table (e.g. a randomizer seed from OOTRR's site): other players can't be "
           "shown. Make the seed in Mods > OoT Remake Randomizer > Settings, or turn this mod off.";
}

std::string OutsideTellLine(std::string_view mod) {
    std::string line = "[randomizer] my mod \"" + ShortName(mod) +
                       "\" has its own size table (e.g. a seed from OOTRR's site): I can't see the others. Make the "
                       "seed in Mods > OoT Remake Randomizer > Settings.";
    if (line.size() > kTellChars) {
        std::size_t cut = kTellChars;
        while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) --cut;
        line.resize(cut);
    }
    return line;
}

std::string TablesStamp(const fs::path& mods_root, const std::vector<fs::path>& tables) {
    std::vector<std::string> lines;
    for (const fs::path& table : tables) {
        const std::string name = CoopMods::ToUtf8(table.lexically_relative(mods_root));
        std::ifstream in(table, std::ios::binary);
        if (!in) {
            lines.push_back(name + " unreadable\n");
            continue;
        }
        std::uint64_t hash = 0xcbf29ce484222325ull, length = 0;  // FNV-1a
        char buffer[1 << 16];
        while (in.read(buffer, sizeof buffer) || in.gcount() > 0) {
            for (std::streamsize i = 0; i < in.gcount(); ++i) {
                hash = (hash ^ static_cast<unsigned char>(buffer[i])) * 0x100000001b3ull;
            }
            length += static_cast<std::uint64_t>(in.gcount());
        }
        char tail[48];
        std::snprintf(tail, sizeof tail, " %llu %016llx\n", static_cast<unsigned long long>(length),
                      static_cast<unsigned long long>(hash));
        lines.push_back(name + tail);
    }
    std::sort(lines.begin(), lines.end());
    std::string out = "coop-tables 1\n";
    for (const std::string& l : lines) out += l;
    return out;
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

std::string NoSeedLine(std::string_view seed, std::string_view why) {
    return "Randomizer: NO SEED - you play the BASE GAME, not the host's seed " + SeedWord(seed) +
           " (other items in chests, bombs need the Goron Bracelet): " + std::string(why) +
           ". To fix: start the game again to try once more; if it still fails, ask the host for other Settings or "
           "send " + kLogFile + " with a report.";
}

std::vector<std::string> LastLines(std::string_view text, std::size_t n) {
    std::vector<std::string> all;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string line(text.substr(at, end - at));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t") != std::string::npos) all.push_back(std::move(line));
        at = end + 1;
    }
    if (all.size() > n) all.erase(all.begin(), all.end() - static_cast<std::ptrdiff_t>(n));
    return all;
}

std::vector<std::string> ErrorLines(std::string_view text, std::size_t n) {
    std::vector<std::string> out;
    for (std::string& line : LastLines(text, static_cast<std::size_t>(-1))) {
        std::string low = line;
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (low.find("error") != std::string::npos || low.find("cannot") != std::string::npos ||
            low.find("can't") != std::string::npos || low.find("failed") != std::string::npos) {
            out.push_back(std::move(line));
            if (out.size() == n) break;
        }
    }
    return out.empty() ? LastLines(text, n) : out;
}

PresetCache ReadPresetCache(std::string_view text) {
    PresetCache c;
    for (const std::string& line : LastLines(text, static_cast<std::size_t>(-1))) {
        const auto sp = line.find(' ');
        if (sp == std::string::npos) continue;
        const std::string a = line.substr(0, sp), b = line.substr(sp + 1);
        if (a == "ootrr") c.key = b;
        else if (b == "ok" || b == "bad") c.ok[a] = b == "ok";
    }
    return c;
}

std::string PresetCacheText(const PresetCache& cache) {
    std::string out = "ootrr " + cache.key + "\n";
    for (const auto& [preset, ok] : cache.ok) out += preset + (ok ? " ok\n" : " bad\n");
    return out;
}

std::string OotrrKey(const fs::path& ootrr) {
    std::error_code ec;
    const auto size = fs::file_size(ootrr, ec);
    if (ec) return {};
    const auto when = fs::last_write_time(ootrr, ec);
    if (ec) return {};
    return std::to_string(size) + "-" + std::to_string(when.time_since_epoch().count());
}

std::string ChatSafe(std::string line, std::size_t max_chars) {
    // No control characters (a tab, a stray carriage return, escape codes): a space each.
    for (char& c : line) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = ' ';
    }
    // The user's profile folder: ...Users\<name>\ (either slash, any case) and /home/<name>/ become <user>.
    for (const std::string_view mark : {std::string_view("users"), std::string_view("home")}) {
        std::size_t from = 0;
        for (;;) {
            std::string low = line;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const std::size_t at = low.find(mark, from);
            if (at == std::string::npos) break;
            const std::size_t name = at + mark.size() + 1;
            const auto sep = [](char c) { return c == '\\' || c == '/'; };
            const bool sep_before = at > 0 && sep(line[at - 1]);
            const bool sep_after = name - 1 < line.size() && sep(line[name - 1]);
            if (!sep_before || !sep_after || name >= line.size()) {
                from = at + 1;
                continue;
            }
            std::size_t end = name;
            while (end < line.size() && !sep(line[end]) && line[end] != '"') ++end;
            line.replace(name, end - name, "<user>");
            from = name + 6;
        }
    }
    if (line.size() > max_chars) {
        std::size_t cut = max_chars;
        while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) --cut;  // a whole UTF-8 character
        line.resize(cut);
        line += "...";
    }
    return line;
}

bool PresetRefused(int code, std::string_view output) {
    return code > 0 && output.find_first_not_of(" \t\r\n") != std::string_view::npos;
}

std::string ReadTail(const fs::path& file, std::size_t max_bytes) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const std::streamoff size = in.tellg();
    if (size < 0) return {};  // (tellg failed)
    const auto cap = static_cast<std::streamoff>(max_bytes);
    const std::streamoff from = size > cap ? size - cap : 0;
    in.seekg(from);
    std::string out(static_cast<std::size_t>(size - from), '\0');
    in.read(out.data(), static_cast<std::streamsize>(out.size()));
    out.resize(static_cast<std::size_t>(in.gcount()));
    return out;
}

std::string SeeLogLine(const std::string& head) {
    return head + ": see " + kLogFile + " in the game's mods folder, " + kFolder + " (the chat has its full path)";
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
    return on ? "Randomizer on: keep the game open until this chat says the seed is ready, then restart it and "
                "start a new save"
              : "Randomizer off: restart the game to play without it";
}

std::string ReadyLine(std::string_view seed) {
    return "Randomizer seed " + SeedWord(seed) +
           " is ready: RESTART the game now and start a NEW save (until then this game plays without it)";
}

std::string RefreshLine(std::string_view seed) {
    return "Randomizer seed " + SeedWord(seed) +
           " was made again for the co-op puppets: RESTART the game to see the other players (your save stays)";
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
    // The manifest is rewritten when its text changes, not only when it is missing:
    // the text carries the settings' names and helps in the player's language, and a
    // mod folder installed by an older build has the English one. The presets an earlier
    // try found this OOTRR cannot make (kPresetsFile) stay left out: the probe that would
    // trim them again is skipped on a cache hit. A new OOTRR is tried again before its
    // first build, and that probe writes the manifest anew.
    std::vector<std::string> offered;
    {
        const PresetCache cache = ReadPresetCache(ReadFileText(folder / kPresetsFile));
        for (const char* p : kPresets) {
            if (const auto known = cache.ok.find(p); known == cache.ok.end() || known->second) offered.emplace_back(p);
        }
    }
    const std::string manifest = ManifestText(offered);
    if (!fs::exists(folder / CoopMods::kManifestFile, ec)) {
        fs::create_directories(folder, ec);
    }
    if (std::string have = ReadFileText(folder / CoopMods::kManifestFile); have != manifest) {
        if (!WriteText(folder / CoopMods::kManifestFile, manifest)) {
            Say("could not write " + CoopMods::ToUtf8(folder / CoopMods::kManifestFile));
            return;
        }
        loader_.Refresh();
    } else if (std::ifstream in(folder / CoopMods::kManifestFile, std::ios::binary); in) {
        // A control mod written before the Settings string (#48): written again with it, offering the presets the
        // probe found (kPresetsFile), all when none was tried.
        std::ostringstream text;
        text << in.rdbuf();
        in.close();
        if (text.str().find("\"key\": \"settings\"") == std::string::npos) {
            std::ifstream cache_in(folder / kPresetsFile, std::ios::binary);
            std::ostringstream cache_text;
            cache_text << cache_in.rdbuf();
            const PresetCache cache = ReadPresetCache(cache_text.str());
            std::vector<std::string> offered;
            for (const char* p : kPresets) {
                const auto it = cache.ok.find(p);
                if (it == cache.ok.end() || it->second) offered.emplace_back(p);
            }
            if (WriteText(folder / CoopMods::kManifestFile, ManifestText(offered))) loader_.Refresh();
        }
    }
    {
        // Whether this run of the game plays a seed: one of ours was indexed at its start.
        bool on_now = false;
        std::string playing;
        for (const CoopMods::Row& r : loader_.Rows()) {
            if (IsSeedFolder(r.name) && r.on_now) {
                on_now = true;
                playing = r.name;
            }
        }
        // The seed this run plays, built beside other size tables than now: no pack actor until it is checked.
        if (on_now) {
            const std::string now = CurrentTables();
            for (const Built& b : Scan(loader_.Root())) {
                if (CoopMods::NameSame(b.name, playing) && b.tables != now) {
                    hold_pack_ = true;
                    if (hooks_.log) {
                        hooks_.log("randomizer: " + playing + " was built beside other size tables than now: the "
                                   "co-op puppets wait until it is checked (they could crash the game)");
                    }
                }
            }
        }
        // Another mod's size table wins (#48: a seed made on OOTRR's page, dropped in by hand): no pack actor this run.
        std::string merged, rule;
        const std::vector<std::string> indexed = loader_.Indexed();
        const auto outside = OutsideSizeTable(loader_.Root(), loader_.Rows(),
                                              loader_.ExeReadsList() ? &indexed : nullptr, &merged, &rule);
        if (!merged.empty() && hooks_.log) {
            hooks_.log("randomizer: the game reads the size table of the mod " + merged +
                       (rule == "contains" ? ", which the kit found holds every co-op puppet entry already"
                                           : ", which the kit merged with the co-op puppets' entries") +
                       " (" + kMergedFile + ", rule " + rule + "): puppets on");
        }
        if (outside) {
            outside_ = true;
            if (hooks_.log) {
                hooks_.log("randomizer: the game reads the size table of the mod " + *outside +
                           ", made without the co-op puppets' entries: no puppet this run (the others show as fairies)");
            }
            const std::lock_guard lock(mutex_);
            outside_mod_ = *outside;
            outside_tell_ = true;
            lines_.push_back(ChatSafe(OutsideLine(*outside), 300));
        }
        // Player report 144/145: a mod whose RSDB table (ActorInfo, GameActorInfo) wins without the pack's rows (an
        // OOTRR seed made with settings that change actors ships its own, built from the game's): the game refuses
        // every puppet create, so the others show as fairies this run and the chat says why. The kit's start merges
        // the rows in (merge_tables.py merge_rows) and marks it (kRowsFile).
        if (const auto missing = OutsideActorRows(loader_.Root(), loader_.Rows(),
                                                  loader_.ExeReadsList() ? &indexed : nullptr)) {
            rows_out_ = true;
            if (hooks_.log) {
                hooks_.log("randomizer: the game reads RSDB/" + missing->second + " of the mod " + missing->first +
                           ", without the co-op puppet pack's rows (no " + kRowsFile +
                           " from the kit's start for it): the game would refuse every puppet; the others show as "
                           "their fairy this run");
            }
            const std::lock_guard lock(mutex_);
            rows_mod_ = missing->first;
            rows_table_ = missing->second;
            lines_.push_back(ChatSafe(RowsLine(rows_mod_, rows_table_), 300));
        }
        const std::lock_guard lock(mutex_);
        on_now_ = on_said_ = on_now;
    }
    loader_.OnSetting([this](const std::string& id, const std::string&, const std::string&) {
        if (id == kId) Check();
    });
    Check();
}

std::map<std::string, bool> Rando::ProbePresets(const fs::path& ootrr, const fs::path& control) {
    const fs::path file = control / kPresetsFile;
    const std::string key = OotrrKey(ootrr);
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    in.close();
    PresetCache cache = ReadPresetCache(text.str());
    if (!key.empty() && cache.key == key && cache.ok.size() == std::size(kPresets)) return cache.ok;
    {
        // Tried in this run already (a preset left unsure is tried again at the next start, not at every build).
        const std::lock_guard lock(mutex_);
        if (!key.empty() && probed_key_ == key) return probed_;
    }
    // A new OOTRR (or none tried yet): each preset made once into a stage ('_': Longshot never loads it). Only
    // `generate` is run: OOTRR 0.5.0 refuses full_shuffle there ("error: This build cannot apply these settings
    // yet: ...", exit 1), as the kit's ootrr.exe did when tried by hand (2026-11-15).
    Say("trying OOTRR's presets once (this OOTRR is new here)");
    if (cache.key != key) cache = PresetCache{key, {}};
    const fs::path stage = control / "_preset-probe";
    std::error_code ec;
    fs::remove_all(stage, ec);
    fs::create_directories(stage, ec);
    std::vector<std::string> good;
    const auto& run = hooks_.probe_run ? hooks_.probe_run : hooks_.run;
    for (const char* p : kPresets) {
        if (const auto known = cache.ok.find(p); known != cache.ok.end()) {
            if (known->second) good.emplace_back(p);
            continue;  // known for this OOTRR (an earlier start left only some unsure)
        }
        const fs::path out = stage / (std::string(p) + ".txt");
        const int code = run(ootrr, GenerateArgs(p, "PROBE", stage / (std::string(p) + ".ootrr")), out);
        if (code == 0) {
            cache.ok[p] = true;
            good.emplace_back(p);
        } else if (PresetRefused(code, ReadTail(out, kTailBytes))) {
            cache.ok[p] = false;
            if (hooks_.log) hooks_.log(std::string("randomizer: OOTRR cannot make the ") + p + " preset: not offered");
        } else {
            // Stopped, timed out, not started, or an exit with nothing said: not known, offered, tried again next start.
            good.emplace_back(p);
            if (hooks_.log) {
                hooks_.log(std::string("randomizer: the ") + p + " preset could not be tried (exit " + std::to_string(code) +
                           "): offered, tried again at the next start");
            }
        }
    }
    fs::remove_all(stage, ec);
    if (!key.empty()) WriteText(file, PresetCacheText(cache));
    WriteText(control / CoopMods::kManifestFile, ManifestText(good));  // the Settings row (read with the next Refresh)
    loader_.Refresh();
    const std::lock_guard lock(mutex_);
    probed_key_ = key;
    probed_ = cache.ok;
    return cache.ok;
}

void Rando::TellOthers() {
    if (!hooks_.tell_others) return;
    std::string outside;
    {
        const std::lock_guard lock(mutex_);
        if (outside_tell_) outside = ChatSafe(OutsideTellLine(outside_mod_), kTellChars);
    }
    if (!outside.empty() && hooks_.tell_others(outside)) {  // (not connected: the next Recheck tries again)
        const std::lock_guard lock(mutex_);
        outside_tell_ = false;
    }
    std::string line;
    {
        const std::lock_guard lock(mutex_);
        if (tell_.empty()) return;
        line = tell_;
    }
    if (!hooks_.tell_others(line)) return;  // not connected: the next Recheck tries again
    const std::lock_guard lock(mutex_);
    if (tell_ == line) tell_.clear();
}

void Rando::SayOutside() {
    const std::lock_guard lock(mutex_);
    if (!rows_mod_.empty()) lines_.push_back(ChatSafe(RowsLine(rows_mod_, rows_table_), 300));
    if (outside_mod_.empty()) return;
    lines_.push_back(ChatSafe(OutsideLine(outside_mod_), 300));
    outside_tell_ = true;  // a player who joined since hears it too
}

std::chrono::steady_clock::time_point Rando::Now() const {
    return hooks_.now ? hooks_.now() : std::chrono::steady_clock::now();
}

void Rando::SayRestart(std::string line) {
    const auto now = Now();
    {
        const std::lock_guard lock(mutex_);
        if (line == restart_) return;  // said already (a build, then its switch): it repeats on its own
        lines_.push_back(line);
        restart_ = line;
        restart_said_ = now;
    }
    if (hooks_.log) hooks_.log("randomizer: " + line);
}

void Rando::RepeatRestart() {
    const auto now = Now();
    const std::lock_guard lock(mutex_);
    if (restart_.empty()) return;
    lines_.push_back(restart_);
    restart_said_ = now;
}

std::vector<std::string> Rando::TakeLines() {
    const auto now = Now();  // (the chat asks every frame: a restart still due comes back every kRepeat)
    const std::lock_guard lock(mutex_);
    if (!restart_.empty() && now - restart_said_ >= kRepeat) {
        lines_.push_back(restart_);
        restart_said_ = now;
    }
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
    // A settings string from OOTRR's page, when set, in place of the preset (#48); one not valid is left out (Check
    // says so).
    for (const CoopMods::SettingRow& s : loader_.Settings(kFolder)) {
        if (s.setting.key == "settings") {
            if (std::string settings = SettingsString(s.value); !settings.empty()) want.preset = std::move(settings);
        }
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

std::string Rando::CurrentTables() const {
    return TablesStamp(loader_.Root(), OtherSizeTables(loader_.Root(), loader_.Rows()));
}

void Rando::Switch(const Plan& plan) {
    for (const std::string& name : plan.off) loader_.SetOn(name, false);
    for (const std::string& name : plan.on) {
        loader_.SetOn(name, true);
        loader_.Move(name, -100000);  // first row: its size table wins
    }
    if (!plan.on.empty() || !plan.off.empty()) {
        Say(plan.why + ": start the game again to play it");
        // A seed switched on (or to the first row, where its size table wins) plays only after a restart.
        if (!plan.on.empty()) SayRestart(ReadyLine(Wanted(nullptr).seed));
    } else if (!plan.why.empty()) {
        Say(plan.why);
    }
}

void Rando::Check() {
    bool seed_locked = false;
    const Want want = Wanted(&seed_locked);
    {
        // A line for the others not sent yet is about the host's seed it named: gone with the host's values, or
        // when the host's seed changed.
        const std::lock_guard lock(mutex_);
        if (!tell_.empty() && (!seed_locked || !loader_.HostValuesSet() || want.seed != tell_seed_)) tell_.clear();
    }
    {
        // Said once per change of the switch, in the tab until the restart and in the chat.
        const std::lock_guard lock(mutex_);
        // Back as this run started (a guest restarted on the host's seed, then joined): nothing to restart for.
        if (want.on != on_said_) {
            on_said_ = want.on;
            restart_.clear();  // a restart line said before is about the other switch
            if (want.on != on_now_) {
                lines_.push_back(SwitchLine(want.on));
                if (hooks_.log) hooks_.log("randomizer: " + lines_.back());
            }
        }
    }
    loader_.NoteChanged(std::string(kFolder) + "/switch", want.on == on_now_ ? std::string() : SwitchLine(want.on));
    {
        // A settings string that is not one (#48): said once per value; the preset is used meanwhile.
        std::string bad;
        for (const CoopMods::SettingRow& s : loader_.Settings(kFolder)) {
            if (s.setting.key == "settings" && SettingsString(s.value).empty() &&
                s.value.find_first_not_of(" \t\r\n") != std::string::npos) {
                bad = s.value;
            }
        }
        const std::lock_guard lock(mutex_);
        if (bad != bad_settings_) {
            bad_settings_ = bad;
            if (!bad.empty()) {
                lines_.push_back("Randomizer: the Settings string is not one of OOTRR's (it starts with OOTRR, "
                                 "letters, digits and '-' only): Settings is used instead");
                if (hooks_.log) hooks_.log("randomizer: " + lines_.back());
            }
        }
    }
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
    const Plan plan = Decide(want, Scan(loader_.Root()), loader_.Rows(), CurrentTables());
    if (plan.what != Plan::Do::build && plan.what != Plan::Do::refresh) {
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
        if (plan.what == Plan::Do::build || plan.what == Plan::Do::refresh) {
            Build(plan, want);
            loader_.Refresh();
        } else {
            Switch(plan);
        }
        // What next: the settings as they are now. A change that came meanwhile set again_ (its Check saw a worker).
        for (;;) {
            want = Wanted(nullptr);
            plan = Decide(want, Scan(loader_.Root()), loader_.Rows(), CurrentTables());
            const std::lock_guard lock(mutex_);
            const bool failed = (plan.what == Plan::Do::build || plan.what == Plan::Do::refresh) &&
                                plan.folder == last_failed_;
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
    // `line` is for this game only (the tab and the log: it may name local folders); `reason` is what the others
    // are told, a fixed plain text with no path or setting value in it (a path can hold the Windows user name).
    const bool refresh = plan.what == Plan::Do::refresh;  // the seed stays: only its size table is taken
    const auto fail = [&](std::string line, std::string_view reason) {
        {
            const std::lock_guard lock(mutex_);
            last_failed_ = plan.folder;
            if (refresh && !plan.for_tables) {
                // The seed plays as it is: only its spoiler.txt / settings.txt are missing.
                lines_.push_back(ChatSafe("Randomizer: the seed's spoiler.txt and settings.txt could not be made "
                                          "(the seed still plays): " + line));
            } else if (refresh) {
                // The seed still plays, with its old size table: only the co-op puppets may not show.
                lines_.push_back(ChatSafe("Randomizer: the seed could not be made again for the co-op puppets (the "
                                          "other players may be invisible): " + line));
            } else if (hosts_seed) {
                // (sent, the line shows in this game's chat too, as "you: ...")
                tell_ = TellLine(reason);
                tell_seed_ = want.seed;
                if (!hooks_.tell_others) lines_.push_back(tell_);
                // and loudly here, with what to do (#59: the guests did not know they played the base game)
                lines_.push_back(ChatSafe(NoSeedLine(want.seed, line), 400));
            } else {
                // A host or a player alone: in the chat too, not only in the Mods tab.
                lines_.push_back(ChatSafe("Randomizer: no seed, this game plays without it: " + line));
            }
        }
        Say(std::move(line));
        TellOthers();
    };
    const auto exit_text = [](int code) {
        return code == kStopped ? std::string("stopped: it ran too long or the game closed")
                                : "exit " + std::to_string(code);
    };
    // OOTRR's own failure (player report: the tab cut the log's path off): the tab names the file plainly, the chat has
    // its full path (once per run) and its first error lines (#16), longshot_log.txt the path and OOTRR's last lines.
    const auto ootrr_failed = [&](const std::string& head, std::string_view reason, const std::string& output) {
        const std::string where = head + ": OOTRR's output is in " + CoopMods::ToUtf8(log);
        if (hooks_.log) {
            hooks_.log("randomizer: " + where);
            for (const std::string& l : LastLines(output, 10)) hooks_.log("randomizer: ootrr: " + l);
        }
        {
            const std::lock_guard lock(mutex_);
            if (!log_in_chat_) {
                log_in_chat_ = true;
                lines_.push_back(ChatSafe(where, 400));  // (the whole path: masked, not cut)
            }
            for (const std::string& l : ErrorLines(output, 5)) lines_.push_back(ChatSafe("OOTRR: " + l));  // what it said, each time
        }
        fail(SeeLogLine(head), reason);
    };
    const auto read_log = [&] { return ReadTail(log, kTailBytes); };
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
                                                                    "the kit's own",
                    "OOTRR not found");
    }
    const auto game = FindGameFolder(game_setting, hooks_.game_dir);
    if (!game) {
        return fail(game_setting.empty()
                        ? std::string("the game's files (romfs beside the game, with Pack and System) are not there: "
                                      "build the kit's game again (Build-Hookshot-Coop)")
                        : "no game files (Pack and System) in \"" + game_setting +
                              "\" (the Game folder setting): empty it to use the kit's own",
                    "game files not found");
    }
    bool in_use = false;
    for (const CoopMods::Row& r : loader_.Rows()) {
        if (CoopMods::NameSame(r.name, plan.folder) && r.on_now) in_use = true;
    }
    if (in_use && !refresh) {
        return fail(plan.folder + " is in use by this game and not a finished seed: start the game again first",
                    "the seed's folder is in use: a restart is needed");
    }
    // A preset this OOTRR cannot make (#16): ours goes back to default (the change builds again: Work), the host's
    // is said plainly.
    if (hooks_.probe_presets) {
        const auto ok = ProbePresets(*ootrr, control);
        const std::string preset = want.preset.empty() ? "default" : want.preset;
        if (const auto it = ok.find(preset); it != ok.end() && !it->second && preset != "default") {
            bool hosts_preset = false;
            for (const CoopMods::SettingRow& s : loader_.Settings(kFolder)) {
                if (s.setting.key == "preset") hosts_preset = s.locked;
            }
            if (hosts_preset) {
                return fail("the host's Settings (" + preset + ") are not in this OOTRR yet: ask the host for another",
                            "the host's Settings preset is not in my OOTRR yet");
            }
            const std::string note = "Randomizer Settings " + preset + " are not in this OOTRR yet: back to default.";
            if (hooks_.log) hooks_.log("randomizer: " + note);
            {
                const std::lock_guard lock(mutex_);
                lines_.push_back(note);
            }
            Say(note);
            loader_.Set(kFolder, "preset", "default");
            return;
        }
    }
    std::error_code ec;
    const fs::path target = root / CoopMods::FromUtf8(plan.folder);
    if (!refresh) fs::remove_all(target, ec);  // a build cut off before: not in use (checked above)
    fs::remove(log, ec);
    Say(plan.why + " (about a minute)");
    const fs::path seed_file = control / (plan.folder + ".ootrr");
    const fs::path spoiler = control / (plan.folder + ".spoiler.txt");  // kept beside the seed file (kSpoilerFile)
    fs::remove(spoiler, ec);  // (one an older run left is not this generate's)
    const int made = hooks_.run(*ootrr, GenerateArgs(want.preset, want.seed, seed_file, spoiler), log);
    if (made != 0) {
        const std::string head = "OOTRR could not make the seed (" + exit_text(made) + ")";
        return ootrr_failed(head, head, read_log());
    }
    // Built in a stage beside the other mods' size tables (StageSizeTables), then moved in. ('_': Longshot never
    // loads it, if a closed game leaves it behind)
    const fs::path stage = control / "_seed-build";
    fs::remove_all(stage, ec);
    const std::string stamp = TablesStamp(root, OtherSizeTables(root, loader_.Rows()));
    if (StageSizeTables(root, loader_.Rows(), stage) < 0) {
        fs::remove_all(stage, ec);
        return fail("could not copy the other mods' size tables into " + CoopMods::ToUtf8(stage),
                    "the other mods' size tables could not be copied");
    }
    const int built = hooks_.run(*ootrr, BuildArgs(seed_file, *game, stage, plan.folder), log);
    const fs::path made_at = stage / CoopMods::FromUtf8(plan.folder);
    bool table_changed = false;
    bool copied = true;
    if (built == 0 && refresh) {
        // Only the new size table(s) over the seed's: its other files are the same seed's (and the game may be
        // reading them now); Longshot opens the table at its start, so this one plays after a restart.
        const fs::path from = made_at / "romfs" / "System" / "Resource";
        std::error_code fec;
        for (fs::directory_iterator f(from, fec), last; !fec && f != last; f.increment(fec)) {
            const fs::path to = target / "romfs" / "System" / "Resource" / f->path().filename();
            if (ReadAll(f->path()) == ReadAll(to)) continue;
            std::error_code cec;
            fs::create_directories(to.parent_path(), cec);
            if (cec || !fs::copy_file(f->path(), to, fs::copy_options::overwrite_existing, cec)) copied = false;
            table_changed = true;
        }
        if (!fs::is_regular_file(made_at / kSeedFile, fec)) copied = false;
        // The kit merged the pack's rows into this seed's actor tables and raised their sizes in the table just
        // replaced (merge_tables.py merge_rows, player report 144/145): their own copies go back, so the seed's files
        // agree again (the game is not left reading a merged table with too small a size); the kit's next start
        // merges them again, and a start without the kit shows the others as fairies and says why.
        if (copied && table_changed) {
            const int back = PutBackKeptRows(target);
            if (back != 0 && hooks_.log) {
                hooks_.log("randomizer: " + plan.folder +
                           (back > 0 ? ": its own actor tables are back (the kit's next start merges the puppet rows "
                                       "in again)"
                                     : ": its own actor tables could not be put back (the kit's next start merges "
                                       "the puppet rows in again)"));
            }
        }
    } else if (built == 0) {
        fs::rename(made_at, target, ec);
    }
    fs::remove_all(stage, ec);
    if (refresh && built == 0 && !copied) {
        return fail("the seed's new size table could not be copied into " + CoopMods::ToUtf8(target),
                    "the seed's size table could not be updated");
    }
    if (built != 0 || !fs::is_regular_file(target / kSeedFile, ec)) {
        if (!refresh) fs::remove_all(target, ec);  // (a refresh keeps the seed as it was)
        const std::string output = read_log();
        const std::string hint = BuildFailureHint(output);
        const std::string head = "OOTRR could not build the seed (" + exit_text(built) + ")";
        if (hint.empty()) return ootrr_failed(head, head, output);
        return fail(head + ": " + hint, "game files not version 1.1.1");
    }
    WriteText(target / kPresetFile, (want.preset.empty() ? std::string("default") : want.preset) + "\n");
    WriteText(target / kTablesFile, stamp);
    // For the players to share (player report: no spoiler log, no settings string to post): the seed's OOTRR1-
    // string (the seed file's; the preset's name when it has none) and OOTRR's spoiler log. settings.txt is written
    // last and always, so Decide does not make this seed again for it.
    std::error_code sec;
    if (fs::is_regular_file(spoiler, sec)) {
        fs::copy_file(spoiler, target / kSpoilerFile, fs::copy_options::overwrite_existing, sec);
        if (sec && hooks_.log) {
            hooks_.log("randomizer: " + std::string(kSpoilerFile) + " not copied into " + plan.folder);
        }
    } else if (hooks_.log) {
        hooks_.log("randomizer: OOTRR wrote no spoiler log for " + plan.folder);
    }
    const std::string settings = SeedSettings(ReadAll(seed_file));
    WriteText(target / kSettingsFile, (settings.empty() ? (want.preset.empty() ? std::string("default") : want.preset)
                                                        : settings) +
                                          "\n");
    {
        const std::lock_guard lock(mutex_);
        last_failed_.clear();
        tell_.clear();  // built after all: a line not sent yet is not true any more
    }
    if (!refresh) {
        loader_.NoteChanged(kFolder,
                            "Randomizer seed " + SeedWord(want.seed) + " is ready: start the game again to play it.");
        SayRestart(ReadyLine(want.seed));
    } else if (table_changed && in_use) {
        loader_.NoteChanged(kFolder, RefreshLine(want.seed));
        SayRestart(RefreshLine(want.seed));  // (HoldPack stays: this run's table is the old one)
    } else {
        Say(plan.why + ": done");
        if (in_use && hold_pack_.exchange(false) && hooks_.log) {
            hooks_.log("randomizer: " + plan.folder + "'s size table was right: the co-op puppets are made now");
        }
    }
}

}  // namespace CoopRando
