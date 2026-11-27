// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_playtest_core.h"

#include <cctype>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <system_error>
#include <utility>

namespace CoopPlaytest {

std::optional<std::uint16_t> PortFrom(std::string_view value) {
    unsigned port = 0;
    const char* end = value.data() + value.size();
    const auto r = std::from_chars(value.data(), end, port);
    if (value.empty() || r.ec != std::errc{} || r.ptr != end || port < 1024 || port > 65535) return std::nullopt;
    return static_cast<std::uint16_t>(port);
}

double IdleFrom(std::string_view value) {
    double s = 0;
    const char* end = value.data() + value.size();
    const auto r = std::from_chars(value.data(), end, s);
    if (value.empty() || r.ec != std::errc{} || r.ptr != end || !(s >= 0) || s > 86400) return kDefaultIdleS;
    return s;
}

std::optional<std::uint32_t> KeyVk(std::string_view name) {
    if (name.size() == 1) {
        const char c = name[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return static_cast<std::uint32_t>(c);
        return std::nullopt;
    }
    if (name.size() < 2 || name.size() > 3 || name[0] != 'F') return std::nullopt;
    int n = 0;
    for (std::size_t i = 1; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return std::nullopt;
        n = n * 10 + (name[i] - '0');
    }
    if (n < 1 || n > 12 || name[1] == '0') return std::nullopt;
    return static_cast<std::uint32_t>(0x70 + n - 1);   // (VK_F1 .. VK_F12)
}

bool ValidTag(std::string_view tag) {
    if (tag.empty() || tag.size() > 64) return false;
    for (const char c : tag) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                        c == '_';
        if (!ok) return false;
    }
    return true;
}

// ---- the hold ----
// No lock of the module's is held here: the frame hook calls AtFrame first (coop_hookshot.cpp Frame), before any
// of co-op's own work, and the only lock taken is the gate's own, which the wait gives up (condition_variable).

void Gate::AtFrame() {
    std::unique_lock lock(mu_);
    ++frame_;
    if (budget_ > 0 && --budget_ > 0) return;  // a step still running
    if (!want_hold_) return;
    held_ = true;
    if (on_held_) on_held_(true);
    cv_.notify_all();
    while (want_hold_ && budget_ == 0) {
        while (!jobs_.empty()) {
            std::function<void()>* job = jobs_.front();
            jobs_.pop_front();
            lock.unlock();
            (*job)();
            lock.lock();
            *job = nullptr;  // (done: RunHeld waits for this)
            cv_.notify_all();
        }
        if (idle_s_ > 0 && std::chrono::duration<double>(Clock::now() - touched_).count() >= idle_s_) {
            want_hold_ = false;
            end_note_ = "idle";
            break;
        }
        cv_.wait_for(lock, std::chrono::milliseconds(50));
    }
    held_ = false;
    if (on_held_) on_held_(false);
    cv_.notify_all();
}

std::uint64_t Gate::Frame() const {
    std::lock_guard lock(mu_);
    return frame_;
}

bool Gate::Held() const {
    std::lock_guard lock(mu_);
    return held_;
}

bool Gate::Holding() const {
    std::lock_guard lock(mu_);
    return want_hold_;
}

bool Gate::Pause(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mu_);
    touched_ = Clock::now();
    want_hold_ = true;
    if (!held_) budget_ = 0;  // (a step not yet begun is cancelled: the hold comes at the next frame)
    return cv_.wait_for(lock, timeout, [&] { return held_ && budget_ == 0; });
}

void Gate::Resume() {
    std::lock_guard lock(mu_);
    want_hold_ = false;
    budget_ = 0;
    cv_.notify_all();
}

bool Gate::Step(std::uint32_t frames, std::chrono::milliseconds timeout, std::uint64_t* from) {
    if (frames == 0) return false;
    if (!Pause(timeout)) return false;
    std::unique_lock lock(mu_);
    const std::uint64_t start = frame_;
    if (from) *from = start;
    budget_ = frames;
    cv_.notify_all();
    // Held again at exactly start + frames: AtFrame holds at the hook where the budget reaches 0.
    // (a Resume meanwhile, the client gone or the idle time out, ends the wait: not a step done)
    cv_.wait_for(lock, timeout + std::chrono::milliseconds(frames * 100),
                 [&] { return !want_hold_ || (held_ && budget_ == 0 && frame_ == start + frames); });
    return want_hold_ && held_ && budget_ == 0 && frame_ == start + frames;
}

bool Gate::RunHeld(std::function<void()> job, std::chrono::milliseconds timeout) {
    if (!Pause(timeout)) return false;
    std::unique_lock lock(mu_);
    std::function<void()> slot = std::move(job);
    jobs_.push_back(&slot);
    cv_.notify_all();
    if (cv_.wait_for(lock, timeout, [&] { return !slot; })) return true;
    // Not run in time (the hold ended): take it back so it never runs after we return.
    for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
        if (*it == &slot) {
            jobs_.erase(it);
            return false;
        }
    }
    cv_.wait(lock, [&] { return !slot; });  // (running now: wait for it)
    return true;
}

