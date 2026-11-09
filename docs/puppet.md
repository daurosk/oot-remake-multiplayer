# Puppet Link: research

Plan stage 1 step 2 ([plan.md](plan.md)): show each remote player as a second Link in the world, driven by their samples. This file says where the game creates actors, which actor to spawn, where its pose and animation live, where to hook a frame, and how the emulator gets code to run there. Nothing here has been run yet.

Addresses are offsets into `main` of build `e0f3ccfaf50065ae`. **Checked** means read from the instructions or the data files. **Inferred** means taken from a name, a shape or the engine family. The game was not run for this file.

## Results

- The create function is **0x372a70**, the engine's `requestCreateActorAsync`. It builds `Pack/Actor/<name>.pack` and names its heap `ActorMgr`. 38 callers; the manager is the pointer at 0x4e91888.
- `Player_Event` is not a puppet. It has no model and no animation of its own: it is an event stand-in that binds to the actor named `Player`. `Player_Child` and `Player_Adult` are the real player, with input, camera target and the `Player` actor name, so a second copy would clash with the first.
- Chosen approach: **(a)**, a real actor created by a code patch. Fritchii already has both halves: a built-in patch table that writes checked bytes into `main` at load, and custom SVC numbers that game code calls into the emulator with (`svc #0xf3`). The puppet is a small actor pack of our own that uses the game's own Link model and animation files.
- Per-frame hook point: the `bl 0x3b4e300` at **0x6e6dd8** in `ModuleSystem::moduleCalc_`, after the frame's job graph has been waited on.

## The create function

| Address | What | Status |
|---|---|---|
| **0x372a70** | `requestCreateActorAsync`. Returns at once when bit 1 of manager +0x109 is set; returns when the name is empty | Checked |
| 0x4e91888 | Pointer to the actor manager, the function's `x0`. 34 code references | Checked |
| 0x3cf1a0b, 0x3ca8071 | `"Pack/Actor/"` and `".pack"`: the function builds the pack path from the name | Checked |
| 0x3cf1a17 | `"ActorMgr"`, the heap name it sets up | Checked |
| 0x4e91920 | Actor module; its byte +0x1d3 bit 3 picks a second path in the function | Checked; the path was not read |

Arguments, from three callers that agree (`SeqExecuteCreateActor` run slot 0x32bca10, the preload walker 0x9456b0, and 0x26e62b0):

| Register | What | Status |
|---|---|---|
| `x0` | The manager, `[0x4e91888]` | Checked |
| `x1` | Pointer to a name: its first 8 bytes are a `char*` to a 0-terminated name, the int at +8 its length or capacity | Checked |
| `x2` | Pointer to the create argument, below | Checked |
| `x3` | Pointer to an 8-byte handle slot the manager fills. The preload walker skips a record whose slot is already set | Checked; what the manager stores there is inferred to be the actor link |
| `w4` | 1 from the preload walker | Checked; meaning not read |
| `x5`, `x6`, `x7` | 0 from the preload walker | Checked |
| stack +0, +8, +0x10 | A byte 0, then two 0 pointers | Checked |

The create argument is about 0x100 bytes on the caller's stack. Fields the callers set:

| Offset | What | Status |
|---|---|---|
| +0x0 | Position, 3 floats; zero from the default at 0x3ee4f8c | Checked as stores; meaning inferred |
| +0xc | Rotation, 3×3 floats, copied from the identity at 0x3ee4f9c | Checked |
| +0x30 | Scale, 3 floats, (1, 1, 1) from 0x3ee5008 | Checked |
| +0x40 | Pointer, set to 0x3ee4f8c (an empty default) | Checked; meaning not read |
| +0x48 | Word 0x10001 | Checked; meaning not read |
| +0x4f to +0x55 | Zero | Checked |
| +0x58 | 0x41 bytes cleared | Checked |

The safe way to fill it is to copy what the preload walker at 0x9456b0 does (0x945750 to 0x9457f8) and then set the position and rotation.

How the game creates the player: the walker 0x9456b0 goes over an array of 0x70-byte records and calls 0x372a70 for each one whose handle slot (+0x18) is empty, with the name at record +0x20. Its caller 0x945610 runs it on two lists. A name table at 0x944a44 holds `Player`, `Player_Base`, `Player_Child` and `Player_Adult`. That the player goes through this walker is **inferred** from the names; the link between the table and the records was not read.

## Which actor to spawn

Read from the actor packs in the romfs, `Actor/<name>.engine__actor__ActorParam.bgyml`.

