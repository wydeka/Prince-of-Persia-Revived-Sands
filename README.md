# Prince of Persia: The Revived Sands (Beta)

**v 1.1.3.11 (Beta)** - a hard-mode mod for *Prince of Persia: The Forgotten Sands* (2010, Steam, PC) by **wydeka**.
[Русская версия](README_RU.md) - [Discord](https://discord.gg/HRFtZ2sNJs)

This mod rethinks the game's combat, turning passive enemies into dangerous tactical opponents. It does not change the health of the Prince or the enemies: all difficulty comes from timings, faster animations and advanced AI (through the `dinput8.dll` library). Every mistake in a fight is punished.

> **Beta.** Tested by its author in one full playthrough. The mod covers the game up to the awakening of the Sand army; later levels are unchanged.

## What it changes

**1. Timings and animations (edits inside the game archives)**
- Combat pace: soldiers and sergeants wind up, strike and recover from a strike faster. The window to dodge is shorter.
- Mobility: enemies turn to the Prince faster, get up faster after a knockdown and strafe more actively.
- Balance: damage of regular enemies is raised (from 10 to 15). The kick's "domino effect" now knocks down at most 3 targets instead of the whole crowd.

**2. Smart AI (`dinput8.dll`)**
- Against charged attacks: if you hold LMB, the crowd rings the Prince and keeps its distance, and the nearest enemy may strike quickly to break the charge.
- Tactics: in a mixed crowd mostly the shield-bearers attack; they also take charged hits and do not retreat.
- Roll punishment: the AI takes into account which side you like to roll to, and after a chain of rolls the nearest enemy strikes almost at once.
- Pressure: the ring closes in harder when the Prince backs off or kills an enemy (the limit on simultaneous attackers is lifted for a moment). Wounded soldiers retreat and return to the fight more aggressively.

**3. Resources**
- Health from vases restores half as much, so every bit of health matters.

In game: `F3` turns the smart AI on/off, `F5` reloads `revived.ini`.

## Install

1. Download `RevivedSands_Setup.exe` from the [Releases](../../releases) page and close the game.
2. Run it (it asks for administrator rights: the game usually lives in *Program Files*), choose the game folder and what to install.
3. Start the game from Steam as usual.

The installer checks that the game files are original and writes nothing if something does not match. Windows SmartScreen or an antivirus may warn about an unsigned file that replaces `dinput8.dll` - the full source is here.

**Remove:** Settings > Apps > *Prince of Persia: The Revived Sands* > Uninstall. This puts the original game files back. If anything goes wrong: Steam > game Properties > Verify integrity of game files.

Options in the wizard: smart AI (with "reset `revived.ini`"), timings (with the experimental **Malik model** instead of the Prince - it works only on levels where Malik exists, elsewhere the game crashes; off by default).

## Compatibility
- Steam version of the game, `Prince of Persia.exe` of 19,447,296 bytes. Other builds: the AI hooks check the expected addresses and skip anything that does not match.
- Single player only. No other `dinput8.dll` mod at the same time (the installer keeps your old one as `dinput8.dll.before_revived` and puts it back on uninstall).

## Settings and logs
See [docs/CONFIG.md](docs/CONFIG.md). A public build writes no log files; to send a bug report put `[log]` `enabled=1` into `revived.ini`, restart the game and attach `revived_trace.txt`.

## Build from source
Needs Python (`pip install ziglang`), Go 1.24+, Inno Setup 6 (Windows).
```
build_all.bat
```
builds the DLL (`ai/`), the installer engine (`installer/`) and the wizard (`setup/`, result: `setup/Output/RevivedSands_Setup.exe`). On Linux/macOS `./build_all.sh` builds the DLL and the engine. Tests: `cd installer && go test -run TestSynthetic -v .` (a synthetic game folder is built from `cfg.json`, no game files needed). More: [docs/HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md).

## Repository
| Folder | Contents |
|---|---|
| `ai/` | Smart AI: `dinput8.c`, `dinput8.def`, default `revived.ini` |
| `installer/` | Go engine: patch data (`cfg.json`), install/restore logic, local web UI, tests |
| `setup/` | Inno Setup wizard script and texts |
| `docs/` | Settings, how it works |

## License
Source code: [MIT](LICENSE). Prince of Persia and The Forgotten Sands belong to Ubisoft; this is a fan-made, non-commercial mod, not affiliated with or endorsed by Ubisoft. You need your own copy of the game.
