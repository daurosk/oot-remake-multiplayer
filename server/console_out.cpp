#include "console_out.hpp"

#include <utility>

namespace coop::server {

ConsoleOut::ConsoleOut(Sink sink, std::size_t max_lines)
    : sink_(std::move(sink)), max_lines_(max_lines == 0 ? 1 : max_lines), writer_([this] { run(); }) {}

ConsoleOut::~ConsoleOut() { stop(); }

bool ConsoleOut::push(std::string line) {
  {
    std::lock_guard lock(mutex_);
    if (!stopped_) {
      if (lines_.size() >= max_lines_) {
        ++dropped_;
        return false;
      }
      lines_.push_back(std::move(line));
      wake_.notify_one();
      return true;
    }
  }
  sink_(line);  // after stop(): nothing else writes any more
  return true;
}

void ConsoleOut::stop() {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
    wake_.notify_one();
  }
  if (writer_.joinable()) {
    writer_.join();
  }
  std::lock_guard lock(mutex_);
  stopped_ = true;
}

std::uint64_t ConsoleOut::dropped() const {
  std::lock_guard lock(mutex_);
  return dropped_;
}

std::string ConsoleOut::dropped_note(std::uint64_t n) {
  return "coop_server: " + std::to_string(n) + " console line(s) dropped: the window took no output for a while "
         "(a selection in it?); the server went on";
}

void ConsoleOut::run() {
  std::unique_lock lock(mutex_);
  for (;;) {
    wake_.wait(lock, [this] { return stopping_ || !lines_.empty() || dropped_ != reported_; });
    if (lines_.empty() && dropped_ == reported_) {
      return;  // stopping, all written
    }
    std::deque<std::string> batch;
    batch.swap(lines_);
    const std::uint64_t missed = dropped_ - reported_;
    reported_ = dropped_;
    lock.unlock();
    // The lines queued before the drops come first; the note goes after them, where the gap was.
    for (const std::string& line : batch) {
      sink_(line);
    }
    if (missed != 0) {
      sink_(dropped_note(missed));
    }
    lock.lock();
  }
}

}  // namespace coop::server
