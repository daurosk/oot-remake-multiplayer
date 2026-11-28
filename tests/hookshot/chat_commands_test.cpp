// The Multiplayer tab's Commands page (hookshot/src/coop_pause_menu.cpp Commands) against the chat commands the
// game really handles (lichii/hook/coop_hook.cpp SendChat: each module's OnChat, its Parse): every handled command
// is on the page, and every form on the page is one the game takes (row 226: /dbno and /difficulty were missing).

#include <doctest/doctest.h>

#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "coop_dbno.h"
#include "coop_difficulty.h"
#include "coop_emote.h"
#include "coop_lobby.h"
#include "coop_party_wait.h"
#include "coop_pause_menu.h"
#include "coop_pvp.h"
#include "coop_teleport.h"

namespace {

std::string ReadSource(const std::string& relative) {
    std::ifstream f(std::string(COOP_TEST_SOURCE_DIR) + "/../" + relative, std::ios::binary);
    std::ostringstream text;
    text << f.rdbuf();
    return text.str();
}

// The text without its // comments (the "/pvpx: not ours" examples live there).
std::string Code(const std::string& text) {
    std::string out;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        const auto comment = line.find("//");
        out += line.substr(0, comment) + "\n";
    }
    return out;
}

// One form as the page writes it ("/teleport <player>"), as a player types it.
std::string Typed(std::string form) {
    for (const std::string placeholder : {"<player>"}) {
        for (auto at = form.find(placeholder); at != std::string::npos; at = form.find(placeholder)) {
            form.replace(at, placeholder.size(), "Saria");
        }
    }
    return form;
}

// Does the game take it: one of the modules SendChat asks (a parser's "bad" is a form it does not take).
bool Taken(const std::string& text) {
    if (CoopTeleport::Parse(text) || CoopPvp::Parse(text)) {
        return true;
    }
    if (const auto c = CoopEmote::Parse(text)) {
        return c->kind != CoopEmote::Command::Kind::unknown;
    }
    if (const auto c = CoopDbno::Parse(text)) {
        return c->kind != CoopDbno::Command::Kind::bad;
    }
    if (const auto c = CoopLobby::Parse(text)) {
        return c->kind != CoopLobby::Command::Kind::bad;
    }
    if (const auto c = CoopDifficulty::ParseCommand(text)) {
        return c->kind != CoopDifficulty::Command::Kind::bad;
    }
    if (const auto c = CoopPartyWait::Parse(text)) {
        return *c != CoopPartyWait::Command::bad;
    }
    return false;
}

std::string PageText() {
    std::string all;
    for (const auto& c : CoopPauseMenu::Commands()) {
        all += std::string(c.typed) + "\n";
    }
    return all;
}

}  // namespace

TEST_CASE("chat commands: every form on the Commands page is one the game takes") {
    for (const auto& c : CoopPauseMenu::Commands()) {
        std::string typed = c.typed;
        for (std::size_t start = 0; start <= typed.size();) {   // ("/pvp on, /pvp off": each form)
            const auto comma = typed.find(", ", start);
            const std::string form = typed.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            INFO("form: " << form);
            CHECK(Taken(Typed(form)));
            if (comma == std::string::npos) break;
            start = comma + 2;
        }
    }
}

