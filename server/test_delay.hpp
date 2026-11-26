#pragma once

// TEST ONLY: a bad connection made inside coop_server (--test-delay-ms,
// --test-jitter-ms; refused unless OOT_COOP_TEST=1). Hookshot and Longshot
// games cannot go through tools/netsim/netsim.py the way the emulator runs
// do, so the server itself holds each packet before handing it on: what a
// player sends waits before the session sees it, and what the session sends
// waits before ENet gets it. The same rules as netsim.py: the round trip is
// split half each way, every packet gets +- up to the jitter on top, and one
// connection's packets in one direction keep their order (as on one route),
// so reliable stays reliable and in order. Never in a kit's start-server.

#include <cstddef>
#include <cstdint>
#include <map>
#include <random>
#include <utility>
#include <vector>

namespace coop::server {

struct TestDelay {
  std::uint32_t round_trip_ms = 0;  // added round trip: half each way
  std::uint32_t jitter_ms = 0;      // each packet +- up to this, each way
  std::uint64_t seed = 1;           // the jitter's random numbers
  bool on() const { return round_trip_ms != 0 || jitter_ms != 0; }
};

// When each packet may go on. A lane is one connection in one direction;
// due() never gives a lane a time before the one it gave last, so a lane's
// packets leave in the order they came.
class DelayClock {
 public:
  explicit DelayClock(TestDelay config) : config_(config), random_(config.seed) {}

  // The time (in the server's microseconds) a packet entering `lane` at
  // `now_us` may leave.
  std::uint64_t due(std::uint64_t lane, std::uint64_t now_us);
  // The latest time a lane has handed out (0: none), for an event that
  // must come after everything queued on it.
  std::uint64_t last(std::uint64_t lane) const;
  void forget(std::uint64_t lane) { last_.erase(lane); }

 private:
  TestDelay config_;
  std::mt19937_64 random_;
  std::map<std::uint64_t, std::uint64_t> last_;
};

// Items held until their time, then given back in time order (and in the
// order they were put in for the same time).
template <typename T>
class DelayQueue {
 public:
  void push(std::uint64_t due_us, T item) { items_.emplace(Key{due_us, next_++}, std::move(item)); }

  std::vector<T> take_due(std::uint64_t now_us) {
    std::vector<T> out;
    auto it = items_.begin();
    while (it != items_.end() && it->first.due_us <= now_us) {
      out.push_back(std::move(it->second));
      it = items_.erase(it);
    }
    return out;
  }

  template <typename Pred>
  void drop_if(Pred pred) {
    std::erase_if(items_, [&](const auto& entry) { return pred(entry.second); });
  }

  std::size_t size() const { return items_.size(); }

 private:
  struct Key {
    std::uint64_t due_us;
    std::uint64_t order;
    bool operator<(const Key& o) const { return due_us != o.due_us ? due_us < o.due_us : order < o.order; }
  };
  std::map<Key, T> items_;
  std::uint64_t next_ = 0;
};

}  // namespace coop::server