void Gate::Touch() {
    std::lock_guard lock(mu_);
    touched_ = Clock::now();
}

std::string Gate::TakeEndNote() {
    std::lock_guard lock(mu_);
    return std::exchange(end_note_, {});
}

// ---- the requests ----

Axes StickAxes(double angle_deg, double strength) {
    if (!std::isfinite(angle_deg) || !std::isfinite(strength)) return {};
    strength = strength < 0 ? 0 : strength > 1 ? 1 : strength;
    const double a = angle_deg * 3.14159265358979323846 / 180.0;
    Axes out{std::sin(a) * strength, std::cos(a) * strength};
    if (std::fabs(out.x) < 1e-9) out.x = 0;
    if (std::fabs(out.y) < 1e-9) out.y = 0;
    return out;
}

std::optional<Axes> MoveAxes(const std::array<float, 16>& m, double yaw_deg, double strength) {
    if (!std::isfinite(yaw_deg) || !std::isfinite(strength)) return std::nullopt;
    for (const float v : m) {
        if (!std::isfinite(v)) return std::nullopt;
    }
    // Ground directions (x, z) in world space: forward = where clip w (depth) grows, right = where clip x grows. The
    // x row may carry some of w (an off-centre or jittered projection): only its part across forward is the right.
    double fx = m[12], fz = m[14];
    const double fl = std::hypot(fx, fz);
    if (!(fl > 1e-6)) return std::nullopt;
    fx /= fl, fz /= fl;
    double rx = m[0], rz = m[2];
    const double along = rx * fx + rz * fz;
    rx -= along * fx, rz -= along * fz;
    const double rl = std::hypot(rx, rz);
    if (!(rl > 1e-6 * std::max(1.0, std::hypot(static_cast<double>(m[0]), static_cast<double>(m[2]))))) {
        return std::nullopt;
    }
    rx /= rl, rz /= rl;
    strength = strength < 0 ? 0 : strength > 1 ? 1 : strength;
    const double a = yaw_deg * 3.14159265358979323846 / 180.0;
    const double dx = std::sin(a), dz = std::cos(a);
    Axes out{(dx * rx + dz * rz) * strength, (dx * fx + dz * fz) * strength};
    if (std::fabs(out.x) < 1e-9) out.x = 0;
    if (std::fabs(out.y) < 1e-9) out.y = 0;
    return out;
}

std::optional<double> ViewYaw(const std::array<float, 16>& m) {
    const double fx = m[12], fz = m[14];
    if (!std::isfinite(fx) || !std::isfinite(fz) || !(std::hypot(fx, fz) > 1e-6)) return std::nullopt;
    double d = std::atan2(fx, fz) * 180.0 / 3.14159265358979323846;
    if (d >= 180.0) d -= 360.0;
    return d;
}

void MoveHold::Start(double yaw_deg, double strength) {
    std::lock_guard lock(mu_);
    on_ = true, yaw_ = yaw_deg, strength_ = strength, counts_ = {};
}

MoveHold::Counts MoveHold::Stop() {
    std::lock_guard lock(mu_);
    on_ = false;
    return std::exchange(counts_, {});
}

bool MoveHold::On() const {
    return on_.load(std::memory_order_acquire);
}

bool MoveHold::Apply(const std::optional<std::array<float, 16>>& camera, std::int32_t stick[4]) {
    if (!on_.load(std::memory_order_acquire)) return false;  // no move: no lock taken
    std::lock_guard lock(mu_);
    if (!on_) return false;                                  // stopped since
    ++counts_.reads;
    const auto axes = camera ? MoveAxes(*camera, yaw_, strength_) : std::nullopt;
    if (!axes) {
        ++counts_.no_camera;
        stick[0] = stick[1] = 0;
        return true;
    }
    const auto axis = [](double v) {
        v = v < -1 ? -1 : v > 1 ? 1 : v;
        return static_cast<std::int32_t>(std::lround(v * 32767));
    };
    stick[0] = axis(axes->x), stick[1] = axis(axes->y);
    counts_.last = axes;
    counts_.view = ViewYaw(*camera);
    return true;
}

namespace {

constexpr std::string_view kButtons[] = {"A",  "B",    "X",     "Y",  "L",  "R",  "ZL",   "ZR",
                                         "PLUS", "MINUS", "LS", "RS", "UP", "DOWN", "LEFT", "RIGHT"};

bool KnownButton(std::string_view b) {
    for (const auto k : kButtons) {
        if (k == b) return true;
    }
    return false;
}

std::optional<std::uint32_t> Frames(const CoopModJson::Value& v, std::string& error) {
    const auto n = v.Number("frames");
    if (!v.Get("frames")) return 1u;
    if (!n || *n != std::floor(*n) || *n < 1 || *n > kMaxFrames) {
        error = "frames: a whole number, 1 to 3600";
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*n);
}

bool Stick(const CoopModJson::Value& v, std::string_view key, std::optional<Axes>& out, std::string& error) {
    const CoopModJson::Value* s = v.Get(key);
    if (!s || s->kind == CoopModJson::Value::Kind::Null) return true;
    if (s->kind != CoopModJson::Value::Kind::Array || s->array.size() != 2 ||
        s->array[0].kind != CoopModJson::Value::Kind::Number || s->array[1].kind != CoopModJson::Value::Kind::Number ||
        !std::isfinite(s->array[0].number) || !(s->array[1].number >= 0 && s->array[1].number <= 1)) {
        error = std::string(key) + ": [angle in degrees, strength 0 to 1]";
        return false;
    }
    out = StickAxes(s->array[0].number, s->array[1].number);
    return true;
}

bool PlainText(std::string_view s, std::size_t max) {
    if (s.empty() || s.size() > max) return false;
    for (const char c : s) {
        if (static_cast<unsigned char>(c) < 0x21 || c == ';' || c == '#' || c == 0x7f) return false;
    }
    return true;
}

}  // namespace

