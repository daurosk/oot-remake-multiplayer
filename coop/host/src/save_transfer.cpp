#include "coop/host/save_transfer.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace coop::host::save {

namespace fs = std::filesystem;
namespace proto = coop::protocol;

namespace {

constexpr std::string_view kMagic = "OOTSAVE1";

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

std::uint32_t get_u32(std::span<const std::uint8_t> b, std::size_t at) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(b[at + i]) << (8 * i);
  return v;
}

std::optional<std::vector<std::uint8_t>> read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) return std::nullopt;
  return bytes;
}

// Written to a .tmp beside it, then renamed over: a game that reads the file
// meanwhile sees the old one or the new one, never half of one.
bool write_file(const fs::path& path, std::span<const std::uint8_t> bytes) {
  fs::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) return false;
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

bool slot_name(const std::string& name) {
  return name.size() == 7 && name.rfind("slot_", 0) == 0 && name[5] >= '0' && name[5] <= '9' &&
         name[6] >= '0' && name[6] <= '9';
}

}  // namespace

bool slot_file(std::string_view name) {
  return std::find(std::begin(kSlotFiles), std::end(kSlotFiles), name) != std::end(kSlotFiles);
}

std::vector<std::uint8_t> pack(const SaveSlot& slot) {
  std::vector<std::uint8_t> out(kMagic.begin(), kMagic.end());
  out.push_back(static_cast<std::uint8_t>(slot.files.size()));
  for (const auto& f : slot.files) {
    out.push_back(static_cast<std::uint8_t>(f.name.size()));
    out.insert(out.end(), f.name.begin(), f.name.end());
    put_u32(out, static_cast<std::uint32_t>(f.bytes.size()));
    out.insert(out.end(), f.bytes.begin(), f.bytes.end());
  }
  if (!unpack(out)) return {};
  return out;
}

std::optional<SaveSlot> unpack(std::span<const std::uint8_t> bytes) {
  if (bytes.size() > proto::kMaxSaveBytes || bytes.size() < kMagic.size() + 1) return std::nullopt;
  if (std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) != 0) return std::nullopt;
  std::size_t at = kMagic.size();
  const std::size_t count = bytes[at++];
  if (count == 0 || count > std::size(kSlotFiles)) return std::nullopt;
  SaveSlot slot;
  for (std::size_t i = 0; i < count; ++i) {
    if (at + 1 > bytes.size()) return std::nullopt;
    const std::size_t name_size = bytes[at++];
    if (at + name_size + 4 > bytes.size()) return std::nullopt;
    std::string name(reinterpret_cast<const char*>(bytes.data() + at), name_size);
    at += name_size;
    if (!slot_file(name)) return std::nullopt;
    for (const auto& f : slot.files) {
      if (f.name == name) return std::nullopt;
    }
    const std::size_t size = get_u32(bytes, at);
    at += 4;
    if (size > bytes.size() - at) return std::nullopt;
    slot.files.push_back({std::move(name), {bytes.begin() + static_cast<std::ptrdiff_t>(at),
                                            bytes.begin() + static_cast<std::ptrdiff_t>(at + size)}});
    at += size;
  }
  if (at != bytes.size()) return std::nullopt;
  const bool progress = std::any_of(slot.files.begin(), slot.files.end(),
                                    [](const SaveFile& f) { return f.name == kSlotFiles[0]; });
  if (!progress) return std::nullopt;
  return slot;
}

std::vector<std::uint8_t> pack_no_save() {
  std::vector<std::uint8_t> out(kMagic.begin(), kMagic.end());
  out.push_back(0);
  return out;
}

bool is_no_save(std::span<const std::uint8_t> bytes) {
  return bytes.size() == kMagic.size() + 1 && std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) == 0 &&
         bytes[kMagic.size()] == 0;
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
  static const auto table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  std::uint32_t c = 0xFFFFFFFFu;
  for (const std::uint8_t b : bytes) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

std::vector<proto::SavePart> split(std::span<const std::uint8_t> packed, std::uint32_t transfer_id,
                                   proto::SaveReason reason, std::uint8_t to_player) {
  const auto total = static_cast<std::uint32_t>(packed.size());
  const std::uint16_t count = proto::save_part_count(packed.size() > proto::kMaxSaveBytes ? 0 : total);
  std::vector<proto::SavePart> parts;
  if (count == 0) return parts;
  const std::uint32_t crc = crc32(packed);
  for (std::uint16_t i = 0; i < count; ++i) {
    proto::SavePart p;
    p.to_player = to_player;
    p.transfer_id = transfer_id;
    p.reason = reason;
    p.total_size = total;
    p.crc32 = crc;
    p.index = i;
    p.count = count;
    const std::size_t at = static_cast<std::size_t>(i) * proto::kMaxSavePart;
    const std::size_t size = proto::save_part_size(total, i);
    p.data.assign(packed.begin() + static_cast<std::ptrdiff_t>(at),
                  packed.begin() + static_cast<std::ptrdiff_t>(at + size));
    parts.push_back(std::move(p));
  }
  return parts;
}

