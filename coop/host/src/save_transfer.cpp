#include "coop/host/save_transfer.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include "coop/host/flags.hpp"
#include "coop/host/game_items.hpp"

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

std::vector<Planner::Send> Planner::due(std::optional<Time> newest, std::uint64_t now_us) {
  std::vector<Send> out;
  if (!newest) {
    // No save on disk (a new game): each asker hears so now instead of waiting for nothing.
    for (const std::uint8_t p : asks_) out.push_back({p, proto::SaveReason::join, true});
    asks_.clear();
    fresh_owed_.clear();
    round_.reset();
    return out;
  }
  if (reload_ && (!loaded_at_ || *newest > *loaded_at_)) {
    reload_ = false;
    loaded_at_.reset();
    asks_.clear();  // everyone gets this one
    fresh_owed_.clear();
    round_.reset();   // (a save after the load is also after the ask)
    out.push_back({0, proto::SaveReason::host_load});
    return out;
  }
  // A round: its asks wait for the save asked for, until the time limit (then the save on disk, as before).
  bool fresh = false;
  if (round_) {
    fresh = *newest > round_->since;
    if (!fresh && now_us - round_->start_us < kFreshSaveUs) return out;
    round_.reset();
  }
  // Asks are answered at once with the newest save on disk, also while the send after a load waits.
  for (const std::uint8_t p : asks_) {
    out.push_back({p, proto::SaveReason::join, false, fresh || fresh_owed_.count(p) != 0});
  }
  asks_.clear();
  fresh_owed_.clear();
  return out;
}

std::string GuestNotices::on_part(std::uint32_t transfer_id, std::size_t count) {
  if (count <= 1 || (started_ && transfer_id == transfer_)) return {};
  started_ = true;
  transfer_ = transfer_id;
  loading_ = true;
  said_early_ = false;
  return std::string(kLoading);
}

std::string GuestNotices::on_ready(bool in_game) {
  loading_ = false;
  return std::string(in_game ? kSuccessPlaying : kSuccessTitle);
}

std::string GuestNotices::on_in_game(bool in_game) {
  const bool came_in = in_game && !was_in_game_;
  was_in_game_ = in_game;
  if (!came_in || !loading_ || said_early_) return {};
  said_early_ = true;
  return std::string(kTooEarly);
}

std::string GuestNotices::on_gave_up() {
  loading_ = false;
  return std::string(kGaveUp);
}

namespace {
// An ask is due again (reask_due), the kGuestMaxAsks cap left out.
bool ask_again(const GuestSession& s, std::uint64_t now_us) {
  if (!s.asked || s.ready || s.no_save || s.unwritten) return false;
  if (now_us - s.asked_us < kGuestReaskUs) return false;
  const auto& a = s.assembler;
  if (a.received() == 0 && s.last_part_us <= s.asked_us) return true;  // (nothing at all came)
  // Parts came: only a transfer that stopped part way (a part lost), quiet for kGuestReaskUs.
  return a.received() != 0 && a.received() < a.count() && now_us - s.last_part_us >= kGuestReaskUs;
}
}  // namespace

bool reask_due(const GuestSession& s, std::uint64_t now_us) {
  return s.asks < kGuestMaxAsks && ask_again(s, now_us);
}

void note_part(GuestSession& s, std::uint32_t transfer_id, std::uint64_t now_us) {
  if (s.last_part_us == 0 || transfer_id != s.transfer) s.gave_up = false;
  s.transfer = transfer_id;
  s.last_part_us = now_us;
  s.said_stuck = false;
}

void note_write_failed(GuestSession& s) {
  s.unwritten.reset();
  s.ready = false;
  s.gave_up = true;   // stops give_up_due and "coming"; a new transfer (note_part) clears it
}

bool save_coming(const GuestSession& s, std::uint64_t now_us, std::uint64_t stuck_us) {
  const auto& a = s.assembler;
  return s.unwritten.has_value() || (a.received() != 0 && a.received() < a.count() && now_us - s.last_part_us < stuck_us) ||
         (s.asked && !s.ready && !s.no_save && !s.gave_up);
}

