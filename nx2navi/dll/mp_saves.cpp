// SPDX-License-Identifier: GPL-3.0-or-later
#include "mp_saves.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>

#include "overlay_model.hpp"

namespace CoopNx2navi::MpSaves {
namespace {

namespace fs = std::filesystem;

fs::path PathOf(std::string_view utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string Utf8Of(const fs::path& path) {
    const std::u8string u = path.u8string();
    return std::string(u.begin(), u.end());
}

bool Forbidden(unsigned char c) {
    return c < 0x20 || c == 0x7f || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
           c == '"' || c == '<' || c == '>' || c == '|';
}

bool SlotName(std::string_view name) {
    return name.size() == 7 && name.substr(0, 5) == "slot_" && name[5] >= '0' && name[5] <= '9' &&
           name[6] >= '0' && name[6] <= '9';
}

// One name under the root and nothing else: no separator, no "." or "..".
bool OneFolder(std::string_view folder) {
    if (folder.empty() || folder == "." || folder == ".." || folder.back() == ' ' ||
        folder.back() == '.') {
        return false;
    }
    return std::none_of(folder.begin(), folder.end(),
                        [](char c) { return Forbidden(static_cast<unsigned char>(c)); });
}

std::int64_t Seconds(fs::file_time_type time) {
    // The file clock's epoch is not the system clock's everywhere: by the
    // distance to now on both.
    const auto system = std::chrono::system_clock::now() +
                        std::chrono::duration_cast<std::chrono::system_clock::duration>(
                            time - fs::file_time_type::clock::now());
    return std::chrono::duration_cast<std::chrono::seconds>(system.time_since_epoch()).count();
}

std::string ReadWhole(const fs::path& path, std::size_t most) {
    std::ifstream in(path, std::ios::binary);
    std::string text;
    if (in) {
        text.resize(most);
        in.read(text.data(), static_cast<std::streamsize>(most));
        text.resize(static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0)));
    }
    return text;
}

// The account folders of one save folder: <save>\save\<account>.
std::vector<fs::path> Accounts(const fs::path& save_root) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(save_root / "save", ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code dec;
        if (it->is_directory(dec)) {
            out.push_back(it->path());
        }
    }
    return out;
}

// The newest slot of an account folder, by its progress.sav.
std::optional<fs::path> NewestSlot(const fs::path& account_dir, fs::file_time_type* when = nullptr,
                                   unsigned* count = nullptr) {
    std::optional<fs::path> best;
    fs::file_time_type best_time{};
    std::error_code ec;
    for (fs::directory_iterator it(account_dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code dec;
        if (!it->is_directory(dec) || !SlotName(Utf8Of(it->path().filename()))) {
            continue;
        }
        const auto time = fs::last_write_time(it->path() / "progress.sav", dec);
        if (dec) {
            continue;
        }
        if (count != nullptr) {
            ++*count;
        }
        if (!best || time > best_time) {
            best = it->path();
            best_time = time;
        }
    }
    if (best && when != nullptr) {
        *when = best_time;
    }
    return best;
}

bool CopyOne(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (!fs::is_regular_file(from, ec)) {
        return true;  // nothing to copy
    }
    // A copy of its own: the runtime keeps a save's files as hard links between
    // its folders, and a link here would let a write to one reach the other.
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return false;
    }
    // The save's own date ("last played"), whatever the copy made of it.
    std::error_code tec;
    const auto time = fs::last_write_time(from, tec);
    if (!tec) {
        fs::last_write_time(to, time, tec);
    }
    return true;
}

// `inner` is `outer` or lies in it.
bool Inside(const fs::path& inner, const fs::path& outer) {
    if (inner.empty() || outer.empty()) {
        return false;
    }
    std::error_code ec;
    const fs::path in = fs::weakly_canonical(inner, ec).lexically_normal();
    const fs::path out = fs::weakly_canonical(outer, ec).lexically_normal();
    const auto rel = in.lexically_relative(out);
    return !rel.empty() && *rel.begin() != "..";
}

bool Taken(const fs::path& root, std::string_view name) {
    const std::string want = Folded(name);
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (Folded(Utf8Of(it->path().filename())) == want) {
            return true;
        }
    }
    return false;
}

