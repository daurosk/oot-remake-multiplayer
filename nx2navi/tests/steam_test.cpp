// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for co-op over Steam, the parts that need neither Steam nor a game:
// what the two games of a room say around the session's traffic
// (dll/steam_model.hpp) and the Multiplayer menu's Steam pages (dll/steam_menu.hpp).
#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "menu_model.hpp"
#include "mp_saves.hpp"
#include "overlay_model.hpp"
#include "steam_menu.hpp"
#include "steam_model.hpp"

namespace Menu = CoopNx2navi::Menu;
namespace Model = CoopNx2navi::SteamModel;
namespace Pages = CoopNx2navi::Menu::SteamPages;

namespace {

// Two lines of at most 46 characters: what the game's caption box shows whole.
bool Fits(const std::string& text) {
  std::size_t lines = 1, start = 0;
  for (std::size_t i = 0; i <= text.size(); ++i) {
    if (i == text.size() || text[i] == '\n') {
      if (CoopNx2navi::Ui::Utf8Count(std::string_view(text).substr(start, i - start)) > 46) {
        return false;
      }
      start = i + 1;
      lines += i < text.size() ? 1 : 0;
    }
  }
  return lines <= 2;
}

std::vector<std::uint8_t> Bytes(std::size_t n, std::uint8_t seed) {
  std::vector<std::uint8_t> out(n);
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = static_cast<std::uint8_t>(seed + i * 7 + (i >> 8));
  }
  return out;
}

std::string Kinds(const std::vector<Menu::Row>& rows) {
  std::string out;
  for (const auto& r : rows) {
    out += r.kind == Menu::RowKind::action ? 'a' : r.kind == Menu::RowKind::separator ? '-' : 't';
  }
  return out;
}

Menu::SaveInfo Save(const std::string& folder) {
  Menu::SaveInfo info;
  info.folder = folder;
  info.name = folder;
  info.played = "2026-11-13 05:47";
  info.place = "HyruleField_Adult";
  return info;
}

// A game with Steam for mods, signed in as "Zelda", with one multiplayer save.
Menu::State Ready(Menu::Lang lang) {
  Menu::State s;
  s.lang = lang;
  s.name = "PC";
  s.saves_on = true;
  s.dim_steam = true;
  Menu::SetSaves(s, {Save("Coop")});
  Pages::Update(s, Menu::SteamAvail::ready, "Zelda", "", {}, {}, {});
  return s;
}

std::vector<Menu::SteamFriend> SixFriends() {
  return {{11, "Saria", true, true}, {12, "Mido", true, false},    {13, "Malon", true, false},
          {14, "Ruto", false, false}, {15, "Darunia", false, false}, {16, "Nabooru", false, false}};
}

}  // namespace

TEST_CASE("steam model: an ENet datagram goes whole in one Steam datagram, a long one in parts") {
  // What the session sends (1100 bytes at most) is one datagram: its kind and the bytes.
  const auto small = Bytes(1100, 3);
  auto wrapped = Model::Wrap(small, 7);
  REQUIRE(wrapped.size() == 1);
  CHECK(wrapped[0].size() == 1101);
  auto parsed = Model::Parse(wrapped[0]);
  REQUIRE(parsed);
  CHECK(parsed->kind == Model::Kind::data);
  CHECK(std::vector<std::uint8_t>(parsed->bytes.begin(), parsed->bytes.end()) == small);
  // The longest that fits, and the first that does not.
  CHECK(Model::Wrap(Bytes(Model::kSteamDatagram - 1, 1), 0).size() == 1);
  CHECK(Model::Wrap(Bytes(Model::kSteamDatagram, 1), 0).size() == 2);
  // ENet's largest datagram: four parts at most, each within Steam's limit, put together in any order.
  const auto large = Bytes(4096, 9);
  wrapped = Model::Wrap(large, 0x1234);
  REQUIRE(wrapped.size() == 4);
  Model::Joiner joiner;
  std::optional<std::vector<std::uint8_t>> whole;
  for (const std::size_t i : {2u, 0u, 3u, 0u, 1u}) {
    CHECK(wrapped[i].size() <= Model::kSteamDatagram);
    parsed = Model::Parse(wrapped[i]);
    REQUIRE(parsed);
    CHECK(parsed->kind == Model::Kind::part);
    CHECK(parsed->number == 0x1234);
    CHECK_FALSE(whole);
    whole = joiner.Add(*parsed);
  }
  REQUIRE(whole);
  CHECK(*whole == large);
  // Nothing for an empty datagram or one longer than four parts.
  CHECK(Model::Wrap({}, 0).empty());
  CHECK(Model::Wrap(Bytes(5000, 1), 0).empty());
}