Assembler::Result Assembler::add(const proto::SavePart& part, std::uint8_t host_number) {
  if (host_number == 0 || part.player_number != host_number) return Result::refused;
  const std::uint16_t count = proto::save_part_count(part.total_size);
  if (count == 0 || part.count != count || part.index >= count ||
      part.data.size() != proto::save_part_size(part.total_size, part.index)) {
    return Result::refused;
  }
  if (!started_ || part.transfer_id != id_ || part.player_number != from_) {
    started_ = true;
    id_ = part.transfer_id;
    from_ = part.player_number;
    reason_ = part.reason;
    total_ = part.total_size;
    crc_ = part.crc32;
    bytes_.assign(total_, 0);
    have_.assign(count, false);
    have_count_ = 0;
  } else if (part.total_size != total_ || part.crc32 != crc_ || part.reason != reason_) {
    return Result::refused;
  }
  if (!have_[part.index]) {
    std::copy(part.data.begin(), part.data.end(),
              bytes_.begin() + static_cast<std::ptrdiff_t>(part.index * proto::kMaxSavePart));
    have_[part.index] = true;
    ++have_count_;
  } else if (have_count_ < have_.size()) {
    return Result::partial;  // the same part again
  } else {
    return Result::refused;  // the transfer is already done
  }
  if (have_count_ < have_.size()) return Result::partial;
  if (crc32(bytes_) != crc_) {
    started_ = false;
    bytes_.clear();
    have_.clear();
    have_count_ = 0;
    return Result::bad_crc;
  }
  done_ = std::move(bytes_);
  bytes_.clear();
  return Result::complete;
}

std::vector<std::uint8_t> Assembler::take() { return std::exchange(done_, {}); }

std::vector<Planner::Send> Planner::due(std::optional<Time> newest) {
  std::vector<Send> out;
  if (!newest) {
    // No save on disk (a new game): each asker hears so now instead of waiting for nothing.
    for (const std::uint8_t p : asks_) out.push_back({p, proto::SaveReason::join, true});
    asks_.clear();
    return out;
  }
  if (reload_ && (!loaded_at_ || *newest > *loaded_at_)) {
    reload_ = false;
    loaded_at_.reset();
    asks_.clear();  // everyone gets this one
    out.push_back({0, proto::SaveReason::host_load});
    return out;
  }
  // Asks are answered at once with the newest save on disk, also while the send after a load waits.
  for (const std::uint8_t p : asks_) out.push_back({p, proto::SaveReason::join});
  asks_.clear();
  return out;
}

bool reask_due(const GuestSession& s, std::uint64_t now_us) {
  if (!s.asked || s.ready || s.no_save || s.unwritten || s.asks >= kGuestMaxAsks) return false;
  if (s.assembler.received() != 0 || s.last_part_us > s.asked_us) return false;  // (parts came: the stuck note waits)
  return now_us - s.asked_us >= kGuestReaskUs;
}

std::optional<fs::path> newest_slot(const fs::path& game_dir) {
  std::error_code ec;
  std::optional<fs::path> best;
  fs::file_time_type best_time{};
  for (fs::directory_iterator it(game_dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_directory(ec) || !slot_name(it->path().filename().string())) continue;
    const auto progress = it->path() / std::string(kSlotFiles[0]);
    std::error_code tec;
    const auto t = fs::last_write_time(progress, tec);
    if (tec) continue;
    if (!best || t > best_time) {
      best = it->path();
      best_time = t;
    }
  }
  return best;
}

std::optional<fs::file_time_type> newest_write(const fs::path& game_dir) {
  const auto slot = newest_slot(game_dir);
  if (!slot) return std::nullopt;
  std::error_code ec;
  const auto t = fs::last_write_time(*slot / std::string(kSlotFiles[0]), ec);
  if (ec) return std::nullopt;
  return t;
}

std::optional<SaveSlot> read_slot(const fs::path& slot_dir) {
  SaveSlot slot;
  for (const std::string_view name : kSlotFiles) {
    const fs::path path = slot_dir / std::string(name);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
      if (name == kSlotFiles[0]) return std::nullopt;
      continue;
    }
    auto bytes = read_file(path);
    if (!bytes) return std::nullopt;
    slot.files.push_back({std::string(name), std::move(*bytes)});
  }
  return slot;
}

