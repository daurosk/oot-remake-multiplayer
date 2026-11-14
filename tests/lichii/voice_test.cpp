#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
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
    CHECK(d.mode == coop::voice::Mode::off);  // voice chat is off until the player turns it on
    CHECK(d.test == Test::none);
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
    CHECK(From({{"LICHII_COOP_VOICE", "ptt"}}, errors).mode == coop::voice::Mode::push_to_talk);
    // an older file's on, 1, yes: push to talk as they meant, never a live open mic unasked
    CHECK(From({{"LICHII_COOP_VOICE", "on"}}, errors).mode == coop::voice::Mode::push_to_talk);
    CHECK(From({{"LICHII_COOP_VOICE", "1"}}, errors).mode == coop::voice::Mode::push_to_talk);
    CHECK(From({{"LICHII_COOP_VOICE", "yes"}}, errors).mode == coop::voice::Mode::push_to_talk);
    CHECK(From({{"LICHII_COOP_VOICE", "open"}}, errors).mode == coop::voice::Mode::open_mic);
    CHECK(From({{"LICHII_COOP_VOICE", "PTT"}}, errors).mode == coop::voice::Mode::push_to_talk);
    CHECK(From({{"LICHII_COOP_VOICE_TEST", "tone"}}, errors).test == Test::tone);
    CHECK(From({{"LICHII_COOP_VOICE_TEST", "log"}}, errors).test == Test::log);
    CHECK(From({{"LICHII_COOP_VOICE_TEST", "0"}}, errors).test == Test::none);
    CHECK(From({{"LICHII_COOP_VOICE_KEY", "0x14"}}, errors).key == 0x14u);  // Caps Lock
    CHECK(errors.empty());
}