TEST_CASE("steam model: parts of a lost datagram do not spoil the next one") {
  const auto first = Bytes(2000, 1), second = Bytes(2100, 2);
  const auto a = Model::Wrap(first, 1), b = Model::Wrap(second, 2);
  REQUIRE(a.size() == 2);
  REQUIRE(b.size() == 2);
  Model::Joiner joiner;
  CHECK_FALSE(joiner.Add(*Model::Parse(a[0])));  // its second part never comes
  CHECK_FALSE(joiner.Add(*Model::Parse(b[1])));
  const auto whole = joiner.Add(*Model::Parse(b[0]));
  REQUIRE(whole);
  CHECK(*whole == second);
  // The late part of the first one alone gives nothing.
  CHECK_FALSE(joiner.Add(*Model::Parse(a[1])));
}

TEST_CASE("steam model: hello, the secret, and bytes that are neither") {
  auto parsed = Model::Parse(Model::Hello());
  REQUIRE(parsed);
  CHECK(parsed->kind == Model::Kind::hello);
  CHECK(parsed->version == Model::kVersion);
  std::array<std::uint8_t, 16> random{};
  for (std::size_t i = 0; i < random.size(); ++i) {
    random[i] = static_cast<std::uint8_t>(i * 17 + 3);
  }
  const std::string secret = Model::SecretFrom(random);
  CHECK(secret.size() == Model::kSecretChars);
  CHECK(secret.substr(0, 6) == "031425");
  CHECK(Model::ValidSecret(secret));
  CHECK_FALSE(Model::ValidSecret("short"));
  CHECK_FALSE(Model::ValidSecret(std::string(32, 'G')));
  // A join password of the shared protocol: no blank, 128 characters at most.
  CHECK(Menu::ValidPassword(secret));
  parsed = Model::Parse(Model::Secret(secret));
  REQUIRE(parsed);
  CHECK(parsed->kind == Model::Kind::secret);
  CHECK(parsed->secret == secret);
  // Refused: nothing, one byte, an unknown kind, a secret of another shape, a part out of range.
  CHECK_FALSE(Model::Parse({}));
  CHECK_FALSE(Model::Parse(std::vector<std::uint8_t>{3}));
  CHECK_FALSE(Model::Parse(std::vector<std::uint8_t>{9, 1, 2}));
  CHECK_FALSE(Model::Parse(Model::Secret("not a secret")));
  CHECK_FALSE(Model::Parse(std::vector<std::uint8_t>{1, 1, 1}));
  CHECK_FALSE(Model::Parse(std::vector<std::uint8_t>{4, 0, 0, 5, 4, 1}));  // part 5 of 4
  CHECK_FALSE(Model::Parse(std::vector<std::uint8_t>{4, 0, 0, 0, 9, 1}));  // of 9 parts
  CHECK_FALSE(Model::Parse(std::vector<std::uint8_t>{4, 0, 0, 0, 2}));     // no bytes
  CHECK_FALSE(Model::Parse(Bytes(Model::kSteamDatagram + 1, 3)));
}

