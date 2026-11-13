#include "coop/host/game_flag_events.hpp"

namespace coop::host {

bool GameFlagEvents::push(const GameFlagWrite& write) {
  if (ring_.push(write)) {
    pushed_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  dropped_.fetch_add(1, std::memory_order_relaxed);
  overflow_.store(true);
  return false;
}

void GameFlagEvents::clear() {
  GameFlagWrite gone;
  while (pop(gone)) {
  }
  overflow_.store(false);
}

bool GameFlagCalls::put(const GameFlagWrite& write) {
  const std::lock_guard lock(mutex_);
  const Id id{write.type, write.id};
  if (const auto it = values_.find(id); it != values_.end()) {
    it->second = write.value;
    return true;
  }
  if (values_.size() >= kMost) return false;
  values_.emplace(id, write.value);
  order_.push_back(id);
  return true;
}

std::optional<std::uint32_t> GameFlagCalls::pending(GameFlagType type, std::uint64_t id) const {
  const std::lock_guard lock(mutex_);
  const auto it = values_.find(Id{type, id});
  if (it == values_.end()) return std::nullopt;
  return it->second;
}

std::size_t GameFlagCalls::run(std::size_t most, const std::function<bool(const GameFlagWrite&)>& call) {
  std::size_t done = 0;
  while (done < most) {
    GameFlagWrite next;
    {
      const std::lock_guard lock(mutex_);
      if (order_.empty()) break;
      const Id id = order_.front();
      next = GameFlagWrite{id.first, id.second, values_.at(id)};
    }
    // The setter runs without our lock: it is the game's code. The write
    // stays readable (pending) until the setter has it in the game's queue.
    if (!call(next)) break;
    {
      const std::lock_guard lock(mutex_);
      // Only run() takes writes out, so the front is still this flag. A
      // newer value put while the setter ran stays, for the next run.
      const auto it = values_.find(Id{next.type, next.id});
      if (it != values_.end() && it->second == next.value) {
        values_.erase(it);
        order_.pop_front();
      }
    }
    ++done;
    handed_.fetch_add(1, std::memory_order_relaxed);
  }
  return done;
}

std::size_t GameFlagCalls::size() const {
  const std::lock_guard lock(mutex_);
  return values_.size();
}

void GameFlagCalls::clear() {
  const std::lock_guard lock(mutex_);
  values_.clear();
  order_.clear();
}

}  // namespace coop::host