std::optional<Request> ParseRequest(std::string_view line, std::string& error) {
    if (line.size() > kMaxLine) {
        error = "the line is longer than 64 KB";
        return std::nullopt;
    }
    std::string why;
    const auto v = CoopModJson::Parse(line, &why);
    if (!v || v->kind != CoopModJson::Value::Kind::Object) {
        error = "not a JSON object" + (why.empty() ? std::string() : ": " + why);
        return std::nullopt;
    }
    Request r;
    if (const auto* id = v->Get("id")) r.id = *id;
    const auto cmd = v->Text("cmd");
    if (!cmd) {
        error = "cmd: a command name";
        return std::nullopt;
    }
    r.cmd = *cmd;
    const std::string& c = r.cmd;
    if (c == "hello" || c == "state" || c == "pause" || c == "resume") return r;
    if (c == "step") {
        const auto f = Frames(*v, error);
        if (!f) return std::nullopt;
        r.frames = *f;
        return r;
    }
    if (c == "input") {
        const auto f = Frames(*v, error);
        if (!f) return std::nullopt;
        r.frames = *f;
        if (const auto* b = v->Get("buttons"); b && b->kind != CoopModJson::Value::Kind::Null) {
            if (b->kind != CoopModJson::Value::Kind::Array || b->array.size() > 16) {
                error = "buttons: a list of names";
                return std::nullopt;
            }
            for (const auto& x : b->array) {
                if (x.kind != CoopModJson::Value::Kind::String || !KnownButton(x.string)) {
                    error = "buttons: A B X Y L R ZL ZR PLUS MINUS LS RS UP DOWN LEFT RIGHT";
                    return std::nullopt;
                }
                r.buttons.push_back(x.string);
            }
        }
        if (!Stick(*v, "stick", r.stick, error) || !Stick(*v, "rstick", r.rstick, error)) return std::nullopt;
        if (const auto* e = v->Get("exclusive"); e && e->kind != CoopModJson::Value::Kind::Bool) {
            error = "exclusive: true or false";
            return std::nullopt;
        }
        r.exclusive = v->Bool("exclusive").value_or(true);
        return r;
    }
    if (c == "move") {
        const auto f = Frames(*v, error);
        if (!f) return std::nullopt;
        r.frames = *f;
        const auto yaw = v->Number("yaw");
        if (!yaw || !std::isfinite(*yaw)) {
            error = "yaw: the world direction in degrees (0 toward +z, 90 toward +x)";
            return std::nullopt;
        }
        r.yaw = *yaw;
        if (v->Get("strength")) {
            const auto s = v->Number("strength");
            if (!s || !(*s >= 0 && *s <= 1)) {
                error = "strength: 0 to 1";
                return std::nullopt;
            }
            r.strength = *s;
        }
        if (const auto* e = v->Get("exclusive"); e && e->kind != CoopModJson::Value::Kind::Bool) {
            error = "exclusive: true or false";
            return std::nullopt;
        }
        r.exclusive = v->Bool("exclusive").value_or(true);
        return r;
    }
    if (c == "warp") {
        const auto scene = v->Text("scene");
        if (!scene || !PlainText(*scene, 64)) {
            error = "scene: a map name";
            return std::nullopt;
        }
        r.scene = *scene;
        if (const auto* e = v->Get("entrance"); e && e->kind != CoopModJson::Value::Kind::Null) {
            if (e->kind != CoopModJson::Value::Kind::String || !PlainText(e->string, 64)) {
                error = "entrance: a start position name";
                return std::nullopt;
            }
            r.entrance = e->string;
        }
        if (v->Get("timeout")) {
            const auto t = v->Number("timeout");
            if (!t || !(*t >= 1 && *t <= 600)) {
                error = "timeout: 1 to 600 seconds";
                return std::nullopt;
            }
            r.timeout_s = *t;
        }
        return r;
    }
    if (c == "flag_read") {
        const auto name = v->Text("name");
        if (!name || !PlainText(*name, 128)) {
            error = "name: a flag name, a 0x hash or key64:0xKEY";
            return std::nullopt;
        }
        if (v->Get("value")) {
            error = "value: not taken; flag_read only reads (flag writes)";
            return std::nullopt;
        }
        r.name = *name;
        return r;
    }
    if (c == "flag") {
        const auto name = v->Text("name");
        if (!name || !PlainText(*name, 128)) {
            error = "name: a flag name or a 0x hash";
            return std::nullopt;
        }
        r.name = *name;
        const auto* val = v->Get("value");
        using K = CoopModJson::Value::Kind;
        if (!val) {
            error = "value: a number, true or false, or an enum value's name";
            return std::nullopt;
        }
        if (val->kind == K::Bool) {
            r.value = val->boolean ? "1" : "0";
        } else if (val->kind == K::Number && val->number == std::floor(val->number) && val->number >= -2147483648.0 &&
                   val->number <= 2147483647.0) {  // (int32, as the game's Int flags; review-01111-b)
            r.value = std::to_string(static_cast<long long>(val->number));
        } else if (val->kind == K::String && PlainText(val->string, 128)) {
            r.value = val->string;  // an enum value's name: setenum (NAME[i]: a String64Array element's text)
        } else if (val->kind == K::String && val->string.empty() && r.name.ends_with(']')) {
            r.value.clear();  // row 294: NAME[i] "" empties a String64Array element
        } else {
            error = "value: a whole number from -2147483648 to 2147483647, true or false, or an enum value's name";
            return std::nullopt;
        }
        return r;
    }
    if (c == "act") {
        const auto lines = v->Text("lines");
        if (!lines || lines->empty() || lines->size() > 256) {
            error = "lines: one test line of at most 256 characters (kill NAME, hit NAME N, hurt N, place X Y Z, ...)";
            return std::nullopt;
        }
        for (const char ch : *lines) {  // one line only: no second line, no comment, no control character
            if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f || ch == '#' || ch == ';') {
                error = "lines: one line, with no new line, ';' or '#'";
                return std::nullopt;
            }
        }
        r.lines = *lines;
        return r;
    }
    if (c == "text") {
        // Read only (the texts the game set in its text panes, coop_talk_text.h): not gated.
        if (const auto* s = v->Get("since"); s && s->kind != CoopModJson::Value::Kind::Null) {
            const auto n = v->Number("since");
            if (!n || !(*n >= 0 && *n <= 9007199254740991.0) || *n != std::floor(*n)) {
                error = "since: the `last` of an earlier text reply (0: every text kept)";
                return std::nullopt;
            }
            r.since = static_cast<std::uint64_t>(*n);
        }
        if (const auto* l = v->Get("layout"); l && l->kind != CoopModJson::Value::Kind::Null) {
            if (l->kind != CoopModJson::Value::Kind::String || !PlainText(l->string, 64)) {
                error = "layout: part of a layout's name (MessageInGame_00 is the message window)";
                return std::nullopt;
            }
            r.layout = l->string;
        }
        return r;
    }
    if (c == "give") {
        const auto item = v->Text("item");
        if (!item || !PlainText(*item, 128)) {
            error = "item: bombs, bow, hookshot or an item flag name";
            return std::nullopt;
        }
        r.item = *item;
        return r;
    }
    if (c == "dump" || c == "shot") {
        const auto tag = v->Text(c == "dump" ? "tag" : "name");
        if (c == "dump" && (!tag || !ValidTag(*tag))) {
            error = "tag: 1 to 64 letters, digits, - and _";
            return std::nullopt;
        }
        if (c == "shot" && v->Get("name") && (!tag || !ValidTag(*tag))) {
            error = "name: 1 to 64 letters, digits, - and _ (the shot goes in the bridge's shots folder)";
            return std::nullopt;
        }
        if (c == "shot" && v->Get("path")) {
            error = "path: not taken; give a name (the shot goes in the bridge's shots folder)";
            return std::nullopt;
        }
        r.tag = tag.value_or("");
        return r;
    }
    if (c == "key") {
        // A host key as the window gives it to the module (hos_coop_key; port-g7 f1-event C3-1, B #137). A key, not a
        // character: text is typed with `act chat`, the game's pad is `input`.
        const auto key = v->Text("key");
        const auto vk = key ? KeyVk(*key) : std::nullopt;
        if (!vk) {
            error = "key: F1 to F12, A to Z or 0 to 9 (a host key, as the window gives it to the module)";
            return std::nullopt;
        }
        r.key = *key;
        r.vk = *vk;
        return r;
    }
    error = "cmd: hello, state, pause, resume, step, input, move, act, warp, flag, flag_read, text, give, dump, shot or key";
    return std::nullopt;
}

