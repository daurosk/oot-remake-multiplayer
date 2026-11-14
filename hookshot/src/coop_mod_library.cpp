// coop_mod_library.h. Every file call takes an error code: a folder that
// cannot be read is a mod left out, never an exception on the game's thread.
#include "coop_mod_library.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <system_error>

#include <monocypher.h>

#include "coop_mod_json.h"
#include "hos_coop_mods.h"

namespace CoopMods {

namespace fs = std::filesystem;
using CoopModJson::Value;

namespace {

char Fold(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool IsLowerAlnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool Printable(std::string_view s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x20 && c != 0x7f; });
}

std::string NumberText(double n) {
    char buf[32];
    if (n == std::floor(n) && std::fabs(n) < 1e15) {
        std::snprintf(buf, sizeof buf, "%.0f", n);
        return buf;
    }
    const auto [end, ec] = std::to_chars(buf, buf + sizeof buf, n);
    return ec == std::errc() ? std::string(buf, end) : std::string("0");
}

std::optional<double> ReadNumber(std::string_view s) {
    double n = 0;
    if (s.empty() || s.size() > 32) {
        return std::nullopt;
    }
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (ec != std::errc() || end != s.data() + s.size() || !std::isfinite(n)) {
        return std::nullopt;
    }
    return n;
}

// The last part of a mod's name.
std::string_view Leaf(std::string_view name) {
    const auto slash = name.rfind('/');
    return slash == std::string_view::npos ? name : name.substr(slash + 1);
}

bool StartsFolded(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && NameSame(s.substr(0, prefix.size()), prefix);
}

// Co-op's own packs (the puppet pack, the HUD pack): the kit installs and updates them.
bool Internal(std::string_view name) {
    const std::string_view leaf = Leaf(name);
    return StartsFolded(leaf, "coop-puppet") || StartsFolded(leaf, "coop_hud") || StartsFolded(leaf, "coop-hud");
}

bool Listable(std::string_view name) {
    return !name.empty() && name.size() < HOS_COOP_MODS_NAME &&
           std::all_of(name.begin(), name.end(), [](char c) { return c >= 0x20 && c < 0x7f; });
}

std::optional<std::string> ReadFile(const fs::path& path, std::size_t most) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::string text(most + 1, '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    const auto got = static_cast<std::size_t>(in.gcount());
    if (got > most) {
        return std::nullopt;
    }
    text.resize(got);
    return text;
}

bool WriteFileSafely(const fs::path& path, std::string_view text, std::string* problem) {
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            if (problem != nullptr) {
                *problem = "could not write " + ToUtf8(tmp);
            }
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        if (problem != nullptr) {
            *problem = "could not write " + ToUtf8(path);
        }
        return false;
    }
    return true;
}

// ---- mod.json ----

struct Field {
    const Value& object;
    std::string& problem;

    // A text member no longer than `most`; false (problem set) when it is there but is not that.
    bool Text(const char* key, std::size_t most, std::string& out) const {
        const Value* v = object.Get(key);
        if (v == nullptr) {
            return true;
        }
        if (v->kind != Value::Kind::String || v->string.size() > most || !Printable(v->string)) {
            problem = std::string("\"") + key + "\" must be a text of at most " + std::to_string(most) + " characters";
            return false;
        }
        out = v->string;
        return true;
    }

    bool Flag(const char* key, bool& out) const {
        const Value* v = object.Get(key);
        if (v == nullptr) {
            return true;
        }
        if (v->kind != Value::Kind::Bool) {
            problem = std::string("\"") + key + "\" must be true or false";
            return false;
        }
        out = v->boolean;
        return true;
    }

    bool Number(const char* key, double& out) const {
        const Value* v = object.Get(key);
        if (v == nullptr) {
            return true;
        }
        if (v->kind != Value::Kind::Number) {
            problem = std::string("\"") + key + "\" must be a number";
            return false;
        }
        out = v->number;
        return true;
    }
};

bool ValidKey(std::string_view key) {
    return !key.empty() && key.size() <= kMaxSettingKey &&
           std::all_of(key.begin(), key.end(), [](char c) { return IsLowerAlnum(c) || c == '_'; });
}