TEST_CASE("steam model: a Steam name as a player's name, and a room ID as typed") {
  CHECK(Model::PlayerName("Zelda") == "Zelda");
  CHECK(Model::PlayerName("  Link \t") == "Link");
  CHECK(Model::PlayerName("A name far longer than sixteen") == "A name far longe");
  CHECK(Model::PlayerName("H\xc3\xa9l\xc3\xa8ne") == "H\xc3\xa9l\xc3\xa8ne");
  CHECK(Model::PlayerName(std::string("a\x01" "b\x7f" "c")) == "abc");
  CHECK(Model::PlayerName("\xff\xfe") == "");
  CHECK(Model::PlayerName("") == "");
  CHECK(Menu::ValidName(Model::PlayerName("\xe3\x82\xbc\xe3\x83\xab\xe3\x83\x80 the 1st of her name")));

  CHECK(Model::RoomId("ABCD-2345") == "ABCD-2345");
  CHECK(Model::RoomId(" abcd 2345\r\n") == "ABCD-2345");
  CHECK(Model::RoomId("abcd2345") == "ABCD-2345");
  CHECK(Model::RoomId("ABCD-234") == "");
  CHECK(Model::RoomId("ABCD-23456") == "");
  CHECK(Model::RoomId("ABCD-2340") == "");  // no 0, 1, I or O in a room ID
  CHECK(Model::RoomId("192.168.1.20") == "");
  CHECK(Model::RoomId("") == "");
}

TEST_CASE("steam menu: the two entries say Open, or Unavailable and why") {
  Menu::State s;
  s.lang = Menu::Lang::fr;
  s.dim_steam = true;
  // A game without Steam for mods: as before, dimmed.
  auto rows = Menu::Rows(s);
  REQUIRE(rows.size() == Menu::kRows);
  CHECK(Kinds(rows) == "aaaaa-aa");
  CHECK(rows[0].label == "H\xc3\xa9" "berger sur Steam");
  CHECK(rows[0].value == "Indisponible");
  CHECK(rows[0].disabled);
  CHECK(rows[2].label == "Rejoindre via Steam");
  CHECK(rows[2].value == "Indisponible");
  CHECK(rows[2].disabled);
  CHECK(Menu::Activate(s, 0) == Menu::Action::none);
  CHECK(s.page == Menu::Page::main);
  // Each reason has its sentence, in both languages, on two lines of the caption box.
  for (const Menu::Lang lang : {Menu::Lang::fr, Menu::Lang::en}) {
    for (int a = 0; a <= static_cast<int>(Menu::SteamAvail::other); ++a) {
      Menu::State t;
      t.lang = lang;
      t.dim_steam = true;
      Pages::Update(t, static_cast<Menu::SteamAvail>(a), "Zelda", "", {}, {}, {});
      const bool ready = static_cast<Menu::SteamAvail>(a) == Menu::SteamAvail::ready;
      rows = Menu::Rows(t);
      for (const std::size_t row : {std::size_t{0}, std::size_t{2}}) {
        CHECK(rows[row].disabled == !ready);
        CHECK(rows[row].value == (ready ? (lang == Menu::Lang::fr ? "Ouvrir" : "Open")
                                        : (lang == Menu::Lang::fr ? "Indisponible" : "Unavailable")));
        CHECK_MESSAGE(Fits(rows[row].description), rows[row].description);
        CHECK_FALSE(rows[row].description.empty());
      }
      if (!ready) {
        CHECK(rows[0].description == rows[2].description);
        CHECK(rows[0].description == Pages::WhyUnavailable(lang, static_cast<Menu::SteamAvail>(a)));
      }
    }
    for (int f = 1; f <= static_cast<int>(Menu::SteamFail::other); ++f) {
      const std::string why = Pages::WhyFailed(lang, static_cast<Menu::SteamFail>(f));
      CHECK_FALSE(why.empty());
      CHECK_MESSAGE(Fits(why), why);
    }
    CHECK(Fits(Menu::T(lang, Menu::Text::steam_making)));
    CHECK(Fits(Menu::T(lang, Menu::Text::steam_entering)));
  }
  Menu::State t;
  t.lang = Menu::Lang::fr;
  Pages::Update(t, Menu::SteamAvail::no_client, "", "", {}, {}, {});
  CHECK(Menu::Rows(t)[0].description.find("Steam n'est pas lanc\xc3\xa9") == 0);
  Pages::Update(t, Menu::SteamAvail::no_library, "", "", {}, {}, {});
  CHECK(Menu::Rows(t)[0].description.find("steam_api64.dll manque") == 0);
  Pages::Update(t, Menu::SteamAvail::game_coop, "", "", {}, {}, {});
  CHECK(Menu::Rows(t)[0].description.find("Le co-op de NX2navi est activ\xc3\xa9") == 0);
  // Without the dimming (an older game's page): the rows can be reached and do nothing.
  t.dim_steam = false;
  CHECK_FALSE(Menu::Rows(t)[0].disabled);
  CHECK(Menu::Activate(t, 2) == Menu::Action::none);
}

