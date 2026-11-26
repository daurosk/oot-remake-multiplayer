// SPDX-License-Identifier: GPL-3.0-or-later
// The Discord status's activity (hookshot/src/hos_discord_activity.h, patch 0026's host/hos_discord_activity.h):
// with no setting the frame is Longshot's own byte for byte; settings replace one word each; discord_image=none
// sends no art; bad text never makes bad JSON.
#include <doctest/doctest.h>

#include <cstdio>
#include <string>

#include "hos_discord_activity.h"

namespace {

// Longshot v3.0.0-rc.4's host/discord_rpc.c send_activity, before patch 0026: its format string, verbatim.
std::string Upstream(unsigned nonce, unsigned long pid, const char* version, long long started) {
    char json[640];
    std::snprintf(json, sizeof json,
                  "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"%u\",\"args\":{\"pid\":%lu,\"activity\":{\"details\":\"Longshot\","
                  "\"state\":\"Longshot %s\",\"timestamps\":{\"start\":%lld},\"assets\":{\"large_image\":\"%s\",\"large_text\":"
                  "\"Longshot %s\"}}}}",
                  nonce, pid, version, started, "longshot", version);
    return json;
}

std::string Frame(const HosDiscordWords* w, const char* module_line, const char* version = "v3.0.0-rc.4") {
    char activity[1536];
    char json[2048];
    hos_discord_activity(activity, sizeof activity, "Longshot", version, "longshot", w, module_line, 1763300000LL);
    hos_discord_set_activity(json, sizeof json, 7, 4242, activity);
    return json;
}

HosDiscordWords Words(const char* image, const char* details, const char* state, const char* large) {
    HosDiscordWords w{};
    std::snprintf(w.image, sizeof w.image, "%s", image);
    std::snprintf(w.details, sizeof w.details, "%s", details);
    std::snprintf(w.state, sizeof w.state, "%s", state);
    std::snprintf(w.large_text, sizeof w.large_text, "%s", large);
    return w;
}

}  // namespace

TEST_CASE("Discord status: no settings and no module line = Longshot's frame byte for byte") {
    const HosDiscordWords none = Words("", "", "", "");
    for (const char* v : {"v3.0.0-rc.4", "v3.0.0", "dev"}) {
        CHECK(Frame(&none, "", v) == Upstream(7, 4242, v, 1763300000LL));
        CHECK(Frame(nullptr, nullptr, v) == Upstream(7, 4242, v, 1763300000LL));
    }
}

TEST_CASE("Discord status: each setting replaces its own word only") {
    const HosDiscordWords w = Words("", "Squadala! 0.11.4", "", "");
    const std::string f = Frame(&w, "");
    CHECK(f.find("\"details\":\"Squadala! 0.11.4\"") != std::string::npos);
    CHECK(f.find("\"state\":\"Longshot v3.0.0-rc.4\"") != std::string::npos);
    CHECK(f.find("\"large_image\":\"longshot\"") != std::string::npos);   // (any app id: upstream's art)

    const HosDiscordWords set = Words("my_art", "First", "Second", "Hover");
    const std::string g = Frame(&set, "");
    CHECK(g.find("\"details\":\"First\",\"state\":\"Second\"") != std::string::npos);
    CHECK(g.find("\"large_image\":\"my_art\",\"large_text\":\"Hover\"") != std::string::npos);
}

TEST_CASE("Discord status: the module's line comes before discord_state=") {
    const HosDiscordWords w = Words("", "Squadala! 0.11.4", "Solo", "");
    CHECK(Frame(&w, "").find("\"state\":\"Solo\"") != std::string::npos);
    CHECK(Frame(&w, "Co-op: 3 players").find("\"state\":\"Co-op: 3 players\"") != std::string::npos);
}

TEST_CASE("Discord status: discord_image=none sends no art (Discord shows the application's icon)") {
    const HosDiscordWords w = Words("none", "Squadala! 0.11.4", "Solo", "");
    const std::string f = Frame(&w, "");
    CHECK(f.find("assets") == std::string::npos);
    CHECK(f.find("large_image") == std::string::npos);
    CHECK(f == "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"7\",\"args\":{\"pid\":4242,\"activity\":{\"details\":\"Squadala! "
               "0.11.4\",\"state\":\"Solo\",\"timestamps\":{\"start\":1763300000}}}}");
    CHECK(hos_discord_valid_image("none"));
}

