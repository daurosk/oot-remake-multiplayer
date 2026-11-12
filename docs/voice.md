# Proximity voice chat

Players hear each other talk as if their Links were speaking: the voice comes from where that player's Link stands, louder up close, duller and quieter further off, silent past the range, and never from another scene. It plays in the game's own sound, so the game's volume, its effects and the speaker layout (stereo or 5.1) apply to it as to any other sound. Up to 16 players. Hookshot only for now (the emulator has no voice).

## For players

| Setting | `coop-options.ini` | Default |
|---|---|---|
| How you talk | `LICHII_COOP_VOICE` = `ptt` (push to talk), `open` (open mic: sends when you speak), `off` | `ptt` |
| Push-to-talk key | `LICHII_COOP_VOICE_KEY` = a letter or digit, or a Windows key code (`0x14`) | `V` |
| The others' volume | `LICHII_COOP_VOICE_VOLUME` = 0 to 200 (percent) | 100 |
| How far you hear | `LICHII_COOP_VOICE_RANGE` = 5 to 100 (metres) | 30 |

The microphone is the one chosen in Hookshot's Insert menu (Gameplay: Microphone), the same the Ocarina's voice mode uses. Off there means the others hear nothing. The co-op settings screen in the game sets the same (title-menu, `CoopVoice::Set`). Typing in the chat box is never push to talk; the key only counts while the game's window is in front.

## How it works

- **Talking** (`coop/voice/gate.hpp`): every 20 ms of microphone (48 kHz mono) goes through the gate: push to talk sends while the key is held and 100 ms after; open mic sends while the level is 12 dB over the room's noise floor (which follows the room slowly) and 300 ms after. When the gate closes, one empty frame with the `end` flag says so.
- **Codec**: Opus, voice mode, 24 kbit/s, 20 ms frames, in-band forward error correction tuned for 10% loss. Hookshot's own libopus (its `third_party/opus`, as `nn::codec` uses it), handed to the module by our patch 0009.
- **Wire** ([protocol.md](protocol.md), "Voice"): `VOICE_FRAME` on channel 5, unreliable, encrypted like all traffic. About 150 bytes a frame on the wire (60 of them Opus), 50 a second while talking: some 7 KB a second per listener. The server sends it only to the players in the speaker's scene within 35 m (its `voice_range_m`), so 16 players spread over the world cost little; 16 in one room cost the host about 15 x 15 streams only while all talk at once.
- **Receiving** (`coop/voice/jitter.hpp`): one jitter buffer per speaker orders the frames, waits 40 to 200 ms before a sentence starts (more when the line is bumpy: RFC 3550's jitter estimate), fills a missing frame from the next frame's FEC or Opus's concealment, and drops what comes too late.
- **Playback** (`coop/voice/spatial.hpp`, `engine.hpp`): each voice sits a little under the top of its Link's head as presence shows it (the same place as the puppet and the name tag). Distance is from our Link; direction is the game camera's (its world-to-clip matrix); with no camera known the voices are centred. Full volume to 3 m, then 1/distance, faded to silence over the last 6 m before the range; a low-pass closes from 20 kHz to 2.5 kHz with distance; equal-power pan, and on 5.1 the rear speakers for a voice behind. A hidden player (another scene, our cutscene) is not heard.
- **In the game's mix** (Hookshot patch 0009): Hookshot's mixer renders the game's nn::audio graph every 5 ms; just before the final mix's effects and volume it hands the final mix's buffers to the module (`hos_coop_voice_render`, [hos_coop_voice.h](../hookshot/src/hos_coop_voice.h)), which adds the voices. The microphone is a stream of the module's own on the source the game's input streams use.
- **Speaking indicator**: `CoopVoice::Speaking(player)` is true while a player's voice plays here (heard in the last 250 ms), `CoopVoice::Talking()` while we send: for the native name tags.

## Switches

`HOS_COOP_VOICE=0` (Hookshot): the module is never handed voice. `LICHII_COOP_VOICE=off`: no microphone opened.

## Later

The voice goes through the game's mix, not yet through the game's own 3D sound system (aal and SLink, with its reverb sends and occlusion): a looping SLink emitter on each puppet whose nn::audio voice carries the speaker's audio would give that. It needs the game running to find the emitter's voice.