TEST_CASE("steam menu: Host over Steam needs a save and nothing else, then shows the room") {
  Menu::State s = Ready(Menu::Lang::fr);
  CHECK(Menu::Rows(s)[0].value == "Ouvrir");
  CHECK(Menu::Activate(s, 0) == Menu::Action::steam_open);
  CHECK(s.page == Menu::Page::steam_host);
  CHECK(s.focus == 1);
  CHECK(Menu::Title(s) == "H\xc3\xa9" "berger sur Steam");
  auto rows = Menu::Rows(s);
  REQUIRE(rows.size() == Menu::kRows);
  CHECK(Kinds(rows) == "taaaa-aa");
  CHECK(rows[0].label == "Nom");
  CHECK(rows[0].value == "Zelda");  // the Steam name, not the PC's
  CHECK(rows[1].label == "ID du salon");
  CHECK(rows[1].value == "-");
  CHECK(rows[1].disabled);
  CHECK(rows[2].label == "Inviter un ami");
  CHECK(rows[2].disabled);
  CHECK(rows[3].label == "Joueurs");
  CHECK(rows[4].label == "Sauvegarde");
  CHECK(rows[4].value == "Choisir");
  CHECK(rows[6].label == "D\xc3\xa9marrer");
  CHECK(rows[6].value == "D\xc3\xa9marrer");
  CHECK(rows[7].label == "Retour");
  // No room yet: its rows do nothing.
  CHECK(Menu::Activate(s, 1) == Menu::Action::none);
  CHECK(Menu::Activate(s, 2) == Menu::Action::none);
  CHECK(Menu::Activate(s, 3) == Menu::Action::none);
  // Start without a save chosen: said, nothing started.
  CHECK(Menu::Activate(s, Menu::kRowGo) == Menu::Action::redraw);
  CHECK(s.phase == Menu::Phase::failed);
  CHECK(Menu::Rows(s)[6].description.find("Choisissez d'abord une sauvegarde") == 0);
  // The Save row leads to the saves; a save chosen there comes back to this page, on Start.
  CHECK(Menu::Activate(s, Menu::kRowExtra) == Menu::Action::redraw);
  CHECK(s.page == Menu::Page::saves);
  CHECK(Menu::Activate(s, 0) == Menu::Action::redraw);
  CHECK(s.page == Menu::Page::save);
  CHECK(Menu::Activate(s, 0) == Menu::Action::save_chosen);
  CHECK(s.page == Menu::Page::steam_host);
  CHECK(s.focus == 1 + Menu::kRowGo);
  CHECK(s.save == "Coop");
  CHECK(Menu::Rows(s)[4].value == "Coop");
  CHECK(Menu::Activate(s, Menu::kRowGo) == Menu::Action::start_steam_host);
  // The owner starts it; once the room is there the page shows it.
  s.phase = Menu::Phase::hosting;
  CHECK(Pages::Update(s, Menu::SteamAvail::ready, "Zelda", "ABCD-2345", {"Zelda", "Saria"}, SixFriends(), {}));
  CHECK_FALSE(Pages::Update(s, Menu::SteamAvail::ready, "Zelda", "ABCD-2345", {"Zelda", "Saria"}, SixFriends(), {}));
  rows = Menu::Rows(s);
  CHECK(rows[1].value == "ABCD-2345");
  CHECK_FALSE(rows[1].disabled);
  CHECK(rows[2].value == "6");
  CHECK_FALSE(rows[2].disabled);
  CHECK(rows[3].value == "2");
  CHECK(rows[3].description == "Zelda, Saria");
  CHECK(rows[6].value == "H\xc3\xa9" "bergement");
  // A press on the room ID copies it, and says so.
  CHECK(Menu::Activate(s, 1) == Menu::Action::copy_room);
  Pages::Copied(s, true);
  CHECK(Menu::Rows(s)[1].description.find("Copi\xc3\xa9") == 0);
  // Start again while the session runs: said, nothing more.
  CHECK(Menu::Activate(s, Menu::kRowGo) == Menu::Action::redraw);
  CHECK(s.message.find("Une session tourne d\xc3\xa9j\xc3\xa0") == 0);
  // Back to the first page, the cursor on the entry that led here.
  CHECK(Menu::Activate(s, Menu::kRowBack) == Menu::Action::redraw);
  CHECK(s.page == Menu::Page::main);
  CHECK(s.focus == 1);
}