bool ReadSetting(const Value& row, Setting& out, std::string& problem) {
    if (row.kind != Value::Kind::Object) {
        problem = "each of \"settings\" must be an object";
        return false;
    }
    const Field f{row, problem};
    std::string type = "toggle";
    if (!f.Text("key", kMaxSettingKey, out.key) || !f.Text("type", 16, type)) {
        return false;
    }
    if (!ValidKey(out.key)) {
        problem = "a setting's \"key\" must be 1 to 32 of a-z, 0-9 and _";
        return false;
    }
    out.name = out.key;
    double max_length = 64;
    if (!f.Text("name", 64, out.name) || !f.Text("help", 256, out.help) || !f.Flag("restart", out.restart) ||
        !f.Flag("shared", out.shared) || !f.Number("min", out.min) || !f.Number("max", out.max) ||
        !f.Number("step", out.step) || !f.Number("max_length", max_length)) {
        return false;
    }
    if (type == "toggle") {
        out.type = SettingType::Toggle;
        out.initial = "0";
    } else if (type == "choice") {
        out.type = SettingType::Choice;
    } else if (type == "number") {
        out.type = SettingType::Number;
        if (!(out.min < out.max) || !(out.step > 0)) {
            problem = "setting \"" + out.key + "\": a number needs \"min\" below \"max\" and a \"step\" above 0";
            return false;
        }
        out.initial = NumberText(out.min);
    } else if (type == "text") {
        out.type = SettingType::Text;
        if (!(max_length >= 1) || max_length > static_cast<double>(kMaxSettingValue)) {
            problem = "setting \"" + out.key + "\": \"max_length\" must be 1 to " + std::to_string(kMaxSettingValue);
            return false;
        }
        out.max_length = static_cast<std::size_t>(max_length);
    } else {
        problem = "setting \"" + out.key + "\": \"type\" must be toggle, choice, number or text";
        return false;
    }
    if (const Value* choices = row.Get("choices"); out.type == SettingType::Choice) {
        if (choices == nullptr || choices->kind != Value::Kind::Array || choices->array.empty() ||
            choices->array.size() > 64) {
            problem = "setting \"" + out.key + "\": a choice needs \"choices\", 1 to 64 of them";
            return false;
        }
        for (const Value& c : choices->array) {
            std::string value, caption;
            if (c.kind == Value::Kind::String) {
                value = caption = c.string;
            } else if (c.kind == Value::Kind::Object) {
                value = c.Text("value").value_or("");
                caption = c.Text("name").value_or(value);
            }
            const bool twice = std::any_of(out.choices.begin(), out.choices.end(),
                                           [&](const auto& have) { return have.first == value; });
            if (value.empty() || value.size() > 64 || caption.size() > 64 || !Printable(value) || !Printable(caption) ||
                twice) {
                problem = "setting \"" + out.key +
                          "\": each choice must be a text or {\"value\", \"name\"}, at most 64 characters, no value twice";
                return false;
            }
            out.choices.emplace_back(std::move(value), std::move(caption));
        }
        out.initial = out.choices.front().first;
    }
    if (const Value* initial = row.Get("default"); initial != nullptr) {
        switch (initial->kind) {
        case Value::Kind::Bool:
            out.initial = initial->boolean ? "1" : "0";
            break;
        case Value::Kind::Number:
            out.initial = NumberText(initial->number);
            break;
        case Value::Kind::String:
            out.initial = initial->string;
            break;
        default:
            out.initial = "\n";  // never valid
        }
    }
    if (!ValidValue(out, out.initial)) {
        problem = "setting \"" + out.key + "\": its \"default\" is not a value it can have";
        return false;
    }
    return true;
}

// ---- hashing ----

struct Blake {
    crypto_blake2b_ctx ctx;
    Blake() { crypto_blake2b_init(&ctx, sizeof(Hash)); }
    void Add(const void* data, std::size_t n) { crypto_blake2b_update(&ctx, static_cast<const std::uint8_t*>(data), n); }
    void Add(std::uint64_t n) {
        std::uint8_t le[8];
        for (int i = 0; i < 8; ++i) {
            le[i] = static_cast<std::uint8_t>(n >> (8 * i));
        }
        Add(le, sizeof le);
    }
    Hash Done() {
        Hash h{};
        crypto_blake2b_final(&ctx, h.data());
        return h;
    }
};

