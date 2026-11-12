// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_live_events.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace CoopLiveEvents {

namespace {

// Near one of the event's places in this scene; nullopt when it has none here.
std::optional<bool> NearPlace(std::string_view event, std::string_view scene,
                              const std::optional<std::array<float, 3>>& link) {
    bool any = false;
    for (const Place& p : Places()) {
        if (p.event != event || p.scene != scene) {
            continue;
        }
        any = true;
        if (!link) {
            continue;
        }
        const float dx = (*link)[0] - p.x;
        const float dy = (*link)[1] - p.y;
        const float dz = (*link)[2] - p.z;
        if (dx * dx + dy * dy + dz * dz <= kNearM * kNearM) {
            return true;
        }
    }
    return any ? std::optional<bool>(false) : std::nullopt;
}

} // namespace

std::optional<std::string> Decider::Next(const std::vector<std::string>& held, const std::string& scene,
                                         bool event_running, std::optional<std::array<float, 3>> link) {
    if (scene.empty()) {
        return std::nullopt;  // no scene read (a load): the same visit, nothing asked
    }
    Visit(scene);
    if (event_running) {
        return std::nullopt;
    }
    std::vector<std::string> wanted = held;
    for (const std::string& name : early_) {
        if (std::find(wanted.begin(), wanted.end(), name) == wanted.end()) {
            wanted.push_back(name);
        }
    }
    for (const std::string& name : wanted) {
        const bool named = allowed_.contains(name);
        if (!allowed_.empty() && !named) {
            continue;
        }
        if (asked_.contains(name)) {
            continue;
        }
        const auto near = NearPlace(name, scene, link);
        if (near ? !*near : !named) {
            continue;  // not near it yet (it waits), or no place known and not asked by name
        }
        asked_.insert(name);
        early_.erase(name);
        return name;
    }
    return std::nullopt;
}

void Decider::Visit(const std::string& scene) {
    if (scene != scene_) {
        scene_ = scene;
        asked_.clear();
        early_.clear();
    }
}

void Decider::Started(const std::string& name, const std::string& scene) {
    if (scene.empty() || name.empty()) {
        return;
    }
    Visit(scene);
    if (!asked_.contains(name)) {
        early_.insert(name);
    }
}

void Decider::Ours(const std::string& name, const std::string& scene) {
    if (scene.empty() || name.empty()) {
        return;
    }
    Visit(scene);
    asked_.insert(name);
    early_.erase(name);
}

std::vector<std::string_view> EventsIn(std::string_view scene) {
    std::vector<std::string_view> out;
    for (const Place& p : Places()) {
        if (p.scene == scene && std::find(out.begin(), out.end(), p.event) == out.end()) {
            out.push_back(p.event);
        }
    }
    return out;
}

namespace {

// The name of `names` that `bytes` hold at `at` as a whole C string (the
// start or a NUL before it, a NUL after it), or nullptr.
const std::string_view* NameAt(const std::vector<std::uint8_t>& bytes, std::size_t at,
                               const std::vector<std::string_view>& names) {
    if (at > 0 && bytes[at - 1] != 0) {
        return nullptr;
    }
    for (const std::string_view& n : names) {
        if (at + n.size() < bytes.size() && bytes[at + n.size()] == 0 &&
            std::memcmp(bytes.data() + at, n.data(), n.size()) == 0) {
            return &n;
        }
    }
    return nullptr;
}

// `size` bytes at `address`, read in 0x100-byte pieces; a piece that cannot
// be read stays zero (a mapping may end inside the block).
std::vector<std::uint8_t> ReadSome(Reader read, void* context, std::uint64_t address, std::size_t size) {
    std::vector<std::uint8_t> out(size, 0);
    for (std::size_t at = 0; at < size; at += 0x100) {
        const std::size_t n = std::min<std::size_t>(0x100, size - at);
        if (!read(context, address + at, out.data() + at, n)) {
            std::fill(out.begin() + static_cast<std::ptrdiff_t>(at),
                      out.begin() + static_cast<std::ptrdiff_t>(at + n), std::uint8_t{0});
        }
    }
    return out;
}

// A word that may point into the game's memory (not 0, not a small number).
bool MayPoint(std::uint64_t w) {
    return w >= 0x10000 && w < (std::uint64_t{1} << 48);
}

} // namespace

