# Azura Launcher changelog

What each launcher release brings, newest first, as players see it: the launcher shows its own
version's section once in Play after it is installed or updated, and the release scripts publish
it as the update's notes (`scripts/release-common.sh`). Bumping the version adds its section, and a
release without one fails. One list item (`- `) per change, in English; an item may continue on
indented lines.

## 0.4.0

- Play has a Prepare shaders card: it compiles the shaders the game stored in your earlier
  sessions before you play, so they are ready instead of being made while you play. It shows its
  progress, remembers when it last ran, and offers to prepare again once the game has stored new
  shaders since.

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
