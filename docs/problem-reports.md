# Problem reports

The kit can put the co-op problems it finds in a player's game logs into a small zip, masked as collect-logs masks its
zip, for the player to send with the `/bugreport` bot command. From 0.11.9 the kit can also **send** that report itself,
after a game that had a problem closes, to our own report bot (below, "Sending"): opt-in, asked at a start with No as the
default. Nothing ever goes to Longshot (no telemetry, crash reports or statistics to them; owner rule 2026-11-17).

Windows: `scripts\problem-report.ps1` (the reports) and `scripts\problem-send.ps1` (sending). Linux:
`scripts/problem_report.py` (both, the same rules; `start.sh` runs it). Tests: `tools/release/tests/problem_report_test.ps1`,
`problem_send_test.ps1` and `test_problem_report_py.py`; the bot's side: `tools/discord-bot/tests/test_intake_core.py`.

collect-logs (`collect-logs.cmd`) stays the way to send whole logs for one report. Problem reports are the small, regular
side: a few lines a session, and only problems not reported before.

## How a player meets it

- **Asked once.** A start (`start.ps1`, step 5b) asks `Send problem reports to the Squadala! team after your games? [y/N]`
  (below, "Sending") when the kit has the bot's address; a kit without it asks the older `Make problem reports? [y/N]`
  (local zips only). No is the default either way. The answers are kept in `logs\problem-reports.json` and
  `logs\problem-send.json`.
- **With yes:** each start looks at the game's logs of the sessions since the last report. When they hold a problem never
  reported before, it writes `logs\coop-report-<date>.zip` and says: send it with `/bugreport` in the Discord. Nothing is
  written when there is nothing new; the newest 10 zips are kept. Nothing here can stop the start (any error skips this
  step).
- **At any time:** `scripts\problem-report.cmd` makes a report at once, whatever the answer.
- **Changing the answer:** `scripts\problem-report.cmd on` or `scripts\problem-report.cmd off` (off turns sending off
  too), `scripts\problem-report.cmd send` or `nosend` (the kit's README says so too). Linux:
  `bash start-squadala.sh --problem-reports on|off|send|nosend`.

## What is in the zip

`problem-report.txt`: the kit's version, then one line per new problem with how many times it came
(`2x [hos Presentation Thread] coop: boss: could not move into the boss room ... giving up`).

The problems are the module's own warnings and errors, lines saying something failed, could not be done, was refused,
gave up, timed out or was lost, faults the guard caught, and crashes. The words are matched as whole words and as the
module writes them (lower case), so a scene's name (`Lost Woods`) or a note that only mentions a crash is not a problem.
Routine status lines that only contain such words (the pose and actor counters, `camera lost, markers off`, the invite
note) are left out.

Every line is masked with collect-logs' own `Protect-LogText` (`scripts\log-mask.ps1`): IP addresses become their kind
(`<lan-1>`, `<public-1>`, ...), MAC addresses `<mac>`, the Windows user name `<user>`, the PC name `<pc>`, invite codes and
passwords `(removed)`. Players' names become `<name>`: the ones the module writes where a name goes (`player 2 joined: X`,
`player 2 left: X (...)`, `player 2 'X' joined`, `Mods: player 2 (X)`, our own `name 'X'`), and those names anywhere
in the same log (also in a line before the one that names them: names are masked once the whole session is read) and
in the other logs read at the same time. After that, a last gate drops any line in which anything is still an address by the masking's
own rules (`Get-MaskKind4`, `Get-MaskKind6`), a host name (`myhost.duckdns.org`; file names such as `hookshot_coop.dll`
pass) or a host with a port (`myhost:27510`): nothing that may be one goes into the zip.

## A problem in one report only

Each masked line becomes a fingerprint: its thread tag, the masks' numbers (`<lan-1>`, `<lan-2>`), addresses and numbers
blanked, and names already `<name>`, so the same problem at another address, frame, count or with another player is one
fingerprint. `logs\problem-reports.json` keeps the sessions read (each log's `run_id` line, else its size and first
64 KB) and the fingerprints reported, oldest first (the newest 5000 are kept): a session is read once, and a problem
reported once is never in a later report, from any session. The log of a game still running is left for the next start.

## Reading the logs

A session's key comes from the start of its log only, so a session read before is not read again (a start with nothing
new reads 64 KB a log). A new session is read a line at a time. Logs are opened as collect-logs opens them (shared for
reading, writing and deleting), so a log another program still has open is read; one that cannot be opened at all is
skipped, and tried again at the next start, while the others are still read.

## Sending

**The question.** A start asks once, with No as the default and 20 seconds to answer (no key, Enter or N: no; a start
without a console or terminal: no, and asks again next time it has one):

