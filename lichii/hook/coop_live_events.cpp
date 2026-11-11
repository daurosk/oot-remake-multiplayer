// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_live_events.h"

#include <algorithm>
#include <cstring>

namespace CoopLiveEvents {

std::optional<std::string> Decider::Next(const std::vector<std::string>& held, const std::string& scene,
                                         bool event_running) {
    if (scene != scene_) {
        scene_ = scene;
        asked_.clear();
    }
    if (event_running) {
        return std::nullopt;
    }
    for (const std::string& name : held) {
        if (!allowed_.empty() && !allowed_.contains(name)) {
            continue;
        }
        if (asked_.insert(name).second) {
            return name;
        }
    }
    return std::nullopt;
}

std::set<std::string> ParseNames(std::string_view text) {
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
    return out;
}

void Requester::Want(std::string name) {
    if (name.empty() || name.size() >= 0x80 || wanted_.size() >= 8) {
        notes_.push_back("live event " + name + " not asked (name too long or too many waiting)");
        return;
    }
    if (std::find(wanted_.begin(), wanted_.end(), name) == wanted_.end() && name != current_) {
        wanted_.push_back(std::move(name));
    }
}

std::vector<std::string> Requester::TakeNotes() {
    std::vector<std::string> out;
    out.swap(notes_);
    return out;
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
                current_.clear();
                return std::nullopt;  // nothing to free: built without a name
            }
            const std::size_t n = std::min<std::size_t>(current_.size(), cap - 1);
            std::string text = current_.substr(0, n);
            text.push_back('\0');
            if (!guest.Write(buffer, text.data(), text.size())) {
                notes_.push_back("live event " + current_ + ": name not written; not asked");
                current_.clear();
                return std::nullopt;
            }
            std::uint64_t holder = 0;
            std::uint64_t manager = 0;
            if (!guest.Read(main + addr::kManagerPtr, &holder, 8) || holder == 0 || !guest.Read(holder, &manager, 8) ||
                manager == 0) {
                notes_.push_back("live event " + current_ + ": no event manager; not asked");
                next_ = Next::free1;  // still free what the build made
                const std::uint64_t vtable = main + addr::kStringVtable;
                guest.Write(block_ + addr::kArgMember1, &vtable, 8);
                c.function = main + addr::kStringFree;
                c.x[0] = block_ + addr::kArgMember1;
                return c;
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
            const std::uint64_t vtable = main + addr::kStringVtable;
            guest.Write(block_ + addr::kArgMember1, &vtable, 8);
            c.function = main + addr::kStringFree;
            c.x[0] = block_ + addr::kArgMember1;
            next_ = Next::free1;
            return c;
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
    if (current_.empty()) {
        if (wanted_.empty()) {
            return std::nullopt;
        }
        current_ = std::move(wanted_.front());
        wanted_.pop_front();
    }
    c.function = main + addr::kArgBuild;
    c.x[0] = block_;
    next_ = Next::build;
    return c;
}

} // namespace CoopLiveEvents