TEST_CASE("Discord status: all or nothing, the longest words fit discord_rpc.c's 1536 and 2048 bytes") {
    char small[40];
    CHECK(hos_discord_activity(small, sizeof small, "Longshot", "v3.0.0-rc.4", "longshot", nullptr, "", 1) == 0);
    CHECK(std::string(small).empty());
    CHECK(hos_discord_set_activity(small, sizeof small, 1, 2, "{\"details\":\"a long enough activity\"}") == 0);
    CHECK(std::string(small).empty());
    // The worst case: every word 127 quotes (254 escaped), a 255-byte image, a 255-byte module line cut by the reader.
    HosDiscordWords w{};
    const std::string quotes(127, '"');
    std::snprintf(w.details, sizeof w.details, "%s", quotes.c_str());
    std::snprintf(w.state, sizeof w.state, "%s", quotes.c_str());
    std::snprintf(w.large_text, sizeof w.large_text, "%s", quotes.c_str());
    std::snprintf(w.image, sizeof w.image, "%s", std::string(255, 'a').c_str());
    char activity[1536];
    char json[2048];
    REQUIRE(hos_discord_activity(activity, sizeof activity, "Longshot", "v3.0.0-rc.4", "longshot", &w, quotes.c_str(),
                                 -9223372036854775807LL - 1) == 1);
    CHECK(hos_discord_set_activity(json, sizeof json, 4294967295u, static_cast<unsigned long>(-1), activity) == 1);
}

TEST_CASE("Discord status: the checks Discord needs") {
    CHECK(hos_discord_valid_text("Solo"));
    CHECK(hos_discord_valid_text("Ocarina \xc3\xa9t\xc3\xa9"));          // UTF-8
    CHECK_FALSE(hos_discord_valid_text("A"));                            // too short
    CHECK_FALSE(hos_discord_valid_text(std::string(128, 'x').c_str()));  // too long
    CHECK(hos_discord_valid_text(std::string(127, 'x').c_str()));
    CHECK_FALSE(hos_discord_valid_text("tab\there"));
    CHECK_FALSE(hos_discord_valid_text("caf\xe9"));                      // Latin-1, not UTF-8
    CHECK_FALSE(hos_discord_valid_text("ab\xc3"));                       // cut sequence
    CHECK_FALSE(hos_discord_valid_text("ab\xc0\xaf"));                   // overlong
    CHECK_FALSE(hos_discord_valid_text("ab\xed\xa0\x80"));               // surrogate
    CHECK(hos_discord_valid_image("squadala_logo"));
    CHECK(hos_discord_valid_image("https://example.org/a.png"));
    CHECK_FALSE(hos_discord_valid_image("bad\"key"));
    CHECK_FALSE(hos_discord_valid_image(""));
}

TEST_CASE("Discord status: JSON text is escaped, and bytes that are no UTF-8 are left out") {
    char out[64];
    hos_discord_json_text(out, sizeof out, "say \"hi\" \\ ok");
    CHECK(std::string(out) == "say \\\"hi\\\" \\\\ ok");
    hos_discord_json_text(out, sizeof out, "caf\xe9 \xc3\xa9\x01!");
    CHECK(std::string(out) == "caf \xc3\xa9!");
    // 2-, 3- and 4-byte characters pass as they are, with no escape; next to quotes and backslashes too.
    hos_discord_json_text(out, sizeof out, "caf\xc3\xa9");
    CHECK(std::string(out) == "caf\xc3\xa9");
    hos_discord_json_text(out, sizeof out, "\xe3\x83\x8a\xe3\x83\x93");   // (two 3-byte characters)
    CHECK(std::string(out) == "\xe3\x83\x8a\xe3\x83\x93");
    hos_discord_json_text(out, sizeof out, "\xf0\x9f\xa7\x9a!");        // (a 4-byte character)
    CHECK(std::string(out) == "\xf0\x9f\xa7\x9a!");
    hos_discord_json_text(out, sizeof out, "\"\xc3\xa9\\\xe3\x83\x8a\"");
    CHECK(std::string(out) == "\\\"\xc3\xa9\\\\\xe3\x83\x8a\\\"");
    // And the whole activity with them is the plain text between the quotes.
    HosDiscordWords w{};
    std::snprintf(w.details, sizeof w.details, "%s", "Caf\xc3\xa9 \"Squadala!\"");
    char activity[512];
    hos_discord_activity(activity, sizeof activity, "Longshot", "v3", "none", &w, "", 1);
    CHECK(std::string(activity) == "{\"details\":\"Caf\xc3\xa9 \\\"Squadala!\\\"\",\"state\":\"Longshot v3\",\"timestamps\":{\"start\":1}}");
    // Cut before a character that does not fit: never half an escape or half a sequence.
    char small[5];
    hos_discord_json_text(small, sizeof small, "ab\"c");
    CHECK(std::string(small) == "ab\\\"");
    hos_discord_json_text(small, sizeof small, "abc\xc3\xa9");
    CHECK(std::string(small) == "abc");
}
