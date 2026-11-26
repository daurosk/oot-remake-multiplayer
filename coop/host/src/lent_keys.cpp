// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop/host/lent_keys.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace coop::host {
namespace {

std::int32_t as_int(std::uint32_t v) { return static_cast<std::int32_t>(v); }
std::uint32_t as_bits(std::int32_t v) { return static_cast<std::uint32_t>(v); }

std::string name_of(FlagKey key) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%#010x", key.hash);
  return buf;
}

}  // namespace

void LentKeys::lend(FlagKey count) {
  std::lock_guard lock(mutex_);
  if (asked_lends_.size() < 16) asked_lends_.push_back(count);
}

void LentKeys::give_back(FlagKey count) {
  std::lock_guard lock(mutex_);
  if (asked_backs_.size() < 16) asked_backs_.push_back(count);
}

void LentKeys::forget() {
  std::lock_guard lock(mutex_);
  counts_.clear();
  asked_lends_.clear();
  asked_backs_.clear();
}

std::vector<FlagKey> LentKeys::take_spent() {
  std::lock_guard lock(mutex_);
  return std::exchange(spent_, {});
}

std::int32_t LentKeys::lent(FlagKey count) const {
  std::lock_guard lock(mutex_);
  const auto it = counts_.find(count);
  return it == counts_.end() ? 0 : it->second.lent;
}

std::int32_t LentKeys::observe(FlagKey key, std::int32_t game) {
  // mutex_ held by the caller.
  const auto it = counts_.find(key);
  if (it == counts_.end()) return game;
  Count& c = it->second;
  if (c.seen && game < *c.seen && c.lent > 0) {
    // Our game's own spend: the lent key goes first (the lock it was lent for).
    const std::int32_t used = std::min(c.lent, *c.seen - game);
    c.lent -= used;
    for (std::int32_t i = 0; i < used; ++i) spent_.push_back(key);
  }
  c.seen = game;
  const std::int32_t shown = std::max(game - c.lent, 0);
  if (c.lent == 0) counts_.erase(it);
  return shown;
}

void LentKeys::note_written(FlagKey key, std::int32_t game) {
  // mutex_ held by the caller.
  if (const auto it = counts_.find(key); it != counts_.end()) it->second.seen = game;
}

std::vector<std::string> LentKeys::frame(FlagStore& game) {
  std::lock_guard lock(mutex_);
  std::vector<std::string> lines;
  const auto read = [&game](FlagKey key) -> std::optional<std::int32_t> {
    const auto now = game.read(key);
    if (!now || now->kind != protocol::FlagKind::int32) return std::nullopt;
    return as_int(now->value);
  };
  // Our game's own spends since the last frame, before anything is written.
  for (auto it = counts_.begin(); it != counts_.end();) {
    const FlagKey key = it->first;
    ++it;  // observe() may erase the entry
    if (const auto now = read(key)) observe(key, *now);
  }
  for (const FlagKey key : std::exchange(asked_lends_, {})) {
    const auto now = read(key);
    Count& c = counts_[key];
    if (!now || c.lent >= kMaxLent) {
      lines.push_back("doors: small key " + name_of(key) + " not lent (" +
                      (now ? "too many lent" : "count not readable") + ")");
      if (c.lent == 0) counts_.erase(key);
      continue;
    }
    if (!game.write(protocol::FlagEntry{key, protocol::FlagKind::int32, as_bits(*now + 1)})) {
      lines.push_back("doors: small key " + name_of(key) + " not lent (the write failed)");
      if (c.lent == 0) counts_.erase(key);
      continue;
    }
    ++c.lent;
    c.seen = *now + 1;
    lines.push_back("doors: small key " + name_of(key) + " lent to our game only (" + std::to_string(*now) +
                    " -> " + std::to_string(*now + 1) + "; the group's count stays " +
                    std::to_string(*now + 1 - c.lent) + ")");
  }
  for (const FlagKey key : std::exchange(asked_backs_, {})) {
    const auto it = counts_.find(key);
    const auto now = read(key);
    if (it == counts_.end() || it->second.lent == 0 || !now) {
      continue;  // already spent (the lock opened), or a load forgot it
    }
    Count& c = it->second;
    if (*now <= 0 || !game.write(protocol::FlagEntry{key, protocol::FlagKind::int32, as_bits(*now - 1)})) {
      lines.push_back("doors: lent small key " + name_of(key) + " could not be taken back");
      --c.lent;  // never shown to the group either way
    } else {
      --c.lent;
      c.seen = *now - 1;
      lines.push_back("doors: lent small key " + name_of(key) + " taken back (" + std::to_string(*now) + " -> " +
                      std::to_string(*now - 1) + ")");
    }
    if (c.lent == 0) counts_.erase(it);
  }
  return lines;
}

std::optional<protocol::FlagEntry> LentKeyStore::read(FlagKey key) {
  std::optional<protocol::FlagEntry> now = inner_.read(key);
  if (!now || now->kind != protocol::FlagKind::int32) return now;
  std::lock_guard lock(lent_.mutex_);
  if (!lent_.counts_.contains(key)) return now;
  now->value = as_bits(lent_.observe(key, as_int(now->value)));
  return now;
}

bool LentKeyStore::write(const protocol::FlagEntry& entry) {
  std::unique_lock lock(lent_.mutex_);
  const FlagKey key = key_of(entry);
  const auto it = lent_.counts_.find(key);
  if (it == lent_.counts_.end() ||
      (entry.kind != protocol::FlagKind::int32 && entry.kind != protocol::FlagKind::count_delta)) {
    lock.unlock();
    return inner_.write(entry);
  }
  if (entry.kind == protocol::FlagKind::count_delta) {
    // Another player's change added to the game's value: the lent keys stay,
    // and a drop made this way is no spend of ours.
    lock.unlock();
    const bool ok = inner_.write(entry);
    if (ok) {
      std::lock_guard again(lent_.mutex_);
      if (const auto c = lent_.counts_.find(key); c != lent_.counts_.end() && c->second.seen) {
        c->second.seen = std::max(*c->second.seen + as_int(entry.value), 0);
      }
    }
    return ok;
  }
  // A total of the group's: our lent keys stay on top of it.
  protocol::FlagEntry with = entry;
  with.value = as_bits(as_int(entry.value) + it->second.lent);
  lock.unlock();
  const bool ok = inner_.write(with);
  if (ok) {
    std::lock_guard again(lent_.mutex_);
    lent_.note_written(key, as_int(with.value));
  }
  return ok;
}

bool LentKeyStore::write_load_back(const protocol::FlagEntry& entry) { return inner_.write_load_back(entry); }

bool LentKeyStore::write_exact(const protocol::FlagEntry& entry) {
  // The host's save load: its values as they are. The caller forgets the lent keys.
  return inner_.write_exact(entry);
}

}  // namespace coop::host