// Adds the file's size and bytes; false when it cannot be read whole or `stop` said so.
bool AddFile(Blake& b, const fs::path& path, std::uint64_t& bytes, const std::function<bool()>* stop) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::error_code ec;
    const std::uint64_t size = fs::file_size(path, ec);
    if (ec) {
        return false;
    }
    b.Add(size);
    std::vector<char> chunk(1 << 20);
    std::uint64_t left = size;
    while (left > 0) {
        if (stop != nullptr && *stop && (*stop)()) {
            return false;
        }
        const auto want = static_cast<std::streamsize>(std::min<std::uint64_t>(left, chunk.size()));
        in.read(chunk.data(), want);
        if (in.gcount() != want) {
            return false;  // (it shrank while we read)
        }
        b.Add(chunk.data(), static_cast<std::size_t>(want));
        left -= static_cast<std::uint64_t>(want);
    }
    bytes += size;
    return true;
}

}  // namespace

fs::path FromUtf8(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

std::string ToUtf8(const fs::path& path) {
    const std::u8string s = path.generic_u8string();
    return std::string(s.begin(), s.end());
}

bool NameLess(std::string_view a, std::string_view b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return static_cast<unsigned char>(Fold(x)) < static_cast<unsigned char>(Fold(y));
    });
}

bool NameSame(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return Fold(x) == Fold(y); });
}

std::string IdFrom(std::string_view name) {
    std::string id;
    for (const char raw : name) {
        const char c = Fold(raw);
        if (IsLowerAlnum(c)) {
            id += c;
        } else if (!id.empty()) {  // (an id starts with a letter or a digit)
            id += c == '.' || c == '_' ? c : '-';
        }
        if (id.size() == kMaxId) {
            break;
        }
    }
    return id.empty() ? std::string("mod") : id;
}

bool ValidId(std::string_view id) {
    return !id.empty() && id.size() <= kMaxId && IsLowerAlnum(id.front()) &&
           std::all_of(id.begin(), id.end(), [](char c) { return IsLowerAlnum(c) || c == '.' || c == '_' || c == '-'; });
}

bool SafeRelative(std::string_view path) {
    if (path.empty() || path.size() > 260 || !Printable(path) || path.find(':') != std::string_view::npos) {
        return false;
    }
    std::size_t at = 0;
    while (at <= path.size()) {
        std::size_t end = path.find_first_of("/\\", at);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        const std::string_view part = path.substr(at, end - at);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        at = end + 1;
    }
    return true;
}

bool ValidValue(const Setting& setting, std::string_view value) {
    if (value.size() > kMaxSettingValue || !Printable(value)) {
        return false;
    }
    switch (setting.type) {
    case SettingType::Toggle:
        return value == "0" || value == "1";
    case SettingType::Choice:
        return std::any_of(setting.choices.begin(), setting.choices.end(),
                           [&](const auto& c) { return c.first == value; });
    case SettingType::Number: {
        const auto n = ReadNumber(value);
        return n && *n >= setting.min && *n <= setting.max;
    }
    case SettingType::Text:
        return value.size() <= setting.max_length;
    }
    return false;
}

Manifest PlainManifest(std::string_view folder_name) {
    Manifest m;
    m.id = IdFrom(folder_name);
    m.name = std::string(folder_name);
    return m;
}

