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

## Animation

Read from the dump for the sampler; not run. This settles Not done item 1, and corrects the "Current AS frame" row above: controller +0x18 is the slot count, not an index.

### Where the current animation is

| Item | Where | Status |
|---|---|---|
| AS controller | actor +0x270, then +0x40, +0x18, +0x18 (the walk in 0x974b80, 0x1081f60, 0x3abebc0 and 0x2022a0) | Checked |
| Slot count | controller +0x18, s32; every reader refuses an index that is negative or not below it | Checked |
| Slot array | controller +0x20, entries of 0x48 bytes; the first 8 bytes of an entry point to the slot | Checked |
| Slot 0 | The default slot: the slot-name lookup 0xacd6d0 gives 0 for an empty or unknown name | Checked; that it is Link's whole body is inferred |
| Current AS command | slot +0x10, `char*`. 0x2022a0 with request +0x20 = 1 compares the new name with it (pointer, then bytes) and does nothing when they match; `ExecuteASSyncPartialAnimation` (0x3abec10) reads it to replay a slot's command on another slot. An empty string (0x3cddd43) means nothing plays | Checked |
| Rate | slot +0xd4, f32 (0xdf49e0). 0x3abec10 passes it as the rate of its replay request | Checked as a read; that it is the rate is inferred from that use |
| Frame | slot +0xe8, f32 (0x3abebc0) | Checked as a read; meaning inferred, as above |

Other slots (upper body, face and so on) are named in each actor's AS resource (controller +0x48, then +0x10, +8; 0xacd6d0 walks the names there). Only slot 0 is sent for now.

`PlayerReader::read_animation` follows this chain from the player actor and fills PLAYER_SAMPLE field 7: name, frame, rate. It reads the name in 16-byte pieces that stay inside a 16-byte block, so a name at the end of a mapped page still reads, and refuses an empty or unterminated name, more than 64 characters, anything but printable ASCII, and a frame or rate that is not finite or out of range. The sampler sends a sample when the command or rate changes, not for a new frame alone: the receiver runs the frame on from the rate, and every keep-alive corrects it.

### How the puppet plays it

The request to 0x2022a0, with what this read adds to the table above:

| Offset | What | Status |
|---|---|---|
| +0x10 | s32 slot index; 0x2022a0 checks it against the slot count. 0 for the whole body | Checked |
| +0x14 | Rate: the received rate | Inferred, as above |
| +0x20 | Byte 1: skip if this slot already plays the same command | Checked |

So, from the hook routine (see How the emulator runs code there), for each puppet each frame:

1. When the received name differs from what was last asked: play request {puppet actor, name, slot 0, rate, -1, -1, skip-if-same 1}, call 0x2022a0. The name must stay in guest memory while the slot uses it: put it in the block from the game's `malloc`, one buffer per puppet, not on the stack.
2. Set the frame: **0x37b5d70**(controller, slot, `s0` = frame), the call `ExecuteASPlaySetASFrameEachFrameByPlug` (0x26202f0) makes after its play. The controller is the chain above, read by the handler. Do it when the puppet's frame is off by more than a frame or two from the received frame run on by the rate, so the game's own playback stays smooth in between.

Age: the sample's age field (GameUI +0x5c0) chooses `CoopPuppet_Child` or `CoopPuppet_Adult`; a change means delete and create again, since the model differs.

### Not done (animation)

1. Nothing here was run. To check: slot 0's name changes between standing, walking, running and rolling, and the frame counts up at the rate. Needs a debug log of `read_animation()` in the hook (a `lichii/**` change).
2. Held item, sword and shield shown: see Equipment below.
3. The other slots (upper body, face) and partial animations; and whether 0x2022a0 copes with a name from a heap block (it compares by pointer first, then by bytes).

## Equipment

Caleb (2026-11-09): the puppet wears the remote player's tunic, boots, sword, shield and mask, and holds what they hold, drawn or sheathed. Read from the dump and the romfs for this section; **nothing here was run**. Message: `PLAYER_EQUIP` ([protocol.md](protocol.md)).

