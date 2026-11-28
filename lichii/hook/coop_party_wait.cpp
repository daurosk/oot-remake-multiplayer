// SPDX-License-Identifier: GPL-3.0-or-later
// Wait for the party (docs/party-wait.md): the rules. Pure.
#include "coop_party_wait.h"

#include <algorithm>
#include <cctype>

namespace CoopPartyWait {

std::string WaitLine(unsigned here_with_us, unsigned all_with_us) {
    return "Waiting for the other players to sync... (" + std::to_string(here_with_us) + "/" +
           std::to_string(all_with_us) + " here)";
}

std::string WhereText(Kind kind, const std::string& dungeon) {
    const char* what = kind == Kind::boss_door ? "the boss door" : kind == Kind::combat_room ? "a locked room" : "a big chest";
    return dungeon.empty() ? std::string(what) : std::string(what) + " (" + dungeon + ")";
}

std::optional<Command> Parse(std::string_view text) {
    auto lower = [](std::string_view s) {
        std::string out(s);
        for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };
    auto trim = [](std::string_view s) {
        while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
        while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
        return s;
    };
    text = trim(text);
    if (text.size() < 5 || lower(text.substr(0, 5)) != "/wait" || (text.size() > 5 && text[5] != ' ')) {
        return std::nullopt;
    }
    const std::string arg = lower(trim(text.substr(5)));
    if (arg.empty()) return Command::show;
    if (arg == "on" || arg == "1" || arg == "yes") return Command::on;
    if (arg == "off" || arg == "0" || arg == "no") return Command::off;
    return Command::bad;
}

void Waiter::Reset() {
    waiting_.reset();
    scene_.clear();
    done_.clear();
}

Output Waiter::Frame(const Input& in) {
    Output out;
    if (!in.on || in.others == 0 || in.scene.empty()) {
        waiting_.reset();   // off, alone or not in a game: nothing shown, nothing kept waiting
        if (in.scene.empty()) {
            scene_.clear();
            done_.clear();
        }
        return out;
    }
    if (in.scene != scene_) {   // a new visit: every moment may wait again
        scene_ = in.scene;
        done_.clear();
        waiting_.reset();
    }
    const unsigned here = std::min(in.here, in.others);
    if (waiting_) {
        if (!in.moment || in.moment->id != waiting_->moment.id) {
            waiting_.reset();   // our Link left the moment: the wait ends, nothing said
        } else if (here == in.others) {
            out.notices.push_back("Everyone is here.");
            waiting_.reset();
            return out;
        } else if (in.now_us - waiting_->since_us >= kGiveUpUs) {
            out.notices.push_back("Not everyone is here: going on.");
            waiting_.reset();
            return out;
        } else {
            if ((here != waiting_->shown_here || in.others != waiting_->shown_all) &&
                in.now_us - waiting_->shown_us >= kRecountUs) {
                out.notices.push_back(WaitLine(here + 1, in.others + 1));
                waiting_->shown_us = in.now_us;
                waiting_->shown_here = here;
                waiting_->shown_all = in.others;
            }
            return out;
        }
    }
    if (!in.moment || std::find(done_.begin(), done_.end(), in.moment->id) != done_.end()) {
        return out;
    }
    done_.push_back(in.moment->id);
    if (here == in.others) {
        return out;   // everyone is here already: no wait
    }
    waiting_ = Waiting{*in.moment, in.now_us, in.now_us, here, in.others};
    out.notices.push_back(WaitLine(here + 1, in.others + 1));
    out.chat = "Waiting for the party at " + in.moment->where;
    return out;
}

}  // namespace CoopPartyWait