std::optional<Manifest> ParseManifest(std::string_view text, std::string_view folder_name, std::string* problem) {
    std::string why;
    const auto fail = [&]() -> std::optional<Manifest> {
        if (problem != nullptr) {
            *problem = "mod.json: " + why;
        }
        return std::nullopt;
    };
    if (text.size() > kMaxManifestBytes) {
        why = "the file is too long";
        return fail();
    }
    const auto doc = CoopModJson::Parse(text, &why);
    if (!doc) {
        return fail();
    }
    if (doc->kind != Value::Kind::Object) {
        why = "it must be one object: { ... }";
        return fail();
    }
    Manifest m = PlainManifest(folder_name);
    const Field f{*doc, why};
    std::string share = "everyone";
    double api = 0;
    if (!f.Text("id", kMaxId, m.id) || !f.Text("name", 64, m.name) || !f.Text("version", kMaxVersion, m.version) ||
        !f.Text("author", 64, m.author) || !f.Text("description", 512, m.description) ||
        !f.Text("coop", 16, share) || !f.Number("api", api)) {
        return fail();
    }
    if (!ValidId(m.id)) {
        why = "\"id\" must be 1 to 48 of a-z, 0-9, '.', '_' and '-', starting with a letter or a digit";
        return fail();
    }
    if (m.name.empty() || m.version.empty()) {
        why = "\"name\" and \"version\" must not be empty";
        return fail();
    }
    if (share == "everyone") {
        m.share = Share::Everyone;
    } else if (share == "own") {
        m.share = Share::Own;
    } else {
        why = "\"coop\" must be \"everyone\" (every player needs the mod) or \"own\" (each player's own business)";
        return fail();
    }
    if (api != std::floor(api) || api < 0 || api > 100000) {
        why = "\"api\" must be a whole number";
        return fail();
    }
    m.api = static_cast<int>(api);
    if (const Value* code = doc->Get("code"); code != nullptr) {
#ifdef _WIN32
        const char* const system = "windows";
#else
        const char* const system = "linux";
#endif
        if (code->kind == Value::Kind::String) {
            m.code = code->string;
        } else if (code->kind == Value::Kind::Object) {
            for (const auto& [key, value] : code->object) {
                if (value.kind != Value::Kind::String) {
                    why = "\"code\" must be a path, or {\"windows\": path, \"linux\": path}";
                    return fail();
                }
            }
            m.code = code->Text(system).value_or("");
        } else {
            why = "\"code\" must be a path, or {\"windows\": path, \"linux\": path}";
            return fail();
        }
        if (!m.code.empty() && !SafeRelative(m.code)) {
            why = "\"code\" must be a path inside the mod's folder (no drive, no \"..\")";
            return fail();
        }
    }
    if (const Value* settings = doc->Get("settings"); settings != nullptr) {
        if (settings->kind != Value::Kind::Array || settings->array.size() > kMaxSettings) {
            why = "\"settings\" must be a list of at most " + std::to_string(kMaxSettings) + " rows";
            return fail();
        }
        for (const Value& row : settings->array) {
            Setting s;
            if (!ReadSetting(row, s, why)) {
                return fail();
            }
            if (std::any_of(m.settings.begin(), m.settings.end(), [&](const Setting& have) { return have.key == s.key; })) {
                why = "setting \"" + s.key + "\" is there twice";
                return fail();
            }
            m.settings.push_back(std::move(s));
        }
    }
    return m;
}

namespace {

void Found(const fs::path& dir, const std::string& name, bool off, std::vector<Mod>& out) {
    std::error_code ec;
    Mod mod;
    mod.name = name;
    mod.folder = dir;
    mod.has_romfs = fs::is_directory(dir / "romfs", ec);
    mod.has_exefs = fs::is_directory(dir / "exefs", ec);
    mod.has_manifest = fs::is_regular_file(dir / kManifestFile, ec);
    mod.folder_off = off;
    mod.internal = Internal(name);
    mod.listable = Listable(name);
    const std::string leaf(Leaf(name));
    mod.manifest = PlainManifest(leaf);
    if (mod.has_manifest) {
        const auto text = ReadFile(dir / kManifestFile, kMaxManifestBytes);
        std::string why = "mod.json: the file is too long or cannot be read";
        auto manifest = text ? ParseManifest(*text, leaf, &why) : std::nullopt;
        if (manifest) {
            mod.manifest = std::move(*manifest);
        } else {
            mod.problem = why;
        }
    }
    if (!mod.manifest.code.empty()) {
        // The file itself, links followed, must still be inside the mod's folder.
        const fs::path root = fs::weakly_canonical(dir, ec);
        const fs::path file = ec ? fs::path() : fs::weakly_canonical(dir / FromUtf8(mod.manifest.code), ec);
        const fs::path rel = ec ? fs::path("..") : file.lexically_relative(root);
        if (mod.manifest.api > kApiVersion) {
            mod.problem = "its code needs a newer co-op (plugin interface " + std::to_string(mod.manifest.api) +
                          ", this one has " + std::to_string(kApiVersion) + ")";
        } else if (rel.empty() || *rel.begin() == ".." || !fs::is_regular_file(file, ec)) {
            mod.problem = "its code file " + mod.manifest.code + " is missing (or leaves the mod's folder)";
        } else {
            mod.code = file;
        }
    }
    out.push_back(std::move(mod));
}

void Find(const fs::path& dir, const std::string& name, int depth, bool off, std::vector<Mod>& out) {
    std::error_code ec;
    if (depth > 0 && (fs::is_directory(dir / "romfs", ec) || fs::is_directory(dir / "exefs", ec) ||
                      fs::is_regular_file(dir / kManifestFile, ec))) {
        Found(dir, name, off, out);
        return;
    }
    if (depth >= 4) {
        return;
    }
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code ignore;
        if (!it->is_directory(ignore)) {
            continue;
        }
        const std::string leaf = ToUtf8(it->path().filename());
        if (leaf.empty() || leaf.front() == '.') {
            continue;
        }
        Find(it->path(), name.empty() ? leaf : name + "/" + leaf, depth + 1,
             off || leaf.front() == '-' || leaf.front() == '_', out);
    }
}

}  // namespace

