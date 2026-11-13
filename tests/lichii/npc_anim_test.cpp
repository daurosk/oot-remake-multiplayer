// Tests for the walking townspeople's frame arithmetic and alias book
// (lichii/hook/coop_npc_anim.h).
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_set>

#include "coop_npc_anim.h"

using namespace CoopNpcAnim;

TEST_CASE("npc-anim: SeenRate measures the owner's speed between two samples") {
    // 100 ms is 3 game frames; 6 frames played in it: 2 frames a game frame.
    CHECK(SeenRate(10.0f, 1'000'000, 16.0f, 1'100'000, 1.0f) == doctest::Approx(2.0f));
    // Standing on its last frame: 0.
    CHECK(SeenRate(30.0f, 1'000'000, 30.0f, 1'100'000, 1.0f) == doctest::Approx(0.0f));
    // Capped.
    CHECK(SeenRate(0.0f, 1'000'000, 100.0f, 1'100'000, 1.0f) == doctest::Approx(4.0f));
}

TEST_CASE("npc-anim: SeenRate falls back to the nominal rate when the samples cannot tell") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(SeenRate(0.0f, 1'000'000, 1.0f, 1'010'000, 0.5f) == 0.5f);   // too close
    CHECK(SeenRate(0.0f, 1'000'000, 1.0f, 1'600'000, 0.5f) == 0.5f);   // too far apart
    CHECK(SeenRate(20.0f, 1'000'000, 2.0f, 1'100'000, 0.5f) == 0.5f);  // loop began again
    CHECK(SeenRate(0.0f, 1'100'000, 1.0f, 1'000'000, 0.5f) == 0.5f);   // time went back
    CHECK(SeenRate(nan, 1'000'000, 1.0f, 1'100'000, 0.5f) == 0.5f);
}

TEST_CASE("npc-anim: Loop learns the loop length when the frame drops back") {
    Loop loop;
    loop.See("Walk", 0.0f);
    CHECK(loop.Length() == 0.0f);
    loop.See("Walk", 10.0f);
    loop.See("Walk", 29.0f);
    CHECK(loop.Length() == 0.0f);  // no end seen yet
    loop.See("Walk", 1.0f);
    CHECK(loop.Length() == doctest::Approx(29.0f));
    loop.See("Walk", 0.5f);  // a small drop is noise, not a loop
    CHECK(loop.Length() == doctest::Approx(29.0f));
    loop.See("Run", 4.0f);  // another animation: learned again
    CHECK(loop.Length() == 0.0f);
    loop.See("Run", -1.0f);  // ignored
    loop.See("Run", std::numeric_limits<float>::infinity());
    CHECK(loop.Length() == 0.0f);
}

TEST_CASE("npc-anim: Wrapped and FrameGap work the short way round the loop") {
    CHECK(Wrapped(5.0f, 0.0f) == 5.0f);
    CHECK(Wrapped(35.0f, 30.0f) == doctest::Approx(5.0f));
    CHECK(Wrapped(20.0f, 30.0f) == 20.0f);
    CHECK(Wrapped(-3.0f, 30.0f) == 0.0f);
    CHECK(Wrapped(std::numeric_limits<float>::quiet_NaN(), 30.0f) == 0.0f);

    CHECK(FrameGap(29.0f, 1.0f, 30.0f) == doctest::Approx(2.0f));
    CHECK(FrameGap(29.0f, 1.0f, 0.0f) == doctest::Approx(28.0f));
    CHECK(FrameGap(10.0f, 14.0f, 30.0f) == doctest::Approx(4.0f));
    CHECK(FrameGap(0.0f, 40.0f, 30.0f) == doctest::Approx(40.0f));  // past the loop: as is
}

TEST_CASE("npc-anim: Aliases keep the owner's play while the slot shows the AI's name") {
    Aliases book;
    constexpr std::uint64_t kActor = 0x1000;
    constexpr std::uint64_t kAiName = 0x5000;
    // Nothing played yet: only the real name counts.
    CHECK_FALSE(book.Shows(kActor, "Run", kAiName, "Wait"));
    CHECK(book.Find(kActor) == nullptr);
    book.Played(kActor, "Run", kAiName, "Wait");
    CHECK(book.Size() == 1);
    // The debug line names the play hidden under the AI's name.
    REQUIRE(book.Find(kActor) != nullptr);
    CHECK(book.Find(kActor)->played == "Run");
    // The slot names the AI's "Wait" at the same pointer: our "Run" plays.
    CHECK(book.Shows(kActor, "Run", kAiName, "Wait"));
    // The owner went on to another animation: play it.
    CHECK_FALSE(book.Shows(kActor, "Walk", kAiName, "Wait"));
    // The AI asked for another one (its play went through): play ours again.
    CHECK_FALSE(book.Shows(kActor, "Run", 0x6000, "Talk"));
    CHECK_FALSE(book.Shows(kActor, "Run", kAiName, "Talk"));
    // The slot really shows the owner's: nothing hidden, the entry goes.
    CHECK(book.Shows(kActor, "Run", 0x7000, "Run"));
    CHECK(book.Size() == 0);
}

TEST_CASE("npc-anim: Aliases give back the entries no longer wished") {
    Aliases book;
    book.Played(1, "Run", 0x10, "Wait");
    book.Played(2, "Walk", 0x20, "Wait");
    book.Played(3, "Run", 0x30, "Talk");
    const auto dropped = book.Drop(2);
    REQUIRE(dropped.has_value());
    CHECK(dropped->played == "Walk");
    CHECK_FALSE(book.Drop(2).has_value());

    const auto gone = book.DropAllBut(std::unordered_set<std::uint64_t>{1});
    REQUIRE(gone.size() == 1);
    CHECK(gone[0].first == 3);
    CHECK(gone[0].second.name == "Talk");
    CHECK(gone[0].second.name_at == 0x30);
    CHECK(book.Size() == 1);
}
