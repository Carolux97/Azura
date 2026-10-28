# RAAM's Shadow DLC

Investigation notes on running the RAAM's Shadow campaign DLC: what the DLC package needs from the
game, what the Title Update (TU) provides, and what each test showed. Facts are marked as
**verified** (seen in a run, a dump or the code) or **inferred** (deduced, not yet checked), as in
[reverse-engineering.md](reverse-engineering.md).

Nothing here is game data: only names, sizes and identifiers. The player's own DLC and TU packages
are only ever used locally; Azura never ships them.

## Contents

- [The DLC package](#the-dlc-package)
- [The Title Update](#the-title-update)
- [Test results](#test-results)
- [The TU script patches](#the-tu-script-patches)
- [DLC packages that need the TU's classes](#dlc-packages-that-need-the-tus-classes)
- [What the DLC imports from GearGame](#what-the-dlc-imports-from-geargame)
- [How Azura plays it with the disc build](#how-azura-plays-it-with-the-disc-build)
- [Options considered](#options-considered)
- [Tools](#tools)

## The DLC package

- **Format (verified):** an STFS `LIVE` package, content type `00000002` (Marketplace content),
  title `4D5308AB`, display name "Campaign".
- **Install (verified):** the SDK's `ContentManager::InstallContent(path)` extracts it to
  `<user data>/0000000000000000/4D5308AB/00000002/<package>/` and writes
  `Headers/00000002/<package>.header` with the license from the package. The game then enumerates
  it (`XamContentCreateEnumerator`), mounts it (`XamContentCreate`, root `DLC0`) and reads its
  files.
- **Content (verified):** Act 6 and chapters 31 to 35 (`SP_Bank_P`, `SP_Caravan_P`,
  `SP_School_P`, `SP_CityHall_P`, `SP_Orphanage_Intro_P`), and configuration that sets
  `[GearGame.GearGame] bIsCampaignDLC1Installed_FIX=true`.
- **Main menu (verified):** the disc build's main menu lists the entry as
  `MAINMENUOP_Campaign` → `(Label="CampaignDLC",Next=MAINMENUOP_None,bIsDisabled=true)` with the
  text "Coming Soon!".

## The Title Update

- **Format (verified):** an STFS package, content type `000B0000` (Title Update).
- **Content (verified):**
  - `default.xexp`, 1.9 MB: a delta patch for `default.xex`;
  - `GearGame/Patches/ScriptPatch_Engine.bin`, `ScriptPatch_GearGame.bin` and
    `ScriptPatch_IpDrv.bin`, each with a `_us` companion file of a few bytes;
  - `TU__*.ini` configuration and `TU__*.<language>` localisation files, about 95 in all.
- **Executable (verified):** the SDK applies a `default.xexp` that sits next to `default.xex`
  (`UserModule::LoadFromFile`), and `rexglue codegen` loads through it. The patched executable is
  version 0.0.6.11 (the disc build is 0.0.0.11). The SDK warns that the patch's signature hash does
  not match the base executable, but the patch applies and the game runs.
- **Code layout (verified):** the patched executable has 56233 functions, against 56180 on disc.
  Only 2131 of the disc build's function addresses are still function starts in the TU build. The
  script interpreter (`0x822DF9E0`, `0x822DF4D0`) and the three manifest `midasm_hook` addresses
  (`0x823F42D8`, `0x82762C54`, `0x827C9628`) are not where they were. Every hard-coded address in
  Azura would need its TU equivalent.
- **Mount (verified):** the SDK mounts `update:` from `update_data_root`. Both the disc and the TU
  executables list the root of `UPDATE:` at start-up and probe a `_us` companion for each file
  there.

## Test results

All runs use the player's own packages, with the DLC installed in a test user data folder.

| Executable | `update:` contents | Boot | DLC in the Campaign menu |
| --- | --- | --- | --- |
| Disc (Azura) | none | yes | no; chapter 31 forced through the URL loads endlessly |
| Disc (Azura) | `TU__*` files only | yes | no |
| Disc (Azura) | `TU__*` files and `default.xexp` | yes | no |
| Disc (Azura) | `GearGame/Patches` only | **stuck on the loading screen**, no error in the log | — |
| Disc (Azura) | whole TU | **stuck on the loading screen** | — |
| TU, bare SDK app (no Azura code) | whole TU | yes | **yes; chapter 31 (Bank) plays** |

- **Verified:** the DLC shows and plays only with the TU executable and its script patches.
- **Verified:** the script patches alone stop the disc executable on the loading screen. The
  `TU__*` files and `default.xexp` in `update:` do not.
- **Verified:** the property `bIsCampaignDLC1Installed_FIX` does not exist in the disc build's
  `GearGame` class (168 properties dumped), so the DLC's configuration line has no effect there.
- **Inferred:** the script patches extend classes with new properties and functions that the TU's
  native code was built for, so the disc executable cannot use them.
- **Pitfall (verified):** the SDK's host folder device does not follow a symbolic link to a
  folder. A `GearGame/Patches` that is a symlink is never read, so test folders must hold real
  copies.
- **Interpreter (verified):** in the TU build, 39 functions that the code generator did not find
  run in the interpreter. They would need function hints in the manifest.

With Azura's own script patch on the disc executable (see "How Azura plays it with the disc
build"):

- **Verified:** the main campaign still loads (chapter 1 from the command line, no access
  violation, HUD up).
- **Verified:** chapter 31 (Bank) starts from the menu and from a checkpoint, and plays past the
  Reavers, the Options menu opened during play, and the satellite Hammer of Dawn section
  (`AddEnemyLocators` uses the camera's store), up to the meeting with Jace, with no access
  violation in the log.
- **Verified:** the player finished Bank: the satellite marks the enemies, and the game goes on to
  Caravan.
- **Verified:** chapters 32 to 35 (Caravan, School, City Hall, Orphanage) load from the menu with no
  access violation or fatal error (City Hall after adding the post process functions).
- **Not verified yet:** playing chapters 32 to 35 through, RAAM's shield, the Brumak, and the
  DLC's achievements.

## The TU script patches

- **Format (verified):** each `ScriptPatch_*.bin` is an Unreal Engine 3 compressed chunk: tag
  `0x9E2A83C1`, block size `0x20000`, total compressed and uncompressed sizes, then one
  (compressed, uncompressed) size pair per block, all big-endian. Each block is XMemCompress LZX
  (window 2^17) split into frames. A frame starts with `0xFF`, a 16-bit uncompressed size and a
  16-bit compressed size, or with just a 16-bit compressed size for a full 0x8000-byte frame.
  Joining the frames' data and decoding it as one LZX stream with libmspack gives the exact
  uncompressed size.
- **Sizes (verified):**
  - Engine: 10271 bytes uncompressed, 10 functions;
  - GearGame: 1912800 bytes uncompressed, 458 functions and enums;
  - IpDrv: 3733 bytes uncompressed, no functions.
- **Layout (verified for the start, big-endian):**
  - the package name;
  - the name table: a count, then strings (a 32-bit length including the terminator, then the
    characters);
  - the new exports: a count, then for each one its class, super and outer indices (32-bit), its
    name as a string, and 48 more bytes (object flags `0x0007000400000000` on every one, the rest
    mostly zero);
  - the imports, in the same style;
  - then the new objects' data and replacement bytecode for existing functions, named by their
    full path (`GearGame.UIScene_FrontEnd:StartCampaign`).
  - Outer indices point into the original package's export table, or past its end for objects
    the patch itself adds. Class indices are imports of the original package, or 0 for a new
    class.
- **New exports (verified):**
  - Engine: 19 (properties and functions);
  - GearGame: 1365, including **55 new classes**: weapons and projectiles for events
    (`GearWeap_*_Event`), tutorials, mutators (`GearMut_Hardcore`), leaderboards, matchmaking
    tweaks, and campaign classes such as `GearDLCCampaignActData`, `AICmd_Base_CoverAI_vsRaam`,
    `Turret_SatelliteHODBase` and `GearPawn_SatelliteHODCamera_Base`.
- **Native code (inferred):** the TU executable has only 53 more functions than the disc one, too
  few for 55 native classes, so the new classes are most likely script only.
- **New names in GearGame (verified):** `bIsCampaignDLC1Installed`,
  `bIsCampaignDLC1Installed_FIX`, `IsCampaignDLC1Installed`, `PlayerHasCampaignDLC`,
  `ServerSetCampaignDLCFlag`, `ShowMissingCampaignDLCMessage`, `CampaignDLCPurchaseID`,
  `GetCampaignDLCPurchaseID`, `bShowCampaignDLC1Bang` and `PartyMemberLacksRequiredCampaignDLC`.
- **Patched functions, GearGame (verified):**
  - about 100 of the 458 are about DLC, campaign, chapters or the front end;
  - the classes with most changes are `UIScene_FrontEnd` (49), `GearPC` (40),
    `GearPawn` and `GearGameMP_Base` (17 each), `GearHUD_Base` (15), `GearProfileSettings` (13)
    and `UIWidget_MainMenu` (12);
  - functions on the campaign path include `GearCampaignActData:InitializeChapters`,
    `UIWidget_MainMenu:ConstructActList`, `UIWidget_MainMenu:ConstructChapterSelectData`,
    `UIScene_FrontEnd:StartCampaign`, `UIScene_FrontEnd:DLCEnumerationFinished`,
    `GearGRI:PlayersHaveRequiredContentForMap` and the enum
    `GearLeaderboardSettingsBase:EChapterContexts`;
  - the rest are multiplayer, weapons, AI and balance fixes.
- **Patched functions, Engine (verified):**
  - `PlayerController:InitInputSystem` and `PlayerController:BaseSpectating.ReplicateMove`;
  - `PlayerController:RemoveMissingPeersFromSession`;
  - `PhysicsVolume:PostBeginPlay`;
  - `GameInfo:Login`;
  - `IniLocPatcher:AddFileToDownload`;
  - `InterpActor:ShouldSaveForCheckpoint`;
  - `PlayerReplicationInfo:RegisterPlayerWithSession` and `PlayerReplicationInfo:CopyProperties`;
  - `UIDataStore_OnlinePlayerData:OnRegister`.

## DLC packages that need the TU's classes

- **Packages (verified):** the DLC holds 1070 `.xxx` packages, one `.upk`, movies (`.bik`) and
  texture caches (`.tfc`). Cooked packages have an uncompressed summary followed by UE3
  compressed chunks in the format above. Decompressing every chunk shows each package's name
  table in plain text.
- **Verified:** 22 DLC packages name 8 of the classes that only the TU adds, and every chapter
  has at least one:

  | TU class | DLC packages that name it |
  | --- | --- |
  | `GSM_GrapplingHook_DynamicClimbDrop` | 16, in Bank, Caravan, City Hall and Orphanage |
  | `GearPawn_LocustEliteBoomerFlailBase` | 5 (Caravan, City Hall, Orphanage) |
  | `ThumperSpikeBase` | 4 (Caravan, City Hall) |
  | `PawnBlockingVolume` | 4 (School, City Hall, Orphanage) |
  | `GearPawn_SatelliteHODCamera_Base` | 3 (Bank, City Hall) |
  | `Turret_SatelliteHODBase` | 2 (Bank, City Hall) |
  | `SeqEvent_BerserkerVulnerable` | 1 (School) |
  | `AICmd_Brumak_Action_AttackScriptedTargets` | 1 (Caravan) |

- **Inferred:** this is why chapter 31 loads endlessly on the disc build: its levels hold objects
  of classes that do not exist there. Objects in a level also store their property values by
  name, so any replacement class must declare the same properties with the same types.

## What the DLC imports from GearGame

- **Verified:** reading the import table of every DLC package (`tools/dlc` reads packages; the
  check compares each import of a disc script package with that package's exports) gives the
  complete list of what the DLC needs from GearGame and the disc does not have:
  - the 8 classes above, their default objects and every object those own (components, camera
    shakes, force feedback waveforms);
  - three properties of disc classes: `GearPawn_LocustGeneralRaamBase.bShieldDispersed` (RAAM's
    shield, in the Caravan and City Hall fights), `Vehicle_Reaver_Base.TaccomMesh` and
    `GearGRI.SHODTargets` (the satellite Hammer of Dawn sections of Bank and City Hall);
  - four functions of disc classes, which the DLC's classes override:
    `GearPawn.NonFinalActivateRoadieRunComet`, and `GetHitAnimName_Damage`,
    `PlayHitReactionAnimation` and `ShouldTurnCursorRedFor` of `Vehicle_Reaver_Base`.
- **Verified:** the DLC also calls three TU functions by name (`EX_VirtualFunction`), which no
  import table shows. Crossing every virtual call in the DLC's script with the functions the TU
  adds finds only these; the DLC's other calls of TU names reach a function of the same name that
  the disc has in a parent class.
  - `GearPC.EnablePostProcessEffects(Effect, Chain, ChainName)` and
    `GearPC.DisablePostProcessEffects(Effect, ChainName)`, called by Myrrah's transmissions
    (`SeqAct_MyrrahTalk`, in Caravan and City Hall) and the satellite camera. Without them City
    Hall stops with "Failed to find function EnablePostProcessEffects in GearPC_SP" (verified).
    They add a post process chain to the local player and turn one of its effects on, and turn it
    off and remove the chains that hold it.
  - `GearGRI.AddSHODTarget`, below.
- **Verified:** about `AddSHODTarget`:
  `GearPawn_SatelliteHODCamera.AddEnemyLocators` (in `SP_Bank_01_S`, `SP_CityHall_04_S` and
  `SP_CityHall_04_Seeders`) calls `GRI.AddSHODTarget(Enemy)` as a virtual function. It is also the
  only script that uses `GearGRI.SHODTargets`. In the TU, `AddSHODTarget` adds its argument to
  `SHODTargets` when it is not there yet, as the disc's `GearGRI.AddTaccomTurret` does with
  `TaccomTurrets`.
- **Verified:** the act and its chapters come from the DLC's own configuration
  (`Xbox360-GearGame.ini`: `[Act6 GearDLCCampaignActData]`, `GearTypes.ChapterLevelNames[31..35]`
  and `[ChapterNN GearCampaignChapterData]`). The disc's `EChapterPoint` already has `CHAP_DLC`
  entries for those chapters.

## How Azura plays it with the disc build

Only the player's DLC package is needed. Azura adds what the DLC imports with a script patch of its
own, in the engine's patch format, and fixes what the disc build's patch loader cannot do.

### The script patch (`runtime/src/dlc/raams_shadow_patch.cpp`)

- Written on the player's machine from their own game files: every index, name and layout comes
  from their GearGame and Engine packages, so nothing derived from the game is distributed, and it
  works with any copy of the game whose packages have what it needs. Nothing comes from the Title
  Update.
- The runtime writes it as a one-off task (`azura --make_dlc_patch=<folder>`), in about two
  seconds: it decompresses both packages (`lzx.cpp`), reads them (`ue3.cpp`), builds the patch and
  packs it. `tools/dlc/make_patch.py` is the Python version the format was worked out with; both
  write the same bytes (verified with `cmp`). Its content is Azura's: script
  subclasses of the disc classes with the properties the DLC uses, default objects whose
  subobjects copy the parent's, the three properties of disc classes, and four functions of disc
  classes: `GearPawn.NonFinalActivateRoadieRunComet` (empty), `GearGRI.AddSHODTarget` (the
  bytecode of the disc's `GearGRI.AddTaccomTurret`, with its array and parameter replaced), and
  `GearPC.EnablePostProcessEffects` and `DisablePostProcessEffects`, written with
  `tools/dlc/bytecode.py` (with the flags of the disc's `GearPC.ClientColorFade`: reliable, client,
  simulated).
- `tools/dlc/bytecode.py` assembles UnrealScript bytecode: stored object references take 4 bytes
  and loaded ones 8, and jump targets and context skips count the loaded form. Verified: it
  reproduces the TU's compiled bytes of both post process functions exactly (with the TU's
  indices, as a test only).
- The patch does not declare the three `Vehicle_Reaver_Base` functions: see "Calls to parent
  functions the disc lacks" below.
- `GearPawn_SatelliteHODCamera_Base` has one property of Azura's own, `SHODTargetStore`, where the
  satellite camera keeps its targets (see below).
- The packages are Xbox LZX data (XMemCompress frames in Unreal Engine 3 chunks). The SDK's LZX
  decoder (libmspack) expects a padding byte after an uncompressed block of odd length, which Xbox
  streams do not have, so Azura has its own decoder (`lzx.cpp`, and `tools/dlc/lzx.py`). The patch
  is stored as LZX uncompressed blocks, which the engine reads like any other LZX data; the engine
  also reads `ScriptPatch_GearGame.bin_us`, the uncompressed size as text.
- The patch is only valid for the GearGame.xxx it was made from. The launcher keeps it in
  `<runtime>/dlc/raams-shadow/`, with a `dlc_patch.json` that names that file's SHA-256 and the
  runtime that made it, and makes it again when either changes (a runtime update replaces the
  runtime folder, so a new runtime always makes its own).

### Format details the disc's loader is strict about (verified, by test)

- A class blob: NetIndex, Next, SuperStruct, Children, bytecode sizes and bytecode, probe mask,
  label table offset (16 bits), state flags, function map, class flags, ClassWithin, config name,
  component map, interfaces, DLL bind name, default object.
- A function blob: NetIndex, no tagged properties (None), Next, SuperStruct, Children, bytecode
  size in memory (each object reference takes 8 bytes there, 4 in the file), bytecode size in the
  file, bytecode, native index (16 bits), operator precedence (8 bits), function flags. There is
  no friendly name in cooked data.
- Parameters are chained from the last declared to the first, then the return value; a call
  passes arguments in that order.
- `StaticMeshComponent` and `BrushComponent` serialize an extra (empty) array after their
  properties.
- An array property comes right after its inner property, has `CPF_NeedCtorLink`, and, in a class
  that has fields, its Next must name one of them (with `Next` 0 the game crashes at boot).
- A patch property or function of an existing class is linked in as the class's first field, and
  the class is laid out again (each time one of its new functions is preloaded). A property with
  `ArrayDim` 1 shifts every field after it, which the executable's native code does not expect; with
  `ArrayDim` 0 it takes no room. Its `Next` names the class's first field, so the class keeps them
  all.
- A new property and new functions on the same existing class (`TaccomMesh` and the Reaver
  functions on `Vehicle_Reaver_Base`) make the game crash; either one alone loads. `GearGRI` takes
  `SHODTargets` and `AddSHODTarget` without trouble, so the cause is not just the pairing; it is
  not understood.
- Classes must not be native (no `CLASS_Native` class flag, no `RF_Native` object flag): the disc
  executable has no C++ for them ("Can't bind to native class").
- Default objects can be empty (NetIndex and None): the engine points their component properties
  at their own subobjects by name, as the disc's own default objects show (`Default__Turret_TroikaBase`
  holds 12 bytes).

### What the runtime fixes (`runtime/src/dlc/class_fixups.cpp`)

- **A patch property of an existing class keeps offset 0** (verified: the class's layout gives it
  no room, see above). Once each property and its class are loaded, the runtime gives it a place,
  and puts it back after every later layout of the class:
  - `bShieldDispersed` takes a free bit of `GearPawn_Infantry`'s last bit field word.
  - `TaccomMesh` goes at the end of `Vehicle_Reaver_Base`, which grows by 4 bytes, inside the
    16-byte alignment of its size (the engine copies new objects from a default object allocated
    with the old size; the runtime zeroes those bytes of it). It refuses when a subclass is
    already laid out.
  - `SHODTargets` gets no place in `GearGRI` (see below).
- **`GearGRI.SHODTargets` lives in the satellite camera.** `GearGRI` cannot grow (it has native
  subclasses, and growing `GearGRI_SP` makes its objects copy past the end of their default
  object), and every array it has is in use in campaign games. Sharing `MatchHistory` was tried and
  is wrong (verified): campaign kills fill it, and the garbage collector, following the shared
  property, read its `HistoryItem` entries as objects (a crash at `EndLoad` reading `0x03000000`,
  the `DeathType` byte of an entry). Instead:
  - the patch gives `GearPawn_SatelliteHODCamera_Base` an array of actors, `SHODTargetStore`,
    which the garbage collector follows like any other property;
  - as the camera starts `AddEnemyLocators`, the runtime sets `SHODTargets`'s offset to
    `camera + store - GRI` (32-bit guest arithmetic), so `GRI.SHODTargets` in script, and in
    `AddSHODTarget`, is the camera's store;
  - `SHODTargets`'s inner property is an int (the same 4 bytes as an object reference), so the
    garbage collector's token stream of `GearGRI` gets nothing for it;
  - the engine copies, constructs and destroys the arrays in a class's property lists at their
    offset whatever their `ArrayDim` (verified: with offset 0 the game crashed at boot copying the
    default object's first words as an array), so the runtime takes `SHODTargets` out of the lists
    of `GearGRI` and of each subclass as each one is laid out. In the class, `RefLink` is at +92,
    `PropertyLink` at +96 and `ConstructorLink` at +100; a property links to the next one of each
    at +96, +88 and +92 (verified by walking them in memory).
- **The disc loader breaks on a patch property that is the first field of a class without
  fields** (`GearPawn_LocustGeneralRaamBase`): it preloads the property while still reading the
  class, which then reads the rest of its data from the wrong place. The runtime skips that nested
  preload; the linker loads the property right after.

### Calls to parent functions the disc lacks (`runtime/src/dlc/missing_super.cpp`)

- The DLC's `Vehicle_ReaverGeneralRaam` overrides `PlayHitReactionAnimation` and
  `ShouldTurnCursorRedFor` and calls the parent's version with `Super`. Without the TU functions
  that import stays unresolved, and the call reaches `UObject::CallFunction` with no function. The
  runtime runs the nearest function of the same name in the caller's parent classes
  (`GearVehicle.ShouldTurnCursorRedFor`), or skips the call's arguments and leaves the result
  empty. It has not fired in any test run so far.

### The Title Update's classes: what each one does

The patch declares the classes the DLC needs. Most have no code of their own in the Title Update
(verified, by reading its script patch), so a script subclass with the same properties behaves the
same: `GearPawn_LocustEliteBoomerFlailBase`, `Turret_SatelliteHODBase`,
`GearPawn_SatelliteHODCamera_Base`, `ThumperSpikeBase`, `GearDLCCampaignActData`. The others:

- **`PawnBlockingVolume`** (City Hall 05, School 04 and 09, Orphanage 02) is a native class in the
  Title Update: it blocks every pawn but those of its `AllowedClasses` (City Hall 05's let the
  squad and most Locust through; the others, Wretches, Tickers or Elite Therons). As a script
  subclass of `DynamicBlockingVolume` it would block them all, the squad included. The runtime
  replaces the native `ABlockingVolume::IgnoreBlockingBy` (vtable slot 96; guest `0x8243CB08`,
  which `BlockingVolume`, `DynamicBlockingVolume` and the patch's class share; found by comparing
  the vtables of the disc's blocking volume classes, `GearPawnBlockingVolume` overriding it) to let
  the allowed classes through (`class_fixups.cpp`).
- **`AICmd_Brumak_Action_AttackScriptedTargets`** (Caravan 07): the AI command with which the DLC's
  Brumak attacks its scripted targets, the convoy's vehicles. The patch has its three functions and
  its state (`EvaluateActivation`, `Pushed`, `Popped`, state `AttackTruck`), written with
  `bytecode.py`; with the Title Update's indices they give its compiled bytes exactly (verified),
  but for the interval between attacks: the Title Update reads it from
  `GearAI_Brumak.AttackScriptedTargetInterval`, a property it adds to a native class that the patch
  cannot extend (`GearAI_Brumak_Kadar` extends it and is loaded with it), so the patch uses the
  value the DLC's `GearAI_Brumak_DLC` sets, 13 seconds, as a constant.
- **`SeqEvent_BerserkerVulnerable`** (School 12): the Title Update's
  `GearPawn_LocustBerserkerBase.VulnerabilityChanged` fires it when the Berserker becomes
  vulnerable or not; the disc's does not, so it never fires. In School 12 it only switches the
  squad's hints ("light her on fire", "use the cars") on and off (verified in the level's
  sequence), not the fight. Not done: it needs replacing a disc function's bytecode.
- **`GSM_GrapplingHook_DynamicClimbDrop`** (most levels): the drop from the grappling hook. The
  Title Update's `SpecialMoveStarted` adds `PlayFall(0)` and a starting vertical speed
  (`FallStartSpeedZ`) to its parent's start, and its default object sets that speed (-400), the
  three body stances (an emergence's `Emerge_Burst_fall` and `Emerge_Burst_land`, disc animations)
  and `bLockPawnRotation`. The patch has the same: the function's bytecode gives the Title Update's
  exactly with its indices (verified), and the default object's properties are the same values
  (verified by decoding both), written by the generator.

### Achievements (`runtime/src/dlc/dlc_achievements.cpp`)

- **Verified:** RAAM's Shadow's achievements are 56 to 65 in the package's own achievement database
  (`spa.bin`, an XDBF file with all 65 of the game and its first packs; 51 to 55 are the Horde
  Command Pack's). `default.xex` defines only the game's 50.
- **Verified:** the game unlocks an achievement by its `EGearAchievement` value
  (`GearPC.ClientUnlockAchievement` passes it as the ID), and the disc's enum already reserves 56
  to 60 (`eGA_DLC1` to `eGA_DLC5`). But the SDK's achievement store ignores IDs it does not know,
  so the runtime registers the package's 56 to 65 when RAAM's Shadow is installed, with their texts
  in the player's language and their artwork (extracted to the cache, as the notice reads it from a
  file).
- **Verified (by reading the scripts):** three of them are unlocked by Title Update script:
  - 56 and 57 (the add-on completed in a standard or an arcade campaign):
    `GearProfileSettings.CompleteChapterByDifficulty` gains a branch for chapters 31 and up, with a
    new `HasCompletedSPDLC`;
  - 58 (in four player co-op): `MarkCoopChapterComplete` calls a new
    `UpdateCoopDLCChapterCompleteProgression`;
  - 61 (50 kills with the satellite's Hammer of Dawn): the add-on's script calls
    `UpdateAchievementProgression(61)`, whose disc version only counts the game's achievements.
  The others belong to the multiplayer modes. The disc's chapter progress already has room for
  chapters 31 to 35 (two 32-bit words per difficulty), so completing them is recorded correctly.
- The runtime unlocks 56, 57 and 61 itself, from those calls: it keeps the add-on's chapters each
  kind of campaign completed and the satellite's kills in
  `<saves>/achievements/4D5308AB-raams-shadow.txt`. 58 and the multiplayer ones are not unlocked.
- **Not verified yet:** these unlocks in play (finishing the add-on, 50 satellite kills).
- The launcher exports the package's database too (`azura --export_achievements=<folder>
  --export_achievements_from=<spa.bin>`) and shows 56 to 65, in a section of their own in the
  view by acts, when the add-on is installed.

### Menu and launcher

- The campaign menu's DLC option (`CampaignDLC`, greyed out and "Coming Soon!" on the disc) shows
  the add-on's name. Without the add-on it stays greyed out and reads "Not Installed". With it
  (`--dlc_campaign=true`) it opens the standard campaign choices, and the runtime starts chapter
  31 when the travel names a chapter of the main campaign.
- The launcher installs the player's package (`azura --install_content=<package>`, through the
  SDK's content manager, after checking it is Gears of War 3 Marketplace content), makes the patch
  when the package is RAAM's Shadow (and before starting the game, when it is missing), and starts
  the game with the patch as its update volume when RAAM's Shadow is installed.

## Options considered

1. **Support the TU build, with the player's own TU package.**
   - **Verified:** this works.
   - **Not chosen:** the player would need the Title Update, which is not on any disc, and every
     Azura address would need a second table for the TU executable.
2. **Disc build and the DLC package only (chosen).** See above.

## Tools

- `tools/dlc/bytecode.py` (and `runtime/src/dlc/bytecode.cpp`): the bytecode assembler the patch's
  functions are written with.
- `azura --game_data_root=<game folder> --make_dlc_patch=<output folder>` writes the script patch
  (see above), as the launcher does.
- `tools/dlc/make_patch.py <game folder> <output folder> [--uncompressed <cache>] [--only <names>]`
  writes the same patch in Python, to try changes. Decompressing GearGame.xxx in Python takes a
  few minutes; `--uncompressed` keeps the result for the next runs, and `--only` builds part of the
  patch. A change made there must be made in `raams_shadow_patch.cpp` too, and checked with `cmp`.
- `runtime/tools/stfs_extract.cpp` (`azura_stfs_extract <package> [output folder]`) lists and
  extracts an STFS package with the SDK's reader.