std::vector<Mod> Scan(const fs::path& mods_root) {
    std::vector<Mod> mods;
    Find(mods_root, "", 0, false, mods);
    std::sort(mods.begin(), mods.end(), [](const Mod& a, const Mod& b) { return NameLess(a.name, b.name); });
    for (std::size_t i = 0; i < mods.size(); ++i) {
        const auto same = [&](const std::string& id) {
            return std::any_of(mods.begin(), mods.begin() + static_cast<std::ptrdiff_t>(i),
                               [&](const Mod& earlier) { return earlier.manifest.id == id; });
        };
        if (!same(mods[i].manifest.id)) {
            continue;
        }
        const std::string first = mods[i].manifest.id;
        for (int n = 2;; ++n) {
            const std::string tail = "-" + std::to_string(n);
            const std::string id = first.substr(0, kMaxId - tail.size()) + tail;
            if (!same(id)) {
                mods[i].manifest.id = id;
                break;
            }
        }
        if (mods[i].problem.empty()) {
            mods[i].problem = "another mod has the id \"" + first + "\" too: this one counts as \"" +
                              mods[i].manifest.id + "\"";
        }
    }
    return mods;
}

bool State::IsOff(std::string_view name) const {
    return std::any_of(off.begin(), off.end(), [&](const std::string& n) { return NameSame(n, name); });
}

void Normalize(State& state, const std::vector<Mod>& mods) {
    const auto listed = [](const Mod& m) { return !m.internal && m.listable; };
    const auto find = [&](std::string_view name) -> const Mod* {
        for (const Mod& m : mods) {
            if (listed(m) && NameSame(m.name, name)) {
                return &m;
            }
        }
        return nullptr;
    };
    const auto has = [](const std::vector<std::string>& names, std::string_view name) {
        return std::any_of(names.begin(), names.end(), [&](const std::string& n) { return NameSame(n, name); });
    };
    std::vector<std::string> order, off;
    for (const std::string& name : state.order) {
        if (const Mod* m = find(name); m != nullptr && !has(order, m->name)) {
            order.push_back(m->name);
        }
    }
    // The rest as Hookshot orders mods no list names: by name, the last one winning.
    for (auto m = mods.rbegin(); m != mods.rend(); ++m) {
        if (listed(*m) && !has(order, m->name)) {
            order.push_back(m->name);
        }
    }
    for (const std::string& name : state.off) {
        if (const Mod* m = find(name); m != nullptr && !has(off, m->name)) {
            off.push_back(m->name);
        }
    }
    state.order = std::move(order);
    state.off = std::move(off);
}

std::string ListText(const State& state, const std::vector<Mod>& mods) {
    State s = state;
    Normalize(s, mods);
    std::string text = "coop-mods " + std::to_string(HOS_COOP_MODS_VERSION) + "\n";
    std::size_t rows = 0;
    for (const std::string& name : s.order) {
        if (rows++ == HOS_COOP_MODS_MAX) {
            break;
        }
        text += s.IsOff(name) ? "off " : "on ";
        text += name;
        text += '\n';
    }
    return text;
}

bool ReadListText(std::string_view text, State& state) {
    const auto list = std::make_unique<HosCoopModsList>();
    if (!hos_coop_mods_parse(text.data(), text.size(), list.get())) {
        return false;
    }
    state.order.clear();
    state.off.clear();
    for (int i = 0; i < list->count; ++i) {
        state.order.emplace_back(list->rows[i].name);
        if (list->rows[i].off) {
            state.off.emplace_back(list->rows[i].name);
        }
    }
    return true;
}

