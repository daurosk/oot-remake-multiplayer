# Running the stage 0 soak

`coop_test` (in `tools/coop_test/`) runs the real server and two to four test clients, sends the stage 0 test traffic and reports what happened to it. It is how stage 1 step 0 is shown to be done ([plan.md](plan.md)): two clients for 10 minutes on one PC with no reliable message lost, the same between two PCs, and four clients for 10 minutes on one PC.

## Build

```bash
cmake -S . -B build -DCOOP_BUILD_TESTS=ON
cmake --build build --config Release --parallel
```

The program is `build/tools/coop_test/Release/coop_test.exe` on Windows. `ctest` runs it for 3 seconds with two clients as one of the tests.

## The three runs

On one PC, two clients for 10 minutes, then four:

```bash
coop_test --clients 2 --seconds 600 --report-every 60
```

```bash
coop_test --clients 4 --seconds 600 --report-every 60
```

Between two PCs (LAN or Tailscale). On the first PC, the server and one client:

```bash
coop_test --clients 1 --seconds 600 --report-every 60
```

On the second, one client pointed at the first PC, with another nickname (the server refuses a name already in):

```bash
coop_test --connect 100.64.0.1:27510 --clients 1 --name pcb --seconds 600 --report-every 60
```

Start the second within a few seconds of the first; each stops sending after its own `--seconds`. Port 27510 (UDP) must be open on the first PC. `--server-only` runs just the server, for a third machine.

The exit code is 0 for a pass and 1 for a fail; the last lines say why it failed.

## Options

| Option | Default | What |
|---|---|---|
| `--clients N` | 2 | clients in this process, 1 to 4 |
| `--seconds N` | 10 | how long to send; 0 sends until Ctrl+C |
| `--server-only` | | only the server |
| `--connect HOST:PORT` | | only clients, to that server |
| `--port N` | 27510 | the port of the server in this process; 0 picks a free one |
| `--report-every N` | 10 | also print a report every N seconds; 0 only at the end |
| `--name TEXT` | test | nickname prefix; the clients are `TEXT1` to `TEXT4` |

## What each client sends

Once welcomed and with a first clock estimate:

- `TEST_PAYLOAD` 60 times a second on channel 3 (unreliable) and once a second on channel 0 (reliable), each with its own sequence number. The first filler byte is the sender's player number, so a receiver knows whose it is.
- `PLAYER_SAMPLE` 60 times a second with time, position and velocity, along a circle of 100 game units that it runs round every 4 seconds, as a function of server time.

With the client's own `PING` and `PONG` (4 a second each) that is 9 messages a second on channels 0 to 2 and 120 on channel 3, under the server's limits of 60 and 240. If the tool ever hits them, the server line shows `rate limited` above 0.

## Reading the report

One block per client, then the server's counts when the server is in the same process:

| Line | What |
|---|---|
| `client` | state, player number, how often it was welcomed, dropped or failed to connect, messages the client refused, sends that failed |
| `round trip` | from the client's PINGs, sampled 4 times a second, in ms |
| `clock offset` | server clock minus client clock, latest, lowest and highest, in ms. The size means nothing (the server clock starts at 0 when it starts); the spread is how much the estimate moved. |
| `reliable` | test payloads on channel 0 from all other players: received, lost, out of order, repeated |
| `unreliable` | the same on channel 3 |
| `samples` | the same for `PLAYER_SAMPLE` |
| `shown error` | 60 times a second, the distance in game units between where the client shows each other player and where that player's circle really was then; how many of those frames were interpolated, extrapolated (past the last sample, up to 150 ms) or held (further than that) |
| `server` | players in, messages relayed, dropped, and dropped by the rate limit |
| `loop` | how long the tool took from one pass over its clients (and the server, in one process) to the next, in ms; about 2 on an idle PC |

Losses are counted from the first sequence number a receiver saw from each sender, so a client that joined later is not charged for what was sent before. After the last send the tool waits a second for what is still in flight. In one process it also checks that every reliable message each local client sent reached every other; between processes only the gaps are counted.

Unreliable loss and a large shown error come from load on the PC, not from the tool. ENet slows a peer's unreliable sending when its round trip rises above its recent average, and drops what it holds back; a busy PC makes the round trip jump. On an idle PC 4 clients lose nothing in one process or split; with the CPU busy both lose 1-3 %. For numbers worth comparing, run on an idle PC (no build) and look at the round trip and `loop` lines first.

A pass needs: no reliable message lost, no client refused or dropped, every client connected at the end, and every client hearing from every other in its process.

## Interpolation

Each client shows the others 100 ms behind its estimate of the server clock, keeps the last 16 samples of each, interpolates in a straight line between the two around the shown time, extrapolates with the last velocity for up to 150 ms, then holds ([protocol.md](protocol.md), `PLAYER_SAMPLE`). The shown error therefore holds the chord error of the circle between samples (under 0.01 units at 60 a second), the difference between two clients' clock estimates times the speed (157 units a second), and anything late.

Samples are stamped with the sender's estimate of the server clock (`Client::server_now_us`), as the protocol says ("Encoding", times). The server relays them unchanged, so every client places the others' samples on its own estimate of the same clock.