std::string Quote(std::string_view text) {
    return Compact(CoopModJson::Value::Of(std::string(text)));
}

std::string Compact(const CoopModJson::Value& value) {
    using K = CoopModJson::Value::Kind;
    switch (value.kind) {
    case K::Array: {
        std::string out = "[";
        for (std::size_t i = 0; i < value.array.size(); ++i) out += (i ? "," : "") + Compact(value.array[i]);
        return out + "]";
    }
    case K::Object: {
        std::string out = "{";
        for (std::size_t i = 0; i < value.object.size(); ++i) {
            out += (i ? "," : "") + Quote(value.object[i].first) + ":" + Compact(value.object[i].second);
        }
        return out + "}";
    }
    default: {
        // (a scalar: the file writer's text is one token, then its newline; control characters are escaped)
        std::string out = CoopModJson::Write(value);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        return out;
    }
    }
}

namespace {
std::string Num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6f", v);
    return buf;
}
}  // namespace

std::string ControlInput(const Request& r, int control_id) {
    std::string step = "{\"at\":0,\"frames\":" + std::to_string(r.frames);
    if (!r.buttons.empty()) {
        step += ",\"buttons\":[";
        for (std::size_t i = 0; i < r.buttons.size(); ++i) step += (i ? "," : "") + Quote(r.buttons[i]);
        step += "]";
    }
    // Both sticks always set (centred when not given), so the step is never empty and an exclusive input holds
    // the sticks still.
    const Axes l = r.stick.value_or(Axes{}), rr = r.rstick.value_or(Axes{});
    step += ",\"lx\":" + Num(l.x) + ",\"ly\":" + Num(l.y) + ",\"rx\":" + Num(rr.x) + ",\"ry\":" + Num(rr.y) + "}";
    return "{\"id\":" + std::to_string(control_id) + ",\"cmd\":\"sequence\",\"steps\":[" + step +
           "],\"exclusive\":" + (r.exclusive ? "true" : "false") + ",\"wait\":false}";
}

