# Proximity voice chat

Players hear each other talk as if their Links were speaking: the voice comes from where that player's Link stands, louder up close, duller and quieter further off, silent past the range, and never from another scene. It plays in the game's own sound, so the game's volume, its effects and the speaker layout (stereo or 5.1) apply to it as to any other sound. Up to 16 players. Hookshot only for now (the emulator has no voice).

**Voice chat is off until you turn it on.** Off means off both ways: your microphone is never opened and you hear nobody's voice. Turn it on with the **Proximity voice chat** row of the co-op settings in the game (Multiplayer tab, Settings), or with the line `LICHII_COOP_VOICE=open` in the kit's `coop-options.ini`. Turned on, it is **open mic**: just talk, you are heard when you speak (silence, fans and key clicks are not sent). The **Voice mode** row under it switches to **Push to talk (V)**: hold **V** to talk, let go and you are silent. The chat says which once voice is on (`Voice chat on: open mic (change to push-to-talk in Settings)`). Everyone who wants to talk or listen turns it on in their own game.

**On speakers, not headphones?** Open mic hears the others' voices from your speakers and could send them back as an echo. While someone is heard, open mic needs your voice 10 dB louder before it sends (the echo guard below), which stops most of it; Opus has no echo cancellation, so headphones or push to talk are best.

## For players

| Setting | `coop-options.ini` | Default |
|---|---|---|
| Voice chat and its mode | `LICHII_COOP_VOICE` = `off`; `open`, `on` or `1` (open mic: sends when you speak, what turning it on gives); `ptt` (push to talk, as before) | `off` |
| Push-to-talk key | `LICHII_COOP_VOICE_KEY` = a letter or digit, or a Windows key code (`0x14`) | `V` |
| The others' volume | `LICHII_COOP_VOICE_VOLUME` = 0 to 200 (percent) | 100 |
| How far you hear | `LICHII_COOP_VOICE_RANGE` = 5 to 100 (metres) | 30 |

The microphone is the one chosen in Hookshot's Insert menu (Gameplay: Microphone), the same the Ocarina's voice mode uses. Off there means the others hear nothing. The co-op settings' Proximity voice chat row in the game turns it on (open mic) and off, and the Voice mode row (shown while voice is on) chooses Open mic or Push to talk, at once and for the next starts (`CoopVoice::SetMode`; they keep that one line of `coop-options.ini`: `open`, `ptt` or no line). Typing in the chat box is never push to talk; the key only counts while the game's window is in front.

