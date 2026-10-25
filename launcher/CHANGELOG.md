# Azura Launcher changelog

What each launcher release brings, newest first, as players see it: the launcher shows its own
version's section once in Play after it is installed or updated, and the release scripts publish
it as the update's notes (`scripts/release-common.sh`). Bumping the version adds its section, and a
release without one fails. One list item (`- `) per change, in English; an item may continue on
indented lines.

## 0.4.1

- A custom controller layout changes what the buttons do in play only: the menus keep the
  controller's own buttons (A accepts, B goes back), and their prompts show them.
- The PlayStation controllers in the controls menu are drawn closer to the real ones: the
  touchpad's light inside its edges, Create and Options beside it, and the PS button.
- Keyboard and mouse button prompts are easier to read: letters and digits in a smooth font, no
  glow smudging the text around them, and the movement keys drawn as two keys, W above ASD, instead
  of four tiny ones.
- PlayStation prompts: grey symbols for a DualSense and coloured ones for a DualShock 4, and the
  Create (Share) button where the game shows Back.
- Prompts in the HUD no longer cut the game's own marks around a button, such as the dots of a
  "hold" prompt.

## 0.4.0

- Play has a Prepare shaders card: it compiles the shaders the game stored in your earlier
  sessions before you play, so they are ready instead of being made while you play. It shows its
  progress, remembers when it last ran, and offers to prepare again once the game has stored new
  shaders since.
- The launcher leaves the game the processor: its animations pause while the game runs or the
  launcher is in the background, it minimizes itself while you play and comes back when the game
  ends (Graphics, Launcher while playing), and it draws the game log only when it can be seen.
- The mouse turns the camera directly, as in a PC game: no dead zone for slow aiming, no limit on
  fast turns, and the camera stops as soon as the mouse does. Mouse sensitivity now sets the turn
  per pixel; if you raised it before, lower it again.
- Controls: in the game's Options, Controller and Keyboard and mouse open menus of their own.
  Controller offers the game's schemes (Default, Alternate) and a Custom one where any button can
  take another's action, shows what each does around a drawing of your controller, and has the
  game's controller settings (inverted axes, sensitivities, vibration and the rest). Keyboard and
  mouse gives every action two keys or mouse buttons (side buttons and the wheel included), and
  has the mouse settings: sensitivity for looking, aiming and zooming, inverted Y, acceleration,
  and pressing instead of holding to aim, zoom and roadie run.
- New keyboard and mouse defaults closer to Gears of War on PC, with no key doing two things.
  Keys you changed before are kept; Defaults in the Keyboard and mouse menu brings the new ones.
- In the menus Enter accepts and Esc goes back, as on a PC; in play Esc pauses, and on the title
  screen Enter starts. The main menu has an Options entry instead of Start's button.
- The button prompts in Azura's menus follow the device in your hands, keyboard and mouse or
  controller, and change as soon as you use another.
- Video options have a Performance overlay in the corner you choose: FPS shows the game's frame
  rate and the time per frame, Detailed adds the game's CPU and GPU use, and Advanced adds its
  memory use, a graph of the last seconds of frame times, the average frame rate and the 1% low.
- On Linux, the game and the programs the launcher starts use your system's libraries instead of
  the launcher's own copies (it fixes keyboard errors in the game log on newer systems).
- Windows (10 and 11, x86-64): the launcher comes as an installer, builds the game from your copy
  and keeps itself and the game up to date, as on macOS and Linux. To build the game it installs
  LLVM and Visual Studio's Build Tools with winget when they are missing.

## 0.3.0

- Local co-op: two players can play the campaign in split screen on one computer. In the game's
  Options, Local co-op gives a controller, or the keyboard and mouse, to player 2, who plays with
  a profile of their own (your gamertag with "(1)") and saves of their own.
- When a player's only controller switches off, the game pauses for them and a notice asks to
  reconnect it, or to press the confirm button on another controller to use that one. The same
  controller coming back plays for the same player again.
- On the title screen after player 2 joins, only player 1 can start, so the game always loads
  player 1's profile and saves.
- In the Options, Keyboard and mouse is followed by Local co-op, and the mature content settings
  (violence, mature language) are now at the end of Game.
- Image scaling offers what your graphics support: on Linux, AMD CAS and AMD FSR 1 as well.
- On Linux, far fewer hitches while the game prepares new graphics in the background.

## 0.2.2

- On macOS, Play shows the chip as one entry again, without an empty CPU entry.

## 0.2.1

- Fewer hitches the first time a new area or cutscene appears: the game prepares new shaders in
  the background, and shadows wait less for theirs.
- After this update, the game prepares its shaders again at its first start (the "Compiling
  shaders" notice); it takes a few minutes, only once.
- Play names the graphics as they are: Metal on macOS, and on Linux the processor and the graphics
  card as separate entries.

## 0.2.0

- Builds the game for this computer from your own copy of Gears of War 3, a folder or a disc
  image (.iso), with every fix of the project, and installs the compiler it needs when it is
  missing.
- Updates itself and the game when a new version is published: most game fixes install in seconds.
- Can be used with a controller (Xbox, PlayStation, Nintendo and others), with the buttons of the
  controller in hand on screen and an on-screen keyboard to type.
- In the game, the controller in your hands is player 1, whatever order your controllers were
  connected in.
- Display mode, render resolution, game language and gamertag, saves you can move and back up, and
  the achievements with their artwork.