bool is_coop_dir(const fs::path& dir) {
  fs::path p = dir;
  if (!p.has_filename()) p = p.parent_path();  // a trailing separator
  const std::string name = p.filename().string();
  return name.size() > kCoopSuffix.size() &&
         name.compare(name.size() - kCoopSuffix.size(), kCoopSuffix.size(), kCoopSuffix) == 0;
}

fs::path coop_dir_of(const fs::path& own_dir) {
  fs::path p = own_dir;
  if (!p.has_filename()) p = p.parent_path();
  if (is_coop_dir(p)) return p;
  p += std::string(kCoopSuffix);
  return p;
}

fs::path own_dir_of(const fs::path& coop_dir) {
  fs::path p = coop_dir;
  if (!p.has_filename()) p = p.parent_path();
  if (!is_coop_dir(p)) return p;
  const std::string name = p.filename().string();
  return p.parent_path() / name.substr(0, name.size() - kCoopSuffix.size());
}

WriteResult write_coop_save(const fs::path& coop_dir, const SaveSlot& slot, const fs::path& own_dir) {
  if (!is_coop_dir(coop_dir) || is_coop_dir(own_dir) || slot.files.empty()) return WriteResult::not_coop;
  std::error_code ec;
  if (fs::equivalent(coop_dir, own_dir, ec)) return WriteResult::not_coop;
  fs::create_directories(coop_dir, ec);
  if (ec) return WriteResult::failed;
  // The player's own settings, so the game does not start as on a new console.
  for (const char* name : {"option.sav", "account.sav"}) {
    const fs::path to = coop_dir / name;
    if (fs::exists(to, ec)) continue;
    const fs::path from = own_dir / name;
    if (fs::is_regular_file(from, ec)) fs::copy_file(from, to, ec);
  }
  std::set<std::string> names{"slot_00"};
  for (const fs::path& from : {coop_dir, own_dir}) {
    std::error_code iec;
    for (fs::directory_iterator it(from, iec), end; !iec && it != end; it.increment(iec)) {
      const std::string name = it->path().filename().string();
      std::error_code dec;
      if (slot_name(name) && it->is_directory(dec)) names.insert(name);
    }
  }
  for (const auto& name : names) {
    const fs::path dir = coop_dir / name;
    fs::path part = dir;
    part += ".part";
    std::error_code dec;
    fs::remove_all(part, dec);
    dec.clear();
    fs::create_directories(part, dec);
    bool ok = !dec;
    for (const auto& f : slot.files) {
      if (!ok) break;
      ok = slot_file(f.name) && write_file(part / f.name, f.bytes);
    }
    // Whole, then in place: slot_NN is the old slot or the new one, never half of one. The old one is moved to
    // slot_NN.old first and put back if the new one cannot go in (a file the game holds open on Windows).
    fs::path old = dir;
    old += ".old";
    const bool had_old = ok && fs::exists(dir, dec);
    if (ok && had_old) {
      fs::remove_all(old, dec);
      dec.clear();
      fs::rename(dir, old, dec);
      ok = !dec;
    }
    if (ok) {
      fs::rename(part, dir, dec);
      ok = !dec;
      if (!ok && had_old) {
        std::error_code bec;
        fs::rename(old, dir, bec);
      }
    }
    std::error_code rec;
    fs::remove_all(part, rec);
    if (!ok) return WriteResult::failed;
    if (had_old) fs::remove_all(old, rec);
  }
  return WriteResult::written;
}

int clear_coop_slots(const fs::path& coop_dir) {
  if (!is_coop_dir(coop_dir)) return -1;
  std::error_code ec;
  if (!fs::is_directory(coop_dir, ec)) return 0;
  std::vector<fs::path> gone;
  for (fs::directory_iterator it(coop_dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (slot_name(it->path().filename().string()) && it->is_directory(ec)) gone.push_back(it->path());
  }
  int n = 0;
  for (const auto& d : gone) {
    std::error_code rec;
    fs::remove_all(d, rec);
    if (!rec) ++n;
  }
  return n;
}

PendingStep pending_step(bool slot_in, bool title_up, bool in_game) {
  if (in_game) return PendingStep::drop;
  return slot_in && title_up ? PendingStep::go : PendingStep::keep;
}

bool decide_again(bool decided, bool over, bool in_game) {
  return decided && over && !in_game;
}

FolderChoice folder_choice(std::optional<bool> env, bool can_switch, bool connected, bool is_host, bool in_game) {
  if (!can_switch) return env && *env ? FolderChoice::coop : FolderChoice::own;
  if (!connected) return FolderChoice::wait;
  if (is_host || in_game) return FolderChoice::own;
  return FolderChoice::coop;
}

}  // namespace coop::host::save
