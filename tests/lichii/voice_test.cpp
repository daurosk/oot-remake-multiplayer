#include <doctest/doctest.h>

#include <map>
#include <string>
#include <vector>

#include "coop_voice.h"

using namespace CoopVoice;

namespace {

Settings From(const std::map<std::string, std::string>& vars, std::vector<std::string>& errors) {
    return SettingsFrom(
        [&](const char* name) {
            const auto it = vars.find(name);
            return it == vars.end() ? std::string() : it->second;
        },
        errors);
}

} // namespace

TEST_CASE("voice settings: defaults, then LICHII_COOP_VOICE*") {
    std::vector<std::string> errors;
    const Settings d = From({}, errors);
    CHECK(errors.empty());
    CHECK(d.mode == coop::voice::Mode::push_to_talk);
    CHECK(d.key == 0x56u);
    CHECK(d.range_m == doctest::Approx(30.0f));
    const Settings s = From({{"LICHII_COOP_VOICE", "Open"},
                             {"LICHII_COOP_VOICE_KEY", "t"},
                             {"LICHII_COOP_VOICE_VOLUME", "150"},
                             {"LICHII_COOP_VOICE_RANGE", "20"}},
                            errors);
    CHECK(errors.empty());
    CHECK(s.mode == coop::voice::Mode::open_mic);
    CHECK(s.key == static_cast<std::uint32_t>('T'));
    CHECK(s.volume == doctest::Approx(1.5f));
    CHECK(s.range_m == doctest::Approx(20.0f));
    CHECK(From({{"LICHII_COOP_VOICE", "off"}}, errors).mode == coop::voice::Mode::off);
    CHECK(From({{"LICHII_COOP_VOICE_KEY", "0x14"}}, errors).key == 0x14u);  // Caps Lock
    CHECK(errors.empty());
}

TEST_CASE("voice settings: what cannot be read stays at the default and is named") {
    std::vector<std::string> errors;
    const Settings s = From({{"LICHII_COOP_VOICE", "loud"},
                             {"LICHII_COOP_VOICE_KEY", "F13x"},
                             {"LICHII_COOP_VOICE_VOLUME", "300"},
                             {"LICHII_COOP_VOICE_RANGE", "2"}},
                            errors);
    CHECK(errors.size() == 4);
    CHECK(s == Settings{});
}

TEST_CASE("voice settings: the engine's, and where a voice comes from") {
    Settings s;
    s.mode = coop::voice::Mode::open_mic;
    s.range_m = 12.0f;
    s.muted = true;
    const auto e = EngineSettings(s, false);
    CHECK(e.gate.mode == coop::voice::Mode::open_mic);
    CHECK(e.muted);
    CHECK(e.spatial.range_m == doctest::Approx(12.0f));
    CHECK(e.spatial.fade_m == doctest::Approx(4.0f));
    CHECK_FALSE(e.spatial.pan);  // no camera: no panning
    CHECK(EngineSettings(s, true).spatial.pan);
    const auto mouth = MouthOf({1, 2, 3}, 1.6f);
    CHECK(mouth[1] == doctest::Approx(2.0f + 1.44f));
    CHECK(MouthOf({1, 2, 3}, 0.0f)[1] == doctest::Approx(3.2f));
}

TEST_CASE("voice microphone: push to talk opens it only while the key is held or the spurt ends") {
    Settings ptt;  // the default: push to talk
    CHECK_FALSE(MicWanted(ptt, true, false, false));  // connected, never pressed: no microphone
    CHECK(MicWanted(ptt, true, true, false));          // key held
    CHECK(MicWanted(ptt, true, false, true));          // let go: the tail and the end frame
    CHECK_FALSE(MicWanted(ptt, false, true, false));   // not connected or not in the game
    Settings muted = ptt;
    muted.muted = true;
    CHECK_FALSE(MicWanted(muted, true, true, true));
    Settings off;
    off.mode = coop::voice::Mode::off;
    CHECK_FALSE(MicWanted(off, true, true, true));
    Settings open;
    open.mode = coop::voice::Mode::open_mic;
    CHECK(MicWanted(open, true, false, false));
    CHECK_FALSE(MicWanted(open, false, false, false));
    open.muted = true;
    CHECK_FALSE(MicWanted(open, true, false, false));
}
