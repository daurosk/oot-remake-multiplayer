// Tests for the emulator hook's settings parser (lichii/hook/coop_settings.cpp).
// That file has no emulator code, so it is built straight into coop_tests.
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "coop_settings.h"

using CoopHook::ApplyEnvironment;
using CoopHook::ApplyIni;
using CoopHook::LoadSettings;
using CoopHook::Settings;

namespace {

using Env = std::map<std::string, std::string>;

void apply_env(const Env& env, Settings& s, std::vector<std::string>& errors) {
  ApplyEnvironment(
      [&env](const char* name) -> std::optional<std::string> {
        const auto it = env.find(name);
        if (it == env.end()) return std::nullopt;
        return it->second;
      },
      s, errors);
}

bool process_env_set(const char* name) {
#ifdef _MSC_VER
  char* value = nullptr;
  std::size_t size = 0;
  const bool set = _dupenv_s(&value, &size, name) == 0 && value != nullptr;
  std::free(value);
  return set;
#else
  return std::getenv(name) != nullptr;
#endif
}

bool any_coop_env() {
  return process_env_set("LICHII_COOP_SERVER") || process_env_set("LICHII_COOP_PORT") ||
         process_env_set("LICHII_COOP_NAME");
}

std::filesystem::path write_temp(const std::string& name, const std::string& text) {
  const auto path = std::filesystem::temp_directory_path() / name;
  std::ofstream(path, std::ios::binary) << text;
  return path;
}

}  // namespace

TEST_CASE("settings: defaults are off") {
  Settings s;
  CHECK_FALSE(s.enabled());
  CHECK(s.port == 27510);
  CHECK(s.name == "Player");
}

TEST_CASE("settings: ini values") {
  Settings s;
  std::vector<std::string> errors;
  ApplyIni("# comment\n; other comment\n[coop]\n\n  server = 192.168.1.20  \r\n"
           "port=27599\nname = Saria\n",
           s, errors);
  CHECK(errors.empty());
  CHECK(s.enabled());
  CHECK(s.server == "192.168.1.20");
  CHECK(s.port == 27599);
  CHECK(s.name == "Saria");
}

TEST_CASE("settings: comments after a value, as in the docs' example") {
  Settings s;
  std::vector<std::string> errors;
  ApplyIni("server = 192.168.1.20      # IP or host name; host:port also works\n"
           "port = 27510\t; optional, the default\n"
           "name = Saria               # 1 to 16 characters, different for each player\n",
           s, errors);
  CHECK(errors.empty());
  CHECK(s.enabled());
  CHECK(s.server == "192.168.1.20");
  CHECK(s.port == 27510);
  CHECK(s.name == "Saria");

  // A '#' or ';' with no space before it is part of the value.
  ApplyIni("name = Link#2;b", s, errors);
  CHECK(errors.empty());
  CHECK(s.name == "Link#2;b");
}

TEST_CASE("settings: host:port in server, last line wins") {
  Settings s;
  std::vector<std::string> errors;
  ApplyIni("server = coop.example-host.net:4000", s, errors);
  CHECK(errors.empty());
  CHECK(s.server == "coop.example-host.net");
  CHECK(s.port == 4000);

  ApplyIni("port = 5000\nserver = other_host", s, errors);
  CHECK(errors.empty());
  CHECK(s.server == "other_host");
  CHECK(s.port == 5000);
}

TEST_CASE("settings: names count code points, not bytes") {
  Settings s;
  std::vector<std::string> errors;
  // 16 two-byte characters (32 bytes) is still 16 characters.
  std::string name;
  for (int i = 0; i < 16; ++i) name += "\xc3\xa9";
  ApplyIni("name = " + name, s, errors);
  CHECK(errors.empty());
  CHECK(s.name == name);

  ApplyIni("name = " + name + "x", s, errors);
  REQUIRE(errors.size() == 1);
  CHECK(s.name == name);  // a bad value leaves the old one
}

TEST_CASE("settings: each bad ini line is an error with its line number") {
  struct Case {
    const char* text;
    const char* message;
  };
  const Case cases[] = {
      {"server = a b", "bad server address"},
      {"server = bad/host", "bad server address"},
      {"server = host:", "bad port in server"},
      {"server = host:0", "bad port in server"},
      {"server = host:65536", "bad port in server"},
      {"server = host:12x", "bad port in server"},
      {"server =", "bad server address"},
      {"port = 0", "port must be 1 to 65535"},
      {"port = 70000", "port must be 1 to 65535"},
      {"port = -1", "port must be 1 to 65535"},
      {"port = 123456", "port must be 1 to 65535"},
      {"port =", "port must be 1 to 65535"},
      {"name =", "name must be 1 to 16 characters"},
      {"name = 12345678901234567", "name must be 1 to 16 characters"},
      {"name = tab\there", "name must be 1 to 16 characters"},
      {"nmae = Saria", "unknown key 'nmae'"},
      {"just some words", "expected key = value"},
  };
  for (const auto& c : cases) {
    CAPTURE(c.text);
    Settings s;
    std::vector<std::string> errors;
    ApplyIni(std::string("\n# first line is blank\n") + c.text + "\n", s, errors);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find("coop.ini line 3") == 0);
    CHECK(errors[0].find(c.message) != std::string::npos);
  }
}

