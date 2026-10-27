# Reverse engineering notes

What we know about the game's internals and how we found it, so that changing menus, settings or
gameplay does not start from zero. Addresses and offsets are for the supported builds in
`versions.toml` (both editions share the same code). Facts are marked as **verified** (seen in a
run, a dump or the code) or **inferred** (deduced, not yet checked).

Nothing here is game data: only names, offsets and identifiers needed to hook the game.

The RAAM's Shadow DLC and the Title Update it depends on have their own notes in
[raams-shadow-dlc.md](raams-shadow-dlc.md).

## Contents

- [Engine basics](#engine-basics)
- [Script bytecode](#script-bytecode)
- [Debug tools](#debug-tools)
- [Configuration files (coalesced)](#configuration-files-coalesced)
- [Main menu](#main-menu)
- [Options menu](#options-menu)
- [Local co-op](#local-co-op)
- [Keyboard in menus](#keyboard-in-menus)
- [Profile settings](#profile-settings)
- [Mouse look](#mouse-look)
- [Field of view](#field-of-view)
- [Film grain](#film-grain)
- [Render command queue](#render-command-queue)
- [Pitfalls](#pitfalls)

## Engine basics

Gears of War 3 runs on Unreal Engine 3. Its gameplay and menu logic is UnrealScript, executed by
the engine's script interpreter, which is part of the recompiled code. The menus are Scaleform
(Flash) movies driven by UnrealScript classes that extend `GearGFxMoviePlayer` (**verified**:
`UIScene_Options extends GearGFxMoviePlayer`). Flash draws the menus and handles the selection in
lists; the script decides what the menus contain and what choosing something does.

### Script interpreter entry points (verified)

| Guest address | Function | Registers |
| --- | --- | --- |
| `0x822DF4D0` | `UObject::CallFunction` | r3 object, r4 `FFrame&` of the **calling** code, r5 result, r6 `UFunction*` |
| `0x822DF9E0` | `UObject::ProcessInternal` (runs a script function body) | r3 object, r4 `FFrame*` of the function about to run, r5 result |

Both are hooked in `runtime/src/script_hooks.cpp`. The code generator emits every function as a
weak symbol forwarding to `__imp__<name>`, so defining `sub_822DF4D0` replaces it and
`__imp__sub_822DF4D0` still runs the original.

- Script calling script goes through `CallFunction`, then `ProcessInternal`.
- Flash and native code call script functions through `ProcessEvent`, which goes **straight to
  `ProcessInternal`** (**verified**: `UIScene_Options.FilterButtonInput`, `Callback_MenuOptionSelected`
  and others only show up there). A hook or trace on `CallFunction` alone misses them.
- When `CallFunction` is entered, the parameters of the call have not been evaluated yet: the
  calling frame's locals can still be changed before the callee reads them.
- When `ProcessInternal` runs, the parameters are already in the frame's locals. Skipping
  `__imp__sub_822DF9E0` skips the function entirely (this is how Quit Game works). Pointing the
  frame's Node and Code at another function of the same object runs that one instead (how the
  brightness calibration is opened from the Video list).

### Global tables (verified)

| Guest address | What |
| --- | --- |
| `0x8343D148` | `GNames`: `TArray<FNameEntry*>` (data, num, max) |
| `0x83473754` | Object table: `TArray<UObject*>` with 65536 slots. Each slot starts a chain linked through `UObject` +4. Walking every chain (skipping objects already seen) finds about 105000 objects at the title screen, but **not all**: some loaded classes (`GearPC`) are missing. It looks like the engine's object hash (`GObjHash`); how slots are assigned is not clear (**inferred**) |

An `FNameEntry` holds `(index << 1) | unicode flag` at +0 and the characters at +8 (Latin-1, or
UTF-16BE when the flag is set).

### Object layouts (verified, guest offsets, big-endian)

| Type | Offset | Field |
| --- | --- | --- |
| `UObject` | +4 | Next object in the same object table chain |
| | +32 | Internal index (consecutive for objects created together) |
| | +40 | Outer |
| | +44 | Name (FName index) |
| | +52 | Class |
| `UField` | +60 | Next |
| `UStruct` | +64 | SuperStruct |
| | +68 | Children (first `UField`) |
| | +76 | Script: `TArray<BYTE>` with the compiled bytecode |
| `UProperty` | +72 | PropertyFlags (64-bit; `0x80` = parameter) |
| | +84 | Offset of the value in its owner (locals, object or struct) |
| `UEnum` | +64 | Names: `TArray<FName>`, 8 bytes per entry |
| `UClass` | +236 | Class default object (`Default__<Class>`) |
| `OnlinePlayerStorage` | +72 | `ProfileSettings`: current values, `TArray<OnlineProfileSetting>` |
| | +84 | `ProfileMappings`: setting definitions, `TArray<SettingsPropertyPropertyMetaData>` (64 bytes each) |
| `FFrame` | +16 | Node (`UFunction*`) |
| | +20 | Object |
| | +24 | Code (next bytecode to run) |
| | +28 | Locals |
| `FString` | | `TArray<TCHAR>` (data, num, max), UTF-16BE, num includes the terminator |

A function's parameters are its first child properties, followed by its locals. Bool properties
share a 32-bit word; each one's bit is the `BitMask` at +104 of its `BoolProperty` (see Film grain).

## Script bytecode

UnrealScript is compiled to bytecode (`UStruct::Script`). Cooked for the Xbox 360 it is
big-endian, and **object references (properties, functions, objects) take 8 bytes whose high half
is zero**; the guest address is the low half (**verified** in the raw bytes). Names take 8 bytes too
(index, number).

Tokens seen so far (**verified** by matching raw bytes with the code's behaviour):

| Byte | Token | Followed by |
| --- | --- | --- |
| `00` / `01` / `02` | local / instance / default variable | 8-byte property reference |
| `04` | return | expression |
| `06` / `07` | jump / jump if not | 2-byte offset (+ condition) |
| `0B` | nothing | |
| `0F` | assignment | two expressions |
| `16` | end of function parameters | |
| `17` | self | |
| `18` | skip (short-circuit of `&&` / `\|\|`) | 2-byte size, expression |
| `19` | context (`a.b`) | expressions |
| `1B` | virtual call | 8-byte name, parameters, `16` |
| `1C` | final call | 8-byte function reference, parameters, `16` |
| `1D` / `1E` / `1F` | int / float / string constant | 4 bytes / 4 bytes / zero-terminated text |
| `21` | name constant | 8-byte name |
| `24` | byte constant (enum values) | 1 byte |
| `25` / `26` | int 0 / int 1 | |
| `27` / `28` | true / false | |
| `2C` | one-byte int constant | 1 byte |
| `81` / `82` / `84` | native `!` / `&&` / `\|\|` | operands |
| `99` / `9A` / `9B` | native `>=` / `==` / `!=` (int) | two operands, `16` |

**Patching bytecode.** The menu's bytecode lives in guest memory and can be changed at run time,
as long as every jump offset stays valid: a replacement must have exactly the size of what it
replaces. Examples in `script_hooks.cpp`: a one-byte constant changed to another value, a native
operator changed to another one (`==` to `>=`), and
`EX_IntOne` (`26`) replaced with `EX_Self` (`17`) so that `x == 1` becomes `x == self`, never true
for a small number. Patch before the function runs for the first time (in the `ProcessInternal`
hook), and only once.

## Debug tools

All are cvars in the `Azura/Debug` category, off by default. Pass them on the command line, for
example `./azura --script_trace=true`.

| Cvar | What it does |
| --- | --- |
| `script_trace` | Logs the first call of every script function as `Class.Function` (`(event)` when it came through `ProcessEvent`: from Flash or native code), and every call to `PawnDied`, `ShouldDeathCauseFailure`, `GetDeathGameOverReason`, `GameOverPlayer`, `ClientGameOver`, `ClientSetCinematicMode` and `MatineeCancelled` as `[Azura death trace]` with its parameters, to follow deaths around cinematics. Slow. |
| `script_trace_args` | With `script_trace`: comma-separated functions whose every call is logged with its parameter values. Only script functions (it hooks `ProcessInternal`). The values are those at the start of the call, so `ReturnValue` and out parameters are not the results. |
| `script_dump_classes` | Comma-separated classes logged once loaded: super class, functions with parameters, properties with offsets, structs and enums. An entry ending in `*` lists the class names with that prefix instead. For `UIScene_Options` and `GearProfileSettings` it also dumps their defaults (option categories and rows, setting definitions). It keeps looking for about ten minutes, so classes that load later can be caught by playing. |
| `script_dump_code` | With `script_dump_classes`: also logs each function's locals with their offsets, a rough listing of its bytecode (variables, calls, constants and jumps in order) and, for short functions, the raw bytes. |

Classes are found as the game uses them (the class of every script function that runs and of the
object running it, plus their super classes) and in the object table. The dump runs on its own
thread and reads guest memory through `vm_read_overwrite` (see Pitfalls).

Typical workflow: dump a class with its code to learn what its functions do, read the raw bytes of
anything the listing leaves unclear, then trace the interesting functions while using the menu to
see when they run and with what values.

## Configuration files (coalesced)

`GearGame/CookedXbox360/Coalesced_<LANG>.bin` holds every `.ini` and localization file of the game
in one big-endian blob. The format is described in `runtime/src/menu/coalesced.h`.
`runtime/src/menu/coalesced.cpp` reads it and writes it back byte for byte.

Files of interest inside it:

| File | Holds |
| --- | --- |
| `Xbox360-GearUI.ini` | Menu structure: `[GearGame.UIWidget_MainMenu] AllOptions=...` |
| `geargameui.<lang>` | Main menu texts (`[MainMenu]`) |
| `geargame.<lang>` | Options menu (`[UIScene_Options] OptionCategories[N]`), profile setting definitions (`[GearProfileSettings] ProfileMappings[N]`) |
| `Xbox360-GearEngine.ini` | Engine settings, including `[SystemSettings]` (MotionBlur, DepthOfField, Bloom, MaxAnisotropy, ...) and frame rate smoothing |
| `Xbox360-GearGame.ini` | `[GearGame.GearProfileSettings]` tunables |

Language extensions: `int`, `esn` (and `esm`), `deu`, `fra`, `ita`, `jpn`, `kor`, `chn`, `ptb`,
`pol`, `rus`, `cze`, `hun`. The Spain edition ships `INT` and `ESN`.

How localized values are imported (**verified** by changing them and dumping the result):

- A localized struct value may set **any** member, not only the localized ones: rows of the options
  menu with their profile setting ID, type and slider range, setting definitions with their ID and
  type, all load from the text.
- **Arrays never shrink.** An array written with fewer elements than it has keeps the extra ones as
  they were. Writing more elements grows it.
- An array element keeps every member the text leaves out, so moving a struct to another position
  means writing all its members.
- Indexed keys (`ProfileMappings[N]`) only fill positions that exist: one past the end is ignored.

Whether the Xbox 360 build honours edited `[SystemSettings]` values is **not verified**.

### How patched copies reach the game (verified)

`runtime/src/menu/menu_patches.cpp` patches the coalesced files at start-up and writes the copies
to `<cache>/menu_overlay/`. It mounts that folder as `\Device\AzuraOverlay` and links each original
path to its copy with `VirtualFileSystem::RegisterSymbolicLink`. The SDK patch
(`patches/rexglue-0.10.0.patch`) makes `VirtualFileSystem::OpenFile` resolve links on the full file
path. Without it, only the folder is resolved and links to single files are ignored. The game's own
files are never modified.

## Main menu

Defined in `Xbox360-GearUI.ini`, `[GearGame.UIWidget_MainMenu]`, as `AllOptions` entries:

```
(Identifier=MAINMENUOP_MainMenu,Title=...,List=((Label="Campaign",Next=MAINMENUOP_Campaign,...),...))
```

Each list entry has a `Label` (text key in `geargameui.<lang>` `[MainMenu]`), `Next` (screen it
opens), `bHaltProgression` and `bIsDisabled`.

When the player chooses an entry, the script calls
`UIScene_FrontEnd.HandleMenuSelection(ParentScreen, SelectedOption, bIsSimulating, ...)`.
`ParentScreen` 1 is the main menu, and `SelectedOption` is the entry's index in the list as
patched (**verified** with `script_trace_args=HandleMenuSelection`).

### Entries

| Original index | Label | Opens | After our patch |
| --- | --- | --- | --- |
| 0 | `Campaign` | `MAINMENUOP_Campaign` | 0, unchanged |
| 1 | `VS` | `MAINMENUOP_VS_Competitiveness` | 1, text marked "(coming soon)" |
| 2 | `Horde` | `MAINMENUOP_Horde` | 2, unchanged |
| 3 | `Siege` (Beast) | `MAINMENUOP_Siege` | 3, unchanged |
| 4 | `Journal` (War Journal) | `MAINMENUOP_WarJournal` | 4, unchanged |
| 5 | `Calendar` (event calendar) | | removed |
| 6 | `DLC` (Marketplace) | | removed |
| | `Options` (added, as the party client's menu has it) | `MAINMENUOP_None` | 5: the runtime runs `Callback_StartButtonPressed`, what Start does there |
| | `AzuraQuit` (added) | `MAINMENUOP_None` | 6 |

Indices 0 and 5 were confirmed with `HandleMenuSelection` traces; the others follow the list order.
The button bar's Start "Options" (`[UIWidget_MainMenu] OptionsButtonStr` of `geargame.<lang>`) is
replaced with a space, since the list has the option.
Calendar and Marketplace are the last two entries, so removing them shifts no other index.

### Sub-menus we change

Each screen is another `AllOptions` entry with its own `Identifier`. Rows disabled and marked
"(coming soon)" because they need Xbox Live:

| Screen | Rows |
| --- | --- |
| `MAINMENUOP_CampaignStandard`, `MAINMENUOP_CampaignArcade` | `CreatePublic`, `JoinPublic` |
| `MAINMENUOP_Horde` | `HordeStandard` (`HordePrivate` stays) |
| `MAINMENUOP_Siege` | `SiegeStandard` (`SiegePrivate` stays) |
| `MAINMENUOP_VS_Competitiveness` | `Recreational`, `Competitive` (`Private` stays) |

The "(coming soon)" suffix is appended to the row's text key in `geargameui.<lang>` `[MainMenu]`
(same name as the label, `TITLE_VS_Competitiveness` for the Versus title).

### Changes and switches

All in `runtime/src/menu/menu_patches.cpp`, applied to every `Coalesced_*.bin` that has the main
menu (the `INT` file and the edition's language):

- `menu_remove_unused` (default on): removes the calendar and Marketplace.
- `menu_coming_soon` (default on): disables the online rows above and marks them.
  `bIsDisabled=true` greys the row out and it cannot be chosen (**verified** on screen).
- Always: adds `AzuraQuit` with `Next=MAINMENUOP_None,bHaltProgression=true`, and its text and
  tooltip (`AzuraQuit`, `TOOLTIP_AzuraQuit` in `[MainMenu]`) for each language in
  `LocalizedTexts()`. Languages without an entry use the English texts.

### Quit Game

The script does not know `AzuraQuit`: left to itself it waits for a screen that never comes and
the menu stops responding (**verified** in an early test). `script_hooks.cpp` therefore watches
`ProcessInternal`. When the function is `HandleMenuSelection` with `ParentScreen` 1, the
`SelectedOption` of our entry (`MainMenuQuitOption()`) and `bIsSimulating` false, it calls the
quit handler and skips the script. To keep the hook cheap, functions are compared by FName index;
the indices of the functions the hooks react to are found by scanning new `GNames` entries every
1024 calls until all of them have appeared.

### Options

What Start does on the main menu (**verified** from the bytecode of `script_dump_code`):

- `UIScene_FrontEnd.Callback_StartButtonPressed(ControllerIdx)` (locals `ControllerIdx@0`,
  `OpeningPlayer@4`, `LP@8`) opens the Options: when `OptionsMenuClass` and `GetLP()` are set, it
  looks the player up with `GameViewportClient.FindPlayerByControllerId(ControllerIdx)` and does
  nothing when no player has that controller. It then creates `OptionsMenu`, calls
  `SetupMPPreferencesCallback()` and `OptionsMenu.Init(OpeningPlayer)`, sets
  `OnOptionsMenuClose` and `bIsInOptionsMenu`. `OptionsMenuClosed()` clears `bIsInOptionsMenu`.
- `UIWidget_MainMenu.UpdateButtonBar()` builds the bar's HTML text: Accept or Start game, then
  `Spacer` (five spaces, a string constant), `BackButtonStr`, `Spacer`, `OptionsButtonStr`, and the
  guest login, post game stats and War Journal buttons when they apply.

`Callback_MenuOptionWasSelected(HistoryArray, bIsHalted)` calls `HandleMenuSelection` and, for an
entry with `bHaltProgression`, leaves the menu halted until `ContinueFromHaltedState()`, which
depends on `bWantToContinue`. An entry the script does not know and skips must therefore not halt:
with `bHaltProgression=true` the main menu took no input after the Options closed (**verified**
with `script_trace_args`). Quit Game halts, as the game ends anyway.

Our Options entry (`Next=MAINMENUOP_None`, no halt, as in `MAINMENUOP_ClientMenu`): in the
`HandleMenuSelection` hook (as for Quit Game) the runtime runs
`Callback_StartButtonPressed` on the same frame with locals of its own, `ControllerIdx` being the
controller that last pressed a button on the scene (`FilterButtonInput`'s `ControllerId`).

## Options menu

Class `UIScene_Options` (extends `GearGFxMoviePlayer`, package `GearGame`).

### Data

The menu is data driven. `OptionCategories` (`TArray<OptionCategoryInfo>`, object offset 1016)
holds one category per `EOptionsMenuMode`, shown in that order:

```
enum EOptionsMenuMode { OMI_Game, OMI_Video, OMI_Audio, OMI_Multiplayer, OMI_Controller,
                        OMI_StorageLocation, OMI_MatureContent, OMI_Credits, OMI_MAX }
enum EOptionSelectorType { OST_Slider, OST_OptionList, OST_Default, OST_MAX }
enum EOptionsButtonHandler { OBH_Defaults, OBH_ResetTutorials, OBH_ViewController, OBH_MAX }

struct OptionCategoryInfo {          // 64 bytes
  string CategoryDescription;        // +0
  string CategoryTooltip;            // +12
  string OnSelectedCallback;         // +24  function Flash runs instead of opening a list
  bool   bIsGlobalValue;             // +36
  array<OptionInfo> CategoryOptions; // +40
  array<ButtonInfo> MenuButtons;     // +52  the button bar
};
struct OptionInfo {                  // 48 bytes
  int    ProfileSettingId;           // +0   the profile setting the row edits
  string SettingDescription;         // +4
  string SettingTooltip;             // +16
  byte   Type;                       // +28  EOptionSelectorType
  bool   bUsesRawValue, bIsGlobalValue, bHasOnChangeCallback,
         bRequiresMatureLanguageSupport;  // +32, one bit each
  float  Slider_Minimum, Slider_Maximum, Slider_Increment;  // +36, +40, +44
};
struct ButtonInfo {                  // 16 bytes
  string ButtonDescription;          // +0
  byte   ButtonType;                 // +12  0 A, 1 B, 2 X, 3 Y
  byte   ButtonHandler;              // +13  EOptionsButtonHandler
};
```

The stock localization files only set the texts; profile setting IDs, types, slider ranges and
button types are compiled into the class defaults (`Default__UIScene_Options`). But the engine
imports any member written in the localization file (see the coalesced section), which is how rows
and categories are changed. The last entry (index 8, `OMI_MAX`) only holds the buttons of the list
of categories.

### The game's rows (verified, class defaults)

| Category | Row | Profile setting ID | Type |
| --- | --- | --- | --- |
| 0 Game | Hide HUD, subtitles, tooltip icons, weapon tutorials | 36, 40, 48, 97 | list |
| 1 Video | Brightness, TV type | 0 | slider; the category runs `Callback_VideoOptions`, which opens `UIScene_VideoOptions` |
| 2 Audio | Music, effects, dialogue volume | 41, 42, 43 | slider 0..1 step 0.1, live callback |
| 3 Multiplayer | Default COG, default Locust, primary weapon, secondary weapon | 44, 45, 50, 130 | raw list |
| 4 Controller | Invert Y, invert X, omnidirectional evade | 2, 58, 375 | list |
| | Look, aim, zoom sensitivity | 368, 59, 60 | slider step 0.05 |
| | Vibration, sticks, triggers, control scheme | 1, 38, 39, 51 | list |
| 5 Storage location | (none) | | runs `Callback_SelectStorageLocation` |
| 6 Mature content | Violence, mature language | 46, 37 | list |
| 7 Credits | (none) | | runs `Callback_Credits` |

Buttons: Back (type 1, `OBH_MAX`) and Defaults (type 2, `OBH_Defaults`) in every list category;
Game adds Reset tutorials (Y, `OBH_ResetTutorials`), Controller adds View controller (Y,
`OBH_ViewController`).

### Positions the script depends on (verified from the bytecode)

- `Callback_MenuOptionSelected(SelectedOption)` starts with
  `if (SelectedOption == 5 || SelectedOption == 1 || SelectedOption == 7) return;`: the storage
  location, Video and Credits opened another screen, so they never get a list. A category placed
  there shows an empty list that does not even react to Back (Flash switches to the list view, the
  script does not).
- `Callback_ReturnToMain` skips the "leave" sounds when `CurrentMode` is 1 (Video) or 8, and calls
  `SettingChangesHaveBeenMade` (multiplayer weapons) when it is 3.
- `PopulateOptionsMenu` lists categories with `for (I = 0; I < 8; ++I)`.
- A Controller list of 11 rows only showed 10 on screen (**verified**); the cause, probably the
  Flash list, is not known.

### Flow (verified with `script_trace_args`)

1. Opening Options: `Init`, then `PopulateOptionsMenu`, which asks `ShouldOptionBeDisabled(Option)`
   for each category.
2. Choosing a category: `Callback_MenuOptionSelected(SelectedOption)` (from Flash), which sets
   `CurrentMode` (object offset 1028, byte; 8 = the list of categories) and calls
   `PopulateOptionsList`.
3. Every button press and release in the menu: `FilterButtonInput(ControllerId, ButtonName,
   InputEvent)` from Flash. `InputEvent` 0 is pressed, 1 released. Up and Down arrive as
   `XboxTypeS_DPad_Up/Down` (arrow keys, D-pad) or `Gamepad_LeftStick_Up/Down` (W/S, left stick),
   Accept as `XboxTypeS_A`, Back as `XboxTypeS_B`. Holding a direction sends no repeats. Moving
   through rows runs no script: the selected row only exists in Flash, which starts every list on
   its first row and wraps around at both ends (**verified** on screen).
4. Leaving the category (`Callback_ReturnToMain`, from Flash): `SaveSettingsToProfile` reads each
   row's value back from the Flash list with `GetOptionValueAt(Index)` and writes it to the profile
   through `OnlinePlayerStorage` (`GetProfileSettingValueFromListIndex`, `SetProfileSettingValueFloat`, ...).
5. Closing Options: `GearPC.SaveProfile` writes the profile (`OnlineSubsystemLive.WriteProfileSettings`).
6. Video (in the game) runs `Callback_VideoOptions`, which creates `UIScene_VideoOptions` and opens
   it with `ActionScript_OpenVideoOptions`. Closing it runs `CloseVideoOptionsScene`, which calls
   `Callback_ReturnToMain` (back to the list of categories) and `ActionScript_CloseVideoOptions`.

### What the functions do (verified from their bytecode)

`PopulateOptionsList`, for each row of `OptionCategories[CurrentMode].CategoryOptions` (skipping
mature-language rows when not supported), builds a Flash object with `label`, `tooltip`,
`OptionType` and then:

- **Slider:** `Slider_Value = MyProfile.GetProfileSettingValueFloat(psid)`, then
  `Slider_Minimum = GetModifiedSliderMinimum(psid, Slider_Minimum)`, the same for the maximum, and
  `Slider_Increment`. The two `GetModified*` functions only change the sensitivity rows (368, 59,
  60) and return `CurrentDefault` otherwise.
  The slider shows the step it is on, not its value (seen on screen: a 70 to 100 slider with
  step 5 showed 1 to 6), so a value the player must read is better offered as a list.
- **List, `bUsesRawValue` false:** value names from `MyProfile.GetProfileSettingValues(psid)`, the
  selected index from `MyProfile.GetProfileSettingValueId(psid, ValueId, ListIndex)`.
- **List, `bUsesRawValue` true:** value names from `GetProfileSettingValue_SpecialCase(psid,
  OptionValues)` (only fills the character and weapon lists for 44, 45, 50, 130), the index from
  `GetProfileSettingValueInt` and `ConvertRawIdToProviderId`.
- `bIsGlobalValue` rows are disabled for players other than the primary one.
- `bHasOnChangeCallback` rows get a live callback from `GetCallbackForOptionPSID` (only gamma 49,
  TV type 53 and the three volumes 41 to 43 have one), passed to Flash with
  `ActionScript_SetOnChangeCallback`.

Locals of `PopulateOptionsList`: `I` @0 (row), `CurrentOptionPSID` @8, `Slider_Value` @172,
`OptionList_Index` @176.

`SaveSettingsToProfile`, for each row: `OptionValue = GetOptionValueAt(I)` (which asks the Flash
list `GetSelectionValueAt`: the slider value, or the list index), compares it with the profile
value and calls `SetProfileSettingValueFloat`, `SetProfileSettingValueId` or
`SetProfileSettingValueInt`, marking the profile dirty. For an ID the profile does not have, the
read and the write fail and nothing changes.

`OnConfirm_ResetDefaults(Sender, SelectedInputAlias, ...)` (`'ResetAccept'` when confirmed) resets
every row of the category to its profile default and redraws it.

### What Azura changes

Implemented in `runtime/src/menu/pc_options.{h,cpp}` (data and texts) and `script_hooks.cpp`
(behaviour):

| Position | Category | Change |
| --- | --- | --- |
| 1 | Video | A list: Calibrate brightness (opens the game's calibration screen), Full screen, Frame rate limit (a list of our own), Field of view (a list of our own), Anti-aliasing, Motion blur, Film grain (a list of our own), Anisotropic filtering, Render resolution (a list of our own), Image scaling, Sharpness (a slider, only with an image scaling effect that sharpens), Performance overlay and Overlay position (lists of our own), Stutter reduction (a list of our own, Linux and Windows only: the SDK's Vulkan `occlusion_query_no_wait`). Back and Defaults |
| 0 | Game | The game's own rows, then the two of the mature content category (violence 46, mature language 37 with `bRequiresMatureLanguageSupport`) |
| 5 | Keyboard and mouse | Replaces the storage location: Mouse sensitivity. Back and Defaults |
| 6 | Local co-op | Replaces the mature content: no list; choosing it opens the Local co-op menu (`runtime/src/local_coop_menu.h`). Greyed out with no controller connected |

How:

- **Rows and categories** are written into `[UIScene_Options] OptionCategories[1]`, `[5]` and `[6]`
  of every `geargame.<lang>`, with every member, our texts in that language and the game's own
  texts (category titles, button names) kept from the file. `OptionCategories[0]` keeps its text
  from the file and gets the two mature content rows (with every member, as new elements of the
  array) added at the end of its `CategoryOptions`.
- **List rows borrow profile settings the game never uses**, the engine's racing game preferences:
  Movement Control 18 (calibration), Race Camera 20 (anti-aliasing), Brake
  21 (motion blur), Accelerator 22 (full screen); and two generic Xbox profile settings with three
  values that Gears does not read (**inferred**): Controller Sensitivity 13 (anisotropic filtering:
  off, 4x, 16x). Their definitions (`ProfileMappings[2]` and `[11]` to `[15]`) get our value
  names. The calibration row's definition has two values, both named "Open",
  since an array cannot shrink. The sliders use IDs the profile does not have: 9105 (mouse sensitivity) and 9109 (sharpness).
- **Lists of our own** (**verified**): a raw-value list row (`bUsesRawValue=true`) with an ID the
  profile does not have (9106 frame rate limit, 9107 image scaling, whose values are the upscaling
  effects of each platform's build, 9108 render resolution, whose first value is "Automatic" in
  the player's language, 9110 performance overlay, 9111 overlay position, 9112 field of view, 9113 film grain, 9114 stutter reduction) needs no definition, so it can have any number of
  values with any names. `PopulateOptionsList` empties its local `OptionValues`
  (`TArray<string>`), fills it with `GetProfileSettingValue_SpecialCase(psid, OptionValues)` (which
  only knows the multiplayer characters 44/45 and weapons 50/130 and leaves it empty otherwise),
  and hands each name to Flash with `OptionListProvider.SetElementString(J, SafeCaps(name))`. For
  our IDs `script_hooks.cpp` runs bytecode of its own instead of the function's:
  `OptionValues[I] = "name"` (`EX_Let`, `EX_DynArrayElement` with an `EX_IntConst` index and the
  function's `EX_LocalOutVariable` 0x48 to `OptionValues`, `EX_UnicodeStringConst` big-endian
  UTF-16) for each name, then `EX_Return EX_Nothing EX_EndOfScript`. The script interpreter grows
  the array and allocates the strings with the game's allocator. The selected index and saving go
  through the same hooks as the other rows (`OptionList_Index`, `GetOptionValueAt`); the profile
  reads and writes of the unknown ID fail and change nothing.
- **Showing values:** while `PopulateOptionsList` is on one of our rows, every call it makes
  (entered through `CallFunction`) rewrites its locals `Slider_Value` / `OptionList_Index` with the
  real value, so the call that hands them to Flash reads ours.
- **Saving values:** when `GetOptionValueAt(Index)` returns for one of our rows, the value is
  applied to its cvar (`fullscreen`, `frame_rate_limit`, `swap_post_effect`, `motion_blur`,
  `anisotropic_override`, `present_effect`, `mnk_sensitivity`)
  and written to `azura.toml`, leaving the rest of the file as it is. The menu also writes the
  borrowed profile settings, which is harmless.
- **Defaults** resets our rows to the values in `azura.default.toml` (the SDK's default when a key
  is not there).
- **Position 1 shows its list, positions 4 to 7 do not:** the bytecode of
  `Callback_MenuOptionSelected` is patched so that `SelectedOption == 5` becomes
  `SelectedOption >= 4` and `SelectedOption == 1` compares with `self`. Controller (4) and Keyboard
  and mouse (5) then run their callbacks with `CurrentMode` still 8, the list of categories, which
  tells Keyboard and mouse's from Game's Reset tutorials row. The `1` in `Callback_ReturnToMain` becomes 255 too, so leaving
  Video plays the usual sound.
- **Local co-op** has `OnSelectedCallback="Callback_SelectStorageLocation"`, the storage location's
  function, which no category uses any more: Flash runs it instead of opening a list, and the hook
  opens the Local co-op menu in its place without running it. `ShouldOptionBeDisabled(Option)` is
  answered for it (disabled with no controller connected) and for Keyboard and mouse (never
  disabled, whatever the mature content's answer was). Its `Option` parameter is read with its
  property's `ElementSize` (+68 of a `UProperty`, inferred from UE3's layout).
- **Calibrate brightness:** `FilterButtonInput` is watched while Video is open to follow the
  selected row (Up/Down, wrapping, reset to 0 when a list is built). Accept, Left or Right on the
  calibration row runs `Callback_VideoOptions` in place of that `FilterButtonInput`. Closing the
  calibration screen then calls `Callback_ReturnToMain`, which would put the script on the list of
  categories while Flash goes back to the Video list; that one call is skipped.
- **Motion blur** is applied at start-up by setting `[SystemSettings] MotionBlur` in
  `Xbox360-GearEngine.ini`.
- **Live changes** (**verified** on screen): a row's value only lives in Flash until the category
  is left, unless the row has `bHasOnChangeCallback`, in which case Flash runs the delegate
  `GetCallbackForOptionPSID(psid)` gave it every time the value changes, with the new value (the
  slider value or the list index). That function is a switch over the game's IDs (49 gamma,
  53 TV type, 41 to 43 the volumes). Image scaling and Sharpness set the flag and, as
  `GetCallbackForOptionPSID` starts, their IDs are swapped for 53 and 49, whose callbacks
  (`Callback_TVTypeWasChanged`, `Callback_GammaWasChanged` of `UIScene_Options`) no row of the
  Video list uses any more. When those run from one of our rows, our code runs instead: Sharpness
  applies at once, Image scaling saves its value and shows or hides the Sharpness row.
- **Rows shown or hidden with the menu open** (**verified** on screen): a row of ours that depends on another row's
  value (Sharpness) is hidden by lowering the count of `OptionCategories[1].CategoryOptions` (the
  `TArray` at +40 of the category) and shown again by raising it over the same elements, while its
  allocation holds them. Since only the last elements can be left out that way, the array's
  48-byte `OptionInfo` elements are first put in order by their `ProfileSettingId` (+0): the shown
  rows, then the hidden ones; so rows below a hidden one move up. Before every
  `PopulateOptionsList` the order and the count are set; after a change the list is rebuilt from the borrowed
  callback by running bytecode of ours in its frame, `PopulateOptionsList(); return;` (final
  function token `1C` with the function's address, `16`, `04 0B`, `53`), so the engine calls it
  the normal way. `PopulateOptionsList` ends with `OptionsList.selectedIndex = 0.0`: for that
  call its float constant (after the `"selectedIndex"` string) is the changed row's index, so the
  selection stays there.

## Local co-op

What the game does with a second local player (**verified** with the XAM user calls logged and
`script_trace`, 25 October 2026):

- It asks players 2 to 4 for their sign-in state, sign-in info, name and device context
  (`XamUserGetDeviceContext`, constantly), and never calls `XamShowSigninUI`. An unsigned player 2
  can join ("Sesión no iniciada", with the warning that their data will not be saved).
- When the game sees user 1 connected it adds player 2 by itself; in the campaign player 2 waits
  ("Espere para unirse a una partida") and gets a character at the next area. The travel URL keeps
  its `listen` stripped: split screen does not need it.
- A third player is refused: "en una consola Xbox 360 solo pueden jugar dos personas".
- When a player signs in, `GearPC.OnLoginChange` calls `GearMenuPC.OnNewLogin`, which, away from the
  title screen (`IsOnTitleScreen`), shows the login change message (`ShowLoginChangeError`) and calls
  `RestartFrontEnd`: back to the title screen.
- The title screen is the main menu's scene, `UIScene_FrontEnd`, with `bOnTitleScreen` set: the
  first bool of the word at +1000 of the scene, which the Xbox 360 build stores in the highest bit
  (the word reads `88000000` on the title screen and `08000000` in the main menu). There,
  `FilterButtonInput(ControllerId, 'XboxTypeS_Start')` makes that controller's player the primary
  one (`BecomePrimaryPlayer`, `OnPrimaryPlayerSwitch`); `ControllerId` is the guest user index.
- `XN_SYS_INPUTDEVICESCHANGED` (0x12) after a player's controller goes makes the game pause for
  that player and ask for it, and again when it comes back resumes, as on the console.

What Azura does (`runtime/src/input/player_assignment.h`, the SDK's `KernelState::SignInLocalUser`):
players 2 to 4 have profiles of their own in the SDK, signed in when they get a device, with
`XN_SYS_SIGNINCHANGED`; the runtime gives player 2 a device from the Local co-op menu, ignores
player 2's Start and Accept on the title screen, and sends `XN_SYS_INPUTDEVICESCHANGED` when a
player is left with no device or gets one back.

## Keyboard in menus

The SDK's keyboard and mouse driver binds Escape and Return to Start for good (the game needs a
Start in play). In menus the runtime turns them into the PC's keys (`TakeKeyboardStartAsMenuKey`
in `script_hooks.cpp`):

- Every menu button reaches the script as `FilterButtonInput(ControllerId, ButtonName,
  InputEvent)` on the scene (**verified** with `script_trace_args`). Storing 1 in its return
  value and not running it uses the press up (**verified**: the menu ignores it).
- Changing the `ButtonName` local to another button does not work: Flash has already handled the
  press (**verified**). The runtime uses the press up and has the driver press A (Return) or B
  (Escape) for 80 ms instead (`mnk::PressForMenu`); `mnk::HeldStartKey` says which key the Start
  came from.
- Menus and play are told apart by the player controller's class: `GearPartyPC` in the front end
  (**verified** in the log; not `GearMenuPC`), and by `WorldInfo.Pauser` in play.
- On the title screen (`bOnTitleScreen`, see "Local co-op") Return stays Start and Escape is used
  up.
- The Options are open from `UIScene_Options.PopulateOptionsMenu` to `UIScene_Options.OnClose`.
  The `OnOptionsMenuClose` delegate is no use for it: its own body never runs, as whoever opens
  the Options binds it to a function of its own (`UIScene_FrontEnd.OptionsMenuClosed`,
  **verified** with `script_trace`).
- Some scenes never run `FilterButtonInput`: `UIScene_MPPreferences` (Options > Multiplayer) takes
  its buttons in Flash (**verified**: no call in the log while it is open), and the message boxes
  (`ShowUIMessage`, such as the reset tutorials confirmation) are not GFx scenes of their own
  (**inferred**: none of the scene classes is theirs). So the driver also asks the runtime, at
  every poll, whether a menu is shown (`mnk::SetMenuKeysQuery`, answered with `WhereInMenus()`):
  in one, Return presses A and Escape B for 80 ms, for every scene. A press as long as the key's
  froze the Options opened from the main menu with Return still held (**observed**; why is
  **inferred**: the scene opening with A down). Each key keeps what it was when pressed
  until it is released, so the Escape that pauses is not also B in the pause menu it opens.

## Profile settings

Each option row edits a profile setting through `OnlinePlayerStorage` (package `Engine`), which
`OnlineProfileSettings` and the game's `GearProfileSettings` extend. Its functions (**verified**
names) include `GetProfileSettingValueId/Int/Float`, `SetProfileSettingValueId/Int/Float`,
`GetProfileSettingValues`, `GetProfileSettingValueFromListIndex`, `GetProfileSettingRange`,
`GetProfileSettingMappingType` and `FindProfileSettingIndex`. They are probably native C++
(**inferred**: they appear in the first-call trace of `CallFunction` but never reach `ProcessInternal`).

Definitions (`ProfileMappings`, 214 of them in `GearProfileSettings`):

```
struct SettingsPropertyPropertyMetaData {  // 64 bytes
  int    Id;                // +0
  name   Name;              // +4
  string ColumnHeaderText;  // +12
  byte   MappingType;       // +24  PVMT_RawValue, PVMT_PredefinedValues, PVMT_Ranged, PVMT_IdMapped
  array<IdToStringMapping> ValueMappings;  // +28  (Id, Name), 12 bytes each
  array<SettingsData> PredefinedValues;    // +40
  float  MinVal, MaxVal, RangeIncrement;   // +52, +56, +60
};
```

The value names are localized in `geargame.<lang>`, `[GearProfileSettings] ProfileMappings[N]`,
and in `Engine.<lang>`, `[OnlineProfileSettings]`. Positions 16 to 22 and 39 are empty
(`Id=0`, `'None'`). Positions 11 to 15 are the engine's racing game preferences (Movement Control
18, Transmission 19, Race Camera 20, Brake 21, Accelerator 22), which Gears never uses.

The Controller category's rows (`OptionCategories[4]`), whose values the controls menu reads and
writes in `OnlineProfileSettings.ProfileSettings` (24-byte entries, `PropertyId` at +4, value at
+12; the IDs and values come from the class defaults, and reading and writing them from the menu is
not verified yet):

| Id | Setting | Values |
| --- | --- | --- |
| 2 | Invert Y | 0, 1 |
| 58 | Invert X | 0, 1 |
| 375 | Omni evade | 1, 0 |
| 1 | Vibration | 3, 0 |
| 38 | Sticks | 0 to 3 |
| 39 | Triggers | 0, 1 |
| 51 | Control scheme | 0 Default, 1 Alternate |
| 368, 59, 60 | Look, aim and zoom sensitivity | sliders, 0 to 1 in steps of 0.05 (**inferred** range) |

**Only rewrite the value names of an existing list definition** (**verified**). Two other changes
made the game hang with endless null writes while reading the profile ("Reading saved games"):
defining new list settings (new IDs) in the empty positions, and moving a definition (Movement
Control) to an empty position. A list setting probably needs a stored value in the profile
(**inferred**).

Other scene classes loaded in the front end include `UIScene_Pause`, `UIScene_MPPreferences`,
`UIScene_3DSetup`, `UIScene_WaveOptions` and `UIScene_Credits`. `UIScene_VideoOptions` is not
loaded until the video options are opened (**inferred**).

## Pitfalls

- **Never read guest memory that may be unmapped with a plain load from host code.** The runtime's
  exception handler takes any fault in guest memory for the game's own and retries it forever,
  flooding the log with `Unhandled guest access violation`. `class_dump.cpp` reads through
  `vm_read_overwrite`, which fails cleanly instead.
- **Do not do slow work on the script thread.** A scan of the object table inside the
  `ProcessInternal` hook stalled the game for seconds, and the game then crashed with null reads on
  another thread. Run long work on a separate thread.
- **Close the game before rebuilding.** Building replaces the executable and libraries under a
  running process, which ends in the same flood of access violations.
- **Bytecode references are 8 bytes.** Matching them as 4-byte values works by accident (the high
  half is zero) and breaks as soon as the operand's size matters.
- **Arrays in localized values never shrink**, and indexed keys past the end are ignored.
- **An empty localized value does not replace the text**: `OptionsButtonStr` emptied still showed
  the cooked text (**verified**); the engine probably takes empty as missing (**inferred**). Use a
  space.
- **The first launch with an empty shader cache looks frozen** while tens of thousands of pipelines
  are created. It is not a hang.
- **Lock-free code written for the Xbox 360 can break on ARM64.** The Xenon cores run in order,
  so the game publishes data between threads without barriers where it relies on that; ARM64
  reorders memory accesses. See "Render command queue".

## Field of view

The gameplay camera (**verified**, `script_dump_classes` and `script_dump_code`):
`GearPlayerCamera` (extends `GamePlayerCamera`) creates in its `PostBeginPlay` its `GameplayCam`
(+1196), a `GearGameplayCamera` (extends `GameThirdPersonCamera`). Its `Init` creates one camera
mode object per situation with `CreateCameraMode` and keeps them in properties `GearCamCover`
(+408) to `GearCamReaverTargeting` (+580); `FindBestCameraMode` picks the one in use. The modes
are `GameplayCam_*` classes extending `GearGameplayCameraMode`, which extends the engine's
`GameThirdPersonCameraMode`, where `FOVAngle` is declared.

Each mode's `FOVAngle` comes from `GearCamera.ini` in the coalesced file: `GameplayCam_Default` 80,
`GameplayCam_Cover` 95, `GameplayCam_Targeting` and `GameplayCam_CoverTargeting` 50,
`GameplayCam_RoadieRun` 105, `GameplayCam_DBNO` 110, and others between 50 and 120. Vehicles and
turrets have their own values in `GearPawn.ini` (`CameraFOV`, `DefaultFOV`, ...).
`Xbox360-GearEngine.ini` sets `[Engine.LocalPlayer] AspectRatioAxisConstraint=AspectRatio_MaintainXFOV`:
the angles are horizontal.

How a mode gives its field of view (a reading of the rough bytecode listings; the exact
parameters of the weapon calls are not decoded):

```
GearGameplayCameraMode.GetDesiredFOV(ViewedPawn)
  GW = ViewedPawn.Weapon
  if (GW != None) return GW.GetAdjustedFOV(true, FOVAngle + FOVAngle_ViewportAdjustments[CurrentViewportType])
  return FOVAngle + FOVAngle_ViewportAdjustments[CurrentViewportType]
GameplayCam_Targeting.GetDesiredFOV(ViewedPawn)
  if (Wpn != None) return Wpn.GetTargetingFOV(FOVAngle + FOVAngle_ViewportAdjustments[...])
  return GearGameplayCameraMode.GetDesiredFOV(ViewedPawn)
```

`FOVAngle_ViewportAdjustments` (+604 of `GearGameplayCameraMode`) is an adjustment per split-screen
viewport type. Kismet's Set Camera FOV goes through `GearPC.ClientSetForcedCameraFOV`, which sets
`bUseForcedCamFOV` and `ForcedCamFOV` on the player camera instead. Which native code calls
`GetDesiredFOV`, and how often, is **not verified**.

The runtime scales every mode's `FOVAngle` by `field_of_view / 80` just before its `GetDesiredFOV`
runs (`runtime/src/script_hooks.cpp`, "Field of view"), keeping the value the game gave each mode
object to scale it again when the setting changes.

Since the look input is multiplied by the field of view (see Mouse look), a wider view would
also turn faster, with the mouse and with the stick. As the engine's `PlayerInput.PlayerInput`
returns, the runtime divides `aTurn` and `aLookUp` by `field_of_view / 80`, so the turn per mouse
motion or stick deflection stays the game's. Modes whose angle does not come from their
`FOVAngle` (vehicles and turrets with their own `CameraFOV`, if any) would turn that much slower
(**not verified**).

## Film grain

The film grain is drawn by `UberPostProcessEffect` (extends `DOFBloomMotionBlurEffect`), an effect
of the post-process chains (**verified**, reading the objects in the front end):

| Chain | How it is reached | Effects |
| --- | --- | --- |
| The player's, the one drawn | `LocalPlayer.PlayerPostProcess` | 3 `MaterialEffect`, `UberPostProcessEffect`, 2 `MaterialEffect`, `AmbientOcclusionEffect`, `DwTriovizImplEffect` |
| The engine's default, `DefaultScenePostProcess` | `GearEngine.DefaultPostProcess`; the engine is the outer of `LocalPlayer.ViewportClient` (`GearGameViewportClient`) | the same without `DwTriovizImplEffect` |
| A level's | `WorldInfo.WorldPostProcessChain` (+692) | none in the front end |

Both `UberPostProcessEffect` objects have `bEnableImageGrain` set and `SceneImageGrainScale` 0.0125
(`Default__UberPostProcessEffect` has it clear and 0.02). `PostProcessEffect.bUseWorldSettings` is
set too, so the scale may come from the blended `PostProcessSettings.Scene_ImageGrainScale` (+128,
with `bOverride_Scene_ImageGrainScale`) instead; that is **not verified**. No script function reads
or writes any of them (`script_dump_code` of `Camera`, `PostProcessVolume`, `WorldInfo`, `GearPC`).

`UberPostProcessEffect` properties: `bEnableImageGrain`, `bScaleEffectsWithViewSize` and
`bEnableHDRTonemapper` share the word at +272; `SceneImageGrainScale` is at +276. A
`BoolProperty`'s bit is its `BitMask` at +104 of the property object (**verified**: those three are
`0x80000000`, `0x40000000` and `0x20000000`, in the order they are declared; `PostProcessEffect`'s
`bShowInEditor`, `bShowInGame`, `bUseWorldSettings` and `bAffectsLightingOnly` likewise).

The runtime clears `bEnableImageGrain` in the effects of those three chains while `film_grain` is
off, every frame as each player's input runs, and puts back the value each effect had otherwise
(`runtime/src/script_hooks.cpp`, "Film grain").

## Render command queue

Game threads enqueue render commands into the engine's ring buffer (`FRingBuffer`, guest
`0x8343F018`: +0 start, +4 end, +8 write pointer, +16 read pointer). Each enqueue (inlined at about
500 sites) reserves space with `sub_822B2140`, constructs the command (its vtable at +0) and moves
the write pointer, with no barrier in between. The rendering thread (`sub_823F4238`) reads the
write pointer, then each command's vtable, and calls its `Execute` (vtable +4), again with no
barrier.

On ARM64 the rendering thread could read a command before the write pointer that announced it, find
no vtable yet and stop with `Call to invalid or unregistered function at guest address 0x00000000`
(or `0x00000400`) `(called from 0x823F42E8)`. It happened mostly while a Bink movie played (intro
logos, intro movie, the attract movie after idling on the title screen), the heaviest user of the
queue: about once every ten runs (**inferred** from the day's logs).

The fix is a `[[entrypoint.midasm_hook]]` in the manifest at `0x823F42D8` (the read of the vtable)
calling `AzuraCheckRenderCommand` (`runtime/src/render_queue.cpp`): an acquire fence before the
command is read, plus a safety net that waits for, and logs, a command whose vtable is not valid.
With it, 26 unattended runs (logos every time, the attract movie once) neither crashed nor met an
unfinished command (**verified**). The reordering was therefore on the reading side; the writing
side would have shown unfinished commands despite the fence (**inferred**). If the warning
`[Azura render] Command at ... had vtable ...` ever appears, the writing side reorders too, and the
remedy is making every guest store a release store in the code generator (`REX_STORE_*`).

Mid-function hooks: `[[entrypoint.midasm_hook]]` (under `entrypoint`, not at the top level, or the
code generator ignores it) with `address`, `name` and `registers`; the hook is a C++ function
`void Name(PPCRegister& r18, ...)` taking the listed registers, emitted before the instruction at
`address` (after it with `after_instruction = true`). The guest memory base is
`rex::system::kernel_state()->memory()->virtual_membase()`.

## Movie audio language (Bink)

The pre-rendered scenes ship as one movie per scene, `GearGame/Movies/<name>_LOC.bik`, with 27
audio tracks: three dialogue tracks per language and three shared ones (music and effects). How the
game picks them (**verified** with logs of the calls below, in Spanish):

- The movie player (`sub_823871D8`) opens `Movies\<name>.bik` if it exists; otherwise, for a
  language other than `INT`, `<name>_<language>.bik`; otherwise `<name>_LOC.bik`.
- Before opening, `sub_8238A790` calls `BinkSetSoundTrack` (`sub_82FCA250`) with six tracks: the
  shared 4, 2 and 3, and the language's three, chosen only when the movie's name contains `_LOC`:
  `INT` (the default) 0, 1, 5; `FRA` 6-8; `ITA` 9-11; `DEU` 12-14; `ESN` 15-17; `ESM` 18-20;
  `JPN` 21-23; the remaining ones 24-26.

So the game data must keep only the `_LOC` names the disc has. An alias such as `<name>.bik` or
`<name>_ESN.bik` pointing to `<name>_LOC.bik` makes the game open the multilingual movie under a
name without `_LOC`, and the scenes play in English whatever the language (seen with symbolic links
added by hand to a game folder on Linux).

## Mouse look

The game keeps Unreal Engine 3's PC mouse axes, and the runtime turns the camera with them
(`runtime/src/input/mouse_look.h`). How a player's look input becomes a view rotation every frame
(**verified** with `script_trace` and the bytecode listings of `script_dump_code`):

```
GearPC.PlayerTick
  GearPlayerInput.PlayerInput                 (saves strafe/forward, then calls the engine's)
    PlayerInput.PlayerInput                   (Engine)
      RawJoy* = the stick axes
      GearPlayerInput.PreProcessInput         the right stick only:
        ViewFriction                          aim friction while targeting
        ApplyTestTurningFunction              response curve
        ViewAcceleration, ViewPitchAcceleration   turn speeds up while the stick is held past a threshold
        TargetingModeViewScalePct             slower while targeting
        ApplyTurnDeceleration                 FInterpTo of aTurn/aLookUp: the turn slows down gradually
      aTurn, aLookUp *= 100 * DeltaTime * LookRightScale / LookUpScale, then * FOVScale
      AdjustMouseSensitivity(FOVScale)        aMouseX, aMouseY *= MouseSensitivity * FOVScale
      SmoothMouse                             when bEnableMouseSmoothing
      aTurn += aBaseX + aMouseX ; aLookUp += aMouseY ; bInvertMouse, bInvertTurn
      IsLookInputIgnored                      zeroes aTurn and aLookUp (cinematics)
    ... AutoPitchCentering, cover control remapping
    GearPC.ApplySensitivityScaling            aTurn, aLookUp *= GetSensitivityMultiplier()
                                              (profile look, aim or zoom sensitivity)
GearPC.UpdateRotation                         DeltaRot = (aLookUp, aTurn) / TimeDilation,
                                              ApplyAdhesion, ProcessViewRotation, FaceRotation
```

`FOVScale` is `GetFOVAngle() * 0.01111` when `bEnableFOVScaling`, else 1 (**verified** in the
bytecode of `PlayerInput.PlayerInput`: the stick's `aTurn` and `aLookUp` and, through
`AdjustMouseSensitivity`, the mouse axes are multiplied by it). `ApplyTurnDeceleration` keeps its
state in `LastaTurn` and `LastaLookup`, set inside `PreProcessInput`, before that multiplication.

`GetFOVAngle` answers the player camera's `CameraCache.POV.FOV` (`Camera` +776, `POV` +4, `FOV` +24
in `TPOV`; `PlayerController.PlayerCamera` +880). Aiming narrows the view (50 instead of 80), so
FOVScale makes the mouse turn about 0.6 times as far. While the keyboard and mouse aim bind is
held (and not the zoom one), the runtime multiplies the turn per pixel by `field_of_view` over
that angle, as of the last frame, so aiming turns as far per pixel as looking around. The stick
keeps the game's behaviour. The mouse axes are
added after the stick's curve, acceleration and deceleration, so a value written into `aMouseX`
and `aMouseY` before `PlayerInput.PlayerInput` runs turns the view directly: what a mouse posing as
the right stick could not do, as it went through the dead zone, the full-deflection cap and the
deceleration (felt as the camera carrying on after the mouse stops). On the console the mouse axes
are always zero.

`PlayerInput` properties (**verified**, `script_dump_classes`; the runtime finds them by name):

| Property | Offset |
| --- | --- |
| `MouseSensitivity` (60 in the shipped game) | +248 |
| `aMouseX`, `aMouseY` | +264, +268 |
| `aTurn`, `aLookUp` | +276, +288 |
| `RawJoyLookRight`, `RawJoyLookUp` | +324, +328 |
| `LookRightScale`, `LookUpScale` | +340, +344 |

The player a `PlayerInput` belongs to is its outer `PlayerController`'s `Player` (a `LocalPlayer`),
whose `ControllerId` is the guest user index.

