#pragma once

// The server's console lines, written on their own thread (#239, the alt-tab
// freeze). coop_server's say() wrote with printf + fflush on the service
// loop's own thread: while the console took no output (a QuickEdit selection
// or Edit > Mark in its window, the Pause key, a full pipe to start-server.ps1
// whose Write-Host was paused), the write blocked, the loop stopped and every
// player got nothing from the server for as long (16 s, then a reconnect).
//
// push() only queues the line (a short lock, never the console): the writer
// thread hands each queued line to the sink in order. Past kMaxLines waiting,
// a line is dropped and counted; once the sink takes lines again, the writer
// says how many were dropped (one line) before the next one. stop() writes
// what is still queued, then ends the thread.

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace coop::server {

class ConsoleOut {
 public:
  using Sink = std::function<void(const std::string&)>;
  static constexpr std::size_t kMaxLines = 4096;

  explicit ConsoleOut(Sink sink, std::size_t max_lines = kMaxLines);
  ~ConsoleOut();
  ConsoleOut(const ConsoleOut&) = delete;
  ConsoleOut& operator=(const ConsoleOut&) = delete;

  // Queues `line` for the sink; false when it was dropped (the queue is full).
  // Never waits for the sink. After stop(): the line goes to the sink at once.
  bool push(std::string line);
  // Writes every queued line (and the dropped count), then ends the thread.
  void stop();
  std::uint64_t dropped() const;

  // The note the writer gives for `n` dropped lines.
  static std::string dropped_note(std::uint64_t n);

 private:
  void run();

  Sink sink_;
  std::size_t max_lines_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::string> lines_;
  std::uint64_t dropped_ = 0;   // all lines dropped so far
  std::uint64_t reported_ = 0;  // of them, said by a note
  bool stopping_ = false;
  bool stopped_ = false;
  std::thread writer_;
};

}  // namespace coop::server