std::string Reply(const CoopModJson::Value& id, std::uint64_t frame, bool paused, const std::string& rest) {
    std::string out = "{\"id\":" + Compact(id) + ",\"ok\":true,\"frame\":" + std::to_string(frame) +
                      ",\"paused\":" + (paused ? "true" : "false");
    if (!rest.empty()) out += "," + rest;
    return out + "}";
}

std::string TeleportFailure(std::string_view code, std::string_view message) {
    const std::string c = code.empty() ? "failed" : std::string(code);
    if (message.empty()) {
        return "Longshot's teleport: " + c + ", with no message from Longshot (its log, longshot_log.txt, may say why)";
    }
    return "Longshot's teleport: " + c + ": " + std::string(message);
}

std::string ErrorReply(const CoopModJson::Value& id, std::uint64_t frame, bool paused, std::string_view code,
                       std::string_view message) {
    if (message.empty()) {
        const std::string why = "no reason was given for `" + std::string(code) +
                                "` (the game's own log, the `coop:` lines, may say why)";
        return ErrorReply(id, frame, paused, code, why);
    }
    return "{\"id\":" + Compact(id) + ",\"ok\":false,\"frame\":" + std::to_string(frame) +
           ",\"paused\":" + (paused ? "true" : "false") + ",\"error\":" + Quote(code) + ",\"message\":" +
           Quote(message) + "}";
}

std::string HashText(std::uint32_t hash) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08x", static_cast<unsigned>(hash));
    return buf;
}

std::optional<std::uint32_t> HashValue(std::string_view text) {
    if (text.size() < 3 || text.size() > 10 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) return std::nullopt;
    std::uint32_t hash = 0;
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data() + 2, end, hash, 16);
    if (ec != std::errc{} || ptr != end || hash == 0) return std::nullopt;
    return hash;
}

bool IsWholeNumber(std::string_view value) {
    if (!value.empty() && value[0] == '-') value.remove_prefix(1);
    if (value.empty()) return false;
    for (const char ch : value) {
        if (ch < '0' || ch > '9') return false;
    }
    return true;
}

std::string FlagReadMembers(std::string_view name, const FlagValue& v) {
    std::string out = "\"name\":" + Quote(name) + ",\"kind\":" + Quote(v.kind) + ",\"value\":";
    if (v.kind == "enum" && v.hash) {
        out += Quote(HashText(*v.hash));
    } else if (v.kind == "text") {
        out += Quote(v.text);
    } else if (v.kind != "unknown" && v.number) {
        out += std::to_string(*v.number);
    } else {
        out += "null";
    }
    if (v.kind == "int") {
        const bool pending = v.number && v.live && *v.number != *v.live;
        out += std::string(",\"pending\":") + (pending ? "true" : "false");
    }
    return out;
}

void EnumValues::Remember(std::string_view name, std::uint32_t hash) {
    std::lock_guard lock(mu_);
    for (const auto& [n, h] : seen_) {
        if (n == name && h == hash) return;
    }
    if (seen_.size() >= 4096) seen_.erase(seen_.begin());  // (a bound: a run reads far fewer)
    seen_.emplace_back(std::string(name), hash);
}

bool EnumValues::Known(std::string_view name, std::uint32_t hash) const {
    std::lock_guard lock(mu_);
    for (const auto& [n, h] : seen_) {
        if (n == name && h == hash) return true;
    }
    return false;
}

std::string EnumHashRefusal(const EnumValues& known, std::string_view name, std::string_view value) {
    const auto hash = HashValue(value);
    if (!hash) return "value: a 0x enum value hash is 0x and 1 to 8 hex digits, not 0";
    if (!known.Known(name, *hash)) {
        return "value: " + std::string(value) + " is not a value flag_read read from " + std::string(name) +
               " in this game run; write an enum by its value's name, or read the flag first (a raw number is never "
               "written to an Enum)";
    }
    return {};
}

bool EnvIsOn(std::string_view value) { return !value.empty() && value != "0"; }

