# Reverse engineering notes

What we know about the game's internals and how we found it, so that changing menus, settings or
gameplay does not start from zero. Addresses and offsets are for the supported builds in
`versions.toml` (both editions share the same code). Facts are marked as **verified** (seen in a
run, a dump or the code) or **inferred** (deduced, not yet checked).

Nothing here is game data: only names, offsets and identifiers needed to hook the game.

## Contents

- [Engine basics](#engine-basics)
- [Script bytecode](#script-bytecode)
- [Debug tools](#debug-tools)
- [Configuration files (coalesced)](#configuration-files-coalesced)
- [Main menu](#main-menu)
- [Options menu](#options-menu)
- [Profile settings](#profile-settings)
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
share a 32-bit word; their bit masks are not decoded yet.

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
| `9A` / `9B` | native `==` / `!=` (int) | two operands, `16` |

**Patching bytecode.** The menu's bytecode lives in guest memory and can be changed at run time,
as long as every jump offset stays valid: a replacement must have exactly the size of what it
replaces. Examples in `script_hooks.cpp`: a one-byte constant changed to another value, and
`EX_IntOne` (`26`) replaced with `EX_Self` (`17`) so that `x == 1` becomes `x == self`, never true
for a small number. Patch before the function runs for the first time (in the `ProcessInternal`
hook), and only once.

## Debug tools

All are cvars in the `Azura/Debug` category, off by default. Pass them on the command line, for
example `./azura --script_trace=true`.

| Cvar | What it does |
| --- | --- |
| `script_trace` | Logs the first call of every script function as `Class.Function` (`(event)` when it came through `ProcessEvent`: from Flash or native code). Slow. |
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
| | `AzuraQuit` (added) | `MAINMENUOP_None` | 5 |

Indices 0 and 5 were confirmed with `HandleMenuSelection` traces; the others follow the list order.
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
| 1 | Video | A list: Calibrate brightness (opens the game's calibration screen), Full screen, Vsync, Anti-aliasing, Motion blur. Back and Defaults |
| 5 | Keyboard and mouse | Replaces the storage location: Mouse sensitivity. Back and Defaults |

How:

- **Rows and categories** are written into `[UIScene_Options] OptionCategories[1]` and `[5]` of
  every `geargame.<lang>`, with every member, our texts in that language and the game's own texts
  (category titles, button names) kept from the file.
- **List rows borrow profile settings the game never uses**, the engine's racing game preferences:
  Movement Control 18 (calibration), Transmission 19 (vsync), Race Camera 20 (anti-aliasing), Brake
  21 (motion blur), Accelerator 22 (full screen). Their definitions (`ProfileMappings[11]` to
  `[15]`) get our value names. The calibration row's definition has two values, both named "Open",
  since an array cannot shrink. The slider uses ID 9105, which the profile does not have.
- **Showing values:** while `PopulateOptionsList` is on one of our rows, every call it makes
  (entered through `CallFunction`) rewrites its locals `Slider_Value` / `OptionList_Index` with the
  real value, so the call that hands them to Flash reads ours.
- **Saving values:** when `GetOptionValueAt(Index)` returns for one of our rows, the value is
  applied to its cvar (`fullscreen`, `vsync`, `swap_post_effect`, `motion_blur`, `mnk_sensitivity`)
  and written to `azura.toml`, leaving the rest of the file as it is. The menu also writes the
  borrowed profile settings, which is harmless.
- **Defaults** resets our rows to the values in `azura.default.toml` (the SDK's default when a key
  is not there).
- **Positions 1 and 5 show their lists:** the bytecode of `Callback_MenuOptionSelected` is patched
  so that `SelectedOption == 5` compares with 255 and `SelectedOption == 1` with `self`. The `1` in
  `Callback_ReturnToMain` becomes 255 too, so leaving Video plays the usual sound.
- **Calibrate brightness:** `FilterButtonInput` is watched while Video is open to follow the
  selected row (Up/Down, wrapping, reset to 0 when a list is built). Accept, Left or Right on the
  calibration row runs `Callback_VideoOptions` in place of that `FilterButtonInput`. Closing the
  calibration screen then calls `Callback_ReturnToMain`, which would put the script on the list of
  categories while Flash goes back to the Video list; that one call is skipped.
- **Motion blur** is applied at start-up by setting `[SystemSettings] MotionBlur` in
  `Xbox360-GearEngine.ini`.

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
- **The first launch with an empty shader cache looks frozen** while tens of thousands of pipelines
  are created. It is not a hang.
- **Lock-free code written for the Xbox 360 can break on ARM64.** The Xenon cores run in order,
  so the game publishes data between threads without barriers where it relies on that; ARM64
  reorders memory accesses. See "Render command queue".

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
