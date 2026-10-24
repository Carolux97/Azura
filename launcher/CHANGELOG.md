# Azura Launcher changelog

What each launcher release brings, newest first, as players see it: the launcher shows its own
version's section once in Play after it is installed or updated, and the release scripts publish
it as the update's notes (`scripts/release-common.sh`). Bumping the version adds its section, and a
release without one fails. One list item (`- `) per change, in English; an item may continue on
indented lines.

## 0.1.0

- Builds the game for this computer from your own copy of Gears of War 3, a folder or a disc
  image (.iso), with every fix of the project, and installs the compiler it needs when it is
  missing.
- Updates itself and the game when a new version is published: most game fixes install in seconds.
- Display mode, render resolution, game language and gamertag, saves you can move and back up, and
  the achievements with their artwork.