TEST_CASE("voice settings: what cannot be read stays at the default and is named") {
    std::vector<std::string> errors;
    const Settings s = From({{"LICHII_COOP_VOICE", "loud"},
                             {"LICHII_COOP_VOICE_KEY", "F13x"},
                             {"LICHII_COOP_VOICE_VOLUME", "300"},
                             {"LICHII_COOP_VOICE_RANGE", "2"},
                             {"LICHII_COOP_VOICE_TEST", "loud"}},
                            errors);
    CHECK(errors.size() == 5);
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

TEST_CASE("voice off: no microphone and nobody heard, and the settings row turns it on and off") {
    const Settings off;  // the default
    CHECK_FALSE(MicWanted(off, true, true, true));
    CHECK(EngineSettings(off, true).deafened);
    CHECK(EngineSettings(off, true).gate.mode == coop::voice::Mode::off);
    const Settings on = WithEnabled(off, true);
    CHECK(on.mode == coop::voice::Mode::open_mic);   // turned on: open mic
    CHECK_FALSE(EngineSettings(on, true).deafened);
    CHECK(WithEnabled(on, false) == off);
    Settings ptt = on;
    ptt.mode = coop::voice::Mode::push_to_talk;
    ptt.volume = 0.5f;
    CHECK(WithEnabled(ptt, true) == ptt);  // on already: the chosen way of talking stays
    CHECK(WithEnabled(ptt, false).volume == doctest::Approx(0.5f));
    CHECK(MicWanted(on, true, false, false));        // open mic: the microphone is open
    CHECK_FALSE(MicWanted(ptt, true, false, false)); // push to talk: only while held
    CHECK(MicWanted(ptt, true, true, false));
    // the host muted our voice: the microphone closes, in either mode
    CHECK_FALSE(MicWanted(on, true, false, false, true));
    CHECK_FALSE(MicWanted(ptt, true, true, true, true));
    // What a settings screen writes is what the start reads.
    std::vector<std::string> errors;
    for (const auto mode : {coop::voice::Mode::off, coop::voice::Mode::push_to_talk, coop::voice::Mode::open_mic}) {
        CHECK(From({{"LICHII_COOP_VOICE", ModeName(mode)}}, errors).mode == mode);
    }
    CHECK(errors.empty());
}

TEST_CASE("voice test tone: a 440 Hz tone at the clock's pace, in the microphone's place") {
    ToneSource tone;
    std::vector<std::int16_t> pcm(48000);
    tone.start(1'000'000);
    CHECK(tone.read(pcm.data(), 4800, 1'000'000) == 0);       // no time passed
    CHECK(tone.read(pcm.data(), 4800, 1'020'000) == 960);     // 20 ms
    CHECK(tone.read(pcm.data() + 960, 4800, 1'020'000) == 0);
    CHECK(tone.read(pcm.data() + 960, 480, 1'040'000) == 480);  // no more than asked
    CHECK(tone.read(pcm.data() + 1440, 4800, 1'040'000) == 480);  // the rest, nothing lost
    // One wave: loud enough for any gate, never clipped, 440 Hz with no jump between reads.
    int peak = 0;
    int crossings = 0;
    for (std::size_t i = 1; i < 1920; ++i) {
        peak = std::max(peak, std::abs(static_cast<int>(pcm[i])));
        crossings += pcm[i - 1] < 0 && pcm[i] >= 0 ? 1 : 0;
        CHECK(std::abs(pcm[i] - pcm[i - 1]) < 500);
    }
    CHECK(peak > 8000);
    CHECK(peak <= 8192);
    CHECK(crossings == 17);  // 40 ms of 440 Hz: 17.6 waves
    // The game stood still for 3 s: only the last 200 ms come, then the clock's pace again.
    CHECK(tone.read(pcm.data(), 48000, 4'040'000) == 9600);
    CHECK(tone.read(pcm.data(), 48000, 4'040'000) == 0);
    CHECK(tone.read(pcm.data(), 48000, 4'050'000) == 480);
    CHECK(tone.read(nullptr, 48000, 5'000'000) == 0);
    tone.start(9'000'000);
    CHECK(tone.read(pcm.data(), 48000, 8'000'000) == 0);  // a clock before the start
    CHECK(ToneSource::KeyHeld(0));
    CHECK(ToneSource::KeyHeld(1'999'999));
    CHECK_FALSE(ToneSource::KeyHeld(2'000'000));
    CHECK(ToneSource::KeyHeld(4'000'000));
}

TEST_CASE("voice test tone: the backend's own microphone and key are never asked") {
    int mic_calls = 0;
    Backend b;
    b.mic_start = [&] { return ++mic_calls > 0; };
    b.mic_read = [&](std::int16_t*, int) { return ++mic_calls; };
    b.mic_stop = [&] { ++mic_calls; };
    b.key_down = [&](std::uint32_t) { return ++mic_calls > 0; };
    std::uint64_t now = 0;
    UseTestTone(b, [&] { return now; });
    CHECK(b.key_down(0x56));
    CHECK(b.mic_start());
    now = 100'000;
    std::vector<std::int16_t> pcm(4800);
    CHECK(b.mic_read(pcm.data(), 4800) == 4800);
    b.mic_stop();
    now = 2'500'000;
    CHECK_FALSE(b.key_down(0x56));
    CHECK(b.mic_start());  // a new spurt starts from now
    CHECK(b.mic_read(pcm.data(), 4800) == 0);
    CHECK(mic_calls == 0);
}

TEST_CASE("voice test log: who is heard, and why not") {
    CHECK(HeardLine(1, true, 2.44f, 30.0f) == "P1 heard at 2.4 m");
    CHECK(HeardLine(16, false, 24.5f, 10.0f) == "P16 not heard (out of range: 24.5 m, range 10 m)");
    CHECK(HeardLine(2, false, 2.44f, 30.0f) == "P2 not heard (quiet, 2.4 m)");
    CHECK(HeardLine(2, false, std::nullopt, 30.0f) == "P2 not heard (not shown here)");
}

TEST_CASE("voice microphone: push to talk opens it only while the key is held or the spurt ends") {
    Settings ptt;
    ptt.mode = coop::voice::Mode::push_to_talk;
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

TEST_CASE("voice: the tip when voice chat is turned on names the mode and where to change it") {
    Settings s;
    CHECK(TurnedOnTip(s).empty());   // off: no tip
    s.mode = coop::voice::Mode::open_mic;
    CHECK(TurnedOnTip(s) == "Voice chat on: open mic (change to push-to-talk in Settings)");
    s.mode = coop::voice::Mode::push_to_talk;
    CHECK(TurnedOnTip(s) == "Voice chat on: hold V to talk (change to open mic in Settings)");
    s.key = 0x14;   // Caps Lock: no letter to name
    CHECK(TurnedOnTip(s).find("hold the push-to-talk key") != std::string::npos);
    CHECK(KeyName('7') == "7");
}

TEST_CASE("voice: the HUD icon: none while voice is off, muted wins, lit while talking, ours dim when quiet") {
    for (const bool self : {false, true}) {
        CHECK(IconFor(false, true, true, self) == Icon::none);   // voice off here: nothing for anyone
        CHECK(IconFor(true, true, true, self) == Icon::muted);
        CHECK(IconFor(true, true, false, self) == Icon::muted);
        CHECK(IconFor(true, false, true, self) == Icon::talking);
    }
    CHECK(IconFor(true, false, false, true) == Icon::idle);    // ours: on, quiet
    CHECK(IconFor(true, false, false, false) == Icon::none);   // theirs: not known whether their voice is on
}
