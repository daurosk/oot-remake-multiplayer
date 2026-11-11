// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop/host/notify.hpp"

#include <array>
#include <cctype>
#include <utility>

#include "coop/host/game_items.hpp"

namespace coop::host {
namespace {

using Pair = std::pair<std::string_view, std::string_view>;

// The names a player knows, for the ids whose parts do not spell them.
constexpr std::array kItems = std::to_array<Pair>({
    {"Weapon_Range_SlingShot", "Fairy Slingshot"},
    {"Pouch_Range_BowAndArrow", "Fairy Bow"},
    {"Pouch_Range_BowAndArrow_Fire", "Fire Arrows"},
    {"Pouch_Range_BowAndArrow_Ice", "Ice Arrows"},
    {"Pouch_Range_BowAndArrow_Light", "Light Arrows"},
    {"Item_Ocarina_Fairy", "Fairy Ocarina"},
    {"Item_Ocarina_Time", "Ocarina of Time"},
    {"Item_Bomb", "Bombs"},
    {"Item_DekuNut", "Deku Nuts"},
    {"Item_DekuSeed", "Deku Seeds"},
    {"Item_DekuStick", "Deku Sticks"},
    {"Item_Hammer", "Megaton Hammer"},
    {"Item_NayruLove", "Nayru's Love"},
    {"Item_DinFire", "Din's Fire"},
    {"Item_FaroreWind", "Farore's Wind"},
    {"Item_Scale_Silver", "Silver Scale"},
    {"Item_Scale_Gold", "Golden Scale"},
    {"Item_Stone_Forest", "Kokiri's Emerald"},
    {"Item_Stone_Fire", "Goron's Ruby"},
    {"Item_Stone_Water", "Zora's Sapphire"},
    {"Item_Medal_Forest", "Forest Medallion"},
    {"Item_Medal_Fire", "Fire Medallion"},
    {"Item_Medal_Water", "Water Medallion"},
    {"Item_Medal_Shadow", "Shadow Medallion"},
    {"Item_Medal_Spirit", "Spirit Medallion"},
    {"Item_Medal_Light", "Light Medallion"},
    {"Item_Token_Gerudo", "Gerudo Membership Card"},
    {"Weapon_Sword_Kokiri", "Kokiri Sword"},
    {"Weapon_Sword_Master", "Master Sword"},
    {"Weapon_Sword_Giant", "Giant's Knife"},
    {"Weapon_Sword_Biggoron", "Biggoron's Sword"},
    {"Weapon_Shield_Deku", "Deku Shield"},
    {"Weapon_Shield_Hylian", "Hylian Shield"},
    {"Weapon_Shield_Mirror", "Mirror Shield"},
    {"Armor_Adult_HoverBoot", "Hover Boots"},
    {"Armor_Adult_HeavyBoot", "Iron Boots"},
    {"Armor_Adult_Goron", "Goron Tunic"},
    {"Armor_Adult_Zora", "Zora Tunic"},
    {"Armor_Adult_Gauntlet_Silver", "Silver Gauntlets"},
    {"Armor_Adult_Gauntlet_Gold", "Golden Gauntlets"},
});

constexpr std::array kSongs = std::to_array<Pair>({
    {"ZeldaLullaby", "Zelda's Lullaby"},   {"EponaSong", "Epona's Song"},
    {"SariaSong", "Saria's Song"},          {"SunSong", "Sun's Song"},
    {"TimeSong", "Song of Time"},           {"StormSong", "Song of Storms"},
    {"ScarecrowSong", "Scarecrow's Song"},  {"ForestMinuet", "Minuet of Forest"},
    {"FireBolero", "Bolero of Fire"},       {"WaterSerenade", "Serenade of Water"},
    {"ShadowNocturne", "Nocturne of Shadow"}, {"SpiritRequiem", "Requiem of Spirit"},
    {"LightPrelude", "Prelude of Light"},
});

constexpr std::array kDungeons = std::to_array<Pair>({
    {"InsideTheDekuTree", "Deku Tree"},
    {"DodongoCavern", "Dodongo's Cavern"},
    {"InsideJabuJabuBelly", "Jabu-Jabu's Belly"},
    {"BottomOfTheWell", "Bottom of the Well"},
    {"InsideGanonCastle", "Ganon's Castle"},
});

template <std::size_t N>
std::optional<std::string_view> Find(const std::array<Pair, N>& table, std::string_view id) {
  for (const auto& [key, name] : table) {
    if (key == id) return name;
  }
  return std::nullopt;
}

// "Armor_Child_Mask_Keaton" -> "Mask Keaton"; "PocketEgg" -> "Pocket Egg".
std::string Spell(std::string_view id) {
  for (const std::string_view prefix : {"Item_", "Weapon_", "Armor_Child_", "Armor_Adult_", "Pouch_", "Range_"}) {
    if (id.starts_with(prefix)) id.remove_prefix(prefix.size());
  }
  std::string out;
  for (std::size_t i = 0; i < id.size(); ++i) {
    const char c = id[i];
    if (c == '_') {
      if (!out.empty() && out.back() != ' ') out.push_back(' ');
      continue;
    }
    if (std::isupper(static_cast<unsigned char>(c)) && i > 0 && id[i - 1] != '_' &&
        std::islower(static_cast<unsigned char>(id[i - 1]))) {
      out.push_back(' ');
    }
    out.push_back(c);
  }
  return out;
}

std::string_view After(std::string_view name, std::string_view prefix) {
  return name.starts_with(prefix) ? name.substr(prefix.size()) : std::string_view();
}

std::int32_t AsInt(const protocol::FlagEntry& e) { return static_cast<std::int32_t>(e.value); }

}  // namespace

std::string ItemName(std::string_view id) {
  if (const auto name = Find(kItems, id)) return std::string(*name);
  return Spell(id);
}

std::string SongName(std::string_view id) {
  if (const auto name = Find(kSongs, id)) return std::string(*name);
  return Spell(id);
}

std::string DungeonName(std::string_view id) {
  if (const auto name = Find(kDungeons, id)) return std::string(*name);
  return Spell(id);
}

std::optional<Notifier::Notice> Notifier::Explain(const FlagWatcher::Received& r) const {
  const ItemFlag* item = r.key.kind == protocol::FlagKeyKind::named && r.key.index == 0
                             ? find_item(r.key.hash)
                             : nullptr;
  if (item == nullptr) {
    if (extra_ && r.after.value != 0) {
      if (auto words = extra_(r.key, r.after)) return Notice{"other", std::move(*words), ""};
    }
    return std::nullopt;
  }
  const std::string_view name = item->name;
  const bool set = r.after.kind == protocol::FlagKind::boolean && r.before.value == 0 && r.after.value != 0;
  const bool more = r.after.kind == protocol::FlagKind::int32 && r.before.kind == protocol::FlagKind::int32 &&
                    AsInt(r.after) > AsInt(r.before);
  if (const auto id = After(name, "IsGet.Item_Melody_"); !id.empty()) {
    if (set) return Notice{"song", "learned " + SongName(id), ""};
    return std::nullopt;
  }
  if (const auto id = After(name, "IsGet."); !id.empty()) {
    if (!set || id == "Item_Token_Gold") return std::nullopt;  // tokens: by their count
    return Notice{"item", "got the " + ItemName(id), " - you got one too!"};
  }
  if (const auto rest = After(name, "DungeonInfo."); !rest.empty()) {
    const auto dot = rest.find('.');
    if (dot == std::string_view::npos) return std::nullopt;
    const std::string dungeon = DungeonName(rest.substr(0, dot));
    const std::string_view field = rest.substr(dot + 1);
    if (field == "SmallKeyNum") {
      if (!more) return std::nullopt;
      return Notice{"key", "found a Small Key (" + dungeon + ")",
                    " - the group has " + std::to_string(AsInt(r.after))};
    }
    if (!set) return std::nullopt;
    if (field == "IsGetMap") return Notice{"map", "got the map (" + dungeon + ")", ""};
    if (field == "IsGetCompass") return Notice{"map", "got the compass (" + dungeon + ")", ""};
    if (field == "IsGetBossKey") return Notice{"key", "got the Boss Key (" + dungeon + ")", ""};
    return std::nullopt;
  }
  if (name == "PlayerStatus.MaxLife" && more) {
    return Notice{"heart", "got a Heart Container", " - your max health went up"};
  }
  if (name == "PouchData.Item_Token_Gold.StockNum" && more) {
    return Notice{"token", "got a Gold Skulltula token (" + std::to_string(AsInt(r.after)) + ")", ""};
  }
  return std::nullopt;
}

std::string Notifier::Who(std::uint8_t player) const {
  if (player == 0) return "Someone";
  std::string n = name_ ? name_(player) : std::string();
  return n.empty() ? "Player " + std::to_string(player) : n;
}

void Notifier::Say(std::string line, std::uint64_t now_ms, std::vector<std::string>& out) {
  while (!said_ms_.empty() && now_ms - said_ms_.front() >= kWindowMs) said_ms_.pop_front();
  if (said_ms_.size() >= kMaxLines) {
    ++held_back_;
    return;
  }
  said_ms_.push_back(now_ms);
  out.push_back(std::move(line));
}

std::vector<std::string> Notifier::Take(const std::vector<FlagWatcher::Received>& received, std::uint8_t self,
                                        std::uint64_t now_ms) {
  std::vector<std::string> out;
  // A join or a host's save load: one line each, by kind.
  std::map<std::string, int> join;
  std::map<std::string, int> reset;
  std::uint8_t reset_by = 0;
  // Live: per player and kind, in the order they came.
  std::vector<std::pair<std::pair<std::uint8_t, std::string>, std::vector<Notice>>> live;
  for (const auto& r : received) {
    if (r.kind == FlagWatcher::ReceivedKind::live && r.from != 0 && r.from == self) continue;
    const auto notice = Explain(r);
    if (!notice) continue;
    if (r.kind == FlagWatcher::ReceivedKind::join) {
      ++join[notice->kind];
    } else if (r.kind == FlagWatcher::ReceivedKind::reset) {
      ++reset[notice->kind];
      reset_by = r.from;
    } else {
      const auto key = std::make_pair(r.from, notice->kind);
      auto it = live.begin();
      while (it != live.end() && it->first != key) ++it;
      if (it == live.end()) {
        live.push_back({key, {}});
        it = live.end() - 1;
      }
      it->second.push_back(*notice);
    }
  }
  const auto summary = [](const std::map<std::string, int>& counts) {
    static constexpr std::array<Pair, 7> kWords{{{"item", "items"},
                                                 {"song", "songs"},
                                                 {"key", "keys"},
                                                 {"map", "maps and compasses"},
                                                 {"heart", "heart containers"},
                                                 {"token", "tokens"},
                                                 {"other", "other things"}}};
    std::string text;
    for (const auto& [kind, words] : kWords) {
      const auto it = counts.find(std::string(kind));
      if (it == counts.end()) continue;
      if (!text.empty()) text += ", ";
      text += std::to_string(it->second) + " " + std::string(words);
    }
    return text;
  };
  if (!join.empty()) out.push_back("Synced with the group: " + summary(join));
  if (!reset.empty()) {
    out.push_back(Who(reset_by) + " loaded a save - the group follows it (" + summary(reset) + " changed)");
  }
  for (const auto& [key, notices] : live) {
    std::string line = Who(key.first) + " ";
    if (notices.size() == 1) {
      line += notices[0].what + notices[0].you;
    } else if (key.second == "item") {
      // "got the Bombs, the Deku Nuts - you got them too!"
      for (std::size_t i = 0; i < notices.size(); ++i) {
        line += (i == 0 ? notices[i].what : ", " + notices[i].what.substr(4));  // drop "got "
      }
      line += " - you got them too!";
    } else {
      for (std::size_t i = 0; i < notices.size(); ++i) {
        line += (i == 0 ? "" : ", ") + notices[i].what;
      }
      line += notices.back().you;
    }
    Say(std::move(line), now_ms, out);
  }
  // Lines held back: one line once the window has room.
  while (!said_ms_.empty() && now_ms - said_ms_.front() >= kWindowMs) said_ms_.pop_front();
  if (held_back_ != 0 && live.empty() && said_ms_.size() < kMaxLines) {
    said_ms_.push_back(now_ms);
    out.push_back("...and " + std::to_string(held_back_) + " more from the group");
    held_back_ = 0;
  }
  return out;
}

}  // namespace coop::host
