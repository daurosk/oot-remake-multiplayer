#pragma once
// A fixed ring from several pushing threads to a reader, with no lock and no
// allocation: what a hook on one of the game's threads may use (it must not
// wait on anything of co-op's). After D. Vyukov's bounded queue: each cell's
// sequence number says whose turn it is.
//
// T is copied in and out and must be trivially copyable; kSlots is a power
// of two. A push into a full ring fails (the caller counts it); nothing is
// overwritten.
#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace coop::host {

template <typename T, std::size_t kSlots>
class BoundedQueue {
  static_assert(std::is_trivially_copyable_v<T>);
  static_assert(kSlots >= 2 && (kSlots & (kSlots - 1)) == 0);

 public:
  BoundedQueue() {
    for (std::size_t i = 0; i < kSlots; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
  }

  // Any thread. False when full.
  bool push(const T& value) {
    std::size_t pos = tail_.load(std::memory_order_relaxed);
    for (;;) {
      Cell& cell = cells_[pos & (kSlots - 1)];
      const std::size_t seq = cell.seq.load(std::memory_order_acquire);
      const auto ahead = static_cast<std::ptrdiff_t>(seq - pos);
      if (ahead == 0) {
        if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          cell.value = value;
          cell.seq.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (ahead < 0) {
        return false;
      } else {
        pos = tail_.load(std::memory_order_relaxed);
      }
    }
  }

  // Any thread. False when empty.
  bool pop(T& out) {
    std::size_t pos = head_.load(std::memory_order_relaxed);
    for (;;) {
      Cell& cell = cells_[pos & (kSlots - 1)];
      const std::size_t seq = cell.seq.load(std::memory_order_acquire);
      const auto ahead = static_cast<std::ptrdiff_t>(seq - (pos + 1));
      if (ahead == 0) {
        if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          out = cell.value;
          cell.seq.store(pos + kSlots, std::memory_order_release);
          return true;
        }
      } else if (ahead < 0) {
        return false;
      } else {
        pos = head_.load(std::memory_order_relaxed);
      }
    }
  }

 private:
  struct Cell {
    std::atomic<std::size_t> seq{0};
    T value{};
  };
  std::array<Cell, kSlots> cells_;
  std::atomic<std::size_t> head_{0}, tail_{0};
};

}  // namespace coop::host