The microphone is opened only when it may send: with push to talk when you press the key (and closed again 100 ms after you let go), with open mic for as long as you are connected and in the game (the gate then decides what is sent). Voice off (the default), muted or not connected: it is never opened. A push-to-talk player who never presses the key never has a live microphone (Windows' "microphone in use" icon stays off); an open-mic player has it on while connected, as asked.

### Who is talking

The player HUD on the left (the names and hearts under your own hearts) shows a small microphone after a name:

- **lit green, with a ring**: that player's voice plays here now (held 300 ms after its last frame);
- **crossed out in red**: muted (by you, or by the host for everyone);
- nothing: quiet (whether their voice chat is on is not known here).

While your voice chat is on, a **You** row above them shows your own: lit while you send, dim while you are quiet, crossed out when muted. Voice off: no icons at all. The Multiplayer tab's Lobby page marks the same players as talking.

## How it works

- **Talking** (`coop/voice/gate.hpp`): every 20 ms of microphone (48 kHz mono) goes through the gate: push to talk sends while the key is held and 100 ms after; open mic sends while the level is above -50 dBFS and 12 dB over the room's noise floor (which follows the room slowly), opening after two such frames in a row (40 ms: one key click or bump is one frame and is not sent) and staying open 300 ms after the last. **Echo guard**: while any of the others' voices played here in the last 300 ms, both thresholds are 10 dB higher, so their voice coming back from our speakers into the microphone is not sent again; talking over them still opens it. When the gate closes, one empty frame with the `end` flag says so.
- **Codec**: Opus, voice mode, 24 kbit/s, 20 ms frames, in-band forward error correction tuned for 10% loss. Hookshot's own libopus (its `third_party/opus`, as `nn::codec` uses it), handed to the module by our patch 0009.
- **Wire** ([protocol.md](protocol.md), "Voice"): `VOICE_FRAME` on channel 5, unreliable, encrypted like all traffic. About 150 bytes a frame on the wire (60 of them Opus), 50 a second while talking: some 7 KB a second per listener. The server sends it only to the players in the speaker's scene within 35 m (its `voice_range_m`), so 16 players spread over the world cost little. In one room the host sends every voice to every listener: about 7.5 KB a second per voice per listener, so 3 talkers in a full room of 16 take about 340 KB a second (2.7 Mbit/s) of the host's upload. One listener gets at most 4 voices at once (`voice_max_speakers`: the first to start talking keep their place until they stop or are quiet 400 ms), so the worst case is 16 x 4 streams, about 480 KB a second (3.8 Mbit/s), not 16 x 15.
- **Receiving** (`coop/voice/jitter.hpp`): one jitter buffer per speaker orders the frames, waits 40 to 200 ms before a sentence starts (more when the line is bumpy: RFC 3550's jitter estimate), fills a missing frame from the next frame's FEC or Opus's concealment, and drops what comes too late.
- **Playback** (`coop/voice/spatial.hpp`, `engine.hpp`): each voice sits a little under the top of its Link's head as presence shows it (the same place as the puppet and the name tag). Distance is from our Link; direction is the game camera's (its world-to-clip matrix); with no camera known the voices are centred. Full volume to 3 m, then 1/distance, faded to silence over the last 6 m before the range; a low-pass closes from 20 kHz to 2.5 kHz with distance; equal-power pan, and on 5.1 the rear speakers for a voice behind. A hidden player (another scene, our cutscene) is not heard.
- **In the game's mix** (Hookshot patch 0009): Hookshot's mixer renders the game's nn::audio graph every 5 ms; just before the final mix's effects and volume it hands the final mix's buffers to the module (`hos_coop_voice_render`, [hos_coop_voice.h](../hookshot/src/hos_coop_voice.h)), which adds the voices. The microphone is a stream of the module's own on the source the game's input streams use.
- **Speaking indicator**: `CoopVoice::Speaking(player)` is true while a player's voice plays here (heard in the last 300 ms), `CoopVoice::Talking()` while we send; `CoopVoice::IconFor` turns them and the mutes into the HUD's icon (above, "Who is talking").

## Switches

`HOS_COOP_VOICE=0` (Hookshot): the module is never handed voice. No `LICHII_COOP_VOICE` line, or `off`: no microphone opened and nobody heard (the server still relays the others' voices to this game, which drops them unplayed). The game's log says which at start: `coop: voice: off (the default: ...)` or `coop: voice: on (open, key 0x56, ...)`, and `coop: voice: now open` / `now ptt` / `now off` when the settings rows change it.

**Linux**: the voice code is the same; Hookshot's patch 0009 does not attach the module there yet (no voice on Linux), and push to talk reads the key through Windows only, so once it attaches, open mic is the mode that works there.

## Testing

Two test games on one PC have one microphone, one pair of ears and one window in front, so voice has a test source and a test log (`LICHII_COOP_VOICE_TEST`, read at start):

- `tone`: instead of the microphone, a 440 Hz tone at a quarter of full scale, sent as if the push-to-talk key were held 2 s and let go 2 s, whichever window is in front. The real microphone is never opened. It goes the whole way a voice goes: gate, Opus, `VOICE_FRAME`, the server's range relay, the listener's jitter buffer, placement and the game's mix. Voice must be on (`LICHII_COOP_VOICE=ptt`); with voice off the tone goes nowhere.
- `log` (and `tone`): the log says who is heard here whenever that changes, `coop: voice: test: P1 heard at 2.4 m`, `P1 not heard (quiet, 2.4 m)`, `P1 not heard (out of range: 24.5 m, range 10 m)`, `P1 not heard (not shown here)`, and every 2 s while they change the counts, `coop: voice: test: ptt: sent 100 received 0 decoded 0 concealed 0 unplaced 0`. "Heard" is the mixer's own word: a frame of that voice went into the game's mix, audibly, within the last 300 ms.

The autotest scenarios (`tools/autotest/groups/hookshot-voice.txt`, a Hookshot game built with patch 0009):

- `hs-voice`: voice on in both, hearing range 10 m. A beeps at the load spot; B stands beside A (heard), is put 24.5 m away (not heard) and back (heard again). A person at the PC hears the beep come and go too.
- `hs-voice-off`: the same two games with no `LICHII_COOP_VOICE` line: nothing sent, nothing heard, no microphone opened.

Not covered by these, to check once with a real microphone: Windows' "microphone in use" icon appears only while V is held (the log's `coop: voice: microphone opened` line is its witness), and the first word after pressing V is not clipped.

## Later

The voice goes through the game's mix, not yet through the game's own 3D sound system (aal and SLink, with its reverb sends and occlusion): a looping SLink emitter on each puppet whose nn::audio voice carries the speaker's audio would give that. It needs the game running to find the emitter's voice.