bool GatedCommand(std::string_view cmd) { return cmd == "act" || cmd == "warp" || cmd == "flag" || cmd == "give"; }

std::string VerbRefusal(bool runner, bool cheats) {
    if (!runner) {
        return "refused: this is not a test run (LICHII_COOP_PLAYTEST_VERBS is not set by the test runner), and these "
               "commands reach the wire and "
               "other players";
    }
    if (!cheats) return "refused: cheats are off (LICHII_COOP_CHEATS is not set)";
    return {};
}

std::string VerbGateLine(bool runner, bool cheats) {
    return std::string("verbs ") + (runner && cheats ? "on" : "off") + " (test runner: " + (runner ? "yes" : "no") +
           ", cheats: " + (cheats ? "on" : "off") + ")";
}

namespace {
std::string Num(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", std::isfinite(v) ? static_cast<double>(v) : 0.0);
    return buf;
}
}  // namespace

std::string WorldMember(bool enabled, const std::optional<CoopWorldSnapshot::Snapshot>& snapshot,
                        std::uint64_t now_frame, std::uint64_t now_ms, bool full) {
    if (!enabled) return "";
    if (!snapshot) {
        return ",\"world\":{\"frame\":null,\"stale\":false,\"total\":0,\"other_count\":0,\"count\":0,"
               "\"why\":\"no co-op session: the world list is read only while connected and sharing\",\"actors\":[]}";
    }
    const std::uint64_t age = now_ms >= snapshot->taken_ms ? now_ms - snapshot->taken_ms : 0;
    const auto fresh = CoopWorldSnapshot::Judge(snapshot->frame, now_frame, age);
    const bool stale = fresh == CoopWorldSnapshot::Freshness::stale;
    std::string out = ",\"world\":{\"frame\":" + std::to_string(snapshot->frame) +
                      ",\"age_ms\":" + std::to_string(age) +
                      ",\"stale\":" + (stale ? "true" : "false") +
                      ",\"behind\":" + (fresh == CoopWorldSnapshot::Freshness::behind ? "true" : "false") +
                      ",\"cost_us\":" + std::to_string(snapshot->cost_us) +
                      ",\"solo\":" + (snapshot->solo ? "true" : "false");  // the solo walk: no strike by id
    // The snapshot is sorted known kinds first, so the known ones lead; `state` stops at kStateLimit of them.
    std::size_t other = 0;
    for (const auto& a : snapshot->actors) other += a.kind == CoopWorldSnapshot::Kind::other ? 1 : 0;
    std::size_t count = stale ? 0 : snapshot->actors.size();
    if (!full && count > 0) {
        const std::size_t known = snapshot->actors.size() - other;
        count = std::min(known, CoopWorldSnapshot::kStateLimit);
    }
    out += ",\"total\":" + std::to_string(stale ? 0 : snapshot->actors.size()) +
           ",\"other_count\":" + std::to_string(stale ? 0 : other) + ",\"count\":" + std::to_string(count);
    if (stale) {
        out += ",\"why\":" + Quote("stale: no walk for " + std::to_string(age) + " ms (a load, or the session ended)");
    } else if (snapshot->actors.empty()) {
        out += ",\"why\":\"empty: the walk ran and saw no actors\"";
    } else if (count == 0 && other > 0) {  // (state only: the dump lists them)
        out += ",\"why\":\"only other kinds near: dump lists them\"";
    }
    out += ",\"actors\":[";
    for (std::size_t i = 0; i < count; ++i) {
        const auto& a = snapshot->actors[i];
        char hex[24];
        std::snprintf(hex, sizeof hex, "0x%llx", static_cast<unsigned long long>(a.id));
        out += i ? "," : "";
        out += "{\"id\":" + std::to_string(a.id) + ",\"addr\":\"" + hex + "\",\"name\":" + Quote(a.name) + ",\"kind\":\"" +
               CoopWorldSnapshot::KindName(a.kind) + "\",\"pos\":[" + Num(a.pos[0]) + "," + Num(a.pos[1]) + "," +
               Num(a.pos[2]) + "],\"dist\":" + Num(a.dist) +
               ",\"life\":" + (a.life ? std::to_string(*a.life) : "null") +
               ",\"max_life\":" + (a.max_life ? std::to_string(*a.max_life) : "null") +
               ",\"owner\":" + CoopWorldSnapshot::OwnerJson(a.owner) + ",\"alive\":" +
               (a.alive ? (*a.alive ? "true" : "false") : "null") + "}";
    }
    return out + "]}";
}

namespace {
bool Digits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
}
std::string Lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });
    return out;
}
}  // namespace

std::string StrikeIdRefusal(std::string_view lines, bool world_list) {
    if (world_list) return "";
    std::size_t pos = 0;
    while (pos <= lines.size()) {
        std::size_t end = lines.find('\n', pos);
        if (end == std::string_view::npos) end = lines.size();
        const std::string_view line = lines.substr(pos, end - pos);
        pos = end + 1;
        std::vector<std::string_view> words;
        for (std::size_t i = 0; i < line.size();) {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
            const std::size_t start = i;
            while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') ++i;
            if (i > start) words.push_back(line.substr(start, i - start));
        }
        if (words.size() < 2) continue;
        const std::string cmd = Lower(words[0]);
        if ((cmd == "kill" || cmd == "hit") && Digits(words[1])) {
            return "no world list: an actor id is resolved in the objects walk, which runs only while connected and "
                   "sharing; use the name alone (the nearest of that name) or connect first";
        }
    }
    return "";
}

