// coop_mods.h: one mutex around the library's data; callbacks run after it
// is released.
#include "coop_mods.h"

#include <algorithm>
#include <utility>

namespace CoopMods {

namespace {

inline constexpr std::size_t kMaxReasons = 8;

bool Has(const std::vector<std::string>& names, std::string_view name) {
    return std::any_of(names.begin(), names.end(), [&](const std::string& n) { return NameSame(n, name); });
}

bool GameSees(const Mod& m) {
    return (m.has_romfs || m.has_exefs) && !m.folder_off;
}

bool Wanted(const Mod& m, const State& state) {
    return !m.folder_off && !state.IsOff(m.name);
}

// The mods in the tab's order: the state's, then the ones no list can name.
std::vector<const Mod*> InOrder(const std::vector<Mod>& mods, const State& state) {
    std::vector<const Mod*> out;
    for (const std::string& name : state.order) {
        for (const Mod& m : mods) {
            if (NameSame(m.name, name)) {
                out.push_back(&m);
                break;
            }
        }
    }
    for (const Mod& m : mods) {
        if (!m.internal && !Has(state.order, m.name)) {
            out.push_back(&m);
        }
    }
    return out;
}

std::string RestartKey(const Mod& m, const Setting& s) {
    return m.manifest.id + "\n" + s.key;
}

}  // namespace

Loader::Loader(std::filesystem::path mods_root, Hooks hooks) : root_(std::move(mods_root)), hooks_(std::move(hooks)) {}

void Loader::Say(const std::string& line) const {
    if (hooks_.log) {
        hooks_.log(line);
    }
}

std::filesystem::path Loader::Root() const {
    return root_;
}

void Loader::Start() {
    std::vector<std::string> lines;
    {
        const std::lock_guard lock(mutex_);
        mods_ = Scan(root_);
        std::vector<std::string> problems;
        state_ = Load(root_, &problems);
        Normalize(state_, mods_);
        const Applied applied = LoadApplied(root_);
        exe_reads_list_ = applied.valid && applied.pid == hooks_.pid;
        files_now_.clear();
        if (exe_reads_list_) {
            for (const auto& [name, files] : applied.on) {
                files_now_.push_back(name);
            }
        } else {
            for (const Mod& m : mods_) {  // Hookshot's own order: by name
                if (GameSees(m)) {
                    files_now_.push_back(m.name);
                }
            }
        }
        restart_values_.clear();
        for (const Mod& m : mods_) {
            for (const Setting& s : m.manifest.settings) {
                if (s.restart) {
                    restart_values_[RestartKey(m, s)] = ValueOf(m, s, state_);
                }
            }
        }
        code_now_.clear();
        changed_.clear();
        started_ = true;
        std::string problem;
        if (!SaveLocked(&problem)) {
            problems.push_back(problem);
        }
        lines.push_back(std::to_string(mods_.size()) + " mods in " + ToUtf8(root_) +
                        (exe_reads_list_ ? "" : "; this hookshot.exe does not read the Mods tab's list (patch 0011): "
                                                "Hookshot's own order, no switches"));
        for (const Mod* m : InOrder(mods_, state_)) {
            std::string line = m->name + " (" + m->manifest.id + " " + m->manifest.version + "): ";
            line += Wanted(*m, state_) ? "on" : "off";
            line += m->has_romfs ? ", game files" : "";
            line += m->code.empty() ? "" : ", code";
            if (!m->problem.empty()) {
                line += "; " + m->problem;
            }
            lines.push_back(std::move(line));
        }
        for (std::string& p : problems) {
            lines.push_back(std::move(p));
        }
    }
    for (const std::string& line : lines) {
        Say(line);
    }
}

void Loader::Refresh() {
    std::string problem;
    {
        const std::lock_guard lock(mutex_);
        const std::string before = ListText(state_, mods_);
        mods_ = Scan(root_);
        Normalize(state_, mods_);
        if (ListText(state_, mods_) != before) {
            SaveLocked(&problem);
        }
    }
    if (!problem.empty()) {
        Say(problem);
    }
}

bool Loader::ExeReadsList() const {
    const std::lock_guard lock(mutex_);
    return exe_reads_list_;
}

const Mod* Loader::Find(std::string_view name) const {
    for (const Mod& m : mods_) {
        if (NameSame(m.name, name)) {
            return &m;
        }
    }
    return nullptr;
}

bool Loader::SaveLocked(std::string* problem) {
    return Save(root_, state_, mods_, problem);
}

std::vector<Row> Loader::Rows() const {
    const std::lock_guard lock(mutex_);
    std::vector<Row> rows;
    for (const Mod* m : InOrder(mods_, state_)) {
        Row r;
        r.name = m->name;
        r.id = m->manifest.id;
        r.title = m->manifest.name;
        r.version = m->manifest.version;
        r.author = m->manifest.author;
        r.description = m->manifest.description;
        r.on = Wanted(*m, state_);
        const auto code = code_now_.find(m->name);
        r.on_now = Has(files_now_, m->name) || (code != code_now_.end() && code->second.empty());
        r.has_files = m->has_romfs;
        r.has_code = !m->code.empty();
        r.settings = m->manifest.settings.size();
        const bool reachable = m->listable && !m->folder_off;
        r.can_switch = reachable && (exe_reads_list_ || !GameSees(*m));
        r.can_move = reachable && exe_reads_list_;
        if (!m->problem.empty()) {
            r.note = m->problem;
        } else if (code != code_now_.end() && !code->second.empty()) {
            r.note = code->second;
        } else if (m->folder_off) {
            r.note = "Off by its folder's name (a name starting with '-' or '_'): rename the folder to use it.";
        } else if (!m->listable) {
            r.note = "Its folder's name has characters the mods list cannot hold: rename it with plain letters to "
                     "switch or move it.";
        } else if (!exe_reads_list_ && GameSees(*m)) {
            r.note = "This hookshot.exe was built before the mod loader: build the game again to switch or move mods.";
        } else if (m->has_exefs) {
            r.note = "Its code patches (exefs) are not used: this game's code is compiled ahead of time.";
        }
        rows.push_back(std::move(r));
    }
    return rows;
}

bool Loader::SetOn(std::string_view name, bool on, std::string* problem) {
    const std::lock_guard lock(mutex_);
    const Mod* m = Find(name);
    const auto fail = [&](const char* why) {
        if (problem != nullptr) {
            *problem = why;
        }
        return false;
    };
    if (m == nullptr) {
        return fail("There is no such mod (any more).");
    }
    if (!m->listable || m->folder_off) {
        return fail("This mod is switched by its folder's name.");
    }
    if (!exe_reads_list_ && GameSees(*m)) {
        return fail("This hookshot.exe was built before the mod loader: build the game again to switch mods.");
    }
    const auto at = std::find_if(state_.off.begin(), state_.off.end(),
                                 [&](const std::string& n) { return NameSame(n, m->name); });
    if (on == (at == state_.off.end())) {
        return true;
    }
    if (on) {
        state_.off.erase(at);
    } else {
        state_.off.push_back(m->name);
    }
    return SaveLocked(problem);
}

bool Loader::Move(std::string_view name, int by, std::string* problem) {
    const std::lock_guard lock(mutex_);
    const auto fail = [&](const char* why) {
        if (problem != nullptr) {
            *problem = why;
        }
        return false;
    };
    const auto at = std::find_if(state_.order.begin(), state_.order.end(),
                                 [&](const std::string& n) { return NameSame(n, name); });
    if (at == state_.order.end()) {
        return fail("This mod cannot be moved.");
    }
    if (!exe_reads_list_) {
        return fail("This hookshot.exe was built before the mod loader: build the game again to order mods.");
    }
    const auto from = at - state_.order.begin();
    const auto last = static_cast<std::ptrdiff_t>(state_.order.size()) - 1;
    const auto to = std::clamp<std::ptrdiff_t>(from + by, 0, last);
    if (to == from) {
        return true;
    }
    std::string moved = std::move(*at);
    state_.order.erase(at);
    state_.order.insert(state_.order.begin() + to, std::move(moved));
    return SaveLocked(problem);
}

std::vector<SettingRow> Loader::Settings(std::string_view name) const {
    const std::lock_guard lock(mutex_);
    std::vector<SettingRow> rows;
    if (const Mod* m = Find(name); m != nullptr) {
        for (const Setting& s : m->manifest.settings) {
            rows.push_back({s, Effective(*m, s), HostSets(*m, s)});
        }
    }
    return rows;
}

bool Loader::Set(std::string_view name, std::string_view key, std::string_view value, std::string* problem) {
    std::vector<SettingFn> tell;
    std::string id;
    {
        const std::lock_guard lock(mutex_);
        const auto fail = [&](const char* why) {
            if (problem != nullptr) {
                *problem = why;
            }
            return false;
        };
        const Mod* m = Find(name);
        if (m == nullptr) {
            return fail("There is no such mod (any more).");
        }
        const auto s = std::find_if(m->manifest.settings.begin(), m->manifest.settings.end(),
                                    [&](const Setting& have) { return have.key == key; });
        if (s == m->manifest.settings.end()) {
            return fail("This mod has no such setting.");
        }
        if (HostSets(*m, *s)) {
            return fail("In this co-op game the host sets this.");
        }
        if (!ValidValue(*s, value)) {
            return fail("That is not a value this setting can have.");
        }
        if (ValueOf(*m, *s, state_) == value) {
            return true;
        }
        id = m->manifest.id;
        state_.settings[id][std::string(key)] = std::string(value);
        if (!SaveLocked(problem)) {
            return false;
        }
        tell = on_setting_;
    }
    for (const SettingFn& fn : tell) {
        fn(id, std::string(key), std::string(value));
    }
    return true;
}

std::string Loader::Value(std::string_view id, std::string_view key) const {
    const std::lock_guard lock(mutex_);
    for (const Mod& m : mods_) {
        if (m.manifest.id != id) {
            continue;
        }
        for (const Setting& s : m.manifest.settings) {
            if (s.key == key) {
                return Effective(m, s);
            }
        }
    }
    return {};
}

bool Loader::HostSets(const Mod& m, const Setting& s) const {
    if (!s.shared || !host_values_) {
        return false;
    }
    const auto it = host_values_->find(RestartKey(m, s));
    return it != host_values_->end() && ValidValue(s, it->second);
}

std::string Loader::Effective(const Mod& m, const Setting& s) const {
    return HostSets(m, s) ? host_values_->at(RestartKey(m, s)) : ValueOf(m, s, state_);
}

std::vector<SharedValue> Loader::SharedValues() const {
    const std::lock_guard lock(mutex_);
    std::vector<SharedValue> out;
    for (const Mod* m : InOrder(mods_, state_)) {
        if (!Wanted(*m, state_) || m->manifest.share != Share::Everyone) {
            continue;
        }
        for (const Setting& s : m->manifest.settings) {
            if (s.shared) {
                out.push_back({m->manifest.id, s.key, ValueOf(*m, s, state_)});
            }
        }
    }
    return out;
}

void Loader::SetHostValues(std::optional<std::vector<SharedValue>> values) {
    std::vector<SettingFn> tell;
    std::vector<SharedValue> changed;
    {
        const std::lock_guard lock(mutex_);
        std::map<std::string, std::string> before;
        for (const Mod& m : mods_) {
            for (const Setting& s : m.manifest.settings) {
                if (s.shared) {
                    before[RestartKey(m, s)] = Effective(m, s);
                }
            }
        }
        if (values) {
            host_values_.emplace();
            for (SharedValue& v : *values) {
                (*host_values_)[v.id + "\n" + v.key] = std::move(v.value);
            }
        } else {
            host_values_.reset();
        }
        for (const Mod& m : mods_) {
            for (const Setting& s : m.manifest.settings) {
                if (s.shared) {
                    std::string now = Effective(m, s);
                    if (now != before[RestartKey(m, s)]) {
                        changed.push_back({m.manifest.id, s.key, std::move(now)});
                    }
                }
            }
        }
        tell = on_setting_;
    }
    for (const SharedValue& v : changed) {
        for (const SettingFn& fn : tell) {
            fn(v.id, v.key, v.value);
        }
    }
}

void Loader::OnSetting(SettingFn fn) {
    const std::lock_guard lock(mutex_);
    on_setting_.push_back(std::move(fn));
}

Restart Loader::RestartNeeded() const {
    const std::lock_guard lock(mutex_);
    Restart r;
    if (!started_) {
        return r;
    }
    const auto say = [&](std::string line) {
        r.needed = true;
        if (r.why.size() < kMaxReasons) {
            r.why.push_back(std::move(line));
        }
    };
    const auto title = [&](std::string_view name) {
        const Mod* m = Find(name);
        return m != nullptr ? m->manifest.name : std::string(name);
    };
    // Game files: what the next start would index against what this run did.
    std::vector<std::string> next;
    if (exe_reads_list_) {
        next = IndexOrder(mods_, state_);
    } else {
        for (const Mod& m : mods_) {
            if (GameSees(m)) {
                next.push_back(m.name);
            }
        }
    }
    bool same_set = next.size() == files_now_.size();
    for (const std::string& name : next) {
        if (!Has(files_now_, name)) {
            same_set = false;
            say(title(name) + " is on: the game uses its files from the next start.");
        }
    }
    for (const std::string& name : files_now_) {
        if (!Has(next, name)) {
            same_set = false;
            say(title(name) + " is off or gone: the game uses its files until it is started again.");
        }
    }
    if (same_set && !std::equal(next.begin(), next.end(), files_now_.begin(),
                                [](const std::string& a, const std::string& b) { return NameSame(a, b); })) {
        say("The order of the mods changed: it counts from the next start.");
    }
    // Code: loaded once, at the start, and never unloaded.
    for (const Mod& m : mods_) {
        const auto tried = code_now_.find(m.name);
        const bool wanted = !m.code.empty() && Wanted(m, state_);
        if (wanted && tried == code_now_.end()) {
            say(m.manifest.name + "'s code is loaded at the next start.");
        } else if (!wanted && tried != code_now_.end() && tried->second.empty()) {
            say(m.manifest.name + "'s code stays loaded until the game is started again.");
        }
    }
    for (const auto& [name, problem] : code_now_) {
        if (problem.empty() && Find(name) == nullptr) {
            say(name + "'s code stays loaded until the game is started again.");
        }
    }
    for (const Mod& m : mods_) {
        for (const Setting& s : m.manifest.settings) {
            const auto was = restart_values_.find(RestartKey(m, s));
            if (s.restart && was != restart_values_.end() && was->second != Effective(m, s)) {
                say(m.manifest.name + ": \"" + s.name + "\" changes at the next start.");
            }
        }
    }
    for (const auto& [name, reason] : changed_) {
        say(reason);
    }
    return r;
}

void Loader::NoteChanged(std::string_view name, std::string reason) {
    const std::lock_guard lock(mutex_);
    changed_[std::string(name)] = std::move(reason);
}

std::vector<Mod> Loader::CodeToLoad() const {
    const std::lock_guard lock(mutex_);
    std::vector<Mod> out;
    for (const Mod* m : InOrder(mods_, state_)) {
        if (!m->code.empty() && Wanted(*m, state_)) {
            out.push_back(*m);
        }
    }
    return out;
}

void Loader::NoteCode(std::string_view name, std::string problem) {
    const std::lock_guard lock(mutex_);
    code_now_[std::string(name)] = std::move(problem);
}

std::vector<Mod> Loader::Mods() const {
    const std::lock_guard lock(mutex_);
    return mods_;
}

State Loader::Current() const {
    const std::lock_guard lock(mutex_);
    return state_;
}

}  // namespace CoopMods