### How the game shows equipment

Every piece is **an actor of its own**, created by the wearer's EquipmentUser component and bound to the wearer's bones. Nothing is a texture or a flag on the Link model.

| Item | Finding | Status |
|---|---|---|
| Pieces are actors | `Weapon_Sword_Kokiri`, `Weapon_Shield_Deku`, `Armor_Adult_Goron`, `Armor_Child_Mask_Keaton`, `Item_Ocarina_Time` and so on each have their own pack in `Pack/Actor/`. Weapons have `ModelBindRef` `EquipmentCommon`; armor has `ArmorRef`, `SameBoneModelBindRef` (bound bone for bone onto the body) and its own model, so a tunic is a second model laid over Link's | Checked (romfs) |
| The wearer's EquipmentUser | Actor component table (+0x270) **+0x200**: 0x7f7690 reads it, used by `OneShotPlayerEquipDynamicEquipmentFromGameData` (run 0x290b1b0) and `QueryEquipmentUserIsEquipStateDrawnAny` (0x29f1f90) | Checked |
| Its slots | EquipmentUser +0x20: 8 slots of 0x18 bytes, each a link to the equipment actor (holder pointer +0x8, u32 id +0x10, ~0 empty; holder +0x40 the actor, +0x48 its id, the same shape as the player link). A byte per slot at +0x13bc; **bit 1 = drawn**. 0x4a33c0 walks them so and skips a slot whose actor is in state 7 to 11 | Checked; that bit 1 is "drawn, not sheathed" is inferred from the query's name |
| Which slot is which | The EquipmentUser's blackboard names its dynamic slots, in this order: `EquipmentUser_Weapon`, `_SubWeapon`, `_Shield`, `_Bow`, `_Tool`, `_SubTool`, `_Accessory1` (`Component/Blackboard/BlackboardParamTable/EquipmentUser`). So slot 0 sword, 2 shield, 3 bow or slingshot, 4 the held tool (hookshot, boomerang, bombs, ocarina...) | Inferred from the order; the 8th slot is unnamed |
| Bones | `Player_Adult`'s EquipmentUserParam: sword in hand `Weapon_L`, shield in hand `Weapon_R` and on the back `Pod_A`, bow `Weapon_R`, sheathed sword `Pod_B`; `HiddenMaterialGroupList` names the body materials an armor hides (`G_Body`, `G_Boots`, `G_Glove`, ...) | Checked (romfs) |
| Animation | The node `ExecuteEquipmentSyncASCommandWithEquipmentUser` (run 0x26a92b0) runs on the equipment actor: its component +0x1f8 holds a link (+0x20) to the wearer, and it plays the wearer's slot-0 AS command on the piece, so a sword or shield animates with Link | Checked |
| Equip from game data | 0x2df6c80(EquipmentUser, slot kind): reads `PlayerStatus.Equip*` from the GameData manager and calls **0x2df7b20**(EquipmentUser, slot, name, ...), which equips the named actor into a slot (slot below 0x12; state at +0x12d4 + 8 × slot) | Checked as calls; 0x2df7b20's arguments past the name were not read |

### What is read and sent

`PlayerReader::read_equipment()` fills `PLAYER_EQUIP`:

- The eight `PlayerStatus.Equip*` flags ([items.md](items.md)) are String64 flags. Their table is GameData type 16 at manager + 0x610 (the pattern of [flags.md](flags.md)): entries of 0x108 bytes, `char*` to the text at +0x68, `s32` buffer size (64) at +0x70, as the getter **0x3758110** reads them. `GameFlags::get_string64` reads the table, not the write queue, so a change shows one GameData update later. A text that is not printable ASCII or not ended within its buffer is dropped.
- `drawn` is bit *i* for slot *i* of the player's EquipmentUser, set when the slot holds a live actor and its byte has bit 1.
- The sampler sends it on the first frame and on every change; the client sends it again to every newcomer. Age comes from `PLAYER_SAMPLE`.