TEST_CASE("steam menu: the friends to invite, four at a time") {
  Menu::State s = Ready(Menu::Lang::en);
  s.save = "Coop";
  s.page = Menu::Page::steam_host;
  s.phase = Menu::Phase::hosting;
  Pages::Update(s, Menu::SteamAvail::ready, "Zelda", "ABCD-2345", {"Zelda"}, SixFriends(), {});
  CHECK(Menu::Activate(s, 2) == Menu::Action::redraw);
  CHECK(s.page == Menu::Page::steam_friends);
  CHECK(Menu::Title(s) == "Invite a friend");
  auto rows = Menu::Rows(s);
  REQUIRE(rows.size() == Menu::kRows);
  CHECK(Kinds(rows) == "aaaaa-aa");
  CHECK(rows[0].label == "Saria");
  CHECK(rows[0].value == "In the game");
  CHECK(rows[1].value == "Online");
  CHECK(rows[3].label == "Ruto");
  CHECK(rows[3].value == "Offline");
  CHECK(rows[4].label == "More friends");
  CHECK(rows[4].value == "1-4 / 6");
  CHECK(rows[6].value == "ABCD-2345");
  // A friend chosen: the owner is told whom, and the page says it was sent.
  CHECK(Menu::Activate(s, 1) == Menu::Action::invite_friend);
  CHECK(s.steam.invite_id == 12);
  Pages::Invited(s, true);
  CHECK(Menu::Rows(s)[1].description == "Invitation sent to Mido.");
  Pages::Invited(s, false);
  CHECK(Menu::Rows(s)[1].description == "Steam did not take the invitation.");
  // The next friends, an empty row, and round again.
  CHECK(Menu::Activate(s, 4) == Menu::Action::redraw);
  rows = Menu::Rows(s);
  CHECK(rows[0].label == "Darunia");
  CHECK(rows[1].label == "Nabooru");
  CHECK(rows[2].label == "-");
  CHECK(rows[2].disabled);
  CHECK(rows[4].value == "5-6 / 6");
  CHECK(Menu::Activate(s, 2) == Menu::Action::none);
  CHECK(Menu::Activate(s, 0) == Menu::Action::invite_friend);
  CHECK(s.steam.invite_id == 15);
  CHECK(Menu::Activate(s, 4) == Menu::Action::redraw);
  CHECK(Menu::Rows(s)[0].label == "Saria");
  // The room ID can be copied from here too; the Save row's place is the list's own here.
  CHECK(Menu::Activate(s, Menu::kRowGo) == Menu::Action::copy_room);
  CHECK(Menu::Activate(s, Menu::kRowBack) == Menu::Action::redraw);
  CHECK(s.page == Menu::Page::steam_host);
  CHECK(s.focus == 3);
  // A list that got shorter while it showed its second part starts again at the first.
  s.page = Menu::Page::steam_friends;
  s.steam.friend_first = 4;
  Pages::Update(s, Menu::SteamAvail::ready, "Zelda", "ABCD-2345", {"Zelda"}, {{11, "Saria", true, true}}, {});
  CHECK(s.steam.friend_first == 0);
  CHECK(Menu::Rows(s)[4].disabled);
  // Captions fit for every friend's state and every page, in both languages.
  for (const Menu::Lang lang : {Menu::Lang::fr, Menu::Lang::en}) {
    for (const Menu::Page page : {Menu::Page::steam_host, Menu::Page::steam_join, Menu::Page::steam_friends,
                                  Menu::Page::steam_invites}) {
      for (const bool hosting : {false, true}) {
        for (const bool saves : {false, true}) {
          Menu::State t = Ready(lang);
          t.saves_on = saves;
          t.page = page;
          Pages::Update(t, Menu::SteamAvail::ready, "Zelda", hosting ? "ABCD-2345" : "",
                        hosting ? std::vector<std::string>{"A name of sixteen", "Another long one", "And a third name",
                                                           "The fourth is he", "Five", "Six", "Seven", "Eight"}
                                : std::vector<std::string>{},
                        hosting ? SixFriends() : std::vector<Menu::SteamFriend>{},
                        hosting ? std::vector<Menu::SteamInvite>{{"Saria", "ABCD-2345"}, {"", "WXYZ-6789"}}
                                : std::vector<Menu::SteamInvite>{});
          for (const auto& r : Menu::Rows(t)) {
            CHECK_MESSAGE(Fits(r.description), r.description);
          }
          REQUIRE(Menu::Rows(t).size() == Menu::kRows);
        }
      }
    }
  }
}

