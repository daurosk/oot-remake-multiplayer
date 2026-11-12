// SPDX-License-Identifier: GPL-3.0-or-later
// The chat box on Hookshot (hookshot/src/coop_chatbox.h): keys and
// characters as Hookshot's window hands them over.
#include <doctest/doctest.h>

#include <string>

#include "coop_chatbox.h"

using CoopChatBox::Box;
using CoopChatBox::kKeyBack;
using CoopChatBox::kKeyEscape;
using CoopChatBox::kKeyOpen;
using CoopChatBox::kKeyReturn;

namespace {

void Type(Box& b, const std::string& s) {
    for (const char c : s) {
        CHECK(b.Key(static_cast<unsigned char>(c), false));  // the key press (taken while open)
        CHECK(b.Char(static_cast<unsigned char>(c)));        // then its character
    }
}

}  // namespace

TEST_CASE("chat box: T opens it, its own t is not typed, other keys pass while closed") {
    Box b;
    CHECK_FALSE(b.Key('W', false));  // the game's key: not taken
    CHECK_FALSE(b.Char('w'));
    CHECK_FALSE(b.KeysTaken(false));
    CHECK_FALSE(b.Key(kKeyOpen, true));  // an auto-repeat does not open it
    CHECK(b.Key(kKeyOpen, false));
    CHECK(b.open());
    CHECK(b.KeysTaken(false));  // the game reads no keys now
    CHECK(b.Char('t'));         // the T's own character
    CHECK(b.text().empty());
    Type(b, "hi there");
    CHECK(b.text() == "hi there");
}

TEST_CASE("chat box: Enter sends the trimmed text once, a refused send keeps it open") {
    Box b;
    b.Key(kKeyOpen, false);
    b.Char('t');
    Type(b, "  hello  ");
    CHECK(b.Key(kKeyReturn, false));
    const auto sent = b.TakeSend();
    REQUIRE(sent);
    CHECK(*sent == "hello");
    CHECK_FALSE(b.TakeSend());
    b.Sent(false);  // not connected
    CHECK(b.open());
    CHECK(b.failed());
    CHECK(b.Char('!'));  // the text changes: no longer marked
    CHECK_FALSE(b.failed());
    b.Key(kKeyReturn, false);
    REQUIRE(b.TakeSend());
    b.Sent(true);
    CHECK_FALSE(b.open());
    CHECK(b.text().empty());
    // Enter is still held as the box closes: the game must not see it (Plus).
    CHECK(b.KeysTaken(true));
    CHECK_FALSE(b.KeysTaken(false));
    CHECK_FALSE(b.KeysTaken(true));  // a later Enter is the game's again
    CHECK_FALSE(b.Char('\r'));        // its character after the box closed: Hookshot's
}

TEST_CASE("chat box: Enter on nothing and Esc close it, Backspace removes whole characters") {
    Box b;
    b.Key(kKeyOpen, false);
    b.Key(kKeyReturn, false);
    CHECK_FALSE(b.open());
    CHECK_FALSE(b.TakeSend());
    b.Key(kKeyOpen, false);
    b.Char('T');
    b.Char('a');
    b.Char(0xE9);   // e acute: 2 bytes
    b.Char(0x1F600);  // 4 bytes
    CHECK(b.text() == std::string("a\xC3\xA9\xF0\x9F\x98\x80"));
    b.Key(kKeyBack, false);
    CHECK(b.text() == std::string("a\xC3\xA9"));
    b.Key(kKeyBack, false);
    CHECK(b.text() == "a");
    CHECK(b.Char(0x08));  // Backspace's character: dropped
    CHECK(b.Char(0x1B));
    CHECK(b.text() == "a");
    CHECK(b.Key(kKeyEscape, false));
    CHECK_FALSE(b.open());
    CHECK(b.KeysTaken(true));  // Esc still held: not Hookshot's menu
    CHECK_FALSE(b.KeysTaken(false));
}

TEST_CASE("chat box: at most 200 characters") {
    Box b;
    b.Key(kKeyOpen, false);
    b.Char('t');
    for (int i = 0; i < 250; ++i) {
        b.Char(0xE9);
    }
    CHECK(b.text().size() == 400);  // 200 characters of 2 bytes
}

TEST_CASE("chat box: UTF-8") {
    CHECK(CoopChatBox::Utf8('a') == "a");
    CHECK(CoopChatBox::Utf8(0xE9) == "\xC3\xA9");
    CHECK(CoopChatBox::Utf8(0x20AC) == "\xE2\x82\xAC");
    CHECK(CoopChatBox::Utf8(0xD800).empty());
    CHECK(CoopChatBox::Utf8(0x110000).empty());
}