| Actor | What it has | As a puppet |
|---|---|---|
| `Player_Event` | `ASRef` = `AS/Player_Child.root.asb`, `ActorLogicSimpleActor`, an `EventPerformer` that binds to the `Player` actor name, equipment. **No** `ModelInfoRef`, `AnimationRef` or physics | No: nothing to draw, and it attaches to the real player |
| `Player_Child`, `Player_Adult` | Everything from `Player_Base`: `ActorNameRef` = `Player`, camera target, controller physics, lifter, `GameLifeCondition` `NoDelete`, the player AI | No: a second `Player` would take input and confuse whatever looks up the player by name |
| `Enemy_MiddleBoss_DarkLink` | A Link-shaped enemy with Link's `ASInfo` and `AnimationParam`, its own logic | A shortcut for the first spawn test only: an enemy that fights |
| **Our own pack** | The `ModelInfoRef`, `ASRef`, `ASInfoRef`, `AnimationRef` and `ModelBindRef` of `Player_Child` (or `Player_Adult`), `ActorLogicSimpleActor`, no physics, no camera, no player name | **Yes** |

So the puppet is two small packs, `CoopPuppet_Child` and `CoopPuppet_Adult`. Each holds one `ActorParam` we write, whose components point at the game's own component files by path, so no game data is copied. The TotK work did the same: its remote players were a pack of their own (`PlayerPuppetZelda`), not a second player.

The emulator serves the packs as a romfs layer, the same way any LayeredFS mod is served. Two things to check when the pack is first built:

1. Pack files in the romfs are zstd with the game's dictionaries (`Pack/ZsDic.pack.zs`). Whether a plain `.pack` is also accepted was not read; if not, compress with the same dictionary.
2. The engine has a resource size table (`System/Resource/ResourceSizeTable.Product.111.Nin_Ounce_NVN2.rsizetable.zs`). In the engine family a file missing from it can fail to load. If the puppet does not load, add its entries there.

## Pose and animation

| Item | Where | Status |
|---|---|---|
| Position | Actor +0x31c, 3 floats | Checked in `../integration/docs/player-state-map.md` |
| Rotation | Actor +0x328, 3×3 floats in rows | Layout checked there; row order inferred |
| State | Actor +0x2c; 4 is alive and in the world, 7 to 11 gone | Checked there |
| Component table | Actor +0x270 | Checked |
| AS (animation) component | Component table +0x40, then +0x18, then +0x18: the AS controller | Checked, 0x3abebc0 and 0x28aedf8 |
| Current AS frame | Controller +0x20 is an array, controller +0x18 the index into it; the float at entry +0xe8 is the frame. Read by 0x3abebc0 | Checked as a read; that +0xe8 is the frame is inferred from the `ASFrame` plug of `ExecuteASPlaySetASFrameEachFrameByPlug` |
| Play an AS command | **0x2022a0**, 87 callers, with one argument: a request on the stack | Checked |

The request to 0x2022a0, as `OneShotASPlayWithSkipUpdate` (run slot 0x28aed70) builds it:

| Offset | What | Status |
|---|---|---|
| +0x0 | The actor | Checked |
| +0x8 | `char*` to the AS command name | Checked |
| +0x10 | Word 0 | Checked; meaning not read |
| +0x14 | Float 1.0 | Checked; inferred to be the rate |
| +0x18, +0x1c | Float -1.0 each | Checked; inferred to be "default" start frame and blend |
| +0x20, +0x22 | Half-word and byte 0 | Checked |
| +0x28 | The calling node; probably may be 0 | Checked as the node; that 0 is allowed is not known |

So the animation sync is the AS command name plus a frame, as the plan says. The local player's current command name is not found yet: the read side is the next research item for the sampler (see Not done).

Writing the pose: for an actor with no physics the plan is to write +0x31c and +0x328 directly, from the emulator, every frame. Whether the actor's own calc writes them back each frame is not known. If it does, the node `ExecuteActorForceSetMtx` (string 0x3dafe6a, registered at 0x2622d3c) and `OneShotActorRequestSetMtx` (0x3d23020) are the engine's own setters; their run slots were not found.

## The frame hook

| Address | Word | What | Status |
|---|---|---|---|
| **0x6e6dd8** | 0x94d19d4a, `bl 0x3b4e300` | In `moduleCalc_` 0x6e6be0, after the job graph is started (0x6e6d34) and waited on (0x6e6d3c). Hands the frame delta to the object at 0x4e94b20 | Checked; that it runs on every frame is inferred from `actor-map.md` |
| 0x6e6cc4 | | Taken: Fritchii's Dynamic FPS patch calls its routine at 0x3c46200 from here | Checked in `patch_manager.cpp` |
| 0x6e6c40 | | `bl 0x3b4f370`, earlier in `moduleCalc_`, but only on a rate change. Not a frame hook | Checked |
| 0x3adf2d0 | 0x972ec204, `bl 0x68fae0` | Inside the Actor module's frame step. A second choice: it runs as a job, maybe on a worker thread | Checked |