TEST_CASE("steam menu: Join over Steam takes a room ID, typed or pasted, or an invitation") {
  Menu::State s = Ready(Menu::Lang::fr);
  CHECK(Menu::Activate(s, 2) == Menu::Action::steam_open);
  CHECK(s.page == Menu::Page::steam_join);
  CHECK(Menu::Title(s) == "Rejoindre via Steam");
  auto rows = Menu::Rows(s);
  REQUIRE(rows.size() == Menu::kRows);
  CHECK(Kinds(rows) == "ttaaa-aa");
  CHECK(rows[0].value == "Zelda");
  CHECK(rows[1].label == "ID du salon");
  CHECK(rows[1].value == "-");
  CHECK(rows[2].label == "Coller");
  CHECK(rows[3].label == "Invitations");
  CHECK(rows[3].value == "aucune");
  CHECK(rows[4].value == "Celle de l'h\xc3\xb4te");
  CHECK(rows[6].label == "Rejoindre");
  // Join without a room ID: said.
  CHECK(Menu::Activate(s, Menu::kRowGo) == Menu::Action::redraw);
  CHECK(s.phase == Menu::Phase::failed);
  CHECK(Menu::Rows(s)[6].description.find("Tapez ou collez d'abord") == 0);
  // Typed with a gamepad or our own typing: any case, with or without the dash.
  CHECK(Menu::Activate(s, 1) == Menu::Action::begin_edit);
  CHECK(s.editing == Menu::Field::room);
  for (const char c : std::string("abcd2345")) {
    Menu::EditText(s, static_cast<std::uint32_t>(c));
  }
  CHECK(Menu::Rows(s)[1].value == "abcd2345_");
  CHECK(Menu::CommitEdit(s));
  CHECK(s.steam.room == "ABCD-2345");
  CHECK(Menu::Rows(s)[1].value == "ABCD-2345");
  // Something that is no room ID is refused with the rule, and the old one stays.
  Menu::BeginEdit(s, Menu::Field::room);
  s.edit = "hello";
  CHECK_FALSE(Menu::CommitEdit(s));
  CHECK(s.steam.room == "ABCD-2345");
  CHECK(s.message.find("Ce n'est pas un ID de salon") == 0);
  CHECK(Fits(s.message));
  // The game's own typing of the row (a keyboard) ends the same way.
  s.game_typing = true;
  s.message.clear();
  CHECK(Menu::Rows(s)[1].text_max == 16);
  CHECK(Menu::TakeText(s, 1, "wxyz-6789"));
  CHECK(s.steam.room == "WXYZ-6789");
  // Paste: the owner hands the clipboard's text over.
  CHECK(Menu::Activate(s, 2) == Menu::Action::paste_room);
  Pages::Pasted(s, "an address, 192.168.1.20");
  CHECK(s.steam.room == "WXYZ-6789");
  CHECK(Menu::Rows(s)[2].description.find("Le presse-papiers ne contient pas") == 0);
  Pages::Pasted(s, "  efgh-2345\r\n");
  CHECK(s.steam.room == "EFGH-2345");
  CHECK(Menu::Rows(s)[2].description.find("Coll\xc3\xa9") == 0);
  // Join: the owner is told which room.
  CHECK(Menu::Activate(s, Menu::kRowGo) == Menu::Action::start_steam_join);
  CHECK(s.steam.join_room == "EFGH-2345");
  // The name typed here replaces the Steam name; typed empty, the Steam name is back.
  CHECK(Menu::TakeText(s, 0, "Sheik"));
  CHECK(Pages::PlayerName(s) == "Sheik");
  CHECK(s.name == "PC");  // the local network's name is another
  CHECK(Menu::TakeText(s, 0, ""));
  CHECK(Pages::PlayerName(s) == "Zelda");
  Menu::State no_name = Ready(Menu::Lang::en);
  Pages::Update(no_name, Menu::SteamAvail::ready, "", "", {}, {}, {});
  CHECK(Pages::PlayerName(no_name) == "PC");  // Steam told no name: the other pages' one
}