std::optional<std::string> FindRunningEvent(Reader read, void* context, std::uint64_t main,
                                            const std::vector<std::string_view>& names) {
    if (names.empty()) {
        return std::nullopt;
    }
    const auto u64 = [&](std::uint64_t a) -> std::optional<std::uint64_t> {
        std::uint64_t v = 0;
        if (!read(context, a, &v, 8)) {
            return std::nullopt;
        }
        return v;
    };
    const auto manager = u64(main + addr::kEventStatePtr);
    if (!manager || *manager == 0) {
        return std::nullopt;
    }
    const auto session = u64(*manager + addr::kEventSession);
    if (!session || *session == 0) {
        return std::nullopt;
    }
    std::uint64_t event = 0;
    std::uint32_t count = 0;
    std::uint32_t current = 0;
    const auto events = u64(*session + addr::kSessionEvents);
    if (events && *events != 0 && read(context, *session + addr::kSessionCount, &count, 4) &&
        read(context, *session + addr::kSessionCurrent, &current, 4) && current < count &&
        count < 0x100) {
        event = u64(*events + 8ull * current).value_or(0);
    }
    std::size_t longest = 0;
    for (const std::string_view& n : names) {
        longest = std::max(longest, n.size());
    }
    // The string a word points to: one read of the longest name's length;
    // when that fails, each name's own length (a string may end right before
    // unmapped memory).
    const auto pointed = [&](std::uint64_t w) -> std::optional<std::string> {
        std::vector<std::uint8_t> all(longest + 1, 0xff);
        if (read(context, w, all.data(), all.size())) {
            const auto* n = NameAt(all, 0, names);
            return n != nullptr ? std::optional<std::string>(std::string(*n)) : std::nullopt;
        }
        for (const std::string_view& n : names) {
            std::vector<std::uint8_t> s(n.size() + 1, 0xff);
            if (read(context, w, s.data(), s.size()) && s.back() == 0 &&
                std::memcmp(s.data(), n.data(), n.size()) == 0) {
                return std::string(n);
            }
        }
        return std::nullopt;
    };
    const auto search = [&](const std::vector<std::uint8_t>& block,
                            std::vector<std::uint64_t>* words) -> std::optional<std::string> {
        for (std::size_t at = 0; at < block.size(); ++at) {
            if (const auto* n = NameAt(block, at, names)) {
                return std::string(*n);
            }
        }
        for (std::size_t at = 0; at + 8 <= block.size(); at += 8) {
            std::uint64_t w = 0;
            std::memcpy(&w, block.data() + at, 8);
            if (!MayPoint(w)) {
                continue;
            }
            if (auto n = pointed(w)) {
                return n;
            }
            if (words != nullptr) {
                words->push_back(w);
            }
        }
        return std::nullopt;
    };
    std::vector<std::uint64_t> inner;
    if (event != 0) {
        if (auto n = search(ReadSome(read, context, event, addr::kScanEvent), &inner)) {
            return n;
        }
    }
    if (auto n = search(ReadSome(read, context, *session, addr::kScanSession), nullptr)) {
        return n;
    }
    for (const std::uint64_t w : inner) {
        if (auto n = search(ReadSome(read, context, w, addr::kScanInner), nullptr)) {
            return n;
        }
    }
    return std::nullopt;
}