State Load(const fs::path& mods_root, std::vector<std::string>* problems) {
    State state;
    const auto say = [&](std::string line) {
        if (problems != nullptr) {
            problems->push_back(std::move(line));
        }
    };
    std::error_code ec;
    const fs::path list = mods_root / HOS_COOP_MODS_LIST_FILE;
    if (fs::exists(list, ec)) {
        const auto text = ReadFile(list, HOS_COOP_MODS_TEXT_MAX);
        if (!text || !ReadListText(*text, state)) {
            say(ToUtf8(list) + " is not a mods list: starting from Hookshot's own order");
        }
    }
    const fs::path settings = mods_root / kSettingsFile;
    if (fs::exists(settings, ec)) {
        const auto text = ReadFile(settings, CoopModJson::kMaxText);
        std::string why = "it is too long or cannot be read";
        const auto doc = text ? CoopModJson::Parse(*text, &why) : std::nullopt;
        const Value* all = doc ? doc->Get("settings") : nullptr;
        if (all == nullptr || all->kind != Value::Kind::Object) {
            say(ToUtf8(settings) + ": " + (doc ? std::string("no \"settings\" object") : why) +
                ": every mod starts from its own settings");
        } else {
            for (const auto& [id, values] : all->object) {
                if (!ValidId(id) || values.kind != Value::Kind::Object) {
                    continue;
                }
                for (const auto& [key, value] : values.object) {
                    if (ValidKey(key) && value.kind == Value::Kind::String && value.string.size() <= kMaxSettingValue) {
                        state.settings[id][key] = value.string;
                    }
                }
            }
        }
    }
    return state;
}

bool Save(const fs::path& mods_root, const State& state, const std::vector<Mod>& mods, std::string* problem) {
    std::error_code ec;
    fs::create_directories(mods_root, ec);
    if (!WriteFileSafely(mods_root / HOS_COOP_MODS_LIST_FILE, ListText(state, mods), problem)) {
        return false;
    }
    Value all;
    all.kind = Value::Kind::Object;
    for (const auto& [id, values] : state.settings) {
        Value one;
        one.kind = Value::Kind::Object;
        for (const auto& [key, value] : values) {
            one.object.emplace_back(key, Value::Of(value));
        }
        if (!one.object.empty()) {
            all.object.emplace_back(id, std::move(one));
        }
    }
    Value doc;
    doc.kind = Value::Kind::Object;
    doc.object.emplace_back("settings", std::move(all));
    return WriteFileSafely(mods_root / kSettingsFile, CoopModJson::Write(doc), problem);
}

std::string ValueOf(const Mod& mod, const Setting& setting, const State& state) {
    if (const auto values = state.settings.find(mod.manifest.id); values != state.settings.end()) {
        if (const auto value = values->second.find(setting.key);
            value != values->second.end() && ValidValue(setting, value->second)) {
            return value->second;
        }
    }
    return setting.initial;
}

std::vector<std::string> IndexOrder(const std::vector<Mod>& mods, const State& state) {
    const std::string text = ListText(state, mods);
    const auto list = std::make_unique<HosCoopModsList>();
    hos_coop_mods_parse(text.data(), text.size(), list.get());
    std::vector<std::pair<int, std::string>> ranked;
    for (const Mod& m : mods) {
        if (m.folder_off || !(m.has_romfs || m.has_exefs)) {
            continue;  // Hookshot does not see it
        }
        // (a name too long for a row is one the list cannot hold either)
        const int rank = m.listable ? hos_coop_mods_rank(list.get(), m.name.c_str()) : 0;
        if (rank >= 0) {
            ranked.emplace_back(rank, m.name);
        }
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : NameLess(a.second, b.second);
    });
    std::vector<std::string> names;
    for (auto& [rank, name] : ranked) {
        names.push_back(std::move(name));
    }
    return names;
}