TEST_CASE("steam menu: invitations received are listed, and choosing one joins its room") {
  Menu::State s = Ready(Menu::Lang::en);
  s.page = Menu::Page::steam_join;
  CHECK(Menu::Activate(s, 3) == Menu::Action::none);  // none yet
  CHECK(Pages::Update(s, Menu::SteamAvail::ready, "Zelda", "", {}, {},
                      {{"Saria", "ABCD-2345"}, {"", "WXYZ-6789"}}));
  CHECK(Menu::Rows(s)[3].value == "2");
  CHECK(Menu::Activate(s, 3) == Menu::Action::redraw);
  CHECK(s.page == Menu::Page::steam_invites);
  CHECK(Menu::Title(s) == "Invitations");
  auto rows = Menu::Rows(s);
  REQUIRE(rows.size() == Menu::kRows);
  CHECK(Kinds(rows) == "aaaaa-aa");
  CHECK(rows[0].label == "Saria");
  CHECK(rows[0].value == "Join");
  CHECK(rows[1].label == "A friend");  // Steam told no name
  CHECK(rows[2].label == "-");
  CHECK(rows[6].value == "2");
  CHECK(Menu::Activate(s, 2) == Menu::Action::none);
  CHECK(Menu::Activate(s, 1) == Menu::Action::start_steam_join);
  CHECK(s.steam.join_room == "WXYZ-6789");
  CHECK(s.steam.room == "WXYZ-6789");
  CHECK(s.page == Menu::Page::steam_join);
  // While the join is on its way a second choice does nothing.
  s.phase = Menu::Phase::connecting;
  s.page = Menu::Page::steam_invites;
  CHECK(Menu::Activate(s, 0) == Menu::Action::none);
  CHECK(Menu::Activate(s, Menu::kRowBack) == Menu::Action::redraw);
  CHECK(s.page == Menu::Page::steam_join);
  CHECK(Menu::Activate(s, Menu::kRowGo) == Menu::Action::none);  // on its way
  CHECK(Menu::Activate(s, 2) == Menu::Action::none);             // no paste meanwhile
}

TEST_CASE("steam menu: Steam gone while a Steam page shows is said at Start, not before") {
  Menu::State s = Ready(Menu::Lang::en);
  s.save = "Coop";
  s.page = Menu::Page::steam_host;
  Pages::Update(s, Menu::SteamAvail::no_client, "Zelda", "", {}, {}, {});
  CHECK(Menu::Activate(s, Menu::kRowGo) == Menu::Action::redraw);
  CHECK(s.phase == Menu::Phase::failed);
  CHECK(s.message == Pages::WhyUnavailable(Menu::Lang::en, Menu::SteamAvail::no_client));
  // The page closed: the next opening starts at the first page, with nothing left of this one.
  Menu::Closed(s);
  CHECK(s.page == Menu::Page::main);
  CHECK(s.steam.note.empty());
  CHECK(s.phase == Menu::Phase::idle);
}