// A folder that is a multiplayer save: it has a save folder in it, or nothing at all.
bool LooksLikeSave(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return false;
    }
    if (fs::is_directory(dir / "save", ec)) {
        return true;
    }
    return fs::is_empty(dir, ec) && !ec;
}

}  // namespace

fs::path Root(const fs::path& mod_folder) {
    return mod_folder / std::string(kRootName);
}

std::string Folded(std::string_view name) {
    std::string out(name);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

bool ValidName(std::string_view name) {
    if (name.empty() || name.front() == ' ' || name.back() == ' ' || name.back() == '.' ||
        name.front() == '.') {
        return false;
    }
    for (const char c : name) {
        if (Forbidden(static_cast<unsigned char>(c)) || c == '@') {
            return false;
        }
    }
    const std::size_t count = Ui::Utf8Count(name);
    if (count < 1 || count > kMaxName) {
        return false;
    }
    std::string base = Folded(name);
    if (base == kDeletedName) {
        return false;
    }
    if (const auto dot = base.find('.'); dot != std::string::npos) {
        base.resize(dot);  // "con.txt" is the device too
    }
    for (const std::string_view reserved : {"con", "prn", "aux", "nul"}) {
        if (base == reserved) {
            return false;
        }
    }
    if (base.size() == 4 && (base.rfind("com", 0) == 0 || base.rfind("lpt", 0) == 0) &&
        base[3] >= '0' && base[3] <= '9') {
        return false;
    }
    return true;
}

std::string ReceivedFolder(std::string_view host_save_name, std::string_view host_name) {
    const auto clean = [](std::string_view text) {
        std::string out;
        for (const char c : text) {
            out.push_back(Forbidden(static_cast<unsigned char>(c)) || c == '@' ? '_' : c);
        }
        if (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
            out.back() = '_';  // Windows drops a folder name's last space or dot
        }
        while (!out.empty() && out.front() == ' ') {
            out.erase(out.begin());
        }
        return out;
    };
    std::string host = clean(host_name);
    if (host.empty()) {
        host = "host";
    }
    return clean(host_save_name) + "@" + host;
}

NameParts PartsOf(std::string_view folder) {
    NameParts parts;
    const auto at = folder.rfind('@');
    if (at == std::string_view::npos) {
        parts.name = std::string(folder);
        return parts;
    }
    parts.received = true;
    parts.name = std::string(folder.substr(0, at));
    parts.host = std::string(folder.substr(at + 1));
    return parts;
}

std::string FolderOfSaveDir(const fs::path& save_dir) {
    // <...>\coop-saves\<folder>\save\<account>
    fs::path p = save_dir.lexically_normal();
    if (!p.has_filename()) {
        p = p.parent_path();  // a trailing separator
    }
    const fs::path save = p.parent_path();
    const fs::path folder = save.parent_path();
    const fs::path root = folder.parent_path();
    if (p.empty() || Utf8Of(save.filename()) != "save" || Utf8Of(root.filename()) != kRootName) {
        return {};
    }
    return Utf8Of(folder.filename());
}

std::string NameOfSaveDir(const fs::path& save_dir) {
    return PartsOf(FolderOfSaveDir(save_dir)).name;
}

std::string PlaceOf(std::string_view bytes) {
    const auto word = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    };
    for (std::size_t slash = bytes.find('/'); slash != std::string_view::npos;
         slash = bytes.find('/', slash + 1)) {
        std::size_t before = slash;
        while (before > 0 && word(static_cast<unsigned char>(bytes[before - 1]))) {
            --before;
        }
        std::size_t after = slash + 1;
        while (after < bytes.size() && word(static_cast<unsigned char>(bytes[after]))) {
            ++after;
        }
        // "HyruleField/HyruleField_Adult": the stage, then the scene, which
        // starts with the stage's name. A byte of the file's own before the
        // text may be a letter too (a length), so the stage is the longest end
        // of what stands before the slash that the scene starts with.
        const std::string_view scene = bytes.substr(slash + 1, after - slash - 1);
        for (std::size_t from = before; slash - from >= 3 && scene.size() <= 64; ++from) {
            const std::string_view stage = bytes.substr(from, slash - from);
            if (scene.size() >= stage.size() && scene.substr(0, stage.size()) == stage) {
                return std::string(scene);
            }
        }
    }
    return {};
}