### How the puppet applies it (for the puppet spawner)

1. **The pack.** Give `CoopPuppet_Child` and `CoopPuppet_Adult` the player's `EquipmentUserRef` (`Component/EquipmentUserParam/Player_<Child|Adult>`, as `Player_Event` has it) and its blackboard table. Without it the puppet has nowhere to put a piece. A change to `tools/puppet_pack` (`KEEP_COMPONENTS`).
2. **Weapons, shield, bow, tool at creation.** The EquipmentUser's slot names are blackboard **instance** parameters (`IsInstanceParam`), so the create request can carry them, as event scenes do for `Player_Event`: `EquipmentUser_Weapon` = the received `weapon`, `EquipmentUser_Shield` = `shield`. Where the create argument holds instance parameters (+0x40, +0x48 in the table at the top) is not read yet.
3. **A change after creation** (a new sword, a shield taken off): the plain way is to delete and create the puppet again with the new names, as an age change does. Faster later: call 0x2df7b20 on the puppet's EquipmentUser with the slot and name from the hook routine (a fourth command kind beside create and play), once its arguments are read.
4. **Armor** (tunic, boots, mask, gauntlets): the same 0x2df7b20 path with the armor slot kinds 0x2df6c80 uses; the blackboard has no keys for armor, so this needs the call, not a creation parameter. Until then the puppet shows the default Kokiri tunic.
5. **Drawn or sheathed**: the equipment state comes with the AS commands the puppet already plays (the game draws the sword in the draw animation), and `OneShotEquipmentUserRequestChangeState` and `OneShotPlayerRequestAllEquipmentSheathed` are the nodes that set it; their run slots were not read. Use `drawn` to check the puppet matches, and to correct it once the change-state call is known.

### Not done (equipment)

1. Nothing was run. To check: equip the Goron tunic and the Hylian shield, read the flags and the drawn byte while drawing and sheathing.
2. The name of the actor in each slot (for a held tool such as the hookshot or bombs, which are not in `PlayerStatus.Equip*`): the actor's name field was not found. The item buttons are `PlayerStatus.AssignableItem.*` (an enum, [items.md](items.md)).
3. 0x2df7b20's other arguments, the armor slot kinds, the change-state call, and where a create request carries blackboard instance parameters.
4. String64 flags still in the write queue (the getter checks an object at manager + 0x608 first).

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

## Pack

`tools/puppet_pack` builds the two puppet packs on the player's PC from their own game files. Nothing from the game is in the repository: the tool reads the romfs and writes a LayeredFS mod.

```
<repo venv python> tools/puppet_pack/build.py --romfs <extracted romfs> --user-dir <emulator>\user
```