Applied ParseApplied(std::string_view text) {
    Applied a;
    std::size_t at = 0;
    bool first = true;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (first) {
            first = false;
            const std::string head = "coop-mods-applied " + std::to_string(HOS_COOP_MODS_VERSION) + " ";
            if (line.substr(0, head.size()) != head) {
                return a;
            }
            line.remove_prefix(head.size());
            const auto [p, ec] = std::from_chars(line.data(), line.data() + line.size(), a.pid);
            if (ec != std::errc() || p != line.data() + line.size()) {
                return a;
            }
            a.valid = true;
        } else if (line.substr(0, 4) == "off " && line.size() > 4) {
            a.off.emplace_back(line.substr(4));
        } else if (line.substr(0, 3) == "on ") {
            line.remove_prefix(3);
            int files = 0;
            const auto [p, ec] = std::from_chars(line.data(), line.data() + line.size(), files);
            if (ec == std::errc() && p + 1 < line.data() + line.size() && *p == ' ') {
                a.on.emplace_back(std::string(p + 1, line.data() + line.size()), files);
            }
        }
    }
    return a;
}

Applied LoadApplied(const fs::path& mods_root) {
    const auto text = ReadFile(mods_root / HOS_COOP_MODS_APPLIED_FILE, HOS_COOP_MODS_TEXT_MAX * 2);
    return text ? ParseApplied(*text) : Applied{};
}

std::string StartingText(const Starting& s) {
    return "coop-mods-starting 1 " + std::to_string(s.pid) + "\n" + s.name + "\n";
}

std::optional<Starting> ParseStarting(std::string_view text) {
    constexpr std::string_view head = "coop-mods-starting 1 ";
    const std::size_t end = text.find('\n');
    if (text.substr(0, head.size()) != head || end == std::string_view::npos) {
        return std::nullopt;
    }
    Starting s;
    std::string_view pid = text.substr(head.size(), end - head.size());
    if (!pid.empty() && pid.back() == '\r') {
        pid.remove_suffix(1);
    }
    const auto [p, ec] = std::from_chars(pid.data(), pid.data() + pid.size(), s.pid);
    if (pid.empty() || ec != std::errc() || p != pid.data() + pid.size()) {
        return std::nullopt;
    }
    std::string_view name = text.substr(end + 1);
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) {
        name.remove_suffix(1);
    }
    if (name.empty() || name.find('\n') != std::string_view::npos) {
        return std::nullopt;
    }
    s.name = std::string(name);
    return s;
}

bool WriteStarting(const fs::path& mods_root, const Starting& s) {
    return WriteFileSafely(mods_root / kStartingFile, StartingText(s), nullptr);
}

std::optional<Starting> LoadStarting(const fs::path& mods_root) {
    const auto text = ReadFile(mods_root / kStartingFile, HOS_COOP_MODS_TEXT_MAX);
    return text ? ParseStarting(*text) : std::nullopt;
}

void ClearStarting(const fs::path& mods_root) {
    std::error_code ec;
    fs::remove(mods_root / kStartingFile, ec);
}

std::optional<Hashes> HashMod(const Mod& mod, const std::function<bool()>* stop) {
    Hashes h;
    std::error_code ec;
    const fs::path romfs = mod.folder / "romfs";
    std::vector<std::pair<std::string, fs::path>> files;  // key (lower case, '/'), the file
    if (mod.has_romfs) {
        for (fs::recursive_directory_iterator it(romfs, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::error_code ignore;
            if (!it->is_regular_file(ignore)) {
                continue;
            }
            std::string key = ToUtf8(it->path().lexically_relative(romfs));
            std::transform(key.begin(), key.end(), key.begin(), Fold);
            files.emplace_back(std::move(key), it->path());
        }
        if (ec) {
            return std::nullopt;
        }
    }
    std::sort(files.begin(), files.end());
    if (!files.empty()) {
        Blake b;
        for (const auto& [key, path] : files) {
            b.Add(key.size());
            b.Add(key.data(), key.size());
            if (!AddFile(b, path, h.bytes, stop)) {
                return std::nullopt;
            }
        }
        h.game_files = b.Done();
        h.files = files.size();
    }
    if (!mod.code.empty()) {
        Blake b;
        std::uint64_t bytes = 0;
        if (!AddFile(b, mod.code, bytes, stop)) {
            return std::nullopt;
        }
        h.code = b.Done();
    }
    return h;
}

std::string Hex(const Hash& hash) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t byte : hash) {
        out += digits[byte >> 4];
        out += digits[byte & 15];
    }
    return out;
}

}  // namespace CoopMods