std::optional<std::set<std::string>> ParseNames(std::string_view text) {
    std::set<std::string> out;
    const auto trim = [](std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
        return s;
    };
    if (trim(text) == "all") {
        return out;
    }
    while (!text.empty()) {
        const auto comma = text.find(',');
        const std::string_view part = trim(text.substr(0, comma));
        if (!part.empty()) {
            out.emplace(part);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    if (out.empty()) {
        return std::nullopt;  // no names: not "all"
    }
    return out;
}

void Requester::Want(std::string name, std::string scene, std::uint64_t at_us, std::uint64_t max_age_us) {
    if (name.empty()) {
        notes_.push_back("live event with no name not asked");
        return;
    }
    if (name.size() >= 0x80 || wanted_.size() >= 8) {
        notes_.push_back("live event " + name + " not asked (name too long or too many waiting)");
        return;
    }
    const bool waiting =
        std::any_of(wanted_.begin(), wanted_.end(), [&](const Wanted& w) { return w.name == name; });
    if (!waiting && name != current_) {
        wanted_.push_back(Wanted{std::move(name), std::move(scene), at_us, max_age_us});
    }
}

std::vector<std::string> Requester::TakeNotes() {
    std::vector<std::string> out;
    out.swap(notes_);
    return out;
}

std::optional<CoopPuppet::Call> Requester::FreeFirst(CoopPuppet::Guest& guest, std::uint64_t main) {
    const std::uint64_t vtable = main + addr::kStringVtable;
    guest.Write(block_ + addr::kArgMember1, &vtable, 8);
    CoopPuppet::Call c;
    c.function = main + addr::kStringFree;
    c.x[0] = block_ + addr::kArgMember1;
    next_ = Next::free1;
    return c;
}

bool Requester::EventRunning(CoopPuppet::Guest& guest, std::uint64_t main) {
    std::uint64_t manager = 0;
    std::uint64_t session = 0;
    // docs/dungeons-boss.md: an event runs while [[main + 0x4e94840] + 0x68] is set.
    if (!guest.Read(main + addr::kEventStatePtr, &manager, 8) || manager == 0) {
        return false;
    }
    return guest.Read(manager + addr::kEventSession, &session, 8) && session != 0;
}

std::optional<CoopPuppet::Call> Requester::Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                                std::uint64_t main) {
    CoopPuppet::Call c;
    if (after_call) {
        const Next done = next_;
        next_ = Next::none;
        switch (done) {
        case Next::malloc:
            if (result == 0) {
                malloc_failed_ = true;
                notes_.push_back("live events: the game's malloc gave nothing; live events off");
                return std::nullopt;
            }
            block_ = result;
            break;  // then build, below
        case Next::build: {
            // The name into the arg's own buffer, as the node copies it.
            std::uint64_t buffer = 0;
            std::uint32_t cap = 0;
            if (!guest.Read(block_ + addr::kArgName, &buffer, 8) || !guest.Read(block_ + addr::kArgNameCap, &cap, 4) ||
                buffer == 0 || cap == 0) {
                notes_.push_back("live event " + current_ + ": the request has no name buffer; not asked");
                return FreeFirst(guest, main);  // the build made its members: free them
            }
            const std::size_t n = std::min<std::size_t>(current_.size(), cap - 1);
            std::string text = current_.substr(0, n);
            text.push_back('\0');
            if (!guest.Write(buffer, text.data(), text.size())) {
                notes_.push_back("live event " + current_ + ": name not written; not asked");
                return FreeFirst(guest, main);
            }
            std::uint64_t holder = 0;
            std::uint64_t manager = 0;
            if (!guest.Read(main + addr::kManagerPtr, &holder, 8) || holder == 0 || !guest.Read(holder, &manager, 8) ||
                manager == 0) {
                notes_.push_back("live event " + current_ + ": no event manager; not asked");
                return FreeFirst(guest, main);  // still free what the build made
            }
            c.function = main + addr::kRequest;
            c.x[0] = manager;
            c.x[1] = block_;
            next_ = Next::request;
            return c;
        }
        case Next::request: {
            notes_.push_back("live event " + current_ + " asked of the game (result " + std::to_string(result & 0xff) +
                             ")");
            return FreeFirst(guest, main);
        }
        case Next::free1: {
            const std::uint64_t vtable = main + addr::kStringVtable;
            guest.Write(block_ + addr::kArgMember2, &vtable, 8);
            c.function = main + addr::kStringFree;
            c.x[0] = block_ + addr::kArgMember2;
            next_ = Next::free2;
            return c;
        }
        case Next::free2:
            current_.clear();
            return std::nullopt;  // one event a frame
        case Next::none:
            return std::nullopt;
        }
    }
    if (next_ != Next::none || malloc_failed_ || main == 0) {
        return std::nullopt;
    }
    if (block_ == 0) {
        if (wanted_.empty()) {
            return std::nullopt;
        }
        c.function = main + addr::kMalloc;
        c.x[0] = addr::kArgSize;
        next_ = Next::malloc;
        return c;
    }
    if (current_.empty() && wanted_.empty()) {
        return std::nullopt;
    }
    if (EventRunning(guest, main)) {
        return std::nullopt;  // an event began since it was asked: wait, it stays queued
    }
    if (current_.empty()) {
        current_ = std::move(wanted_.front().name);
        wanted_.pop_front();
    }
    c.function = main + addr::kArgBuild;
    c.x[0] = block_;
    next_ = Next::build;
    return c;
}

} // namespace CoopLiveEvents