That writes the mod `user\load\0400486024DEC000\coop-puppet\` (`--mod-dir` picks another folder, `--mod-name` another name). Python 3 with the `zstandard` package. It never changes the romfs, and it rebuilds the tables from the game's copies each time, so running it again is safe.

| Mod file (under `romfs\`) | What |
|---|---|
| `Pack\Actor\CoopPuppet_Child.pack.zs` | 10 files: our ActorParam and the player's own component files it refers to |
| `Pack\Actor\CoopPuppet_Adult.pack.zs` | 9 files, the same for adult Link |
| `RSDB\ActorInfo.Product.111.rstbl.byml.zs` | The game's table plus a row for each puppet |
| `RSDB\GameActorInfo.Product.111.rstbl.byml.zs` | The game's table plus a row `{__RowId}` for each |
| `System\Resource\ResourceSizeTable.Product.111.Nin_Ounce_NVN2.rsizetable.zs` | The game's size table plus the new entries |
| `..\coop_puppet.json` | What was made, from which files (with their SHA-256) |

The puppet's ActorParam has no `$parent`: it is `Player_<Child|Adult>` merged with `Player_Base`, keeping only `ASRef`, `ASInfoRef`, `ASOptimize`, `AnimationRef`, `ModelInfoRef`, `ModelBindRef`, `BoneBindRef`, `LODRef` and `LocalDepthShadowRef`. It adds the shared defaults the simple event actors use (`ActorLogicSimpleActor`, `PreActorSetting/ActorOnly`, `ActorSystemSetting/GameActorDefault`, `WorldInfoParam/Default`, `XLink/ActorBasic`). Left out: input, camera target, physics, equipment, life, AI, the `Player` actor name, ELink and SLink (effects and sound; add later if wanted). The child puppet uses `AS/Player_Child.root.asb` and the `Player_Child` model, the adult one `AS/Player_Adult.root.asb` and `Player_Adult`, with the player's own animation resource lists.

What the game needs for a new actor, read from the files (**checked**) and the TotK engine (**inferred**):

| Item | Finding | Status |
|---|---|---|
| Pack format | SARC, zstd with the `pack.zsdic` dictionary (id 4 in this game; the tool picks it by name and the frame's id, never a fixed number). Members sorted by name hash, data aligned to 8. A SARC the tool writes from a game pack's members is byte-identical to the game's | Checked |
| What a pack holds | The actor's ActorParam and every component file it refers to that is not shared. Shared defaults (`LODParam/Default`, `XLink/ActorBasic` and the like) are in no actor pack. Models, AS and animations are loose romfs files (`Model/`, `AS/`) found by name | Checked |
| `RSDB/ActorInfo` | Every actor has a row (8,363 rows, sorted by `__RowId`): model name, animation resources, radii, `InstanceHeapSize`. The puppet's row is the player's with `ActorName`, `ELinkUserName` and `SLinkUserName` removed, `CalcRadius` 100000 and `CoreUsedLevel` 0 | Checked as data; that a create fails without a row is inferred from TotK |
| `RSDB/GameActorInfo` | Every actor has a row too (8,363); `Player_Event` has only `__RowId` | Checked as data |
| Size table | `RESTBL` v1: (crc32 of the path without `.zs`, size) rows sorted by crc, then 10 named rows for colliding paths. Packs are listed as `Pack/Actor/<name>.pack` with about the unpacked size plus 330 bytes; an ActorParam with about 6 to 8 KB. New entries: the pack (unpacked size rounded up plus 4 KB) and our ActorParam (the player's value). The two RSDB tables' entries are raised by their growth plus 4 KB | Checked as data; that the game needs them is inferred from TotK |
| RSDB and size table compression | zstd without a dictionary | Checked |
| BYML | Version 7. The tool's writer gives the same size as the game's for ActorInfo and GameActorInfo (byte-identical for GameActorInfo and the pack members) because equal nodes are written once | Checked |
| Uncompressed `.pack` | Not tried: the tool writes `.pack.zs` like the game | |

**Load test.** `tools/puppet_pack/place_test.py` adds actors to one scene file of a built mod (a throwaway one), for example both puppets and a cow as a control in `Banc/KakarikoVillage/KakarikoVillage_Child_Dynamic.bcett.byml.zs`. Run once (2026-11-08) in a portable copy of the kit (Fritchii v5 coop build, `.nup` loaded with `-g`): the game booted and played with the mod, but **the mod was not applied at all**. The log has `Patching RomFS` and never `RomFS: LayeredFS patches applied successfully`, and a second run with a deliberately broken 64-byte `ActorInfo` in the mod folder also booted into Kakariko with every actor. So on this build `user\load\<title id>\<mod>\romfs` does nothing for a `.nup`, even though `RomFSFactory::OpenCurrentProcess` calls `PatchRomFS` for it; which early return in `ApplyLayeredFS` it takes was not found. Until the emulator applies LayeredFS to a `.nup` (a `lichii` patch, or loading the game another way), the packs cannot be proven in game.

What is proven: the packs, tables and size table parse back with the tool's readers, the tables keep their sort order, and the tool's own tests (`tools/puppet_pack/tests/test_puppet_pack.py`, synthetic data) pass.

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