std::string DateText(const std::tm& t, bool french) {
    char text[32];
    if (french) {
        std::snprintf(text, sizeof(text), "%02d/%02d/%04d %02d:%02d", t.tm_mday, t.tm_mon + 1,
                      t.tm_year + 1900, t.tm_hour, t.tm_min);
    } else {
        std::snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d", t.tm_year + 1900, t.tm_mon + 1,
                      t.tm_mday, t.tm_hour, t.tm_min);
    }
    return text;
}

std::string DateText(std::int64_t seconds, bool french) {
    if (seconds <= 0) {
        return {};
    }
    const std::time_t time = static_cast<std::time_t>(seconds);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &time) != 0) {
        return {};
    }
#else
    if (localtime_r(&time, &local) == nullptr) {
        return {};
    }
#endif
    return DateText(local, french);
}

std::vector<Entry> List(const fs::path& root) {
    struct Found {
        Entry entry;
        std::int64_t made = 0;
    };
    std::vector<Found> found;
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code dec;
        const std::string folder = Utf8Of(it->path().filename());
        if (!it->is_directory(dec) || folder == kDeletedName || !LooksLikeSave(it->path())) {
            continue;
        }
        Found f;
        f.entry.folder = folder;
        const NameParts parts = PartsOf(folder);
        f.entry.name = parts.name;
        f.entry.host = parts.host;
        f.entry.received = parts.received;
        fs::file_time_type newest{};
        std::optional<fs::path> slot;
        for (const fs::path& account : Accounts(it->path())) {
            fs::file_time_type when{};
            unsigned count = 0;
            const auto s = NewestSlot(account, &when, &count);
            f.entry.slots += count;
            if (s && (!slot || when > newest)) {
                slot = s;
                newest = when;
            }
        }
        if (slot) {
            f.entry.empty = false;
            f.entry.played = Seconds(newest);
            f.entry.place = PlaceOf(ReadWhole(*slot / "caption.sav", 1u << 16));
        }
        const auto made = fs::last_write_time(it->path(), dec);
        f.made = dec ? 0 : Seconds(made);
        found.push_back(std::move(f));
    }
    std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
        if (a.entry.empty != b.entry.empty) {
            return !a.entry.empty;
        }
        if (!a.entry.empty && a.entry.played != b.entry.played) {
            return a.entry.played > b.entry.played;
        }
        if (a.entry.empty && a.made != b.made) {
            return a.made > b.made;
        }
        return a.entry.folder < b.entry.folder;
    });
    std::vector<Entry> out;
    for (Found& f : found) {
        out.push_back(std::move(f.entry));
    }
    return out;
}

std::optional<Entry> Find(const fs::path& root, std::string_view folder) {
    for (Entry& e : List(root)) {
        if (e.folder == folder) {
            return std::move(e);
        }
    }
    return std::nullopt;
}

std::string FreeName(const std::vector<Entry>& saves, std::string_view base) {
    for (unsigned n = 1; n < 1000; ++n) {
        const std::string name = n == 1 ? std::string(base) : std::string(base) + " " + std::to_string(n);
        const std::string folded = Folded(name);
        if (std::none_of(saves.begin(), saves.end(),
                         [&](const Entry& e) { return Folded(e.folder) == folded; })) {
            return name;
        }
    }
    return std::string(base);
}

bool Create(const fs::path& root, std::string_view name, std::string_view account,
            const fs::path& own_save_dir, std::string& error) {
    if (!ValidName(name)) {
        error = "not a name for a save";
        return false;
    }
    if (root.empty() || account.empty() || !OneFolder(account)) {
        error = "the account or the saves folder is not known";
        return false;
    }
    if (Inside(root, own_save_dir)) {
        error = "the multiplayer saves would lie in the player's own save";
        return false;
    }
    std::error_code ec;
    fs::create_directories(root, ec);
    if (Taken(root, name)) {
        error = "a save of that name is there already";
        return false;
    }
    const fs::path target = root / PathOf(name) / "save" / PathOf(account);
    fs::create_directories(target, ec);
    if (ec) {
        error = "cannot make " + Utf8Of(target);
        return false;
    }
    // The player's own settings; no slot: the game has nothing to continue.
    if (!own_save_dir.empty()) {
        CopyOne(own_save_dir / "account.sav", target / "account.sav");
        CopyOne(own_save_dir / "option.sav", target / "option.sav");
    }
    return true;
}