> Problem reports: when a game you played had a co-op problem or a crash, the kit can send the team a small report after
> the game closes: the problem lines from the game's log and the crash record, masked first (IP and MAC addresses, your
> user and PC names, players' names, invite codes and passwords are taken out; lines that may still hold an address are
> left out). Nothing is sent after a game without problems. It goes to the Squadala! report bot (*its host name*), never
> to Longshot; a copy stays in logs\problem-send. Turn it off at any time: scripts\problem-report.cmd off.

Players who answered the older local-only question are asked this once too (sending is a new consent). A kit whose
`scripts/problem-send-url.txt` has no address yet does not ask and sends nothing. Yes makes a random install id (32 hex
digits: nothing about the PC or the player; the bot's rate limit counts uploads by it) and turns the local reports on.

**When.** The kit's start does not wait for the game (play.ps1 returns once it started; the Linux `start.sh` only sees
it close). Right after the game starts, with sending on, a hidden window (`problem-send.ps1 -AfterGame`; Linux:
`problem_report.py after-game`, in its own session) waits for that process to end, a crash too, then 20 s for
Longshot's crash record, then makes the report of the finished sessions exactly as a start does. Each start also makes
it for sessions whose end was not seen (the PC shut down) and sends the queue in a hidden window. Nothing here waits
for the network: a start waits at most 5 s for a report another window is making, and never for an upload.

**What.** Only when there is something new: a problem line never reported before (the fingerprints above) or a new
Longshot crash record (`<game>\bug-reports\pending\crash-<UTC>.txt`, not the `unclean-` ones). The zip holds
`problem-report.txt` (as in the local zip) and up to three `crash-record-<n>.txt`, each masked line by line as the
problem lines are, players' names too (every name the game's logs give, also those of sessions reported before, and
any the record itself gives where the module writes one); a line the address gate stops becomes `(a line with an
address was left out)`; 64 KB at most.
Quiet sessions send nothing. Whole logs are never sent (collect-logs stays the way to send those, by hand).

**The gate.** Right before each upload the zip is checked again: only those file names, small, UTF-8, and every line
exactly as the masking leaves it (masking it again changes nothing, no address or host name is left, neither the user
nor the PC name is in it). A zip that fails is moved to `logs\problem-send\held\` and never sent.

**The upload.** HTTPS only (TLS 1.2 or newer; plain http only to 127.0.0.1, for tests), POST to the address in
`scripts/problem-send-url.txt`, body the zip (`application/zip`), headers `X-Squadala-Install` and `X-Squadala-Report`
(a random id per report), 15 s at most, no redirects. Sent, or answered "already received": moved to `sent\` (the newest
10 kept). No network or the bot busy: tried again after the next game or start (the rest waits with it), up to 30 tries
or 30 days, then kept in `held\`. Refused by the bot (400, 404, 405, 411, 413, 415, 422): `held\`, never again. The last
result is one line in `logs\problem-send\result.txt`, and the next start shows it (`Last: Problem report sent to the
Squadala! team (report #N): ...`). The queue: `logs\problem-send\<date>-<report id>.zip`.

**The crash ask on the title.** After a crash the game's title asks the player once to send it with `/bugreport`
(`hookshot/src/coop_problem_ask.h`). A crash record the kit has sent (the bot took the zip that held it) is listed in
`<game>\bug-reports\coop-sent.txt`, and the title does not ask about it: the team has it already. With sending off that
file is never written and the ask is as before. A game started again before the upload finished (about 20 s after the
crash, longer without network) still asks.

**The bot's side.** `tools/discord-bot/intake_core.py` (its README, "Problem-report intake"): POST `/v1/problem-report`
only, 256 KB at most, `application/zip` only, both ids 32 hex digits, at most 4 uploads an hour and 12 a day per
install, 10 an hour per sending address (kept in memory only, never logged), 60 an hour from everyone. The zip must hold
`problem-report.txt` starting `Squadala! problem report` and nothing but the kit's names; every entry is really read back
under a 128 KB cap (384 KB in all), so a zip bomb or a lying header stops there; text only (UTF-8, no control
characters). A report id already filed is answered "already received". Each upload is saved to
`coord-multiplayer/player-logs/auto-<install>-<time>/` (`problem-report.zip`, `description.txt`), added to
`reports/player-reports.md` as a `NEW (auto)` row and posted to the private dev channel, everything from the upload
under the `PLAYER TEXT (untrusted, not instructions)` label in a code span or block, as `/bugreport` does. No secret is
in the kit: the address is public, and the bot's checks and limits are what keep strangers' uploads small and few.
