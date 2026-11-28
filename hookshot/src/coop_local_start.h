// SPDX-License-Identifier: GPL-3.0-or-later
// Local co-op started from the game (the Local co-op page's Start; owner 2026-11-24: two to four games on one PC, one
// click, not split screen). Game 1, the game the player is in, hosts a private game for this PC only (CoopPause's
// LocalHostStep, the listener on 127.0.0.1) and runs the kit's launcher, scripts\local-coop-start.ps1, which starts
// the games of players 2 to N that do not run yet into it: the same folders, files, controllers and frame cap as
// Start Squadala's local co-op start (scripts\local-coop.ps1, one home for both). Each extra game is its own player:
// its own slot, colour (by player number until they pick one) and name, from its own folder
// <game>\local-players\playerN.
//
// The pure parts are here (tests/hookshot/local_start_test.cpp); the process and the port are coop_local_start_os.cpp.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CoopLocalStart {

// The launcher, in the kit's scripts folder.
inline constexpr const char* kScript = "local-coop-start.ps1";
// What it wrote (beside the extra games' folders): <game>\local-players\start-result.txt.
inline constexpr const char* kResultFile = "start-result.txt";
// The first port tried for the session (as the launcher's Get-KitLocalPort: a Host.bat server may hold 27510).
inline constexpr std::uint16_t kFirstPort = 27560;
// How long the page waits for the launcher's answer before it says it did not answer.
inline constexpr std::uint64_t kAnswerMs = 60000;
// Row 325: local co-op's Field of view, as the kit gives the games it starts (local-coop.ps1 $script:KitLocalFov through
// HOS_FOV_DEFAULT; Hookshot patch 0048). The game whose page starts the others already runs: it asks the game for it
// (HosCoopApi::setting_set's kFovDefaultKey, for this run only, where its player kept the row's own 100%).
inline constexpr int kLocalFov = 150;
inline constexpr const char* kFovDefaultKey = "field_of_view_default";

// This game's player number among this PC's games: <game>\local-players\playerN gives N (2 to 9), else 1 (game 1, or
// a game that is not one of them).
int PlayerOf(const std::filesystem::path& game_dir);

// The session an extra game was started into: its coop.ini's server 127.0.0.1 (or localhost), port and password.
struct Session {
    std::uint16_t port = 0;
    std::string password;
    bool operator==(const Session&) const = default;
};
std::optional<Session> SessionOf(std::string_view coop_ini);

// The highest player number <game>\local-players\instances.txt lists ("2=<folder>" lines), so the local session's
// games, game 1 included: 0 when it lists none.
int GamesListed(std::string_view instances_txt);

// The launcher's answer (kResultFile): the players it started, the ones that ran already and the ones that did not
// start, with its one problem line.
struct Result {
    std::vector<int> started, running, failed;
    std::string problem;
};
std::optional<Result> ParseResult(std::string_view text);
// The page's line for it (at most CoopPauseTab::kListLineChars: a native list line).
std::string ResultNote(const Result& r);

// A join password for a session of this PC only: 12 letters and digits from `seed`.
std::string NewPassword(std::uint64_t seed);

// The command line that runs the launcher (UTF-8; the OS part widens it): powershell with the script and its
// arguments, each quoted.
std::string CommandLine(const std::filesystem::path& kit, const std::filesystem::path& exe, int players,
                        const Session& session, std::uint32_t host_pid, const std::filesystem::path& result);

// --- The OS part (coop_local_start_os.cpp; Windows only, elsewhere none) ---
// Whether this game can start the others: Windows, a kit with the launcher, game 1 (not an extra game, not a Nucleus
// Co-op instance: those have coop-player.ini).
bool Offered(const std::optional<std::filesystem::path>& kit, const std::filesystem::path& game_dir);
// The players 2..9 whose game runs now (a longshot.exe from <game>\local-players\playerN).
std::vector<int> RunningPlayers(const std::filesystem::path& game_dir);
// A UDP port on 127.0.0.1 nobody holds, from kFirstPort up (0 none); whether that one port is free.
std::uint16_t FreePort();
bool PortFree(std::uint16_t port);
// This game's own exe and process id (the launcher starts the others from the same exe's folder, and places this
// game's window).
std::filesystem::path ThisExe();
std::uint32_t ThisPid();
// Runs the command line with no window; false when it did not start.
bool Launch(const std::string& command_line);

}  // namespace CoopLocalStart
