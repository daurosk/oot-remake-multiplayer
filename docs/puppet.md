# Puppet Link: research

Plan stage 1 step 2 ([plan.md](plan.md)): show each remote player as a second Link in the world, driven by their samples. This file says where the game creates actors, which actor to spawn, where its pose and animation live, where to hook a frame, and how the emulator gets code to run there. The spawn, follow and delete were run in game later: see [Proven in game](#proven-in-game-2026-11-08).

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
| +0x40 | `const char*`, default `""`: 0x3ee4f8c is the empty string between `ToPlayerDeg` and `PlayerToTargetDiffDeg` in rodata, and the GOT entry 0x4c7dad0 the director copies is the same kind of empty-string default (2669 uses in main, picked with `csel` against a real string). Not an instance-parameter block | Checked (SeqExecuteCreateActor 0x32bca58..0x32bca80) |
| +0x60 | Pointer; SeqExecuteCreateActor copies `[[node +0x60]]` here when it is not null, else 0. The likeliest carrier of the node's instance parameters (EquipmentUser_* for gear at create) | Store checked; meaning not read |
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
| Current AS frame | Controller +0x20 is an array of slots, controller +0x18 the slot count (see Animation); the float at entry +0xe8 is the frame. Read by 0x3abebc0 | Checked as a read; that +0xe8 is the frame is inferred from the `ASFrame` plug of `ExecuteASPlaySetASFrameEachFrameByPlug` |
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

Alex (2026-11-09): the puppet wears the remote player's tunic, boots, sword, shield and mask, and holds what they hold, drawn or sheathed. Read from the dump and the romfs, then checked in the game on 2026-11-09 (child Link, one emulator: shield Hylian and Deku, sword drawn and sheathed, shield raised, a held item taken out; no second sword, tunic, boots or mask to try). Message: `PLAYER_EQUIP` ([protocol.md](protocol.md)).

### How the game shows equipment

Every piece is **an actor of its own**, created by the wearer's EquipmentUser component and bound to the wearer's bones. Nothing is a texture or a flag on the Link model.

| Item | Finding | Status |
|---|---|---|
| Pieces are actors | `Weapon_Sword_Kokiri`, `Weapon_Shield_Deku`, `Armor_Adult_Goron`, `Armor_Child_Mask_Keaton`, `Item_Ocarina_Time` and so on each have their own pack in `Pack/Actor/`. Weapons have `ModelBindRef` `EquipmentCommon`; armor has `ArmorRef`, `SameBoneModelBindRef` (bound bone for bone onto the body) and its own model, so a tunic is a second model laid over Link's | Checked (romfs) |
| The wearer's EquipmentUser | Actor component table (+0x270) **+0x200**: 0x7f7690 reads it, used by `OneShotPlayerEquipDynamicEquipmentFromGameData` (run 0x290b1b0) and `QueryEquipmentUserIsEquipStateDrawnAny` (0x29f1f90) | Checked |
| Its slots | EquipmentUser +0x20: 8 slots of 0x18 bytes, each a link to the equipment actor (holder pointer +0x8, u32 id +0x10, ~0 empty; holder +0x40 the actor, +0x48 its id, the same shape as the player link). A byte per slot at +0x13bc; **bit 1 = drawn**. 0x4a33c0 walks them so and skips a slot whose actor is in state 7 to 11 | Checked. **Run**: bit 1 is set while the piece is out (sword drawn, shield raised, held item out) and clears when it is put away; bit 2 is set for a moment while a piece is created (actor state 3) |
| Which slot is which | The EquipmentUser's blackboard names its dynamic slots, in this order: `EquipmentUser_Weapon`, `_SubWeapon`, `_Shield`, `_Bow`, `_Tool`, `_SubTool`, `_Accessory1` (`Component/Blackboard/BlackboardParamTable/EquipmentUser`). The order guessed slot 2 for the shield; the game says otherwise | **Run** (child): slot 0 the sword, slot 4 **the shield**, slot 3 a held item; 1, 2, 5, 6, 7 stayed empty. So the slot index is not the blackboard order: use the slot as `kEquipSlot*` in `addresses.hpp` names it. A shield change makes a new shield actor (new id, sometimes a new address); the sword actor stays. Adult slots not run (no adult save, 2026-11-09) |
| Bones | `Player_Adult`'s EquipmentUserParam: sword in hand `Weapon_L`, shield in hand `Weapon_R` and on the back `Pod_A`, bow `Weapon_R`, sheathed sword `Pod_B`; `HiddenMaterialGroupList` names the body materials an armor hides (`G_Body`, `G_Boots`, `G_Glove`, ...) | Checked (romfs) |
| Animation | The node `ExecuteEquipmentSyncASCommandWithEquipmentUser` (run 0x26a92b0) runs on the equipment actor: its component +0x1f8 holds a link (+0x20) to the wearer, and it plays the wearer's slot-0 AS command on the piece, so a sword or shield animates with Link | Checked |
| Equip from game data | 0x2df6c80(EquipmentUser, slot kind): reads `PlayerStatus.Equip*` from the GameData manager and calls **0x2df7b20**(EquipmentUser, slot, name, ...), which equips the named actor into a slot (slot below 0x12; state at +0x12d4 + 8 × slot) | Checked as calls; 0x2df7b20's arguments past the name were not read |

### What is read and sent

`PlayerReader::read_equipment()` fills `PLAYER_EQUIP`:

- The eight `PlayerStatus.Equip*` flags ([items.md](items.md)) are String64 flags. Their table is GameData type 16 at manager + 0x610 (the pattern of [flags.md](flags.md)): entries of 0x108 bytes, `char*` to the text at +0x68, `s32` buffer size (64) at +0x70, as the getter **0x3758110** reads them. `GameFlags::get_string64` reads the table, not the write queue, so a change shows one GameData update later. A text that is not printable ASCII or not ended within its buffer is dropped.
- `drawn` is bit *i* for slot *i* of the player's EquipmentUser, set when the slot holds a live actor whose last change of state was into the hand (the byte's bit 1 is a request that lasts a moment; see [In hand or put away](#in-hand-or-put-away-built-not-run-in-the-game)). Seen in the game: 0x01 sword drawn, 0x10 shield raised, 0x08 held item out, 0x00 all put away.
- Seen in the game: `weapon` `Weapon_Sword_Kokiri`, `shield` `Weapon_Shield_Hylian` / `Weapon_Shield_Deku`, `ocarina` `Item_Ocarina_Fairy`; the other five flags are "" for a child with nothing there. Before a save loads every flag is "" and `drawn` is nothing (no player actor).
- `held` is the name of the actor in slot 3 (`PlayerReader::read_held`): the slot's link to its actor, as `drawn` follows it, then the actor's name (a `char*` at actor +0x260). "" when the slot is empty. The flags name the ocarina and the arrow kind, not what is in hand.
- The sampler sends it on the first frame and on every change; the client sends it again to every newcomer. Age comes from `PLAYER_SAMPLE`.

### How the puppet applies it (for the puppet spawner)

1. **The pack.** Done: `CoopPuppet_Child` and `CoopPuppet_Adult` carry the player's `EquipmentUserRef` (`Component/EquipmentUserParam/Player_<Child|Adult>`, as `Player_Event` has it). Its `$parent` chain (`Player` → `Default` for the child, `Default` for the adult) comes along, and so do the blackboard tables its `BlackboardTableRef` names (`Player_Child` → `Player` → `WeaponUser`, and `EquipmentUser`). The player's own `BlackboardRef` stays out, as in `Player_Event`. The builder fails if the chain names no table with `EquipmentUser_*` instance keys, and lists the keys in `coop_puppet.json` (`equipment_keys`).
2. **Everything worn, at creation.** The table the player's EquipmentUserParam names is `Player`, not `EquipmentUser`: its **instance** parameters (`IsInstanceParam`) are `EquipmentUser_Weapon`, `_Shield`, `_Bow` (default `Weapon_Range_Bow`), **`_ArmorHead`, `_ArmorUpper`, `_ArmorLower`** (checked, romfs, child and adult). So the create request can carry the armor too: `EquipmentUser_Weapon` = the received `weapon`, `EquipmentUser_Shield` = `shield`, `EquipmentUser_ArmorUpper` = the tunic, `_ArmorLower` = the boots, `_ArmorHead` = the mask. No scene of the game sets these keys in a placement's `Dynamic` table (all 160 `.bcett` files read); event scenes must set them another way. Where the create argument holds instance parameters (+0x40, +0x48 in the table at the top) is not read yet.
3. **A change after creation** (a new sword, a shield taken off): the plain way is to delete and create the puppet again with the new names, as an age change does. Faster later: call 0x2df7b20 on the puppet's EquipmentUser with the slot and name from the hook routine (a fourth command kind beside create and play), once its arguments are read.
4. **Armor** (tunic, boots, mask): at creation through the `EquipmentUser_Armor*` keys above. A change after creation is a re-create, or the 0x2df7b20 call with the armor slot kinds 0x2df6c80 uses. Gauntlets and the Stone of Agony have no key; they need the call.
5. **Drawn or sheathed**: the equipment state comes with the AS commands the puppet already plays (the game draws the sword in the draw animation), and `OneShotEquipmentUserRequestChangeState` and `OneShotPlayerRequestAllEquipmentSheathed` are the nodes that set it; their run slots were not read. Use `drawn` to check the puppet matches, and to correct it once the change-state call is known. A cruder correction, not tried: set bit 1 of the puppet's byte at EquipmentUser +0x13bc + *i*.
6. **Slots.** Address pieces by slot as `addresses.hpp` names them (`kEquipSlotSword` 0, `kEquipSlotHeldItem` 3, `kEquipSlotShield` 4, seen in the game), not by the blackboard order, which would put the shield at 2.

### What the director does (built, s2-puppet-live-4)

Research (s2-equip-create) showed that create argument +0x40 is a name pointer, not instance parameters, and that `EquipmentUser_*` keys are only worn through an AI node the puppet does not run. So the director wears gear with the game's equip call instead, once the puppet is live:

- `OnEvent` keeps each remote player's merged `PLAYER_EQUIP` (weapon, shield, head, armor, boots); `Publish` passes it on as `Target::gear`.
- For each piece whose name differs from what the puppet wears, one call `0x2df7b20(eu, kind, &char*, 2, -1, 1, 0)` with `eu = [[actor + 0x270] + 0x200]` and kinds 0, 6, 7, 8, 9. Each kind has its own name cell in the puppet's block (+0x300, 0x40 each; the char* cells at +0x440). Log: `P<n> wears <piece> '<name>'`.
- An empty name (nothing in that slot) is not taken off yet; a gear change later is the same call, no re-create. A re-create wears everything again.
- Proven in game (2026-11-09, autotest `gear` and `gear-change` on one PC): both puppets wear the other player's Kokiri Sword and Hylian Shield (`wears ... (returned 0x1)`), and when A swaps to the Deku Shield in the menu, B's puppet of A swaps too (Hylian on its back before, Deku after). The pack must carry the EquipmentUserParam (main since PR 52): with an older pack the EquipmentUser is 0 and nothing is worn.
- Not run: tunic, boots, mask (none in the test save), taking a piece off (an empty name is skipped), adult Link.

### In hand or put away (built, not run in the game)

Alex's hand test (2026-11-09): only the shield showed on puppets, always on the back, also while its player held it on the arm or guarded. The logs of that test show the sword was worn too (`wears weapon ... (returned 0x1)`); a piece is worn put away and nothing ever changed that.

Read from the dump (static, not run):

| Piece | What |
|---|---|
| Change state | `0xe34c40(EquipmentUser, slot, state)`. State 1 is in hand (it runs 0x6b6820; the shield node 0x2797850 calls it with slot 4, state 1), state 2 is put away on the back (0xe34c70, the state a piece is worn in), state 0 goes to 0x2dfdee0 |
| The slot's byte | EquipmentUser +0x13bc + slot: bit 1 (0x2) in hand, bit 2 (0x4) put away. 0xce7ab0(eu, slot) reads the current state |

What the director does (`Director::Hold`), for a live puppet, after its gear is worn:

- `Target::drawn` is the player's `drawn` bits from `PLAYER_EQUIP` (the test puppet gets our own).
- For the sword (slot 0) and the shield (slot 4): when the player's bit differs from what the puppet was last set to, one call of the change-state function with state 1 or 2. A piece just worn counts as put away, so only "in hand" is asked then.
- After the call the slot's byte is read: with the wanted bit set the change is taken and logged, `P<n> equipment slot <s> in hand` or `put away`. Otherwise the call is made again after 250 ms (`not taken (byte ..)` with `LICHII_COOP_DEBUG_ANIM`).
- Where the piece sits (hand bone, arm, guard) is the game's own: the state picks the bone, and the pose copy moves the arm.

**Two-player run (Alex, 2026-11-09, main 2e3f68b): "swords and equipment didn't show".** What the logs of that run say:

- Each time a player drew the sword, the other game logged `P<n> equipment slot 0 in hand` and, about 40 ms later, `put away`. So the call is taken, but the player's `drawn` bit lasted one or two frames. The byte at +0x13bc is a **request**, not the state: the setters write it (0x6b6820 writes 2, 0xe34c70 writes 4, also when a piece is created) and the game clears it when the change is done. 0xce7ab0 reads the state from the piece itself once the byte is 0. "Bit 1 is set while the piece is out" (above) was wrong.
- Fix (`PlayerReader::read_drawn`): the reader remembers the last request seen for each slot (2: in hand; 4 or 1: not) until the next one; a piece the game makes anew starts put away, as the game makes it. The readers of all frames share one `PlayerReader::DrawnState` (the hook and `Session` keep it).
- The object log of the same run shows every Link, puppets too, with three pieces: `Weapon_Sword_Kokiri`, `Weapon_Sheath_Kokiri` and `Weapon_Shield_Hylian`, all in state 4, the sword 0.88 above the feet on the player and on the puppet alike. About 90 ms after the equip calls the game makes the three again and removes the first set. So a change of state is now made again when the piece in the slot is another one (`Slot::hold_id`).
- The slot record byte (EquipmentUser +0x4b0 + 0xa8 * slot + 0xa4) that PR 73 read as "1 in hand, 0 put away" is not the state: autotest 2026-11-09 read 1 with the sword out and put away alike. Neither the reader nor the puppet uses it any more (it is only in the debug line).

**Sword and shield on the puppet, in hand and put away: seen working (autotest, 2026-11-09, main c8ea805).** Scenarios `back`, `backwarp` and `late` (`tools/autotest/scenarios`): A walks ahead of B, stands 10 s with all put away, draws the sword (Y), guards (ZL), puts away (B). On B's screen the other Link looks like A's own: the shield on the back when put away and over the back while the sword is out, the sword in the hand on Y (also seen mid-swing). On a child Link the put-away sword sits under the shield, on the player too. The debug lines give the same `up`/`off` for the puppet's pieces as for the player's (sword 0.88/0.16 on the back, 0.41 then 0.14 in the hand; shield 0.72/0.19).

What else the runs showed:

- A puppet equips itself: `CoopPuppet_Child` gets sword, sheath and shield about 65 ms after its actor exists, from the **local** game's `PlayerStatus.Equip*` flags. Our equip calls then make the pieces again and the game removes the first set.
- **A parked puppet leaves its pieces behind.** While the puppet is parked 500 under the floor (`Show::hide`, our event or scene exit), its sword and shield read `up 500.88`: they stay where they were. In Alex's run (e2ed2aa) the second set was made while the puppet was parked (first seen at y = -339.87), which would leave it 500 under the puppet once it comes up. So: no equip or in-hand call is made while the puppet is parked (`Director::Equip`, `Director::Hold`), and a worn sword or shield found more than 5 from its shown puppet is worn again, at most once a second (`Director::LeftBehind`, log `P<n> weapon '..' left behind (<distance> away): worn again`). Built and unit tested; the left-behind case itself was not reached in a run (a puppet came back from a park only by a scene change, which deletes it).
- Tunic, boots and head pieces go through the same equip call; the test saves have none, so they are not seen.

With `LICHII_COOP_DEBUG_ANIM=1` every live puppet logs, every 2 s, after its `gear:` line:

- `P<n> slots (puppet <actor>): eu ..; slot <s>: id .. actor .. state .. up <height over the wearer> off <distance on the ground> request .. record .. flags .. want hand|away` for every equipment slot that holds a piece.
- `P<n> slots (local player <actor>): ...` the same for the local player.

`up` near 0.9 and `off` under 1 is on the back; `up` near -500 or a large `off` is a piece left behind; `request` not 0 for seconds means the puppet's equipment user never works its requests off.

A run that checks it: `tools/autotest/run.ps1 -Scenario back -Switches "LICHII_COOP_PUPPET=1,LICHII_COOP_OBJECTS=1,LICHII_COOP_DEBUG_OBJECTS=1,LICHII_COOP_DEBUG_ANIM=1" -ScreenCopy`, with `-Server` a protocol-3 kit server; look at B's screenshots and its `slots (puppet` lines.

**In-hand change: seen working** in the same runs: B logs `P1 equipment slot 0 in hand` when A draws and `put away` only when A puts it away. Guarding (ZL) with the sword out moves the shield with the arm (pose copy); the shield slot itself stays put away, on the player too.

### The held item (slot 3)

Alex's hand test (2026-11-09): "sword appears in hand, slingshot does not". The slingshot is not in any `PlayerStatus.Equip*` flag, so nothing told the other games about it.

- **Where it is.** Slot 3 of the EquipmentUser is the bow slot: the equip call 0x2df7b20 tests slot 3's link (EquipmentUser +0x70, +0x78) when its slot kind is 5, as it tests slot 0's for kind 0 and slot 1's for kind 1 (read from the dump). A child with the slingshot has the actor `Weapon_Range_SlingShot` there all the time: in state 6 while it is put away (`slot 3: ... state 6 ... flags 0x804` in the local player's debug line), and `drawn` bit 3 is set while Link aims.
- **Sent.** `PLAYER_EQUIP` component 10, `held`: the name of the actor in slot 3 ([protocol.md](protocol.md)).
- **Worn.** The director treats it as a sixth piece (`kGearHeld`): one equip call with kind 5 and the name, then the same change-state call as the sword on slot 3 when `drawn` bit 3 changes. An empty name takes nothing off; the piece is put away instead. The left-behind check looks at it only while it is in the hand.
- **Test.** `tools/autotest/run.ps1 -Scenario held` (A aims the slingshot, fires, puts it away, aims again). B's log: `P1 wears held 'Weapon_Range_SlingShot'`, `P1 equipment slot 3 in hand`, `put away`.

### The other hand slots (issues 92 and 91, branch `held-items-2`)

Read from the dump (static; nothing of this was run):

- **The game's two enums.** In rodata at **0x3ef2033** are the names of the eight hand slots of the EquipmentUser, in order: `Weapon`, `SubWeapon`, `ThirdWeapon`, `Bow`, `Shield`, `Tool`, `SubTool`, `Helmet` (slots 0 to 7; the sword in 0, slingshot in 3 and shield in 4 agree with what was seen). At **0x3ef94a8** are the 18 slot kinds the equip call 0x2df7b20 takes: 0 `Weapon`, 1 `SubWeapon`, 2 `ThirdWeapon`, 3 `Sheath`, 4 `Quiver`, 5 `Bow`, 6 `Shield`, 7 `ArmorHead`, 8 `ArmorUpper`, 9 `ArmorLower`, 10 `ArmorHand`, 11 `Tool`, 12 `SubTool`, 13 to 16 `Accessory1` to `4`, 17 `Extra` (the kinds 0, 5, 6, 7, 8, 9 the puppet already used agree). The pack keys `EquipmentUser_Tool`, `EquipmentUser_SubTool`, `EquipmentUser_Bow` and `EquipmentUser_Accessory1` to `3` are the same names.
- **The game's actor names** for things a hand can hold (strings in main): `Item_DekuStick`, `Item_Boomerang`, `Item_Ocarina_Fairy`, `Item_Ocarina_Time`, `Weapon_Hammer`, `Weapon_Range_Bow`; for the hookshot only the pouch item `Item_Hookshot` and the shot actors `Shootable_Hook` / `Shootable_HookLong` ([objects.md](objects.md)). Worn: `Armor_Adult_Goron`, `_Zora`, `_Normal`, `Armor_Adult_HoverBoot`, `_HeavyBoot`, `Armor_Adult_Gauntlet_Silver`, `_Gold`, `Armor_Child_Gauntlet_Goron`, eight `Armor_Child_Mask_*`. Which hand slot each tool goes in was not read: the game decides it in each item's own pack and AI, so the hook sends every slot instead of guessing.
- **Sent.** `PLAYER_EQUIP` components 11 to 15 ([protocol.md](protocol.md)): the actor name in slots 0, 1, 2, 5 and 6, read as `held` is (`PlayerReader::read_slot_name`, the slot's link to its actor, the actor's name at +0x260). Slot 7 (`Helmet`) is the mask, already sent as `head`. Bombs in the hand are an actor of their own (`Item_Bomb`), not in a slot: the objects sync shows them.
- **Worn.** The puppet had eleven pieces (`kGearCount`, thirteen with the Ocarina below): the five before, the held item (kind 5, slot 3), the gauntlets (`hand`, kind 10, no hand slot) and one piece for each of slots 1, 2, 5 and 6 (kinds 1, 2, 11, 12). Each gets the same equip call `0x2df7b20(eu, kind, &char*, 2, -1, 1, 0)` and, for the slot ones, the same change-state call on its slot when its `drawn` bit changes. What is really in slot 0 (a Deku stick) goes in place of the equipped sword's name. The tool pieces, like the held item, are checked for "left behind" only while in the hand. The puppet's block is 0x700 now (0x740 since the Link sounds' handles; name cells from +0x300, thirteen char* cells at +0x640).
- **The Ocarina (issue 152, branch `ocarina-hand`).** Testers: the playing animation reached the other games, the Ocarina did not. It is no hand slot: the player wears it as two accessories on the belt, `Item_Ocarina_Fairy_Holder` and `Item_Ocarina_Fairy` (or `_Time`; actors of their own, seen in every walk next to Link). Both packs have an `AccessoryParam`, and the player's `EquipmentUserParam` binds `Accessory1` and `Accessory4` to the bone `Pod_B` (the belt; `Accessory2` to `Ph_Bangle`, `Accessory3` to `Spine_2`). The playing animations (`Ocarina*`, which the puppet already plays) take it in hand. The puppet now wears the two from `PLAYER_EQUIP`'s `ocarina` (the `EquipOcarina` flag): the holder as kind 13 (`Accessory1`) and the Ocarina as kind 16 (`Accessory4`), with the same equip call; none while the player has none. Not known until a run: that these are the accessory slots the player uses (the two that bind to `Pod_B`), and that the puppet's playing animation moves the Ocarina to its hand as it does the player's. If it stays on the belt, the hand bind is in the player's own code, not the animation.
- **The Ocarina in hand (issue 152, branch `ocarina-hand-2`).** It stayed on the belt (run 20261111-192211: B's puppet played with empty hands). The hold probe (`LICHII_COOP_OCARINA_PROBE=1`) showed the player's own code moving it: A plays with the same `Item_Ocarina_Fairy` actor, and in that actor's first component block (its model bind) the bone index at +0x13a goes from 0x133 (`Pod_B`) to 0x44, the 3x4 offset matrix at +0x140 becomes identity, and the bone name at +0x194 goes from `Pod_B` to `Weapon_R`; all three come back after. `CoopOcarina::HandBind` (`coop_ocarina.{h,cpp}`) does the same on the puppet's Ocarina (never its holder) while their AS command (or the puppet's last play) contains `Ocarina` and it is not hidden (free play sends `DemoOcarina` and holds the puppet for their cutscene, so nothing new is played on it then): it saves the bone word and matrix, writes bone 0x44 and identity every frame, and writes the saved words back when the command changes. It writes only when the bind is exactly a belt one (name `Pod_`, bone `Pod_B`); anything else is logged once (`P1 ocarina not moved: ...`) and left alone. The bone numbers are the skeletons' own (romfs `Model/Player_<Age>.Player_<Age>.bfres`, the bone index at +0x20 of each 0x58-byte bone): child `Pod_B` 0x133 to `Weapon_R` 0x44, and since branch `ocarina-sound-b` adult `Pod_B` 0x145 to `Weapon_R` 0x42 (before, an adult's Ocarina stayed on the belt). The name is not written. Log: `P1 ocarina to the hand ('DemoOcarina')`, `P1 ocarina back on the belt ('Wait')`; with `LICHII_COOP_DEBUG_ANIM=1` also `P1 their command '...'`. Proven in run 20261111-204153 (scenario `ocarina-hand`): the Ocarina in the puppet's right hand at its face while A plays, on the belt before and after.
- **Tests.** `tools/autotest/scenarios/held-tools.A.txt` (child: boomerang, sticks, ocarina, bottle), `held-adult.A.txt` (bow, hookshot, hammer, bombs; `-NoLoad`), `wear-adult.A.txt` (tunics, boots, gauntlets; `-NoLoad`), `wear-masks.A.txt`. The test `wear` command takes `hand` for the gauntlets.
- **Not known until a run:** whether kinds 1, 2, 11, 12 put those tools in the puppet's hand (the slingshot's kind 5 is not proven either), whether a tool needs more than its slot (an ocarina is played with its own animation), and whether a bottle is an actor in a slot at all.

### Taking a piece off (issue 91, branch `held-gear`)

Seen in a test (held-tools, 2026-11-10): the Deku stick and the bottle go into the puppet's hand on slot 1 (SubWeapon), and the slingshot on slot 3. A Deku nut or a bomb is no slot piece: it is a thrown object, which the objects sync shows.

An empty name means nothing in that slot, and how to take a piece off a puppet is not known (no unequip call is proven). Two cases:
- **Carried pieces** (the held item, slots 1, 2, 5, 6): put away, as the player's own are.
- **Worn pieces** (sword, shield, mask, tunic, boots, gauntlets): the old code left them on, so a mask taken off stayed on the other Links. Now a worn piece whose name stays empty for 1.5 s (`Director::kTakeOffMs`, so a moment of nothing during a load does not count) is taken off: armor with the game's call (below), a sword or shield by deleting the puppet and making it again without it. The log says `P<n> takes off <kind> '<name>' (returned 0x1)` for armor (next paragraph) and `P<n> takes off <kind> '<name>': puppet made again without it` otherwise. The puppet is gone for a moment then. The test command `wear <slot> none` empties a flag. `wear-masks` ends with the mask off, and `wear-adult` ends with the green tunic, no boots and no gauntlets.

**Armor comes off with the game's own call (branch `puppet-look`).** Read from the dump: when a `PlayerStatus.Equip*` armor flag is "", the game's GameData path 0x2df6c80 calls **0x10aceb0(EquipmentUser, index, 0)** with index 0 for kind 7 (head), 1 for kind 8 (upper), 2 for kind 9 (lower), 3 for kind 10 (hand). That function empties the EquipmentUser's armor link `index` (EquipmentUser + 0xe0 + 0x18 * index, 13 links shaped like the hand slots: holder +0x8, id +0x10) and deletes its piece (for index 0 it first runs 0x4b8240 on the wearer). It returns 0 when a piece was there and it did not take it off. The director now makes this call for a mask, tunic, boots or gauntlets that stays "" for `kTakeOffMs`: log `P<n> takes off <kind> '<name>' (returned 0x1)`, and the puppet stays. A 0 result, a sword or a shield (no call known) still make the puppet again without it. With `LICHII_COOP_DEBUG_ANIM` the `slots (...)` lines also list the armor links (`armor <i>: id .. actor .. state .. up .. off ..`). Not run.

**Masks did not show (chunk 5 shots, `wear-masks` 20261110-155536).** B's log had `wears head 'Armor_Child_Mask_*' (returned 0x1)` and the object log a new `Armor_Child_Mask_*` actor each time, but no mask was drawn on the puppet's face at any mark (the tunics of `wear-adult` did show: dark red, then blue-green, then green). The test only set A's flag, so A's own Link wore no mask either, and how the game itself wears one was never seen. Masks (and the adult boots) are ZR items (`AssignableItem` `SkullMask`, `HoverBoot`, ...): the scenarios `wear-masks-real` and `wear-boots-real` put them on as a player does (give, `assign`, ZR) and take them off (ZR again), so A's `slots (local player` lines show where the game keeps a worn mask. **A mask is worn in state 1 (branch `puppet-looks`, not run yet).** The equip call keeps its state per kind (EquipmentUser + 0x12d4 + 8 * kind: `{1, state}`, applied when the piece is made), and the GameData path picks 1 or 2 per piece (0x2df7850: `cinc`). Through that path, with 2, the game drew no mask on the player's own Link either (`wear-masks` above), as a sword with 2 sits on the back. So the puppet's head piece now gets state 1; every other piece keeps 2. `LICHII_COOP_HEAD_STATE=2` goes back. The debug `slots (...)` lines end with `kinds k:word/state` for every kind the EquipmentUser holds, for the local player and the puppet alike, so `wear-masks-real` shows the state the game itself gives a mask put on with ZR.

The mask pack (`Armor_Mask_Base`) differs from a tunic's: `SameBoneModelBind` `ArmorCommon` (RotTrans), no AS or physics, and a `MaskParam` with face UVs (`UV`, `UVForAdult`) and hair meshes set to no simulation, which may be applied by the player's own code.

The adult scenarios (`held-adult`, `wear-adult`) now play in Kakariko by day with both players adult (as `adult-day`). In the dark adult Graveyard the shots showed nothing. `hold ZR 90 item1` takes its mark halfway through the hold, while the aimed item is still in the hand.

### Not done (equipment)

0. Which hand slot each tool uses is still not seen in a game (see "The other hand slots"): the scenarios `held-tools` and `held-adult` show it.

1. Not run: a tunic, boots, a mask or a second sword (none in the test save), adult Link's slots, and which item was in slot 3. Run them with a later save: the local debug log (`LICHII_COOP_DEBUG_EQUIP`) prints the flags and every slot.
2. The name of the actor in each slot (for a held tool such as the hookshot or bombs, which are not in `PlayerStatus.Equip*`): the actor's name field was not found. The item buttons are `PlayerStatus.AssignableItem.*` (an enum, [items.md](items.md)).
3. 0x2df7b20's other arguments, the armor slot kinds, the change-state call, and where a create request carries blackboard instance parameters.
5. Where the puppet's Deku shield came from (see the pack load test) and whether the create request (not a placement) carries `EquipmentUser_*`.
6. Adult slots, tunic, boots and mask: Alex has no adult save and none of those items (2026-11-09). Run them when a save has them.
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
- **Code in the padding.** `main`'s code ends at 0x3c461d0 and its last page runs to 0x3c47000, all zero in the dump. Dynamic FPS uses 0x3c46200 to 0x3c46278. **0x3c46280 to 0x3c47000 is free** (3.4 KB). Co-op's uses of it: the svc cave 0x3c46280..0x3c46300, the AI hold cave 0x3c46300..0x3c4634c and its table 0x3c46400..0x3c46610, the pause counters' cave 0x3c46700..0x3c46728 and their ring's address 0x3c46780..0x3c46788 (platforms-b2, [objects.md](objects.md)); all are in the code-mod check (`CoopSites::ChangedSite`, issue 136), with the hooked words main+0x6e6dd8, main+0x6e70f8, main+0x369df8 and main+0x5e6760. The resource poll patch puts `svc #0xf3` in other zero padding (0x17088).
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

That writes the mod `<load folder>\0400486024DEC000\coop-puppet\` (`--mod-dir` picks another folder, `--mod-name` another name). The load folder is the one the emulator's settings name (`load_directory` in `user\config\qt-config.ini`), else `user\load`: the emulator saves that setting as an absolute path, so a copied or moved emulator folder keeps reading the old folder's mods. Python 3 with the `zstandard` package. It never changes the romfs, and it rebuilds the tables from the game's copies each time, so running it again is safe.

`--romfs` also takes the game's `.nup` file. A `.nup` (Fritchii's `src/core/file_sys/nup_container.cpp`) holds the base game, its updates and add-ons as loose RomFS trees, each file raw or one zstd frame, with a SHA-256 per file; nothing is encrypted, so no keys are needed. `tools/puppet_pack/nup.py` reads only the six files the build needs, from the base tree with the newest update laid over it (as the emulator's `AppLoader_NUP::ReadRomFS`), checks each hash, and the build runs on those. Proven 2026-11-10 on this PC's `.nup` (one standalone update package): the mod is byte for byte the one built from a dump of the same game, and the one in the release test kits on this PC. This is the way for players whose keys are too old for Dump RomFS (the emulator logs `Cannot decrypt ...: keys for gen 0xNN+ required.`; gen N needs `master_key_(N-1)`, so 0x17 needs `master_key_16`).

Before building it checks the dump with `tools/puppet_pack/dumpcheck.py` (plain Python, no zstandard), which `make-puppet-pack.cmd` also runs to find the dump. The emulator's Dump RomFS shows the same "RomFS Extraction Failed!" box for nearly every failure and writes no log line of its own, so the check looks at what the dump left: no folder (the dump failed; the emulator deletes the folder then), folders without files (Skeleton instead of Full), a missing table or pack, a file that does not start with the zstd magic (the dump stopped mid-copy: the emulator sizes each file before filling it), no `Pack` folder (the manual dumped instead of the program), or another data version than `Product.111` (the base game dumped because the update is not in the kit's NAND; `--any-version` skips that one). With no dump at all it lists the usual causes: no `keys\prod.keys` (or one folder too deep), an empty NAND, under 20 GB free, a deep kit folder. It looks in the Dump Root setting (`dump_directory` in `qt-config.ini`) and in `user\dump`, and takes the dump, title or romfs folder.

| Mod file (under `romfs\`) | What |
|---|---|
| `Pack\Actor\CoopPuppet_Child.pack.zs` | 17 files: our ActorParam and the player's own component files it refers to |
| `Pack\Actor\CoopPuppet_Adult.pack.zs` | 14 files, the same for adult Link |
| `RSDB\ActorInfo.Product.111.rstbl.byml.zs` | The game's table plus a row for each puppet |
| `RSDB\GameActorInfo.Product.111.rstbl.byml.zs` | The game's table plus a row `{__RowId}` for each |
| `System\Resource\ResourceSizeTable.Product.111.Nin_Ounce_NVN2.rsizetable.zs` | The game's size table plus the new entries |
| `..\coop_puppet.json` | What was made, from which files (with their SHA-256) |

The puppet's ActorParam has no `$parent`: it is `Player_<Child|Adult>` merged with `Player_Base`, keeping only `ASRef`, `ASInfoRef`, `ASOptimize`, `AnimationRef`, `ModelInfoRef`, `ModelBindRef`, `BoneBindRef`, `LODRef`, `LocalDepthShadowRef` and `EquipmentUserRef`, and for sound `SLinkRef` and `SoundFootStepRef` ([Sound](#sound), issue 131; `--no-sound` leaves them out). It adds the shared defaults the simple event actors use (`ActorLogicSimpleActor`, `PreActorSetting/ActorOnly`, `ActorSystemSetting/GameActorDefault`, `WorldInfoParam/Default`, `XLink/ActorBasic`). Left out: input, camera target, physics, the player's blackboard, life, AI, the `Player` actor name, ELink (effects) and the player's own XLink property table. The child puppet uses `AS/Player_Child.root.asb` and the `Player_Child` model, the adult one `AS/Player_Adult.root.asb` and `Player_Adult`, with the player's own animation resource lists.

What the game needs for a new actor, read from the files (**checked**) and the TotK engine (**inferred**):

| Item | Finding | Status |
|---|---|---|
| Pack format | SARC, zstd with the `pack.zsdic` dictionary (id 4 in this game; the tool picks it by name and the frame's id, never a fixed number). Members sorted by name hash, data aligned to 8. A SARC the tool writes from a game pack's members is byte-identical to the game's | Checked |
| What a pack holds | The actor's ActorParam and every component file it refers to that is not shared. Shared defaults (`LODParam/Default`, `XLink/ActorBasic` and the like) are in no actor pack. Models, AS and animations are loose romfs files (`Model/`, `AS/`) found by name | Checked |
| `RSDB/ActorInfo` | Every actor has a row (8,363 rows, sorted by `__RowId`): model name, animation resources, radii, `InstanceHeapSize`. The puppet's row is the player's with `ActorName` and `ELinkUserName` removed (`SLinkUserName` stays for [Sound](#sound); `--no-sound` removes it too), `CalcRadius` 100000 and `CoreUsedLevel` 0. Each puppet keeps its player's `InstanceHeapSize`. **Issue 111 (adult puppet never made).** Every create of `CoopPuppet_Adult` logged about 20 `Unmapped Write64 @ 0x...` (0x0 to 0x90) and `dropped (state 4)`. Read in `main`: that is one 0x98-byte allocation in the factory at main+0x9a9300, registered (static initializer 0x10f6370, type registry 0xec1d80) under MurmurHash64A (seed 0xa9f63e6017234875, hash 0x6695b0) of **`engine__actor__ActorParam`**: the heap's `tryAlloc` (vtable +0x30) returned null and the constructor wrote at 0. The game parses an ActorParam in a heap of its size-table entry. The puppet's ActorParam is the player's merged with its `$parent` `Player_Base`, but its entry was the player's alone: 6,232 for `Player_Adult` (a 776-byte stub) against 8,376 for `Player_Child` (a full file), so only the adult ran out. The entry is now the player's plus `Player_Base`'s (13,336): 19,568 adult, 21,712 child. Capping the instance heap (tried first) changed nothing. Patch 0004 logs the return addresses of any write to address 0, and `build.py --heap CoopPuppet_Adult=<bytes>` sets an instance heap for tests | Checked as data; that a create fails without a row is inferred from TotK |
| `RSDB/GameActorInfo` | Every actor has a row too (8,363); `Player_Event` has only `__RowId` | Checked as data |
| Size table | `RESTBL` v1: (crc32 of the path without `.zs`, size) rows sorted by crc, then 10 named rows for colliding paths. Packs are listed as `Pack/Actor/<name>.pack` with about the unpacked size plus 330 bytes; an ActorParam with about 6 to 8 KB. New entries: the pack (unpacked size rounded up plus 4 KB) and our ActorParam (the sum of the entries of the player's ActorParam and its `$parent` chain, since our file merges them: 19,568 adult, 21,712 child; issue 111). The two RSDB tables' entries are raised by their growth plus 4 KB | Checked as data; that the game needs them is inferred from TotK |
| RSDB and size table compression | zstd without a dictionary | Checked |
| BYML | Version 7. The tool's writer gives the same size as the game's for ActorInfo and GameActorInfo (byte-identical for GameActorInfo and the pack members) because equal nodes are written once | Checked |
| Uncompressed `.pack` | Not tried: the tool writes `.pack.zs` like the game | |

**Load test.** `tools/puppet_pack/place_test.py` adds actors to one scene file of a built mod (a throwaway one), for example both puppets and a cow as a control in `Banc/KakarikoVillage/KakarikoVillage_Child_Dynamic.bcett.byml.zs`. **Proven in game (2026-11-08).** Fritchii v5 (coop build, `.nup` loaded with `-g`) applies LayeredFS to a `.nup` with no patch: the log has `Patching RomFS for title_id=0400486024DEC000` and then `RomFS: LayeredFS patches applied successfully`. With both puppets and a control `Animal_Cow` placed at Link's Kakariko save position (about -119, 96.7, 58.7, from `LICHII_COOP_DEBUG_CAMERA`), the game shows the cow and a second, standing child Link: `CoopPuppet_Child` loads, with the player's model, from the pack alone. `CoopPuppet_Adult` was placed too but was not seen in that child-era scene.

**With the EquipmentUser (2026-11-09, Alex, child save, equip-A kit).** The packs with `EquipmentUserRef` load: the log shows LayeredFS applied, and in Kakariko the cow and the child puppet stand beside Link, the game keeps running. The puppet was placed with `Dynamic` `EquipmentUser_Weapon` `Weapon_Sword_Kokiri`, `_Shield` `Weapon_Shield_Hylian`, `_ArmorHead` `Armor_Child_Mask_Keaton`; it showed **the Deku shield on its back, no sword, no mask**. So a placement's `Dynamic` keys are not worn (the Hylian shield and mask would show), and a piece of equipment does appear on the puppet. The Deku shield was also the player's own shield then (`PlayerStatus.EquipShield` in the log), so it may come from the save's game data or from a default; not told apart. The same run logged 4 null reads (`Unmapped read 0x20` and `0x1c8`, pc 0x80cf4338) at the scene load, none in earlier runs without the EquipmentUser; the game went on. The adult puppet (placed in `KakarikoVillage_Adult_Dynamic` with Master Sword, Mirror Shield, Goron tunic, Hover boots) was not seen: no adult save.

An earlier run that saw no mod at all had copied the kit folder: its `qt-config.ini` still had `load_directory` (and the NAND and SD folders) pointing at the original kit, so the emulator never read the copy's `user\load`. The log's `LoadDir:` line shows the folder actually used. `build.py --user-dir` now follows that setting.

What is proven: the packs, tables and size table parse back with the tool's readers, the tables keep their sort order, and the tool's own tests (`tools/puppet_pack/tests/test_puppet_pack.py`, synthetic data) pass.

## First step (done)

Kept for the record: this was the plan before the pack existed. It is built and run; what is built now is in the sections below.

Prove the create path with the smallest patch, before the pack exists:

1. Add a `coop puppet` built-in patch to `lichii/patches/` with the two records above (the `bl` at 0x6e6dd8 and the routine at 0x3c46280), and an `svc #0xf4` handler in `lichii/hook/` beside the presence code.
2. With `LICHII_COOP_DEBUG_PUPPET=1`, the handler sends one create of an existing harmless actor (`Animal_Cow_LinkHouse`) at the local player's position plus 2 m, a few seconds after the scene has loaded, and logs the handle slot and the actor's +0x2c and +0x31c each second.
3. Run it in Kokiri Forest. Done when the cow stands there and the log shows state 4 at the place asked for. This also shows whether 0x6e6dd8 runs every frame (count the SVCs per second).

Then write `CoopPuppet_Child`, spawn it the same way, and drive its pose and AS commands from the samples.

## Spawn and move: what is built

Approach (a), in `lichii/hook/` and one small emulator patch. Not yet run with the game when this was written; see [the first run](#the-first-run).

| Piece | Where | What |
|---|---|---|
| Patch | `lichii/patches/0003-coop-puppet.patch` | Two calls in the emulator: `PatchManager::PatchNSO` calls `CoopSvc::PatchMain` after its built-in patches, and `Kernel::Svc::Call` asks `CoopSvc::Call` before any other SVC. |
| Code patch | `CoopSvc::PatchMain` (`coop_svc.cpp`) | Only with `LICHII_COOP_PUPPET=1` or `LICHII_COOP_DEBUG_PUPPET=1`, for main of build `E0F3CCFA...3348`. moduleCalc_ calls the frame function 0x3b4e300 from two places: main+0x6e70f8 (0x94d19c82), the one that runs every frame, and main+0x6e6dd8 (0x94d19d4a), a rare branch. Checks that both hold those words and that 0x3c46280 to 0x3c47000 is zero, in the original and in what the other patches made, then writes `bl 0x3c46280` at both and the cave. Logs `Applying "coop puppet"` or why not. |
| Cave | `coop_svc.s`, bytes in `coop_svc.h` | 128 bytes at 0x3c46280. Keeps the game's `x0` and `s0`, takes 0x100 bytes of stack, and loops: `svc #0xf4` with the buffer, then, while the emulator says so, calls the function it put in the buffer with `x0` to `x7`, `s0` (buffer +0x68, for the frame of 0x37b5d70) and the stack arguments from it, and hands the result back at the next SVC. At most 256 calls a frame (16 until the enemy copies needed more). Then it branches to 0x3b4e300 as the game would have called it. |
| SVC | `CoopSvc::Call` | 0xf4 from Ocarina only. Hands the SVC to the director and writes the call it wants into the buffer. |
| Director | `coop_puppet.cpp`, calc thread | One slot per puppet. A slot gets a 0x300-byte block from the game's `malloc` (0x14c6c90) once, and keeps it. It writes the create argument (position, rotation, scale 1, +0x40 from the GOT entry 0x4c7dad0, and as SeqExecuteCreateActor does at 0x32bca40..0x32bcaa0: +0x48 = 0, +0x98 = 1), the name and a handle slot into the block, and asks for `create(manager, &name, &arg, &handle, 1, 0, 0, 0)` with 0 stack arguments. Create fills the handle slot with a 0x30-byte **create request**: +0 low 16 bits the state, +8 the actor. State 3 = ready: the director takes the actor with **0x36fbd0(request, 1, 0)**, as SeqExecuteCreateActor's check (0x32bcb30) does, and clears the slot; state 4 is dropped with 0x36fde0(request), 5 and 6 are failures. Once the actor is in state 4 the puppet is live and its +0x31c and +0x328 are written every frame. (With the walker's +0x48 = 0x10001 the request stayed in state 2.) |
| Delete | 0xa6490 | The run of `OneShotGenericDeleteThisActor` (its node's last vtable slot, 0xc706f0) calls 0xa6490 with the actor and 0. It returns at once for an actor in state 7 to 11. The director asks for it when the player has been out of the plan for 2 s (`kLeaveGraceMs`: a stalled peer fades to 0 for a moment, and without the wait the puppet was deleted and made again), changes model, or the local scene is not the one the puppet was made in. An actor the engine deleted itself (state 7 to 11, or a holder whose id no longer matches) is noticed and made again. |
| Planner | `coop_puppet.cpp`, present thread | `CoopPuppet::Publish`, from `CoopHook::OnFrame`: every remote player that presence shows in your scene and has not faded becomes a target, at its shown position and rotation (`QuatToMatrix`, the inverse of the reader's). With `LICHII_COOP_TEST_PUPPET=1` one more (the test puppet), 2 units along x from where you stood 3 s after the scene was known, staying there. It had no switch of its own until 2026-11-09: `LICHII_COOP_DEBUG_PUPPET=1` made it too, so a run with the logs on showed a second Link and fairy standing frozen where each player started (Alex's run of that day). Now `LICHII_COOP_DEBUG_PUPPET` only writes the status lines. Nothing is planned while the scene is not known (loading). |

The actor is `CoopPuppet_Child` or `CoopPuppet_Adult`, by the age in the player's samples (a change of age deletes it and makes the other); `LICHII_COOP_PUPPET_ACTOR=<pack name>` sets one actor for everyone (`Animal_Cow_LinkHouse` was the default before the pack).

**No create while loading.** The scene is readable as soon as a load starts, but a puppet created during the loading screen reaches state 4 and is never drawn; it showed only after it was deleted and made again (Alex's two-player run, 2026-11-09: player 2 made player 1's puppet at 37.1 s, during its loading screen from 36.8 to 40.2 s, and saw it only after it was remade at 230 s). So a create waits until the renderer's camera (`CoopCamera::Latest`, fresh within 250 ms as for the markers) has run unbroken for 0.75 s (`kDrawnMs`), begun after the last local scene change or engine delete of that puppet; if the camera never broke over such a change, 3 s after it (`kSteadyMs`). The log says `P<n>: waiting for the game to draw` once per wait. With no camera seen since start, creates go at once as before. A dropped or failed create waits 5 s only before the same actor is tried again: a different one (the age read wrong while the other player was loading, which made an adult create) goes at once. To not make that wrong one at all, a create also waits until the wanted actor has stayed the same for 1 s (`Input::settle_ms`): in the two-game runs of 2026-11-09 the game that loaded first read the other's age as adult for about 0.6 s, made `CoopPuppet_Adult`, dropped it and made the child 0.6 s later.

### Animation playback

What [How the puppet plays it](#how-the-puppet-plays-it) planned, built in the director. The session's `on_event` hands every PLAYER_SAMPLE to `CoopPuppet::OnEvent`, which keeps each player's newest field 7 (name, frame, rate) with a counter; the planner passes it on with the target. For a live puppet whose AS chain (actor +0x270, +0x40, +0x18, +0x18) is there:

1. A name it has not played: the name goes into one of two 0x50-byte buffers in the block (in turn, since the slot may keep the pointer), the request at block +0x200 is {actor, name, slot 0, rate, -1.0, -1.0, skip-if-same 0, node 0} (node 0 as the game's own caller at 0x2a95c4), and the cave calls **0x2022a0**(request).
2. In the same frame, **0x37b5d70**(controller, 0, `s0` = received frame). 0x37b5d70 checks the slot index against the count and works on the slot's player (slot +0x48), so it is called rather than writing +0xe8.
3. After that the game plays it on. A new sample (the counter moved) with the same name sets the frame again only when the puppet's slot 0 frame (+0xe8) is more than 3 frames off (`kFrameSlack`), so keep-alives correct drift without jerking.
4. Then every other slot (1 to the slot count, at most 8) gets slot 0's command with **0x3abec10**(actor, 0, slot, 0, 0), the call behind `ExecuteASSyncPartialAnimation`: it reads the source slot's command (+0x10) and rate (0xdf49e0) and plays it on the target slot through 0x37b59c0 (request skip-if-same 1). Alex's test of the slot-0-only version (2026-11-09) showed the legs animating and the upper body in T-pose: the upper body is a partial slot. Not yet run.

**Pose against the actor's own calc.** In the same test the puppet did not follow while it played animations. The engine's matrix setter, the run of `OneShotActorRequestSetMtx` (0x28b2050), calls **0xc6fd60**(actor, mtx34, 0): for an actor with a physics body (component table +0x50, its +0x20) it hands the matrix to the body (0x4c6630); otherwise it stores a pending transform, position at actor **+0x358** and rotation rows at **+0x364**, and sets or clears bit 0x8000 of +0x4a8 (from the third argument). The actor's calc applies that over +0x31c/+0x328. The director now writes both pairs every frame (`Pose`), as the setter would. With `LICHII_COOP_DEBUG_ANIM=1` a line a second, `P<n> pose: at <+0x31c>, request <+0x358>, now to <target>`, shows what the game left there since the last write. Not yet run; the applier itself was not found.

`LICHII_COOP_DEBUG_ANIM=1` logs `P<n> plays '<name>' from frame <f>`, each correction (`frame <now> -> <received>`), and hold/move-again; with `LICHII_COOP_DEBUG_PUPPET=1` the status line shows what each puppet plays and its frame. Names of 0x50 characters or more are not played (the reader refuses more than 64).

### Scene filter

The planner runs `coop::host::visibility` (`coop/host/scene.hpp`) for each remote player with our scene, whether our own event hides the others (`local_event_hides`: `PlayerReading::event_running`, except while we play the Ocarina), and their scene, PLAYER_STATUS flags and newest animation (the AS command in PLAYER_SAMPLE field 7, read by `activity_of`):

| Visibility | Puppet |
|---|---|
| hide, other or unknown scene | Not planned: none is made, one that exists is deleted after the 2 s grace (or at once on our scene change) |
| hide, our cutscene | Kept, written 500 units below its spot (`kHideDrop`), no animation calls, so it is not deleted and made again for every cutscene |
| ride, they ride a horse (issue 199) | Shown as usual, in their riding animation, and a copy of their horse is made under it ([Riding](#riding-issue-199)). Before issue 199 the puppet was parked out of sight here, so a rider vanished for the others |
| hold, their cutscene | Kept where it is, no pose write; slot 0's frame from the start of the hold is set again once a frame, so it stands frozen. Not while they play the Ocarina (then show) |
| show, our Ocarina song starts an event | A song that starts an event where it is played (Zelda's Lullaby on a Triforce plate, the Song of Time block) keeps our slot 0 on an `Ocarina_*` command for the first moments, so the others stay visible at the start of that shot until our animation changes. Known, small |
| show | Pose written every frame, animation as above |

The overlay markers follow the same rule: `Coop::Frame` (`coop_hook.cpp`) notes each player `visibility` hides, and `Place` builds no marker for them. Park, hold and show keep the marker. Each puppet's Navi (`CoopFairy::Publish`) plans from the same remote list (`CoopPuppet::Collect`) and the same event flag, and is parked, held or shown with her puppet; she is also parked while her puppet's actor is not live and shown (Fairy, issue 145).

Every change is logged once: `coop: puppet: P<n> show -> hide (our cutscene)`, `... hide -> show`, `... show -> hold (their cutscene)`, `... show -> hide (riding)`; the fairy's lines start `fairy coop: puppet:`. A puppet made again starts at `show`.

### Cases (issue 86)

What each case does now. "Handled" is by code and unit tests; none of these is proven in the game yet. Each scenario named has a steps file (`tools\autotest\run.ps1 -Scenario <name>`) whose `counts.txt` lines check its case; its header says which shots to look at and when a run is inconclusive.

| Case | Whose | What happens | Status | Scenario |
|---|---|---|---|---|
| Cutscene (event running) | ours | Every puppet and Navi parked, no markers | Handled | `cutscene` (A talks to a sign, B watches) |
| Cutscene | theirs | Their puppet's animation held frozen; it follows their place (issue 194, below) | Handled | `cutscene` (reverse), `cutscene-snap` |
| Title screen, file select | ours | No player actor, so no reading and no scene: nothing planned, puppets deleted | Handled if the title has no player actor (unknown: the title demo may have one) | `screens` (B on the title, then A back on it) |
| Title screen, file select | theirs | They send no samples (no reading): their pose fades in 0.4 s and the puppet is deleted after the 2 s grace | Handled | `screens` |
| Pause menu, item menu | ours | Nothing changes: puppets stay; whether the game draws them behind the menu, and whether our pose writes run while paused, is not known | Unknown | `screens` (B pauses) |
| Pause menu | theirs | Keep-alive samples go on; the puppet stands where they stopped. The `paused` and `in_menu` status bits are never set by the sender, and do not hide | Handled (stands) | `screens` |
| Loading | ours | Scene change deletes every puppet; creates wait until the game draws again (`kDrawnMs`) | Handled (proven before, puppet-join) | `stray` |
| Loading, other scene | theirs | Their scene changes: other place, deleted | Handled (proven, `stray`) | `stray` |
| Riding Epona | theirs | Puppet shown in its riding animation while their command names a horse (`Horse...`, `Evt_Horse_...`, not `DismountHorse` or `HorseAccess`), with a copy of their Epona under it (`CoopRide_Epona`, [Riding](#riding-issue-199)); dismount: the copy goes, the puppet walks on | Built (issue 199); not run in game yet | `epona` |
| Riding Epona | ours | We send `PLAYER_RIDE` while we ride | Built (issue 199) | `epona` |
| Ocarina played freely | theirs | Shown (not held) while their command is `DemoOcarina` (not `DemoOcarinaMelodyGet`) or starts `Ocarina`, though the game sets their event bit then (ocarina-b, 2026-11-12) | Handled; taking it out first is a plain `EvtWait` event (about 4 s): held then, as any talk | `ocarina`, `hs-ocarina-show` |
| Ocarina played freely | ours | Our event does not hide the others while our own command is `DemoOcarina` or starts `Ocarina` | Handled; the take-out `EvtWait` hides them, as any event | `ocarina`, `hs-ocarina-show` |
| Song taught, warp song, Song of Time block | either | `DemoOcarinaMelodyGet...` and what follows are cutscenes: as a cutscene | Handled as a cutscene | `ocarina` (log only) |
| Time travel (Master Sword) | either | A cutscene, then the age changes: the puppet is made again as the other age (settle 1 s) | Handled (the age swap is proven, `stray-adult`) | none new |
| Crawlspace | either | Unknown: the AS tables have no crawl command name (`Crouch...` is crouching); if the game runs it as an event, ours hides puppets and theirs holds them at the entrance | Unknown | `crawl` (both warp into Kokiri Forest; A crawls the Kokiri Sword tunnel, B watches from inside; no save to set up) |

### Cutscenes without snaps (issue 194, branch `cutscene-snap`)

Player test 2026-11-12: after cutscenes (a door opening, the Deku Tree ladder falling, the timed-platform switch) the other player's Link snapped or rubber-banded, and the switch's result came late.
- **Their cutscene.** The puppet was held (frame and place) where the event began. An event moves Link (through a door, a few metres), so when it ended the puppet jumped to where they were. Now a held puppet keeps its frozen frame but follows their place and turn, as their samples go on during events (`sampler.cpp` sends them as always).
- **Any jump.** A move of the place we are handed of more than 0.75 m in one frame and less than 20 m (`CoopSnap::kSnapM`, `kWarpM`, `lichii/hook/coop_snap.h`) is eased: the puppet stays where it was that frame and closes the gap over 250 ms, or at 15 m/s for longer gaps, while it follows their new place. A jump is the target's own move since the last frame, not the drawn step, so their running during a long ease starts no new one (no stutter). A move of 20 m or more is a warp, drawn at once; so is any jump while their cutscene runs (held): their samples move smoothly then, so a jump is a real teleport (a void-out, the event's start mark), not a slide through a wall. While hidden (our cutscene) the drawn place is set to theirs, so it shows again where they are, with no ease from the parked spot.
- **World objects during our event.** The issue 144 rule left every shared change within 8 m of our Link waiting while our event ran. A world object another game moves (`CoopWorld::WorldSync::Follows`: the falling ladder, the falling floors, blocks, and a moved block's base anchor) is no event's partner: its place and played animation now go on during our event (`CoopGuard::Skip`'s `never_waits`), so the ladder no longer stays up during our live-event cutscene and jumps after it. Kills and removes still wait (the Deku Scrub of issue 144 is placed by the enemy sharing, not a world object, so its rule is unchanged).
- **Logs** (`coop: puppet: ...`, also `fairy coop: puppet: ...` for Navi): `P<n> target jump <m> m (show|hold, eased|at once|warp)` for every jump in the place we are handed (before easing), and `P<n> snap <m> m (show|hold|warp)` for every step past 0.75 m of the puppet's own place read back from the game each frame (what it drew; the first frame after a hide is skipped, it was parked).
- **Not changed:** a live event (a switch another player pressed) is still not started while our own event runs (the game runs one event at a time), so it starts when ours ends; the timed platforms then run on our own timer from that moment. Sharing the timer is dungeon object state (deku-dungeon-2).
- Scenario `cutscene-snap` (steps file): the ladder falls (A shoots the eye, a cutscene for both), then B walks through the slingshot room's door (a door cutscene) while A watches. Its `lines` checks: `min=1` that the hold/hide and back happened in both games (else the run checked nothing), no read-back snap, at most 2 target jumps, no Navi snap. `lines ... min=N` is new in `run.ps1` for this.

### The first run

One emulator is enough for the spawn (a server must be set so co-op is on):

```bat
set LICHII_COOP_DEBUG_PUPPET=1
set LICHII_COOP_TEST_PUPPET=1
lichii.exe
```

In the log, in order:

1. `Applying "coop puppet": bl 0x3c46280 at main+0x6e6dd8 and main+0x6e70f8, 124 bytes of cave` and `coop: puppets on, actor Animal_Cow_LinkHouse, test puppet beside you`.
2. Every second, `coop: puppet: <n> cave frames/s, scene '...', <k> wanted`: the cave runs once per game frame (30 or 60).
3. `P0: block at ...`, `P0: create Animal_Cow_LinkHouse requested (returned ...), handle slot ...`.
4. `P0: took actor ... from request ...` and `P0 Animal_Cow_LinkHouse spawned after <ms> ms: request ..., actor ..., state 4, at (x, y, z)`.
5. Then each second `P0 live actor ... state 4 at (...), wanted at (...)`: the two agree when the pose write holds.

For following: two instances ([testing.md](testing.md)) with `LICHII_COOP_PUPPET=1` (or the debug flag) in B; B's log shows `P1` live, at A's position.

### Proven in game (2026-11-08)

Kakariko Village (child), Fritchii v5 coop build, two local instances on one server, no one playing (keys posted to the windows):

- The cave runs every frame (`60 cave frames/s`).
- `Animal_Cow_LinkHouse` spawns 109 ms after the request, state 4, at the asked place, and is seen in the game.
- `LICHII_COOP_PUPPET_ACTOR=CoopPuppet_Child`: our pack spawns as a standing child Link with the player's model, 190 to 360 ms after the request (about 1.1 s in the instance that was loading too).
- Each instance makes a `CoopPuppet_Child` for the other player (A's P2, B's P1). A walked a few steps; B's P1 moved with him, `live ... at` equal to `wanted at`.
- A closed: B logs `P1: delete requested`, `P1 deleted (state 7)`, and the puppet is gone from B's screen.

Not yet seen: animation (the puppet stands in its default pose), rotation in the picture, a scene change with two instances, adult Link. Animation playback and the scene filter are built since (unit tests on fake memory); not yet run in the game.

## Pose

Full-body copy, built because playing the same AS command left the legs slower and stiffer than the body (Alex's test, 2026-11-09). Each frame the puppet takes the other player's whole skeleton, as Anchor's dummy player does on N64 (idea only, no code). Position and rotation still come from `PLAYER_SAMPLE` and the follow fix above.

### Where the skeleton is

Read with the emulator's debug server (`EDEN_NX2_DEBUG_SERVER`, `read_memory` and `search_memory`) in the running game, Kakariko, child, 2026-11-09. Checked means seen in memory there; the code that writes it was not read.

| Item | Where | Status |
|---|---|---|
| Model object (nn::g3d `ModelObj`) | actor +0x270 (component table), then +0x8 (the component with vtable main+0x4c61020), +0x18 (the model unit, vtable main+0x4c088a0), +0x18; the model object sits inline at +0x120 of that, its first word points to the model resource (`FMDL`, name `Player_Child`) | Checked on the player and on `CoopPuppet_Child`. The player also has it through component +0x0, +0x60, but the puppet does not, so that way is not used |
| Skeleton object | model object +0x20; its first word points to the skeleton resource (`FSKL`) | Checked |
| Bone count | skeleton object +0x48, u16: 309 for `Player_Child` (the resource has it at +0x38) | Checked |
| Local transforms | skeleton object +0x18, a pointer to bone count entries of 0x60 bytes | Checked |
| World matrices | skeleton object +0x20 | Seen, not used |

One local transform (0x60 bytes): u32 flags at +0x0 (bits 23 to 30 change with the pose; +0x4 to +0xc stay 0), scale 3 x f32 at +0x10, the rotation as 3 rows of 4 floats at +0x20 (the fourth of each row 0), the translation 3 x f32 at +0x50. Every rotation read was orthonormal.

While Link walked (150 frames), 117 bones changed rotation, 42 translation (bones 1 to 3 are the root and hips) and 11 scale; the flags changed on most of the animated bones. Largest translation 0.58, largest scale 1.01: both fit the protocol's s16 in 1/8192.

A second `Player_Child` model object is in memory (not reached from the player actor); it is not used.

### What the hook does

`lichii/hook/coop_pose.{h,cpp}`, called by the puppet director once a frame on the calc thread (`Director::PoseFrame`, the cave's first SVC of the frame):

1. Finds our player (the same chain as `PlayerReader`), walks to its skeleton and reads all local transforms. The first read is kept; a bone's rotation, translation or scale is counted as moving once it differs from that read by more than 1e-4 (rotation also when its flags change), and stays counted. The moving bones, with their flag byte (bits 23 to 30), are the pose.
2. The network thread sends it as `PLAYER_POSE` ([protocol.md](protocol.md)) at most 15 times a second, when a new one was read, with at most 48 bones: first up to 8 of those unsent for 0.5 s or more, oldest first (every bone goes again within about 3 s, however fast the limbs move), then the ones changed most since they were last sent. The receiver keeps the bones a pose leaves out from its newest pose (`Track::Fill`). About 13 KB/s per player, where every moving bone at 30/s was about 130 to 160 KB/s.
3. A received pose goes to that player's track. Each frame the puppet shows the last two blended (normalised lerp of the rotations, lerp of the rest), one send interval behind, so 30 a second plays smooth at 60 frames.
4. For each live puppet with the same bone count, the shown pose is written into its local transforms: the rows of the rotation, the translation and scale, and the flag bits. Bones not in the pose are left as the puppet's own animation has them.

The pose copy wins over the AS commands: while a player's poses arrive (the last within 1 s), the director plays no AS command on that player's puppet, so only the pose copy moves it. Gear is still put on as before. Without poses (a peer whose build has no pose copy) the puppet plays the AS commands as it did. `LICHII_COOP_POSE_AS=1` plays them as well while poses arrive.

The test puppet (`LICHII_COOP_TEST_PUPPET=1`) copies our own pose when `LICHII_COOP_DEBUG_POSE=1` is set too, so one game shows whether the copy works.

`LICHII_COOP_DEBUG_POSE=1` logs a line a second: `pose: <bones> bones, <n> moving; <s> sent/s, <r> received/s, <w> written/s, <x> refused/s; last write kept <k>, overwritten <o>`. "Kept" and "overwritten" compare one written bone with what the puppet holds a frame later: overwritten means the game's own animation writes the local transforms after our write, and the copy cannot show.

### Proven in game (2026-11-09)

Two local games in the Graveyard (child), autotest scenario `pose` (`tools/autotest/scenarios/pose.A.txt`, `pose.B.txt`: B steps back down the path, A walks, runs, turns, rolls and swings in front of it), `LICHII_COOP_DEBUG_POSE=1`:

- Both games read 309 bones; about 300 count as moving once Link has walked. Each sends 30 poses a second and receives the other's 30.
- B writes the pose on its puppet every frame (about 40 written a second at the game's 40 fps) and every write is still there a frame later (`kept 41, overwritten 0`): the puppet's own AS commands do not write its local transforms, so a write at the start of the frame holds.
- B's screenshots show the puppet in A's poses at the same moments: walking, mid-run, turning to run back, and a hunched pose with hands on the knees that the AS route never showed.
- With no AS command played on the puppet at all (then `LICHII_COOP_POSE_NO_AS=1`, now the default), it still moves through every pose: the copy alone drives the whole body.

- Facing: with scenario `turn` (A walks up, then faces left, right, toward B and away), the yaw that `LICHII_COOP_DEBUG_POSE=1` now logs (`yaw ours <deg>, P<n> <deg>`, from actor +0x328) of B's puppet followed A's own at every turn: 91, 152, 2, -93, 86 on both sides; A's puppet of B stayed at B's -104. The facing is the actor matrix the director writes from `PLAYER_SAMPLE`; the root bone carries none (its local rotation stays identity).

The first two-game run had every write refused: the player reaches its model through component +0x0, +0x60 as well, but the puppet does not; component +0x8 works for both.

### Held pieces (issue 158)

A player test: B saw A's slingshot pulled back with a seed in it, and it stayed that way after A shot. The pose copy wrote Link's skeleton only. The slingshot in Link's hand is an actor of its own (`Weapon_Range_SlingShot` in EquipmentUser hand slot 3) with its own skeleton, which the game animates on A: pulled back while aiming, let go at the shot. Nothing animates the puppet's copy of it.

- **Sent.** Each frame the hook reads the pieces in the local Link's hand slots 1, 2, 3, 5 and 6 (`CoopPose::kPieceSlots`; not the sword in 0 or the shield in 4) that are out (state 4; a put-away slingshot is state 6). A piece with at most 64 bones goes as a `PLAYER_POSE` of its own skeleton, every bone with rotation, translation and scale, right after Link's pose. An unchanged piece is sent again only every 250 ms.
- **Written.** The receiver keeps one pose track per skeleton id for each player. Each frame it writes Link's track onto the puppet and, for every piece out in the puppet's own hand slots, the track of that piece's skeleton. The skeleton id (bone count and model name) tells the poses apart, so there is no new message. A skeleton not heard from for 5 s is dropped; at most 8 are kept per player.
- **Covers.** The slingshot's band, and the same way the bow's string, the boomerang or hookshot in hand, if the game animates them by bones.
- `LICHII_COOP_DEBUG_POSE=1` adds `held pieces <n> ours, <n> sent/s, <n> written/s` to the line it logs each second.
- Not run in a game yet: that the band is a bone animation (and not, say, a material swap) and that the game does not undo the write.

### Not done (pose)

1. The AS commands stop only while poses arrive; if poses stop for more than 1 s the next sample's animation is played again (the slot's last command keeps playing until then).
2. Adult Link's skeleton was not read in a running game (only `Player_Child`): see [Adult](#adult).

## Adult

Alex (2026-11-09): adult Link is synced like child Link. Everything here is built and unit tested; **none of it was run in a game** (in-game tests were stopped on 2026-11-09 before the adult run). What to check is listed at the end.

| Piece | What | Status |
|---|---|---|
| Which puppet | The sample's age chooses `CoopPuppet_Child` or `CoopPuppet_Adult`. When a player's age changes (the pedestal), the planner's actor name changes, and the director deletes the puppet and creates it again at the new age | Unit tested (planner and director); not run |
| Mixed ages | A child scene and its adult twin (`Graveyard_Child`, `Graveyard_Adult`) count as the same place for puppets, fairies and the overlay markers (`coop::host::same_place`, which drops a trailing `_Child` or `_Adult`). Shared objects and flags still need the very same scene | Unit tested; not run |
| Pose | The skeleton id sent with every `PLAYER_POSE` is the bone count (low 9 bits) with a hash (FNV-1a) of the model's name above it. The name is read from the model resource (`FMDL` +0x8: pointer to a u16 length and the characters): `Player_Child` or `Player_Adult`. A puppet takes only a pose of its own model, so a pose that arrives around an age change is dropped, not written on the wrong skeleton. The wire format did not change. A change of our own model starts the moving-bone sets again | Unit tested on fake memory; the adult skeleton was not read in a game. If the path to the skeleton differs for `Player_Adult`, the debug line shows `?` for the model or counts refused writes |
| Gear | Nothing in the gear code is for the child only: it sends the names in `PlayerStatus.Equip*` and equips them by name, so the Master Sword, Biggoron's Sword, the Hylian and Mirror Shield, tunics, boots and the held bow, hookshot or hammer go the same way. The adult's bones are in [Equipment](#equipment) | Not run: adult slots and names were never seen in a game |
| Fairy | The fairy's shoulder offset is scaled by 1.45 (`CoopFairy::Settings::adult_scale`) for a puppet whose actor ends in `_Adult` | Unit tested; the factor is a guess from the two heights |
| Name tag | The tag floats over the head at 1.35 (child) or 1.9 (adult) world units, by the sample's age (`coop_hook.cpp`, from the name tags work) | Heights not checked on an adult |

Age in the game: the player actor itself is `Player_Child` or `Player_Adult` (0x4b6db0, `QueryPlayerIsChild`, reads the actor's parameters). The save's era is the bool flag `Sequence_IsChildhood` (hash 0xcea220d3, in `GameDataList`); `main` also has the names `IsChildWhenChangeBanc`, `IsChildWhenInitializingScene`, `IsCreateChildPlayer`, `ChangeEra` and `IsSetCurrentEra`, not read.

For tests without an adult save: the autotest `warp` to a scene ending in `_Adult` or `_Child` also queues `Sequence_IsChildhood` (false or true) before it asks for the scene ([testing.md](testing.md)). Whether the game then creates `Player_Adult` is **not known**: it was never run. Scenario `adult` (`tools/autotest/scenarios/adult.A.txt`, `adult.B.txt`, run with `-NoLoad`) warps A into `Graveyard_Adult` and B into `Graveyard_Child`; A walks, runs, turns and swings while B watches.

To check in a game (with `LICHII_COOP_DEBUG_POSE=1`):

1. An adult player: its pose line starts `pose: Player_Adult <n> bones` with no refused writes; the other game logs `CoopPuppet_Adult spawned` and shows an adult puppet that walks in the player's poses.
2. Child and adult in the twin scenes see each other, each at the right age and place.
3. Pulling or returning the Master Sword: the other game's puppet is replaced by one of the new age within a second or two.
4. Adult gear on the puppet: Master Sword, a shield on the back and raised, a tunic, boots, the bow or hookshot in hand.
5. The fairy by the adult puppet's shoulder and the name tag just over its head.

## Fairy

Each puppet gets a Navi that hovers by its right shoulder and trails it (`lichii/hook/coop_fairy.*`).

| Piece | What |
|---|---|
| Actor | `Dm_Npc_Navi`, the cutscene Navi from the game's own pack: its own model and glow (`XLink/Npc_Navi`), `ActorLogicSimpleActor`, the generic name `Dm_Npc_Base`. `Npc_Navi` is not used: it is the player's partner, with an AI that follows the player and a name the game looks up, so a second one would clash. `LICHII_COOP_FAIRY_ACTOR=<pack>` picks another (`Npc_Fairy` is the next to try). |
| Create and delete | A second `CoopPuppet::Director`, the same create, take and delete path as the puppet. It registers with the svc cave as caller 20 (`CoopSvc::AddCaller`, after the puppets and CoopSpawn), on the first `CoopFairy::Publish`; the cave asks each caller in turn, each with its own frame and its own call results. Each director takes at most 4 calls a frame, the cave allows 16. Its log lines start with `fairy`. |
| Where | `CoopFairy::Publish`, right after `CoopPuppet::Publish` with the same arguments, runs a second `CoopPuppet::Planner` on the same remotes, so a fairy exists exactly when its puppet does (the test puppet too). Its goal is the puppet's position plus (0.35 right, 1.05 up, 0.25 back) turned by the puppet's rotation, with a 0.06 bob and a 0.08 loop. For an adult puppet the offset is scaled by 1.45 ([Adult](#adult)). It closes on the goal at rate 5/s and jumps when more than 4 units away (a warp, a new scene). |
| Pose | Written into the actor every game frame by the director, like the puppet's. The Navi AI is not pointed at the puppet. |
| On and off | On with the puppets (`LICHII_COOP_PUPPET` or `LICHII_COOP_DEBUG_PUPPET`); `LICHII_COOP_FAIRY=0` turns it off. |

The remote player's real Navi position is not sent: the fairy is placed from the puppet only (no protocol change).

**Never without her puppet (issue 145).** Testers saw another player's Navi float in their room in the Deku Tree while that player was about 40 units away in the compass room, with no Link to be seen. The fairy was planned from the same remotes as the puppet, but not from whether the puppet's actor was really there and shown: a puppet still being made, refused, gone, or (seen 2026-11-09, below) made but not drawn left its Navi alone. Now the puppet director keeps `LiveMask()` (bit n while puppet n's actor is live in this scene and shown or held) and `CoopFairy::Follower::Follow` parks a fairy under the floor while her puppet's bit is off (`why`: "her puppet is not shown"). There is no distance rule: the puppet has none of its own, so a partner far off in a field or a big room keeps both. She is placed from the puppet's current spot, so she appears at its shoulder when she may. Tested in `fairy_test.cpp` and `puppet_test.cpp`; not yet seen in a game.

**Only in our room (issue 145, branch `navi-room-2`).** The live tie above was not enough: a puppet that is live but which the game does not draw (the 47-unit sighting below) keeps its bit, and in the testers' case (A in a room up on the west side, B on the main room's first floor, 40 units apart, one scene) the glow showed with no Link. The game seems not to draw a puppet in a dungeon room it has not loaded, but it draws Navi's glow. So in a dungeon a fairy is also parked while her player is in another room than ours (`why`: "her player is in another room"). The game's own room of Link is not mapped, so the room is guessed (`lichii/hook/coop_rooms.h`) from a table made on each player's PC from their own game, with the puppet pack: `make-puppet-pack.cmd` (`tools/puppet_pack/build.py`, through `dungeon_rooms.py`) writes `coop_rooms.txt` beside `coop_puppet.json`, one line `scene room x y z` for every placement with a `DefaultDungeonRoomIndex` in the dungeon scenes, thinned to 1 unit apart. Nothing of it is in this repo. The hook reads it at start (log `coop: room table: N placements`; none: `coop: no room table, make the puppet pack again`, and no room rule). A position is in the room of the nearest placement, with height counted twice (floors are stacked), within 25 units (else not known: no rule). Every player is tracked each frame, and changes room only once the new room's nearest point is 1.5 units nearer than the old one's (no flicker in a door); a player whose room is not known is forgotten, so the next guess has no slack. Two Links within 8 units (`Settings::room_near`) always keep their fairies, so a doorway or a wrong guess near a wall does not hide one next to us. Outside the dungeons nothing changes. Each park or show by this rule is logged once with both rooms (`fairy coop: rooms: P1 in room 3, we in room 0: Navi parked`), so a run tells a wrong guess from a wrong rule. Checked on the game's table (Python, `COOP_TEST_ROMFS`): every placement is in its own room; the room centre anchors (`DungeonRoomCenterAnchor`) are not always (Deku Tree 12 in 11 different rooms, Dodongo's Cavern 23 in 22, Forest Temple 29 in 24), so the guess is rough in places. `LICHII_COOP_FAIRY_ROOMS=0` turns the rule off. The puppet itself has no room rule. Tested in `rooms_test.cpp`, `fairy_test.cpp` and `test_puppet_pack.py`; scenario `navi-room`; not yet seen in a game.


**Proven in game (2026-11-09).** Kakariko Village (child), two local instances, keys posted, no one playing. Each game logs `fairy ... P<n> Dm_Npc_Navi spawned after` about 0.8 to 1.4 s, at 1 unit above its puppet. With the two Links 6 units apart, each screen shows the other player's puppet with a glowing Navi at its shoulder. Seen on the way: a puppet created while its player was about 47 units away was not drawn when he came close (its fairy was); the same puppet created nearby is drawn. That is the puppet, not the fairy (see Not done).

**Frozen in a hand test (2026-11-09), fixed (branch `fairy-anim`).** The fairies stayed where they were created and did not animate. Two causes:

- **Place.** `Dm_Npc_Navi` has a physics body (a character controller), so a plain write of its place is undone, as for the shared objects ([objects.md](objects.md)). The fairy's director calls the game's own setter `0x4c6580(actor, 0)` once a frame after the write (`Director::SetMatrixCall`, `Director::Move`; only for an actor whose vtable is 0x4b25e18).
- **Wings.** Its animation file (`Dm_Npc_Navi.root.asb`, 330 bytes) has no animation commands, so the actor plays nothing. `Npc_Navi.root.asb` has `Wait`, `Dash`, `Fly_*` and `Talk`. The two share one skeleton (22 bones). So each frame, at the start of the fairy director's cave turn, the local player's own `Npc_Navi` is found on the actor list (`CoopFairy::FindActor`, looked up again every 0.5 s while she is missing) and her skeleton's local transforms are copied onto every live fairy (`CoopFairy::Wings`, `CoopPose::CopySkeleton`, same bone count only). Each fairy so beats its wings and turns as the local Navi does. `LICHII_COOP_FAIRY_WINGS=0` turns the copy off. Log: `fairy wings: copying Npc_Navi <actor> onto the fairies`, then one `fairy wings: fairy <actor> copied Npc_Navi 22 bones -> Dm_Npc_Navi 22 bones` per fairy.

**Proven in game (2026-11-09, `tools\autotest\run.ps1 -Scenario fairy`, `LICHII_COOP_PUPPET=1`).** Graveyard, both child, auto kits with kitfinal-A's server (shots `coop-kit\shots\fairy\20261109-144820`). Both games log the 22-bone copy. B takes a screenshot every 0.1 s:
- While A stands, the fairy at A's puppet shows a different wing shape in each frame (spread, folded, edge-on), glows, and bobs.
- While A walks into view, it trails behind the puppet, catches up, and then stays at the puppet's shoulder.

The wing beat is the local Navi's, not the other player's own: the remote Navi's pose and colour are not sent (no protocol change).

**Missing after a load (issue 93), fixed in two steps.** In an early build a puppet could be without its Navi for up to 15 s after a scene load. The game refuses a create (it returns 0) while a scene unloads or loads, and the director then waited the 10 s create timeout and 5 s more. PR 100 asks again after 1 s (`kRefusedRetryMs`). The cause was still there: the fairies' director never had the [camera gate](#spawn-and-move-what-is-built) the puppets have (`Follower::Follow` passed on only the base and the scene, not `camera`, `drawn_since_ms` and `settle_ms`). So each fairy was asked for during the loading screen, where she is refused or, like a puppet, made and never drawn. Now `CoopPuppet::StampDrawing` fills those fields for both directors and the follower passes them on, so a Navi is made when her puppet is (unit tests `fairy join` in puppet_test, `carries her puppet's wait` in fairy_test). Not yet run in the game.

## Sound

Issue 131 (found by Lixyas, 2026-11-10): the other players' Links made no sound, no footsteps, sword or voice. Read from the romfs (`Product.111`), not run in the game.

**How the game plays Link's sounds.** Through the engine's xlink sound system (SLink), per actor, not from a central player sound call:

| Item | Where | Status |
|---|---|---|
| Sound user | ActorInfo `SLinkUserName` = `Player_Child` / `Player_Adult` (`ELinkUserName` `Player` / `Player_Adult` for effects); the users are in `SLink2/slink2.Product.111.bslnk.zs` | Checked as data |
| Sound component | `SLinkRef` = `Component/SLink/Player_<Child\|Adult>` (`UserName` the same), in `Player_<Child\|Adult>`; `Player_Base` has `Component/SLink/Player` | Checked as data |
| Footsteps | `SoundFootStepRef` = `Component/SoundFootStepParam/Player_<variant>`, `$parent` `Player`: feet bones `Ankle_L`/`Toe_L`, `Ankle_R`/`Toe_R`, a ray cast from the leg bones for the floor, grass, water and equipment noise | Checked as data; that it plays the steps itself is inferred from the names |
| Sounds tied to the animation | `AnimationEvent/AsNode/Player_<variant>.root.baev` (the events of each AS node) holds `AtSound`, `Voice`, `ChangeSeFloorContact`, `NoSeFloorContact` and `EmitEffect` events; `AnimationEvent/Animation/Player_*_Animation.anim.baev` hold per-animation events (`WeaponBlur`) | Checked as strings; that the events fire on the actor's SLink user is inferred (TotK engine) |
| Property table | `XLinkRef` = `Component/XLink/Player` names `Work/XLink/PropertyTableListParam/Player`, which is in no pack (built in); the puppet keeps `XLink/ActorBasic` | Checked as data |

The puppet already runs the player's AS (`AS/Player_<variant>.root.asb`, with the received command on every slot), so the AS node events fire on it. Before this branch the pack left out `SLinkRef`, `SoundFootStepRef` and the ActorInfo `SLinkUserName`, so those events had no sound user and nothing played.

**Chosen path (branch `puppet-sound`): the puppet's own animation plays the sounds.** `tools/puppet_pack/build.py` keeps `SLinkRef` and `SoundFootStepRef` (with their files, 3 more members per pack: 20 child, 17 adult; since `puppet-sound-2` one more, the puppet's own footstep file) and the row's `SLinkUserName`. No hook code, no network message: each game plays the sounds from its own copy of the animation, at the puppet's place, with the sound's own distance falloff. `--no-sound` builds the pack as before (byte for byte the same as main's, checked on this PC's dump). The manifest says `"sound": true` or `false`. A player gets the sound only after building the pack again (`make-puppet-pack.cmd`); an old pack stays silent and still works.

Left out on purpose: `ELinkRef` (footstep dust, sword trails: effects, not asked for; same path if wanted later) and the player's XLink property table (built in, and its properties read the player's own state, which the puppet does not have). Sounds whose choice reads a property (the floor material, perhaps) then take the user's default.

**Footsteps: Dark Link's settings (branch `puppet-sound-2`).** The builds before it shipped the pack above, but nobody heard the puppet yet (the autotest has no audio). One difference stands out in the data: the player's footstep param has `ProcType` = 145979025, the murmur3 hash of `Player`, and `IsUseRayCast` true. Across every actor pack only `Player`, `Player_Child` and `Player_Adult` use ProcType Player; NPCs and enemies leave it unset, and horses and Ganon have their own. Dark Link (`Enemy_MiddleBoss_DarkLink`), the game's own copy of Link running Link's sounds without the player's code, takes the player's footstep param as its `$parent` and changes exactly three fields: `ProcType` 3069107721 (murmur3 of `None`), `IsUseRayCast` false, `IsEnableGrass` false. So the player's footstep processing most likely reads the player's own state, which the puppet does not have.

The pack now gives each puppet its own `Component/SoundFootStepParam/CoopPuppet_<variant>.game__component__SoundFootStepParam.bgyml` with Dark Link's three changes and `$parent` the player's variant file, and points `SoundFootStepRef` at it. Its size-table entry is the player's variant file's (5,584 on Product.111; the game's entries run about 5.3 KB above the file) plus any bytes it is longer. The manifest says `"footsteps": "puppet"` and names the file per actor. `--player-footsteps` keeps the player's file: the packs are then byte for byte the older ones (checked on this PC's dump). The tool's hash changes, so the kit asks players to build the pack again.

Dark Link has its own SLink user (`Enemy_MiddleBoss_DarkLink`) and XLink property table (`Biped`); the puppet keeps the user `Player_Child`/`Player_Adult` and the shared `ActorBasic` XLink (961 actors with an SLink user use ActorBasic, so that pairing is normal). Whether the `Player_*` SLink user treats its sounds as the listener's own (no position) was not read: the SLink2 file (`XLNK`, 5.6 MB) has no parser here yet.

Test switch for that case: `--sound-user NAME` gives each puppet its own SLink param and row `SLinkUserName` naming another actor's user, for example `Enemy_MiddleBoss_DarkLink` (the build refuses a name no ActorInfo row has; the manifest says `"sound_user"`). Footsteps keep working either way; whether Dark Link's user has Link's voice and sword sounds is not known.

**The player's XLink param (branch `puppet-looks`).** The pack used to give the puppet the shared `Component/XLink/ActorBasic` param. Its property table list (RSDB `XLinkPropertyTableList`) is `Common`, `ActorBasic`, `WettingRate`; the player's own `Component/XLink/Player` names `Player`, which adds `FootStep` (the property 足, which foot: Left, Right, ...), `Player` (the weapon kind 武器種別, the spin direction, the slope, wall climbing, ...) and `HitReaction`. These are the properties Link's SLink user reads to pick a sound, so on the puppet they did not exist at all. The pack now keeps the player's `XLinkRef` (and its file) when it has sound; the property values stay at their defaults, since the player's own code sets them (the left foot, the one-handed sword). `--basic-xlink` (test switch) builds the older pack; the manifest says `"xlink": "player"` or `"basic"`. Not heard yet.

**What the puppet plays, in the log (branch `puppet-looks`).** With `LICHII_COOP_DEBUG_ANIM=1`, every live, shown puppet logs what its SLink user plays when that changes, every 250 ms: `P1 sounds: Ocarina_D` or `P1 sounds: none` (the asset names of the user's running events; `coop_puppet_sound.h` says how they are read). So a run shows whether footsteps, swings and voice are played on the puppet at all (issue 131), without ears. If they are played and still not heard, the next suspect is the listener (a `Player_*` sound may be played as the listener's own).

**Not covered, and the fallback.** Sounds the player's own code plays outside the AS (a pickup jingle, the ocarina notes, perhaps the hurt voice) do not come from the animation, so the puppet does not make them. If they are wanted, or if the pack path fails in the game, the next step is a message (types 85 to 89 are reserved for this, protocol 6): the hook catches the SLink play call on the local player and sends the sound's name or hash, and the receiver plays it on the puppet. The SLink play function in `main` has not been looked for yet.

**What the game run must show** (none of it run): footsteps, sword swings and voice from the puppet; louder near it, fading with distance, silent far away; the local player's own sounds unchanged; no crash or `Unmapped` lines when a puppet is made (the size table entries are the pack's new size, the ActorParam's entry is unchanged since the file is still a part of the player's chain). Risks: a sound the user marks as the player's (played without position, or with the listener) could sound as if it were ours; the footstep component may need the physics the puppet does not have (then it is silent, or the create fails: try `--no-sound` to tell).

## Link sounds

Issues 131 and 152 (dauro, 2026-11-13: "other players can hear all other link sounds and especially the ocarina"). The puppet pack's sound parts (above) were never heard, and sounds the player's own code starts do not come from the animation at all. Since branch `ocarina-sound-b` (Hookshot) the game's own calls carry them, on both sides:

| Part | Where | What |
|---|---|---|
| Catch | patch 0014, `hookshot/src/hos_coop_sound.h` | One row in 0005's wrap table: id 24, main+0x440630, xlink2's `searchAndEmit` (user, &asset, &handle, 0), called for every named sound or effect the game starts (48 callers, the AI node `OneShotXLinkSearchAndEmit` and the actors' own code). Phase 0 only reads |
| Keep | `CoopLinkSound::OnEmit` (`lichii/hook/coop_link_sound.h`) | When x0 is our Link's own SLink user (`[[[link + 0x270] + 0x68] + 0x78]`, set each objects walk), the asset's name `[[x1]]` (a char*; the same asset the user's event list points at) is kept, at most 32 a frame. Any other user: nothing, at the cost of one compare |
| Send | `CoopLinkSound::Frame`, `Limiter` | Each frame, `LINK_SOUND` (500) per name: the same name again within 80 ms is dropped (one sound started twice), at most 20 a second |
| Hear | `Planner::Plan`, `CoopLinkSound::InHearing` | A received sound reaches the puppet only while it is shown and within 30 m of our Link; beyond that it is dropped at once |
| Play | `CoopLinkSound::Next` / `Finish`, from the puppet director | The game's own calls on the puppet's own SLink user: 0x43db00 (accessor, &asset, name) once per name (the asset is kept per puppet; one not in its user is not looked for again), then 0x440630 (user, &asset, &handle, 0). The user is the puppet's, so the game places the sound at the puppet and fades it with distance. Not started when the puppet's user already plays that name (its own animation made it), never while an Ocarina note is being started (the two share the block's asset and text cells; the handles are their own: eight cells at block +0x6f8, so the block is 0x740). Each started sound keeps its cell; the game's own handle kill 0x7cc120(&handle) stops it after `kLongestMs` (3 s) or when the same name starts again, so a looped asset (spin charge, swim) never plays on forever: the sender's loop end is not sent yet. A call the cave did not make is dropped (`CoopLinkSound::Dropped`): a kill is asked again, a search or emit loses that one sound. With `LICHII_COOP_OCARINA_SOUND=1` received `Ocarina_*` names are skipped here (the OCARINA_NOTE path plays them) |
| Cap | `CoopLinkSound::Budget`, `Heard` | At most 24 starts a second for all puppets together (16 players), at most 6 waiting per player (the oldest goes), and one older than 0.4 s when it could start is dropped |

Log lines: the sender `coop: link sound 'Pl_...' sent (N sent, M held back by the rate limit)` (the first five, then every 500th); a Hookshot without the wrap `coop: this Hookshot has no sound wrap (patch 0014): our Link's sounds are not sent`; with `LICHII_COOP_DEBUG_ANIM=1` the receiver `P1 sound 'Pl_...'` per start and `P1: the sound '...' is not in its sound user`. `LICHII_COOP_DEBUG_SOUND=1` names every user's sound once (`coop: sound probe: user 0x... (our Link) emits '...'`): what the game starts on our Link, and on which user it starts the Ocarina's notes.

Not known until a run: that every Link sound goes through `searchAndEmit` (the AS's own sound events may start through xlink2's trigger path instead: the probe shows which), that `[[x1]]` is the name in every case, and where the Ocarina's notes are started (the "Ocarina" user, not our Link's: then they are not sent this way and the note path above stays).

## Riding (issue 199)

Alex (2026-11-12): "epona makes another player disappear when they mount her". Up to then a remote rider was parked out of sight on purpose (cutscene-hide): the other game has no horse, so the puppet would sit in the air. Now the rider stays seen, on a copy of the horse (`lichii/hook/coop_ride.h`):

| Part | Where | What |
|---|---|---|
| Rule | `coop/host/scene.hpp` | Their command names a horse: `Visibility::ride` (was `park`). The puppet shows and plays their command (`HorseMove`, `RideonRunHorse` ...), which the puppet's own AS (`Player_Adult.root.asb`) has. Also in a talk on horseback (their cutscene) |
| Sender | `CoopRide::Rider` | While our command names a horse, the nearest `Animal_Epona_Adult` on the actor list within 4 m (walked only then, at most 30 a second) is sent as `PLAYER_RIDE` ([protocol.md](protocol.md#riding-player_ride)): its place and turn in our Link's frame, and its AS slot 0. Got off: riding off, three times |
| Copy | `CoopRide::Rides`, `CoopSpawn::Director::SetRides` | For each adult remote with a fresh ride (under 0.6 s old) whose puppet is live and in our place, with no cutscene of ours on: one copy. The rider's own game decides when the ride starts and ends (its `PLAYER_RIDE`), not the puppet's sampled command, which can trail it. The copy is at the puppet's place plus the offset turned by the puppet's turn, so the horse moves and turns with the puppet whatever the network delay. Its animation is the sender's, moved on by the age. Keys have bit 61 set; a new mount gets a new key (a new actor) |
| Pack | `tools/puppet_pack` STAND_INS | `CoopRide_Epona`: Epona's model, LOD and animation set (`AS/Animal_Horse_New.root.asb`) with the shared defaults; no AI, physics, riding or sound. Only ever the stand-in (`CoopSpawn::StandInOnly`): the game's own Epona made here would run on her own AI |
| Fallback | | A pack without `CoopRide_Epona` (built before issue 199), a rider on an old build (no `PLAYER_RIDE`), or no horse found near the rider: no copy, the puppet still shows in its riding pose where the rider is, never hidden |

Log lines: the rider's game `coop: ride: we ride Animal_Epona_Adult, sending PLAYER_RIDE (1 horse seen)` and `coop: ride: we got off, riding off sent`, or `we ride ('...') but no Animal_Epona_Adult within 4 m on the list` when none is found; the other games `coop: ride: P<n> rides Animal_Epona_Adult (horse copy CoopRide_Epona)` and `coop: ride: P<n> off the horse (copy deleted)`, plus the spawn director's `CoopRide_Epona: create copy ...`. `LICHII_COOP_RIDE=0` turns all of it off (then the rider's puppet shows without a horse). The kit needs its puppet pack made again (make-puppet-pack.cmd) for the horse copy.

**Every adult rides their own Epona.** Each game keeps its own Epona (`Animal_Epona_Adult` is never a shared object, [objects.md](objects.md)), so two players ride at the same time, each on the horse of their own game, and each sees the other's as the stand-in copy. Nothing of another player's horse is made as the game's real Epona, so no save sees a second one. What kept a second player from riding was the quest: `Get_Epona` (the race against Ingo) was each game's own, so only the player who won her had a horse that lets Link stay on. Its end is now shared ([flags.md](flags.md), `Quest::complete_only`): once one player has won Epona, every game's quest is `Complete`. A child gets no horse copy (only an adult rides her), a player who joins or changes scene during a ride gets the horse with the next `PLAYER_RIDE` (30 a second), and our place unread for a frame is not sent as getting off. When a ride finds no horse, the log line lists the actors within 6 m (`within 6 m: ...`), and getting off logs how long the ride was. Scenario `epona-own` (both ride at once).

Not checked in game: that the rider's place while riding is the saddle (the offset copes either way), that `Animal_Epona_Adult` is the horse you ride (Epona's adult pack; the child one cannot be ridden), and that the stand-in plays the horse's commands by the names slot 0 shows.

## Ocarina

Issue 152 (testers, 2026-11-11): the puppet plays the ocarina animation, but the others hear no notes and no song. Read from the romfs and `main` (build e0f3ccfaf50065ae), not run.

| Item | Where | Status |
|---|---|---|
| Note sounds | SLink assets `Ocarina_LowB`, `LowC`, `LowCsharp`, `C`, `Csharp`, `D`, `Dsharp`, `E`, `F`, `Fsharp`, `G`, `Gsharp`, `A`, `Asharp`, `B`, `HighD`, `HighDsharp`, `HighE`, `HighF`, and `System_Ocarina_<note>` (the 2D copies for the score window), in `SLink2/slink2.Product.111.bslnk` | Checked as names |
| Song sounds | SLink assets `Ocarina_ZeldaLullaby`, `EponaSong`, `SariaSong`, `SunSong`, `TimeSong`, `StormSong`, `ForestMinuet`, `FireBolero`, `WaterSerenade`, `ShadowNocturne`, `SpiritRequiem`, `LightPrelude` (the song played back when a melody is right), `Ocarina_Correct` | Checked as names |
| Not AS events | The SLink resource names only the ocarina's start and end AS nodes (`Ocarina_Child_StartWait`, `_End`, the adult ones), not the per-note nodes; the AS node events (`AnimationEvent/AsNode/Player_Child.root.baev`) have `Ocarina`, `HoldOcarina`, `OcarinaFingerRest`, no note. So the puppet's own AS (which plays Link's sounds since branch `puppet-sound`) cannot play the notes | Checked |
| How the fingers follow the note | `AS/Player_Child.root.asb` holds per-note nodes (`Ocarina_Child_A_lp`, `…_Close_1st_lp`, `…_Rest`, the adult ones) chosen by AS variables `OcarinaLastSingleNote`, `OcarinaLastMelodyType`, `OcarinaFrameCount`, `OcarinaPerformanceRate`, `IsOcarinaRestNow` | Checked as names; how the AS reads them is inferred |
| The code | `game::OcarinaSound` (label 0x3cced00, used at 0x2e30cb8), `OcarinaMgr`, `OcarinaCreationMgr`, the AI/logic names `ExexutePlayerPlayOcarina`, `QueryIsPlayedOcarinaNote`, `QueryGetPlayedOcarinaInfo`, `QueryOcarinaHitMelody` | Labels |
| Not found | No code or data pointer reaches the strings `Ocarina_A` … (0x3cce7ff …) or `OcarinaLastSingleNote` (0x3ca647d, 0x3ee5596): the note sounds are most likely looked up by a hash or built from a table, so the call that plays a named SLink asset on an actor was not found by reading. None of these names is a GameData flag | Checked (an ADRP/ADD and pointer scan of `main`) |

**Built (branch `puppet-looks`, protocol 10, `OCARINA_NOTE` 140).** Read from `main` this time:

| Item | Where | Status |
|---|---|---|
| The note table | `OcarinaMgr`'s constructor (main + 0xe7c570; the manager is registered by name at 0x32b52d0, factory 0xe7c510, 0x27680 bytes) writes the 19 names `Ocarina_LowB` ... `Ocarina_HighF` at +0x40, in this order: LowB, LowC, LowCsharp, D, Dsharp, E, F, Fsharp, G, Gsharp, A, Asharp, B, C, Csharp, HighD, HighDsharp, HighE, HighF. The earlier "no code reaches the strings" was an ADRP/ADD scan that missed these | Checked |
| Playing a named sound | The AI node `OneShotXLinkSearchAndEmit` (getName 0x295b660, run 0x295b740): the actor's XLink component `[[actor + 0x270] + 0x68]` holds the effects user at +0x70 and the sound user at +0x78 (xlink2 user instances; u32 +0x18 bit 0x1000 picks the ELink system). `0x43db00([[user + 0x58] + 0x18], &asset, name)` finds the asset, `0x440630(user, &asset, &handle, 0)` plays it. `0x7cc120(&handle)` stops it (OneShotXLinkFade's path). A user's running events are a list at +0x28 (0x37416c0 walks it by asset name) | Checked as code; not run |
| Which note a button plays | The game's prompt while the Ocarina is out: "Play a note: L R Y X A", the five notes from low to high as in Ocarina of Time 3D: L the low D, R F, Y A, X B, A the high D | Inferred from the prompt; not heard |

- **Sending** (`lichii/hook/coop_ocarina_sound.{h,cpp}`): each frame, while our reading's AS command is `DemoOcarina` (not `DemoOcarinaMelodyGet`), a note button pressed on player 1's pad (the test script's input included) sends `OCARINA_NOTE` with the note (table index + 1); letting the last one go sends 0. The newest press wins; letting it go while another is held sends that one. Log: `coop: ocarina note 4 (Ocarina_D) sent`. Not sent: pitch bends, notes the game plays for us.
- **Playing** (`lichii/hook/coop_puppet_sound.{h,cpp}`, called by the director for a live, shown puppet): a new note stops the one sounding (the kill call on the handle in the puppet's block, +0x6b0), finds `Ocarina_<note>` in the puppet's sound user and plays it there. A note older than 1.5 s when the puppet can play it is dropped; a puppet made again starts silent. Logs: `P1's sound user 0x... is 'Player_Child' (SLink flags 0x...)` once, then `P1 plays sound 'Ocarina_D' (returned 0x..)` per note; `P1 has no sound user ...` (a pack without sound) or `P1: the sound 'Ocarina_D' is not in its sound user`.
- **Songs** are sound only, as planned below: the other games hear the notes; no melody check, warp, sun, storm or door runs for a remote player. The song's own jingle (`Ocarina_Correct`, the melody played back) is not sent.
- **Test**: `tools/autotest/run.ps1 -Scenario ocarina-notes` (A plays L R Y X A and Zelda's Lullaby). The proof is in the logs (no audio in the autotest): A's `ocarina note ... sent` lines, B's `plays sound` and `P1 sounds: Ocarina_*` lines.

**Plan** (from `ocarina-sound`, before the above; types 140 to 144 for this, protocol 8):
1. **Where our note is** (this branch): `CoopOcarina::OcarinaProbe` (`lichii/hook/coop_ocarina.{h,cpp}`, `LICHII_COOP_OCARINA_PROBE=1`, reads only). While one of our Link's AS slots plays an ocarina node, it logs each slot's command when it changes (`coop: ocarina probe: slots 0=… 1=…`), and every word of the AS controller (0x400 bytes) and its slots (0x200 bytes each) that steps between two small values (below 64), every 33 ms; a word that steps more than 40 times is muted. If a slot's command names the note (`Ocarina_Child_A_lp`), that is the note; else the word that steps once per note, with the same value for the same button, is `OcarinaLastSingleNote`.
2. **Send it** (next round): `OCARINA_NOTE` (140): player, server time, note (an index into the 19 notes, 0 for none) and the melody played (0 none, else the song); to the players in the sender's scene, like `PLAYER_POSE`. Only notes our Link plays itself: an ocarina the game plays for us (a cutscene, a song taught) is not sent.
3. **Play it at the puppet** (next round): write the note into the puppet's AS variable, so its fingers follow and, if the ocarina code reads the AS state, the note sounds from the puppet; or call the SLink play on the puppet with `Ocarina_<note>` once its address is known (the same probe run, with a breakpoint on the SLink user's lookup, finds it). **A song is sound only**: nothing of the game's melody check (`QueryOcarinaHitMelody`, the warp, the sun, the storm, the doors) runs for a remote player, since only the local `OcarinaMgr` checks melodies and the puppet has no ocarina logic; the song's `Ocarina_<song>` sound plays at the puppet.
## PvP

The puppet itself takes no hits: its game has no collision for another player's Link. PvP ([play-online.md](play-online.md), "PvP"; `lichii/hook/coop_pvp.h`) is decided outside the puppet. The attacker's game compares its own sword swing (the three blade points of the grass sync's foliage cut, [world.md](world.md)) and the place where its own shots ended with each other player's Link as presence shows it (the same place the puppet stands), and sends `PVP_HIT` to that player. The victim's game gives its own Link the hit through the hit replay the enemy sharing has (`Mover::AddHit`, the game's own damage record, so Link flinches and is knocked away from the attacker's place; `LICHII_COOP_PVP_REPLAY=0` leaves it out), and writes the life directly when the game has not taken it within 0.4 s. The hurt the victim's Link shows reaches the attacker's screen through the pose, as any animation does. Not seen in a game yet: whether Link's Life component takes the replayed hit (otherwise only the direct write, with no flinch).

## Colour

Each player's puppet wears a tunic and cap in that player's colour (`tools/puppet_pack/tunic.py`, `lichii/hook/coop_colors.*`, branch `puppet-colors-3`).

**Why a texture and not a parameter.** Two earlier ways showed nothing usable in the game (reports `puppet-colors-2`): writing the tunic material's colour parameters (`ns_param_color0..5` of `Mt_Angin`, through the game's own setter 0x3595f60) never changed the tunic, and the hit-flash model colour (model + 0x20, 0x2e1d200) darkens the whole Link, face and skin too. The game's own looks are data: the Goron and Zora tunics are models of their own (`Armor_Adult_Goron` names `Mt_Angin_Armor_Adult_Goron_Alb` and hides Link's `G_Angin`, `G_Body`, ... material groups through its `ArmorParam` `HideMaterialGroupNameList`), and a model finds its textures in `Tex/` by name.

**What the lifted code shows** (the game's `main`, translated, `f_7103595f60`): the setter stores the 16 bytes at `[slot + 0x48] + offset` (offset from the shader's parameter table, `[[[slot] + 0x10] + 0] + 0x20` + 0x18 * index + 0x10), sets the parameter's bit in the dirty mask `[slot + 0x10]` and bit 0 of the byte at slot + 0x2c, and marks the model unit for upload over its buffered frames (bits 7..9 of the u16 at unit + 0x14, one per buffer, the buffer count in bits 0..1). The material slot is `[unit + 0x108] + 0x80 * index` (or `[[unit + 0x110] + 8 * index]`). So a write is uploaded like any other; the tunic's green simply does not come from those parameters: it is the tile texture.

**The tunic's texture.** Adult `Mt_Angin` (tunic and cap) and the child's cloth take their colour from one tile, `Tex/Angin_Tile_A_Alb.bntx` (BC3 sRGB, 512 x 512 with mips), named as a string in `Model/Player_Adult.Player_Adult.bfres` and `Model/Player_Child.Player_Child.bfres`. Our own Link uses it too, so it is never changed in place.

**What the pack makes** for each colour (`--tunics`, default all nine; green is the game's own):

| File | What |
|---|---|
| `Tex/Angin_Tile_<L>_Alb.bntx` | The tile with both RGB565 end colours of every BC3 block recoloured: each texel keeps its brightness against the tile's average, and the average becomes the colour. The texels are fixed blends of the two end colours, so this needs no encoder; the block order (tiling) does not matter. The name has the same length (`<L>`: R, O, Y, C, B, P, M, W, K). |
| `Model/CoopTunic_<Age>_<Colour>.Player_<Age>.bfres` | The player's model with the tile's name changed (same length, so nothing in the file moves). The model inside is still `Player_<Age>`, so the pose's skeleton id is unchanged. |
| `CoopPuppet_<Age>_<Colour>` | The puppet actor with its own ModelInfo (`FmdbName` `Player_<Age>`, `ModelProjectName` `CoopTunic_<Age>_<Colour>`) and the same in its ActorInfo row. |

All three get size-table entries (the game's own file's size). About 69 MB for the nine colours; the pack takes about a minute. `--test-tunic <colour>` gives the plain puppets a colour too (to test a pack with a hook that knows no colours).

**Which colour.** A player picks one in `coop-options.ini` (`color = blue`, or `#RRGGBB`; see [play-online.md](play-online.md#your-colour)); the kit passes it to the emulator as `LICHII_COOP_COLOR`, and `color` in `coop.ini` works too. It goes to the others in `PLAYER_EQUIP` component 16, `color`, with the equipment, so a newcomer gets it when the equipment is sent again on `PLAYER_JOINED`. A player who chose none gets the colour of their player number (`coop::protocol::slot_color`: 1 blue, 2 red, 3 green, 4 yellow). The planner picks the actor `CoopPuppet_<Age>_<Colour>` for the preset nearest the colour (`CoopColors::TunicOf`), the plain `CoopPuppet_<Age>` for green; a colour change makes the puppet again, as an age change does.

**When a tunic does not work.** A pack whose `coop_puppet.json` lists no tunic puppet (made before them, or with `--tunics none`) turns the colours off at start (`PackHasTunicPuppets`; the log says "the puppet pack has no tunic colours", and the start line "tunics off (the pack has none)"): the game refuses an actor its ActorInfo does not know, so asking for one would leave no puppet at all (Hookshot proof 3, 2026-11-12: `create CoopPuppet_Child_Blue requested (returned 0)`, refused for good). Each coloured actor also falls back on its own to the plain puppet (the planner gets `Director::FailedTunics`; the log names the actor and why): when the game refuses its create 6 times in a row (`kRefusedTunicFallback`, about 18 s: longer than the refusals of a load), or its create times out twice, in both cases only while no tunic puppet has spawned yet (an older pack; once one has spawned with its model, slow loads no longer count), or when it spawns but has no `Player_` model 3 s later (`kTunicModelCheckMs`: neither a model name through the skeleton nor a model object at parts + 0x260), which is how a model copy the engine refused would show. `LICHII_COOP_TUNICS=0` turns the colours off. The old model tint stays for tests only (`LICHII_COOP_PUPPET_TINT=<0 to 1>`, off by default).

**Your own Link (#202).** The pack tool also writes, for each colour but green, the game's tile recoloured under its own name (`Angin_Tile_A_Alb`) into `coop_self/<colour>.bntx.zs` beside the pack's `romfs/`, where the game does not load it; `coop_puppet.json` lists them as `self_tiles`. It is as long as the game's tile once unpacked, so the size-table entry stands. At each start the kit's `play.ps1` (`Set-KitSelfTunic` in `scripts/kit-paths.ps1`) copies the one nearest your `color` (`Get-KitTunic`, the same rule as `CoopColors::TunicOf`) over `romfs/Tex/Angin_Tile_A_Alb.bntx.zs` in the pack, and sets `LICHII_COOP_SELF_TUNIC=<colour>`; for green, no colour, `LICHII_COOP_TUNICS=0` or a pack without it, it takes that copy away and your Link is the game's green. The child model names a second tunic texture, `Angin_Tile_A_Dark_Alb` (the tunic's body; the adult model does not): with only the main tile changed, a child Link's hat and shoulders took the colour and the body stayed green. That texture is ASTC, so its colour copies are the main BC3 tile recoloured a little darker (`tunic.DARK_SCALE`) under the dark name (the two files' string sections have the same place and room). The pack tool writes them for the puppets (`Tex/Angin_Tile_<letter>_Dark_Alb`, named by the child model copies) and for you (`coop_self/<colour>.dark.bntx.zs`, listed as `self_dark_tiles`, raising the game's dark tile's size-table entry to the BC3 size); `Set-KitSelfTunic` copies and removes it with the main tile. The adult's lower tunic and back (mesh `Hood_A`, material `Mt_Vest`, `Mt_Vest_Player_Adult_Alb`, ASTC 8x8) and its rope and hood parts (`Mt_Angin_Acs_Player_Adult_Alb`, BC1) are their own textures: `puppet_pack/tunic_extra.py` changes only their green (and olive) block end colours in place, against the main tile's brightness, so size and layout stay the game's (ASTC weights untouched; base+offset blocks re-encoded as direct ones). Puppets get copies named `..._Coop_<letter>_Adult_Alb`, named by the adult model copies; your own are listed with the tiles in `coop_puppet.json` `self_files` ({colour: {romfs path: file}}), which `Set-KitSelfTunic` copies in, and takes every one away on green or no colour. In bigrun5 the adult's own Link still showed the game's green on the hood tail and skirt although the replaced `Tex/` files were in place (the puppet, with renamed copies, was right), so your own adult Link now also wears the puppet's model copy: `self_files` maps `Model/Player_Adult.Player_Adult.bfres.zs` to `romfs/Model/CoopTunic_Adult_<Colour>.Player_Adult.bfres.zs` in the pack (same size, so the game's size entry stands); the child keeps the game's model. Side effect: other models naming `Angin_Tile_A_Alb` (Mido, the Kokiri bed, the ocarina holder) take your colour too. Only the Kokiri tunic changes: the Goron and Zora tunics are other models with their own textures. A player who chose no colour sees their own Link green while the others see their player number's colour.

Your tile is the one the plain puppet wears too, so with it a green player would wear your colour. The pack therefore also has a green puppet (`CoopPuppet_<Age>_Green`, the game's tile renamed `Angin_Tile_G_Alb`), and the emulator picks it for green players when `LICHII_COOP_SELF_TUNIC` is set (not green) and the installed pack's `coop_puppet.json` lists both green puppets (`PackHasGreenPuppets`); otherwise the plain puppet, and the log says so. The autotest starts the emulator without `play.ps1`: copy the tile and set the switch by hand there.

**Pack hints.** A pack whose `coop_puppet.json` has no tunics (made before the colours, with `--tunics none`, or from a game without the tunic tile) makes the kit say at every start that the other players show in green and to run `make-puppet-pack.cmd` again; a pack built with `--tunics none` on purpose says so in its manifest (`tunics_off`) and gets no line.

**Hookshot.** The recomp reads the same romfs mods, so the same pack gives the same tunics there. The Hookshot start does not call `Set-KitSelfTunic`; your own Link gets its colour the way below instead.

### Changing your colour in the game

Branch `look-live`, Hookshot only: `hookshot/src/coop_look.*` (the API the pause menu calls), `coop_look_tunic.*` (your own Link), `coop_look_live.cpp` (the module's side).

- **The menu.** The pause menu's Settings page shows `CoopLook::Shown()` and calls `SetPending(look)` when the player picks a colour; nothing changes while the game is paused. When the pause screen closes it calls `ApplyOnUnpause()`, and on the game's next frame (`hos_coop_frame`, `CoopLook::Frame`) the look is applied once. A look is one colour per part (`CoopLook::Part`); today the only part is the tunic, and the struct has room for more parts under numbers that never change.
- **The others see it.** `CoopHook::SetColor` hands the colour to the sampler, which sends it in `PLAYER_EQUIP` `color` on the next frame; each other game makes its puppet of you again in the new tunic, as for a colour at the start. "By player number" after a colour sends your slot colour (`slot_color` of your number), since an unset colour would leave the others on the old one. A newcomer gets it with the equipment on `PLAYER_JOINED`, and a session started later from the menu wears it rather than the settings' colour. No new message type.
- **Your own Link changes at once.** A loaded texture's texels sit in the game's memory as in its file, and Hookshot decodes a texture again when its bytes are written through the bridge (`write` marks the pages; its renderer's `textures_written`). The pack tool writes, for every colour and green, the texels of your Link's tunic textures (tile, dark tile, `Mt_Vest`, `Mt_Angin_Acs`) to `coop_look/<colour>.bin` beside the pack's `romfs/` (`tools/puppet_pack/look_live.py`: name, offset in its file, texels). A worker thread searches the game's heap (from 0x1000000000, Hookshot `HOS_HEAP_BASE`, to where it is mapped) for those texels, takes a place only when its file's header (BNTX, the texture's own name, the BRTD block) is in front, so the puppets' renamed copies are left alone, and writes the new colour's texels over them: same format, same size, nothing else moves. It checks its places each second and writes them again after the game loads the texture again (a scene change, an age change). The first change searches the whole heap (a few seconds); later ones are written within a second. With the settings' colour at the start it does the same, so your own Link wears your colour on Hookshot too.
- **Saved.** The colour is written to the kit's `coop-options.ini` (`color = <name>` or `#RRGGBB`; "by player number" comments the line out), else to `coop.ini`, so the next start keeps it.
- **What is live and what is not.** Live: the Kokiri tunic's colour, for you and on every puppet. Only the pack's ten presets are textures, so your own Link wears the preset nearest a `#RRGGBB` colour (the others' puppets too: `CoopColors::TunicOf`). The Goron and Zora tunics are other models and do not change. A pack made before `coop_look/` (the log says "the puppet pack has no coop_look/...") changes only what the others see; make the pack again. Not live: a part-by-part look (shield, sword, boots) needs per-part textures in the pack first.
### The Link customizer

Branch `customizer-full`, Hookshot only. The Multiplayer tab's **Customize Link** page gives each part of Link's look its own colour; Link wears it when the menu closes, with no restart and no scene load, and it is saved for the next start. Files: `hookshot/src/coop_look.*` (the parts, settings, wire form), `coop_look_menu.*` (the page), `coop_look_paint.*` (the painting), `coop_look_parts.*` (which texture holds which part), `coop_look_live.cpp` (the module's side), `lichii/hook/coop_look_share.*` (the network), `tools/puppet_pack/look_live.py` (`coop_look/gear.bin`).

- **The parts, and why these.** Read from the game's own textures (`Tex/`): every shield, sword, pair of boots and pair of gauntlets has **one** colour texture holding all its parts side by side (for example `Mt_Weapon_Shield_Hylian_Alb`, BC1: the rim and the back's cross grey, the face blue, the bird red, the back wood). A block of such a texture has a few end colours and its texels are blends of them, so a part is the set of end colours of one tone (grey, blue, red, brown, gold...). The parts that are truly apart that way:

  | Item | Parts (number) | Tones that make each part |
  |---|---|---|
  | Tunic | Kokiri (0), Goron (1), Zora (2) | Kokiri: the pack's colours (below); Goron: red cloth; Zora: blue cloth |
  | Hylian Shield | Frame (3), Face (4), Emblem (5) | grey; blue; red (the gold Triforce stays) |
  | Mirror Shield | Rim (6), Mirror (7) | red; grey and blue |
  | Deku Shield | Wood (8) | all of it: one piece of wood |
  | Kokiri Sword | Blade (9), Hilt (10) | grey; brown |
  | Master Sword | Blade (11), Hilt (12) | grey; blue (the gold and the gem stay) |
  | Biggoron's Sword | Blade (13), Hilt (14) | grey; blue |
  | Boots | Leather (15) | brown, in all three pairs |
  | Gauntlets | Metal (16) | the Silver's grey, the Golden's gold |

  The numbers never change (settings and network use them). **Not apart:** the cap is in the tunic's tile texture on adult Link (one part with the tunic); a shield's wooden back and the swords' gold guards keep the game's colours (they share tones with nothing worth a part of its own).
- **The page.** On the left, every part under its item's heading: Left and Right try the presets (the game's own, then CoopColors' ten), Confirm opens a mixer for that part (Red, Green and Blue, 8 a press; a preset row; Game's own colour; Done). "All parts back to the game's own" ends the list. On the right, a picture of Link in the colours chosen, the part looked at outlined in gold; the tunic, shield and sword in it are the ones last looked at. Mouse: a click on a value's halves changes it, on a name chooses it.
- **Why a picture, not the 3D model.** Hookshot's menu draws flat over the paused game and has no way to draw a model. Patch 0019 lets a module tab hand the menu coloured shapes (`hos_coop_menu_picture`, drawn over the top of the help column), and the page draws Link's outline in the real colours. The game's own Link shows the result when the menu closes.
- **Your own Link, any colour.** The pack has the game's own texels of the 17 gear textures (`coop_look/gear.bin`, same format as the tunic's colours). On every change the module paints them in the look on a worker thread (`coop_look_paint`: the end colours of a part's tones become the colour, each as bright against the part's average as it was, so the shading stays; ASTC blocks keep their weights and are re-encoded in the same range; a block that cannot be re-encoded exactly stays). This is a C++ port of the pack tool's tunic rewrite (`tunic_extra.py`) and gives the same bytes (unit test with blocks made by the tool; checked on the game's own textures too). A second `CoopLookTunic::Wardrobe` finds the textures in memory and writes them, as for the tunic; every close of the menu searches again at once, so a shield equipped in the pause menu is painted at once. Painting all 17 textures takes about 1.4 s of the worker's time.
- **The others.** The look goes out in `PLAYER_LOOK` (430, [protocol.md](protocol.md#looks-player_look)) when it changes and again whenever someone joins or the game connects again; the tunic's colour still goes out in `PLAYER_EQUIP` `color` too. Each game keeps the others' looks (`CoopLookShare::Of`, log "P<n> wears <k> part colour(s)").
- **Saved.** `coop-options.ini` of the kit, else `coop.ini`: the tunic as `color`, each other part as `look_<part>` (`look_hylian_frame = #E0C030`); a part back to the game's own is commented out.
- **Not done yet.** The others' puppets show only the tunic's colour: a puppet's shield and sword are the game's own items, whose textures every Link on screen shares, so a puppet's parts need its own copies first (a per-player copy of each item in the pack, painted from `PLAYER_LOOK` by the same code). For the same reason, until then your own gear's colours also show on the puppets and on a shop's or a wall's copy of the same item on your screen. The child's tunic textures and the masks have no parts.

## Not done

1. The local player's current AS command name and frame, for the sampler. 0x3abebc0 reads a frame; the name was not found.
2. ~~The engine's actor delete request.~~ Found: 0xa6490 (see [Spawn and move](#spawn-and-move-what-is-built)).
3. The run slots of `ExecuteActorForceSetMtx` and `OneShotActorRequestSetMtx`, if a direct pose write does not hold.
4. ~~What the handle slot holds~~ (a create request, see [Spawn and move](#spawn-and-move-what-is-built)); the meaning of `w4`, the stack arguments and create argument +0x40.
5. Whether the engine accepts an uncompressed pack, and whether the size table needs entries for new packs.
6. Follow, upper-body animation and equipment (worn and changed) are proven in game (2026-11-09); leg speed, scene changes and adult Link are not run. Spawn, follow and delete are [proven in game](#proven-in-game-2026-11-08).
7. A puppet created while its player is far away (about 47 units, 2026-11-09) is not drawn when the player comes near; its fairy is. Recreated nearby, it is drawn. Not looked into.
