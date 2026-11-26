// #239 (the alt-tab freeze): the server's console lines never hold up its service loop.

#include <doctest/doctest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "console_out.hpp"

using coop::server::ConsoleOut;

namespace {

// A console that takes no output until opened (a selection held in its window).
struct StalledConsole {
  std::mutex mutex;
  std::condition_variable cv;
  bool open = false;
  bool entered = false;
  std::vector<std::string> lines;

  void write(const std::string& line) {
    std::unique_lock lock(mutex);
    entered = true;
    cv.notify_all();
    cv.wait(lock, [this] { return open; });
    lines.push_back(line);
  }
  void wait_entered() {
    std::unique_lock lock(mutex);
    cv.wait(lock, [this] { return entered; });
  }
  void release() {
    std::lock_guard lock(mutex);
    open = true;
    cv.notify_all();
  }
};

}  // namespace

TEST_CASE("console out: lines reach the sink in order") {
  std::vector<std::string> got;
  {
    ConsoleOut out([&](const std::string& line) { got.push_back(line); });
    for (int i = 0; i < 100; ++i) {
      CHECK(out.push("line " + std::to_string(i)));
    }
    out.stop();
    CHECK(out.dropped() == 0);
  }
  REQUIRE(got.size() == 100);
  CHECK(got.front() == "line 0");
  CHECK(got.back() == "line 99");
}

TEST_CASE("console out: a stalled console never blocks push, and lines past the bound are dropped and counted") {
  StalledConsole console;
  ConsoleOut out([&](const std::string& line) { console.write(line); }, 4);
  CHECK(out.push("first"));  // the writer takes it and blocks in the console
  console.wait_entered();
  const auto start = std::chrono::steady_clock::now();
  int queued = 0;
  for (int i = 0; i < 1000; ++i) {
    queued += out.push("line " + std::to_string(i)) ? 1 : 0;
  }
  // The service loop's side went on at once (a held console used to stop it for as long as it was held).
  const bool quick = std::chrono::steady_clock::now() - start < std::chrono::seconds(2);
  CHECK(quick);
  CHECK(queued == 4);
  CHECK(out.dropped() == 996);
  console.release();
  out.stop();
  REQUIRE(console.lines.size() == 6);  // first, the 4 queued, then the note
  CHECK(console.lines[0] == "first");
  CHECK(console.lines[1] == "line 0");
  CHECK(console.lines[4] == "line 3");
  CHECK(console.lines[5] == ConsoleOut::dropped_note(996));
}

TEST_CASE("console out: after stop a line is written at once") {
  std::vector<std::string> got;
  ConsoleOut out([&](const std::string& line) { got.push_back(line); });
  out.stop();
  out.stop();  // twice: nothing more
  CHECK(out.push("late"));
  REQUIRE(got.size() == 1);
  CHECK(got[0] == "late");
}