At 0x6e6dd8 the actors of this frame have run, the calc thread is the only one in game code that touches them, and the next draw has not read them yet. That makes it the place to create, to start animations and to write the pose.

## How the emulator runs code there

Fritchii already does both halves for other patches (`src/core/file_sys/patch_manager.cpp`, `src/core/hle/kernel/svc.cpp`):

- **Built-in patches.** `BuiltinPatch` records write bytes at an offset into `main` when the module loads, for this title and build id only, and with `checked` refuse to write unless the original word is there. Optional ones are switched on by an environment variable.
- **Code in the padding.** `main`'s code ends at 0x3c461d0 and its last page runs to 0x3c47000, all zero in the dump. Dynamic FPS uses 0x3c46200 to 0x3c46278. **0x3c46280 to 0x3c47000 is free** (3.4 KB). The resource poll patch puts `svc #0xf3` in other zero padding (0x17088).
- **Custom SVCs.** `svc.cpp` hands unknown SVC numbers to `CallNx2OotPollBackoff`; it acts only for this program and returns the registers unchanged. A new number for co-op works the same way, and its handler can read and write guest memory and the registers of the thread that called it.

The emulator cannot call a game function itself, so the game calls the emulator and then does the work. The plan, one patch set named `coop puppet`, on only while co-op is on:

1. At 0x6e6dd8, replace `bl 0x3b4e300` with `bl 0x3c46280` (checked against 0x94d19d4a).
2. At 0x3c46280, a short routine: save registers, reserve 0x200 bytes of stack, `svc #0xf4` with `x0` = the stack buffer. The handler fills the buffer with one command and returns its kind in `w0`:
   - 0: nothing.
   - 1, create: the buffer holds a create argument (copied from the walker's defaults, with position and rotation) and the pack name. The routine calls 0x372a70 with `x0` = `[0x4e91888]` and the handle slot in a block it got once from the game's `malloc` (0x14c6c90; its address is given to the handler by a first `svc #0xf4` with kind 2).
   - 3, play: the buffer holds the 0x2022a0 request; the routine calls 0x2022a0.
   - The routine loops back to the SVC until the handler says 0, at most a few commands per frame.
3. Restore registers and branch to 0x3b4e300 with the original `x0` and `s0`, so the game's call happens as before.

The pose is written from the handler itself, straight into the puppet's +0x31c and +0x328, once the handle has given its actor. Removing a puppet (the player leaves, the scene changes) needs the engine's delete request, not found yet (see Not done).

## Why not the overlay

Option (b) was to draw a Link model over the picture from the emulator. It needs a renderer of our own for a skinned model, the model and animations read from the game files at run time, the game's depth buffer for anything to hide behind, and the game's lighting to not look pasted on. The overlay today draws flat markers in a separate window ([overlay.md](overlay.md)). That is far more work for a worse result, and it never gives the puppet a place in the world that later steps (shared enemies, hits) can use. It stays as the markers we already have.

## Next step

Prove the create path with the smallest patch, before the pack exists:

1. Add a `coop puppet` built-in patch to `lichii/patches/` with the two records above (the `bl` at 0x6e6dd8 and the routine at 0x3c46280), and an `svc #0xf4` handler in `lichii/hook/` beside the presence code.
2. With `LICHII_COOP_DEBUG_PUPPET=1`, the handler sends one create of an existing harmless actor (`Animal_Cow_LinkHouse`) at the local player's position plus 2 m, a few seconds after the scene has loaded, and logs the handle slot and the actor's +0x2c and +0x31c each second.
3. Run it in Kokiri Forest. Done when the cow stands there and the log shows state 4 at the place asked for. This also shows whether 0x6e6dd8 runs every frame (count the SVCs per second).

Then write `CoopPuppet_Child`, spawn it the same way, and drive its pose and AS commands from the samples.

## Not done

1. The local player's current AS command name and frame, for the sampler. 0x3abebc0 reads a frame; the name was not found.
2. The engine's actor delete request.
3. The run slots of `ExecuteActorForceSetMtx` and `OneShotActorRequestSetMtx`, if a direct pose write does not hold.
4. What the handle slot holds, and the meaning of `w4`, the stack arguments and create argument +0x40 and +0x48.
5. Whether the engine accepts an uncompressed pack, and whether the size table needs entries for new packs.
6. Everything here is from the dump. None of it has been run.