bool give_up_due(const GuestSession& s, std::uint64_t now_us) {
  return !s.gave_up && s.asks >= kGuestMaxAsks && ask_again(s, now_us);
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

ResyncCheck resync_check(bool connected_guest, bool can_switch, bool coming) {
  if (!connected_guest) return ResyncCheck::not_guest;
  if (!can_switch) return ResyncCheck::no_switch;
  if (coming) return ResyncCheck::coming;
  return ResyncCheck::go;
}

bool title_resync_due(const TitleResync& t) {
  return t.title_came && t.connected_guest && t.played_in_session && t.on && t.can_switch && t.plays_hosts_save &&
         !t.ask_waiting;
}

std::optional<fs::path> slot_to_send(const fs::path& game_dir, const std::optional<fs::path>& played,
                                     bool* played_used) {
  if (played_used) *played_used = false;
  if (played) {
    std::error_code ec;
    const fs::path p = played->lexically_normal();
    const bool ours = p.parent_path().lexically_normal() == game_dir.lexically_normal() &&
                      slot_name(p.filename().string()) &&
                      fs::is_regular_file(p / std::string(kSlotFiles[0]), ec);
    if (ours) {
      if (played_used) *played_used = true;
      return p;
    }
  }
  return newest_slot(game_dir);
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

bool copy_dir_whole(const fs::path& from, const fs::path& to, std::string* error) {
  auto fail = [error](const std::string& why) {
    if (error != nullptr) *error = why;
    return false;
  };
  std::error_code ec;
  if (fs::exists(to, ec)) return fail("already there");
  if (!fs::is_directory(from, ec)) return fail("no folder to copy from");
  fs::path part = to;
  part += ".part";
  fs::remove_all(part, ec);  // an earlier try's
  if (ec) return fail("could not remove an earlier copy: " + ec.message());
  fs::copy(from, part, fs::copy_options::recursive, ec);
  if (!ec) fs::rename(part, to, ec);
  if (ec) {
    const std::string why = ec.message();
    fs::remove_all(part, ec);
    return fail(why);
  }
  return true;
}

WriteResult write_coop_save(const fs::path& coop_dir, const SaveSlot& host_slot, const fs::path& own_dir,
                            int* own_kept) {
  if (own_kept != nullptr) *own_kept = -1;
  if (!is_coop_dir(coop_dir) || is_coop_dir(own_dir) || host_slot.files.empty()) return WriteResult::not_coop;
  std::error_code ec;
  if (fs::equivalent(coop_dir, own_dir, ec)) return WriteResult::not_coop;
  fs::create_directories(coop_dir, ec);
  if (ec) return WriteResult::failed;
  // Row 249: the guest's own values (from the slot there now, else as kept at the last clear) in the host's save.
  // Row 328: none kept yet (the first join here): a new player's values, never the host's wallet, bags and bottles.
  keep_own_values(coop_dir);
  SaveSlot slot = host_slot;
  const auto kept = kept_own_values(coop_dir);
  for (auto& f : slot.files) {
    if (f.name != kSlotFiles[0]) continue;
    const int n = put_own_values(f.bytes, kept ? *kept : new_player_values(f.bytes));
    if (own_kept != nullptr) *own_kept = kept ? n : -1;
  }
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
  keep_own_values(coop_dir);  // (row 249: the guest's own values outlive its slots)
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
  return slot_in && title_up && !in_game ? PendingStep::go : PendingStep::keep;
}

bool old_coop_first(bool pending, bool title_up, bool in_game, bool on_pending_dir) {
  return pending && title_up && !in_game && on_pending_dir;
}

bool decide_again(bool decided, bool over, bool in_game) {
  return decided && over && !in_game;
}

FolderChoice folder_choice(std::optional<bool> env, bool can_switch, bool connected, bool is_host, bool in_game) {
  if (!can_switch) return env && *env ? FolderChoice::coop : FolderChoice::own;
  if (!connected) return FolderChoice::wait;
  if (is_host) return FolderChoice::own;
  return in_game ? FolderChoice::coop_later : FolderChoice::coop;
}

bool rejoined_in_game(bool decided, bool ended, bool connected, bool in_game) {
  return decided && ended && connected && in_game;
}

bool role_changed(bool decided, bool connected, bool decided_as_host, bool is_host) {
  return decided && connected && decided_as_host != is_host;
}

bool home_due(bool on_coop_dir, bool title_menu, bool in_game, bool held) {
  return on_coop_dir && title_menu && !in_game && !held;
}

bool keeps_group_save(bool on_coop_dir, bool others_in) {
  return on_coop_dir && others_in;
}

std::optional<SaveValueAt> save_value_at(std::span<const std::uint8_t> progress, std::uint32_t hash,
                                         std::optional<std::uint32_t> type) {
  constexpr std::uint32_t kSaveMagic = 0x01020304;
  constexpr std::size_t kTable = 0x20;
  constexpr std::uint32_t kEnd = 0x20;
  if (hash == 0 || progress.size() < kTable || get_u32(progress, 0) != kSaveMagic) return std::nullopt;
  const std::size_t pool = get_u32(progress, 8);
  if (pool < kTable || pool > progress.size()) return std::nullopt;
  std::optional<std::uint32_t> kind;
  for (std::size_t at = kTable; at + 8 <= pool; at += 8) {
    const std::uint32_t first = get_u32(progress, at), second = get_u32(progress, at + 4);
    if (first == 0) {
      if (second != (kind ? *kind + 1 : 0) || second == kEnd) return std::nullopt;
      kind = second;
    } else if (!kind) {
      return std::nullopt;
    } else if (first == hash) {
      // (a Bool only when asked for by type: a value's lookup never takes a flag with the same hash)
      const bool inline_type = *kind == kSaveTypeInt || *kind == kSaveTypeEnum || *kind == kSaveTypeUInt ||
                               (type && *kind == kSaveTypeBool);
      if (inline_type && (!type || *type == *kind)) return SaveValueAt{at + 4, *kind};
    }
  }
  return std::nullopt;
}

namespace {

bool per_player_value(const ItemFlag& f) {
  return f.rule == ItemRule::per_player && (f.kind == proto::FlagKind::int32 || f.kind == proto::FlagKind::uint32);
}

constexpr std::string_view kOwnValuesHead = "coop-own-values 1";

}  // namespace

std::vector<OwnValue> read_own_values(std::span<const std::uint8_t> progress) {
  std::vector<OwnValue> out;
  for (const auto& f : item_flags()) {
    if (!per_player_value(f)) continue;
    if (const auto at = save_value_at(progress, f.hash)) out.push_back({f.hash, get_u32(progress, at->offset)});
  }
  return out;
}

namespace {

// What a per-player value may be in a save (review-01111-c): never more than that save's own capacity for it (its
// wallet, bags, quiver, magic and hearts: shared by max, so the host's save has the group's), and a bottle's content
// only in a bottle that save has. Capacities are the Int struct 0xf80f8077's members (game_items.cpp kCap*).
struct OwnLimit {
  std::uint32_t value;
  std::uint32_t cap;       // an Int whose value is the most `value` may be (0: none of it)
  std::uint32_t needs;     // a Bool that must be true for `value` to be written
};
const std::vector<OwnLimit>& own_limits() {
  static const std::vector<OwnLimit> limits = [] {
    const auto h = [](const char* name) { return flag_hash(name); };
    const auto bottle = [&](const char* b) {
      const std::string base = std::string("PlayerStatus.AssignableItem.BottleInfo.") + b;
      return OwnLimit{h((base + ".BottleItem").c_str()), 0, h((base + ".IsValid").c_str())};
    };
    return std::vector<OwnLimit>{
        {h("PlayerStatus.CurrentRupee"), 0xe0010a50, 0},             // 0xf80f8077.Rupee (the wallet)
        {h("PouchData.Item_Bomb.StockNum"), 0xe753508b, 0},          // .Bomb (the bomb bag)
        {h("PouchData.Item_Arrow_Normal.StockNum"), 0x418db791, 0},  // .Arrow (the quiver)
        {h("PouchData.Item_DekuSeed.StockNum"), 0xae8cc742, 0},      // .DekuSeed (the bullet bag)
        {h("PouchData.Item_DekuStick.StockNum"), 0x457616b0, 0},     // .DekuStick
        {h("PouchData.Item_DekuNut.StockNum"), 0x735b00f7, 0},       // .DekuNut
        {h("PlayerStatus.MP"), h("PlayerStatus.MPMax"), 0},
        {h("PlayerStatus.Life"), h("PlayerStatus.MaxLife"), 0},
        bottle("BottleA"), bottle("BottleB"), bottle("BottleC"), bottle("BottleD"), bottle("BottleE"),
    };
  }();
  return limits;
}

}  // namespace

int put_own_values(std::vector<std::uint8_t>& progress, std::span<const OwnValue> own) {
  static const std::uint32_t life_hash = flag_hash("PlayerStatus.Life");
  int n = 0;
  for (const auto& v : own) {
    const ItemFlag* f = find_item(v.hash);
    if (f == nullptr || !per_player_value(*f)) continue;  // (only per-player values, whatever the file says)
    const auto at = save_value_at(progress, v.hash);
    if (!at) continue;
    std::uint32_t value = v.value;
    const auto limit = std::find_if(own_limits().begin(), own_limits().end(),
                                    [&](const OwnLimit& l) { return l.value == v.hash; });
    if (limit != own_limits().end()) {
      if (limit->needs != 0) {
        const auto valid = save_value_at(progress, limit->needs, kSaveTypeBool);
        if (!valid || get_u32(progress, valid->offset) == 0) continue;  // (no such bottle in this save)
      }
      if (limit->cap != 0) {
        if (const auto cap = save_value_at(progress, limit->cap, kSaveTypeInt)) {
          const auto most = std::max<std::int32_t>(0, static_cast<std::int32_t>(get_u32(progress, cap->offset)));
          value = static_cast<std::uint32_t>(std::clamp(static_cast<std::int32_t>(value), 0, most));
        }
      }
    }
    if (v.hash == life_hash && static_cast<std::int32_t>(value) <= 0) continue;  // (never a Link with no life)
    for (std::size_t i = 0; i < 4; ++i) progress[at->offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    ++n;
  }
  return n;
}

std::vector<OwnValue> new_player_values(std::span<const std::uint8_t> progress) {
  static const std::uint32_t life = flag_hash("PlayerStatus.Life"), max_life = flag_hash("PlayerStatus.MaxLife");
  static const std::uint32_t mp = flag_hash("PlayerStatus.MP"), mp_max = flag_hash("PlayerStatus.MPMax");
  // An empty bottle's BottleItem: the Enum value "Empty" (murmur3 0xbe69de49, what every bottle of a save with no
  // bottle content holds).
  static const std::uint32_t empty = flag_hash("Empty");
  std::vector<OwnValue> out;
  for (const auto& f : item_flags()) {
    if (!per_player_value(f) || !save_value_at(progress, f.hash)) continue;
    if (f.hash == life || f.hash == mp) {
      // Full: as much as this save's (shared) MaxLife / MPMax; nothing when that cannot be read.
      const auto most = save_value_at(progress, f.hash == life ? max_life : mp_max, kSaveTypeInt);
      if (most) out.push_back({f.hash, get_u32(progress, most->offset)});
    } else if (f.kind == proto::FlagKind::uint32 && f.name.ends_with(".BottleItem")) {
      out.push_back({f.hash, empty});
    } else {
      out.push_back({f.hash, 0});  // rupees and every ammo count
    }
  }
  return out;
}

std::string own_values_text(std::span<const OwnValue> own) {
  std::string out(kOwnValuesHead);
  out += '\n';
  for (const auto& v : own) {
    char buf[16];
    auto r = std::to_chars(buf, buf + sizeof buf, v.hash, 16);
    out += "0x";
    out.append(buf, r.ptr);
    out += ' ';
    r = std::to_chars(buf, buf + sizeof buf, v.value);
    out.append(buf, r.ptr);
    out += '\n';
  }
  return out;
}

std::optional<std::vector<OwnValue>> parse_own_values(std::string_view text) {
  std::vector<OwnValue> out;
  bool head = false;
  while (!text.empty()) {
    const auto nl = text.find('\n');
    std::string_view line = text.substr(0, nl);
    text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty()) continue;
    if (!head) {
      if (line != kOwnValuesHead) return std::nullopt;
      head = true;
      continue;
    }
    const auto sp = line.find(' ');
    if (sp == std::string_view::npos || sp < 3 || line.substr(0, 2) != "0x" || sp + 1 >= line.size()) {
      return std::nullopt;
    }
    const std::string_view h = line.substr(2, sp - 2), v = line.substr(sp + 1);
    OwnValue o{};
    const auto rh = std::from_chars(h.data(), h.data() + h.size(), o.hash, 16);
    const auto rv = std::from_chars(v.data(), v.data() + v.size(), o.value);
    if (rh.ec != std::errc{} || rh.ptr != h.data() + h.size() || rv.ec != std::errc{} ||
        rv.ptr != v.data() + v.size()) {
      return std::nullopt;
    }
    out.push_back(o);
  }
  if (!head) return std::nullopt;
  return out;
}

bool keep_own_values(const fs::path& coop_dir) {
  if (!is_coop_dir(coop_dir)) return false;
  const auto slot = newest_slot(coop_dir);
  if (!slot) return false;
  const auto bytes = read_file(*slot / std::string(kSlotFiles[0]));
  if (!bytes) return false;
  const auto own = read_own_values(*bytes);
  if (own.empty()) return false;  // (not a save this build can read: what was kept stays)
  const std::string text = own_values_text(own);
  return write_file(coop_dir / std::string(kOwnValuesFile),
                    std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

std::optional<std::vector<OwnValue>> kept_own_values(const fs::path& coop_dir) {
  const auto bytes = read_file(coop_dir / std::string(kOwnValuesFile));
  if (!bytes) return std::nullopt;
  return parse_own_values(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
}

}  // namespace coop::host::save