bool WorldListLive(const std::optional<CoopWorldSnapshot::Snapshot>& snapshot, std::uint64_t now_ms,
                   std::uint64_t now_frame) {
    if (!snapshot) return false;
    const std::uint64_t age = now_ms >= snapshot->taken_ms ? now_ms - snapshot->taken_ms : 0;
    return !CoopWorldSnapshot::IsStale(snapshot->frame, now_frame, age);
}

bool HasStrikeLine(std::string_view lines) {
    std::size_t pos = 0;
    while (pos <= lines.size()) {
        std::size_t end = lines.find('\n', pos);
        if (end == std::string_view::npos) end = lines.size();
        std::string_view line = lines.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        std::size_t n = 0;
        while (n < line.size() && line[n] != ' ' && line[n] != '\t' && line[n] != '\r') ++n;
        const std::string cmd = Lower(line.substr(0, n));
        if (cmd == "kill" || cmd == "hit") return true;
    }
    return false;
}

std::string StrikeMember(const std::optional<CoopStrike::Result>& last) {
    if (!last) return "";
    return ",\"strike\":{\"seq\":" + std::to_string(last->seq) + ",\"result\":" + Quote(last->result) +
           ",\"name\":" + Quote(last->name) + ",\"id\":" + std::to_string(last->id) + "}";
}

namespace {
const CoopModJson::Value* GameOf(const CoopModJson::Value* status) {
    const auto* game = status ? status->Get("game") : nullptr;
    return game && game->kind == CoopModJson::Value::Kind::Object ? game : nullptr;
}
}  // namespace

bool SnapshotIsOld(const CoopModJson::Value* status) {
    const auto* game = GameOf(status);
    if (!game) return false;
    const auto at = game->Number("frame"), now = status->Number("frame");
    return at && now && *now - *at > kSnapshotSlack;
}

bool SnapshotMissing(const CoopModJson::Value* status) {
    if (!status) return false;
    const auto* game = GameOf(status);
    return !game || !game->Number("frame");
}

std::string NotReadyReason(const CoopModJson::Value* status) {
    if (!status) return "no answer from Longshot's control bridge, so there is no Link sample (see `control`)";
    if (SnapshotIsOld(status)) {
        return "the Link sample is older than the frame: no Link update ran (a title, a pause or a load is on); "
               "step a frame in play, then read the state again";
    }
    if (SnapshotMissing(status)) {
        return "no Link sample has been taken yet: Longshot takes it in a running Link update (a title, a pause or a "
               "load runs none); step a frame in play, then read the state again";
    }
    return "";
}

std::string StickProblem(const std::vector<StickRead>& reads, int frames) {
    std::string out;
    for (const auto& r : reads) {
        if (!r.before || !r.written || !r.after || *r.written == *r.before || *r.after != *r.before) continue;
        out += (out.empty() ? "did not stick: " : "; ") + r.name + " written " + std::to_string(*r.written) +
               ", the game put back " + std::to_string(*r.after) + " within " + std::to_string(frames) + " frames";
    }
    return out;
}

std::string GiveRefusal(const CoopModJson::Value* status) {
    const auto* game = GameOf(status);
    if (game && game->Bool("loading") == true) {
        return "a scene or save is loading and would put the old values back over the gift: step until the load "
               "ends (the state's `loading` is false), then give again";
    }
    // An old or missing sample says nothing about a load (none runs a Link update). Not given, nothing is lost.
    if (status && (SnapshotIsOld(status) || SnapshotMissing(status))) {
        return "the game has no fresh Link state, so a load, a pause or a title may be on and would take the gift back "
               "or lose it: step until the state's `not_ready` is null (Link in play), then give again";
    }
    return "";
}

std::string LinkMembers(const CoopModJson::Value* status) {
    const auto* game = GameOf(status);
    const bool old = SnapshotIsOld(status);
    std::string out;
    for (const char* k : {"pos", "facing", "camera", "life", "max_life", "rupees", "age", "event", "loading", "prompt"}) {
        const auto* v = old || !game ? nullptr : game->Get(k);
        out += ",\"" + std::string(k) + "\":" + (v ? Compact(*v) : std::string("null"));
    }
    const auto* at = game ? game->Get("frame") : nullptr;
    out += ",\"snapshot_present\":" + (at ? Compact(*at) : std::string("null"));
    out += std::string(",\"snapshot_stale\":") + (old ? "true" : "false");
    const std::string why = NotReadyReason(status);
    out += ",\"not_ready\":" + (why.empty() ? std::string("null") : Quote(why));
    return out;
}

