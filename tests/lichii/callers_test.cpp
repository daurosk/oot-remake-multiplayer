// SPDX-License-Identifier: GPL-3.0-or-later
// The callers' stepper both runtimes share (lichii/hook/coop_callers.h): the
// emulator's cave asks one call at a time (Next), Hookshot runs a frame
// (RunFrame).
#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "coop_callers.h"

namespace {

class NoGuest : public CoopPuppet::Guest {
public:
    bool Read(std::uint64_t, void*, std::size_t) override {
        return false;
    }
    bool Write(std::uint64_t, const void*, std::size_t) override {
        return false;
    }
};

CoopPuppet::Call CallTo(std::uint64_t function, std::uint64_t x0 = 0) {
    CoopPuppet::Call c;
    c.function = function;
    c.x[0] = x0;
    return c;
}

// A caller that makes `calls` calls a frame to `function`, noting what it was
// told after each.
struct Scripted {
    std::string name;
    std::uint64_t function = 0;
    int calls = 0;
    int made = 0;
    std::vector<std::string>* log = nullptr;
    std::optional<CoopPuppet::Call> operator()(CoopPuppet::Guest&, bool after_call, std::uint64_t result,
                                               std::uint64_t) {
        if (!after_call) {
            made = 0;
            log->push_back(name + " start");
        } else {
            log->push_back(name + " got " + std::to_string(result));
        }
        if (made >= calls) {
            return std::nullopt;
        }
        ++made;
        return CallTo(function, static_cast<std::uint64_t>(made));
    }
};

}  // namespace

TEST_CASE("callers: one frame asks the first, then the others by order, with each call's result") {
    std::vector<std::string> log;
    CoopCallers::ResetForTest(Scripted{"first", 0x100, 1, 0, &log});
    CoopCallers::Add(20, Scripted{"late", 0x300, 1, 0, &log});
    CoopCallers::Add(10, Scripted{"early", 0x200, 2, 0, &log});
    NoGuest guest;
    std::vector<std::uint64_t> called;
    const auto made = CoopCallers::RunFrame(guest, 0, 256, [&](const CoopPuppet::Call& c) {
        called.push_back(c.function);
        return std::optional<std::uint64_t>(c.function + c.x[0]);
    });
    CHECK(made == 4);
    CHECK(called == std::vector<std::uint64_t>{0x100, 0x200, 0x200, 0x300});
    CHECK(log == std::vector<std::string>{"first start", "first got 257", "early start", "early got 513",
                                          "early got 514", "late start", "late got 769"});
    CoopCallers::ResetForTest();
}

TEST_CASE("callers: the one-at-a-time stepper gives the same calls as a whole frame") {
    std::vector<std::string> log;
    CoopCallers::ResetForTest(Scripted{"first", 0x100, 2, 0, &log});
    CoopCallers::Add(5, Scripted{"next", 0x200, 1, 0, &log});
    NoGuest guest;
    std::vector<std::uint64_t> called;
    auto call = CoopCallers::Next(guest, false, 0, 0);
    while (call) {
        called.push_back(call->function);
        call = CoopCallers::Next(guest, true, 7, 0);
    }
    CHECK(called == std::vector<std::uint64_t>{0x100, 0x100, 0x200});
    CoopCallers::ResetForTest();
}

TEST_CASE("callers: a frame stops at the call limit and when a call cannot be made, and the next frame starts over") {
    std::vector<std::string> log;
    CoopCallers::ResetForTest(Scripted{"first", 0x100, 5, 0, &log});
    NoGuest guest;
    int n = 0;
    CHECK(CoopCallers::RunFrame(guest, 0, 3, [&](const CoopPuppet::Call&) {
              ++n;
              return std::optional<std::uint64_t>(0);
          }) == 3);
    CHECK(n == 3);
    log.clear();
    CHECK(CoopCallers::RunFrame(guest, 0, 256, [](const CoopPuppet::Call&) { return std::optional<std::uint64_t>(); }) ==
          0);
    CHECK(log == std::vector<std::string>{"first start"});
    log.clear();
    CHECK(CoopCallers::RunFrame(guest, 0, 256, [](const CoopPuppet::Call&) { return std::optional<std::uint64_t>(1); }) ==
          5);
    CHECK(log.front() == "first start");
    CoopCallers::ResetForTest();
}