TEST_CASE("chat commands: every command the game handles is on the Commands page") {
    // The modules SendChat hands a line to before it is chat. A new one: put its commands on the page, and here.
    const std::map<std::string, std::string> files = {
        {"Teleport", "lichii/hook/coop_teleport.cpp"}, {"Pvp", "lichii/hook/coop_pvp.cpp"},
        {"Dbno", "lichii/hook/coop_dbno.cpp"},         {"Difficulty", "lichii/hook/coop_difficulty.cpp"},
        {"Lobby", "lichii/hook/coop_lobby.cpp"},       {"Emote", "lichii/hook/coop_emote.cpp"},
        {"PartyWait", "lichii/hook/coop_party_wait.cpp"},
    };
    const std::string hook = Code(ReadSource("lichii/hook/coop_hook.cpp"));
    const auto begin = hook.find("bool SendChat(const std::string& text) {");
    REQUIRE(begin != std::string::npos);
    const auto end = hook.find("send_chat(text)", begin);
    REQUIRE(end != std::string::npos);
    const std::string send_chat = hook.substr(begin, end - begin);
    std::set<std::string> asked;
    const std::regex on_chat(R"(Coop(\w+)::OnChat\()");
    for (std::sregex_iterator it(send_chat.begin(), send_chat.end(), on_chat), stop; it != stop; ++it) {
        asked.insert((*it)[1].str());
    }
    std::set<std::string> known;
    for (const auto& [name, file] : files) known.insert(name);
    CHECK(asked == known);

    // Each module's own words: "/pvp" literals, and the words it compares (word == "kick"). In a module with a
    // "/word" of its own, the compared words are its sub-commands (/dbno time).
    const std::string page = PageText();
    const std::regex slash(R"re("/([a-z]+)")re");
    const std::regex compared(R"re(word [!=]= "([a-z]+)")re");
    for (const auto& [name, file] : files) {
        const std::string code = Code(ReadSource(file));
        REQUIRE_MESSAGE(!code.empty(), file);
        std::vector<std::string> tops;
        for (std::sregex_iterator it(code.begin(), code.end(), slash), stop; it != stop; ++it) {
            tops.push_back("/" + (*it)[1].str());
        }
        for (const auto& top : tops) {
            INFO(file << ": " << top);
            CHECK(page.find(top) != std::string::npos);
        }
        for (std::sregex_iterator it(code.begin(), code.end(), compared), stop; it != stop; ++it) {
            const std::string word = (*it)[1].str();
            const std::string shown = tops.empty() ? "/" + word : tops.front() + " " + word;
            INFO(file << ": " << shown);
            CHECK(page.find(shown) != std::string::npos);
        }
    }
}

TEST_CASE("chat commands: /difficulty") {
    using CoopDifficulty::Command;
    using CoopDifficulty::ParseCommand;
    using CoopDifficulty::Preset;
    CHECK_FALSE(ParseCommand("hello"));
    CHECK_FALSE(ParseCommand("/difficultyx"));   // (chat, not ours)
    REQUIRE(ParseCommand("/difficulty"));
    CHECK(ParseCommand("  /Difficulty ")->kind == Command::Kind::show);
    CHECK(ParseCommand("/difficulty HARD")->preset == Preset::hard);
    CHECK(ParseCommand("/difficulty easy")->kind == Command::Kind::change);
    CHECK(ParseCommand("/difficulty normal")->preset == Preset::normal);
    CHECK(ParseCommand("/difficulty auto")->auto_scale);
    CHECK(ParseCommand("/difficulty automatic")->auto_scale);
    const auto custom = ParseCommand("/difficulty custom 150 125% 200");
    REQUIRE(custom);
    CHECK(custom->kind == Command::Kind::change);
    CHECK(custom->preset == Preset::custom);
    CHECK(custom->custom.hp_pct == 150);
    CHECK(custom->custom.damage_pct == 125);
    CHECK(custom->custom.count_pct == 200);
    CHECK(custom->custom.boss_hp_pct == 150);   // (bosses as the other enemies)
    // Outside the sliders, off their marks, too few or a word: not taken, the help line instead.
    for (const char* bad : {"/difficulty custom 150 125", "/difficulty custom 510 100 100", "/difficulty custom 20 100 100",
                            "/difficulty custom 100 410 100", "/difficulty custom 100 100 75",
                            "/difficulty custom 100 100 325", "/difficulty custom 130 100 100", "/difficulty custom",
                            "/difficulty hardest", "/difficulty hard now", "/difficulty custom a b c"}) {
        INFO(bad);
        CHECK(ParseCommand(bad)->kind == Command::Kind::bad);
    }
    // The ends are taken.
    CHECK(ParseCommand("/difficulty custom 25 25 100")->kind == Command::Kind::change);
    CHECK(ParseCommand("/difficulty custom 500 400 300")->kind == Command::Kind::change);

    // Applied: on the host's choice now.
    CoopDifficulty::Choice now;
    now.preset = Preset::custom;
    now.custom = {200, 150, 150, 200, 150};
    const auto on = CoopDifficulty::Applied(now, *ParseCommand("/difficulty auto"));
    CHECK(on.auto_scale);
    CHECK(on.preset == Preset::custom);   // (kept for when it goes off)
    const auto hard = CoopDifficulty::Applied(on, *ParseCommand("/difficulty hard"));
    CHECK_FALSE(hard.auto_scale);
    CHECK(hard.preset == Preset::hard);
    const auto mine = CoopDifficulty::Applied(hard, *custom);
    CHECK(mine.preset == Preset::custom);
    CHECK(mine.custom.hp_pct == 150);
    CHECK(mine.custom.count_pct == 200);
}