bool CopySolo(const fs::path& root, std::string_view name, const fs::path& own_save_dir,
              std::string& error) {
    if (!ValidName(name)) {
        error = "not a name for a save";
        return false;
    }
    const std::string account = Utf8Of(own_save_dir.filename());
    if (root.empty() || own_save_dir.empty() || account.empty()) {
        error = "the player's own save folder is not known";
        return false;
    }
    if (!FolderOfSaveDir(own_save_dir).empty() || Inside(own_save_dir, root)) {
        error = "that folder is a multiplayer save's, not the player's own";
        return false;
    }
    if (Inside(root, own_save_dir)) {
        error = "the multiplayer saves would lie in the player's own save";
        return false;
    }
    const auto slot = NewestSlot(own_save_dir);
    if (!slot) {
        error = "the player has no save of their own yet";
        return false;
    }
    std::error_code ec;
    fs::create_directories(root, ec);
    if (Taken(root, name)) {
        error = "a save of that name is there already";
        return false;
    }
    const fs::path save = root / PathOf(name);
    const fs::path target = save / "save" / PathOf(account);
    const fs::path slot_dir = target / slot->filename();
    fs::create_directories(slot_dir, ec);
    if (ec) {
        error = "cannot make " + Utf8Of(slot_dir);
        return false;
    }
    bool ok = CopyOne(own_save_dir / "account.sav", target / "account.sav") &&
              CopyOne(own_save_dir / "option.sav", target / "option.sav");
    for (fs::directory_iterator it(*slot, ec), end; ok && !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (it->is_regular_file(fec)) {
            ok = CopyOne(it->path(), slot_dir / it->path().filename());
        }
    }
    if (!ok || ec || !fs::is_regular_file(slot_dir / "progress.sav", ec)) {
        fs::remove_all(save, ec);  // nothing half made is left in the list
        error = "cannot copy " + Utf8Of(*slot);
        return false;
    }
    return true;
}

bool Rename(const fs::path& root, std::string_view folder, std::string_view name,
            const fs::path& in_use, std::string& error) {
    if (!OneFolder(folder) || folder == kDeletedName || PartsOf(folder).received) {
        error = "not a save of the player's own";
        return false;
    }
    if (!ValidName(name)) {
        error = "not a name for a save";
        return false;
    }
    const fs::path from = root / PathOf(folder);
    if (!LooksLikeSave(from)) {
        error = "no such save";
        return false;
    }
    if (Inside(in_use, from)) {
        error = "the game runs on that save";
        return false;
    }
    if (folder == name) {
        return true;
    }
    // Another spelling of the same name ("zelda" to "Zelda") is the same folder to Windows.
    if (Folded(folder) != Folded(name) && Taken(root, name)) {
        error = "a save of that name is there already";
        return false;
    }
    std::error_code ec;
    fs::rename(from, root / PathOf(name), ec);
    if (ec) {
        error = "cannot rename " + Utf8Of(from);
        return false;
    }
    return true;
}

bool Remove(const fs::path& root, std::string_view folder, const fs::path& in_use, std::string& error) {
    if (root.empty() || !OneFolder(folder) || folder == kDeletedName) {
        error = "not a save";
        return false;
    }
    const fs::path from = root / PathOf(folder);
    if (!LooksLikeSave(from)) {
        error = "no such save";
        return false;
    }
    if (Inside(in_use, from)) {
        error = "the game runs on that save";
        return false;
    }
    std::error_code ec;
    const fs::path bin = root / std::string(kDeletedName);
    fs::create_directories(bin, ec);
    const fs::path to = bin / PathOf(folder);
    fs::remove_all(to, ec);  // an earlier one of that name
    ec.clear();
    fs::rename(from, to, ec);
    if (ec) {
        error = "cannot move " + Utf8Of(from) + " to " + Utf8Of(to);
        return false;
    }
    return true;
}

}  // namespace CoopNx2navi::MpSaves