std::string RoomMember(const CoopModJson::Value* status, std::string_view scene, const CoopRooms::Table& rooms) {
    const auto* game = GameOf(status);
    const auto* pos = game && !SnapshotIsOld(status) && !SnapshotMissing(status) ? game->Get("pos") : nullptr;
    if (!pos || pos->kind != CoopModJson::Value::Kind::Array || pos->array.size() != 3 || scene.empty()) {
        return ",\"room\":null";
    }
    CoopRooms::Vec3 at{};
    for (std::size_t i = 0; i < 3; ++i) {
        if (pos->array[i].kind != CoopModJson::Value::Kind::Number) return ",\"room\":null";
        at[i] = static_cast<float>(pos->array[i].number);
    }
    const auto guess = rooms.Nearest(scene, at);
    return ",\"room\":" + (guess ? std::to_string(guess->room) : std::string("null"));
}

std::string ViewMember(const std::optional<std::array<float, 16>>& clip_from_world, bool y_down) {
    if (!clip_from_world) return ",\"view\":null";
    const auto& m = *clip_from_world;
    for (const float v : m) {
        if (!std::isfinite(v)) return ",\"view\":null";
    }
    const auto yaw = ViewYaw(m);
    if (!yaw) return ",\"view\":null";
    const double pitch = std::atan2(static_cast<double>(m[13]), std::hypot(m[12], m[14])) * 180.0 / 3.14159265358979323846;
    auto num = [](double v, double scale) { return Compact(CoopModJson::Value::Of(std::round(v * scale) / scale + 0.0)); };   // + 0.0: no "-0"
    std::string out = ",\"view\":{\"yaw\":" + num(*yaw, 100) + ",\"pitch\":" + num(pitch, 100) + ",\"clip\":[";
    for (std::size_t i = 0; i < m.size(); ++i) {
        if (i) out += ",";
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.6g", static_cast<double>(m[i]));
        out += buf;
    }
    return out + "],\"y_down\":" + (y_down ? "true" : "false") + "}";
}

std::string BodyMember(const std::optional<CoopWorldSnapshot::Snapshot>& snapshot, bool live) {
    if (snapshot && live) {
        for (const auto& a : snapshot->actors) {
            if (a.name == "Player_Child") return ",\"body\":\"child\"";
            if (a.name == "Player_Adult") return ",\"body\":\"adult\"";
        }
    }
    return ",\"body\":null";
}

namespace {
bool SameName(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

// "child", "adult" or "" from a scene name's ending (any case).
std::string_view AgeEnding(std::string_view scene) {
    for (const std::string_view suffix : {"_Child", "_Adult"}) {
        if (scene.size() > suffix.size() && SameName(scene.substr(scene.size() - suffix.size()), suffix)) {
            return suffix == "_Child" ? "child" : "adult";
        }
    }
    return {};
}
}  // namespace

WarpPlan PlanWarp(const CoopModJson::Value* status, bool entrance_given, std::string_view scene) {
    const auto* game = GameOf(status);
    // A missing sample is no better than an old one: the typed members alone are not a Link sample (not_in_play).
    if (game && !SnapshotIsOld(status) && !SnapshotMissing(status)) {
        if (game->Bool("event") == true) return {WarpRoute::event, {}, {}, {}};
        if (game->Bool("event") == false) {
            const auto want = AgeEnding(scene);
            const auto age = game->Text("age");
            if (!want.empty() && age && *age != want) {
                // Longshot's teleport lands at Link's current age: set the age first, then the map change event.
                return {WarpRoute::map_event, {}, {}, std::string(want)};
            }
            // An entrance names a start point, which Longshot's teleport cannot take: the game's map change event can.
            return {entrance_given ? WarpRoute::map_event : WarpRoute::teleport, {}, {}, {}};
        }
    }
    return {WarpRoute::refuse, "not_in_play",
            "this game's state is not readable now (no Link update: a title, a pause or a load). Try again once the "
            "game runs", {}};
}

TeleportWait ReadTeleport(const CoopModJson::Value& reply, std::string_view scene) {
    TeleportWait out;
    const auto map = reply.Text("map");
    out.wait_for = (map && !map->empty()) ? *map : std::string(scene);
    if (reply.Bool("accepted") == false) {
        out.failed = true;
        out.code = "refused";
        out.why = "Longshot sent the teleport to " + out.wait_for +
                  ", but the game's event manager did not accept it (an event or a load was in the way): try again "
                  "when the game is in free play (Longshot allows one teleport every 8 s)";
    }
    return out;
}

bool WarpArrived(const CoopModJson::Value* status, std::string_view scene) {
    const auto* game = GameOf(status);
    if (!game || SnapshotIsOld(status) || SnapshotMissing(status) || game->Bool("event") != false || game->Bool("loading") != false) return false;
    const auto map = status->Text("map");
    if (!map) return false;
    if (SameName(*map, scene)) return true;
    for (const std::string_view suffix : {"_Child", "_Adult"}) {
        if (scene.size() > suffix.size() && SameName(scene.substr(scene.size() - suffix.size()), suffix) &&
            SameName(scene.substr(0, scene.size() - suffix.size()), *map)) {
            return true;
        }
    }
    return false;
}

}  // namespace CoopPlaytest