TEST_CASE("settings: port bounds") {
  Settings s;
  std::vector<std::string> errors;
  ApplyIni("port = 1", s, errors);
  CHECK(s.port == 1);
  ApplyIni("port = 65535", s, errors);
  CHECK(s.port == 65535);
  CHECK(errors.empty());
}

TEST_CASE("settings: environment wins over the ini") {
  Settings s;
  std::vector<std::string> errors;
  ApplyIni("server = 10.0.0.1\nport = 1000\nname = Saria", s, errors);
  apply_env({{"LICHII_COOP_SERVER", " 10.0.0.2 "}, {"LICHII_COOP_NAME", "Mido"}}, s, errors);
  CHECK(errors.empty());
  CHECK(s.server == "10.0.0.2");
  CHECK(s.port == 1000);  // not set in the environment: the ini's stays
  CHECK(s.name == "Mido");
}

TEST_CASE("settings: LICHII_COOP_PORT wins over a port inside LICHII_COOP_SERVER") {
  Settings s;
  std::vector<std::string> errors;
  apply_env({{"LICHII_COOP_SERVER", "host:2000"}, {"LICHII_COOP_PORT", "3000"}}, s, errors);
  CHECK(errors.empty());
  CHECK(s.server == "host");
  CHECK(s.port == 3000);
}

TEST_CASE("settings: a bad environment value is an error naming the variable") {
  Settings s;
  std::vector<std::string> errors;
  apply_env({{"LICHII_COOP_PORT", "lots"}}, s, errors);
  REQUIRE(errors.size() == 1);
  CHECK(errors[0].find("LICHII_COOP_PORT") == 0);
  CHECK(s.port == 27510);
}

TEST_CASE("settings: LoadSettings reads the file, a missing one is fine") {
  if (any_coop_env()) {
    MESSAGE("LICHII_COOP_* set in this environment; skipped");
    return;
  }
  std::vector<std::string> errors;
  const auto missing = std::filesystem::temp_directory_path() / "coop_tests_missing_dir" / "coop.ini";
  Settings off = LoadSettings(missing, errors);
  CHECK(errors.empty());
  CHECK_FALSE(off.enabled());

  const auto good = write_temp("coop_tests_good.ini", "server = 127.0.0.1\nname = Saria\n");
  Settings on = LoadSettings(good, errors);
  std::filesystem::remove(good);
  CHECK(errors.empty());
  CHECK(on.enabled());
  CHECK(on.server == "127.0.0.1");
  CHECK(on.name == "Saria");
}

TEST_CASE("settings: any error turns co-op off") {
  if (any_coop_env()) {
    MESSAGE("LICHII_COOP_* set in this environment; skipped");
    return;
  }
  std::vector<std::string> errors;
  // The server line is fine; the typo on the next line still turns it off.
  const auto bad = write_temp("coop_tests_bad.ini", "server = 127.0.0.1\nprot = 27510\n");
  Settings s = LoadSettings(bad, errors);
  std::filesystem::remove(bad);
  REQUIRE(errors.size() == 1);
  CHECK(errors[0].find("unknown key 'prot'") != std::string::npos);
  CHECK_FALSE(s.enabled());
}

TEST_CASE("settings: the join password from coop.ini and LICHII_COOP_PASSWORD") {
  Settings s;
  std::vector<std::string> errors;
  CHECK(s.password.empty());
  ApplyIni("server = 26.1.2.3\npassword = hunter2\n", s, errors);
  CHECK(errors.empty());
  CHECK(s.password == "hunter2");
  // A '#' with no space before it stays part of the password.
  ApplyIni("password = a#b  # the comment\n", s, errors);
  CHECK(errors.empty());
  CHECK(s.password == "a#b");
  apply_env({{"LICHII_COOP_PASSWORD", "fromenv"}}, s, errors);
  CHECK(errors.empty());
  CHECK(s.password == "fromenv");
  ApplyIni(std::string("password = ") + std::string(129, 'x') + "\n", s, errors);
  CHECK(errors.size() == 1);
  CHECK(s.password == "fromenv");
}

TEST_CASE("settings: color, a name or #RRGGBB") {
  Settings s;
  std::vector<std::string> errors;
  CHECK_FALSE(s.color);
  ApplyIni("color = #12AB7F\n", s, errors);
  CHECK(errors.empty());
  CHECK(s.color == 0x12AB7Fu);
  ApplyIni("color = Purple  # mine\n", s, errors);
  CHECK(errors.empty());
  CHECK(s.color == 0x9030E0u);
  // Empty: none chosen.
  ApplyIni("color =\n", s, errors);
  CHECK(errors.empty());
  CHECK_FALSE(s.color);
  // The environment wins; a bad one is an error naming the variable.
  apply_env({{"LICHII_COOP_COLOR", "green"}}, s, errors);
  CHECK(errors.empty());
  CHECK(s.color == 0x20C040u);
  apply_env({{"LICHII_COOP_COLOR", "blurple"}}, s, errors);
  REQUIRE(errors.size() == 1);
  CHECK(errors[0].find("LICHII_COOP_COLOR") != std::string::npos);
  CHECK(s.color == 0x20C040u);  // kept
}
