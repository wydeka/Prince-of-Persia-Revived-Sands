# How it works

The mod has two independent parts.

## 1. Timing patch (data)
The game keeps its data in `.forge` archives. The installer changes numbers inside a few entries (animation clip lengths, the soldier damage setting, kick reaction, the Prince model) and nothing else:

- `installer/cfg.json` lists, per archive entry, the byte blocks to replace (`apply_segs`) and the original blocks (`restore_segs`).
- Before writing, the engine checks that every block of the entry equals the expected original (or the expected patched state). Anything unexpected -> nothing is written. "Restore" writes the original blocks back and re-checks.
- `cfg.json` contains fragments of the game's own data (needed to verify and restore). If you fork the project, keep that in mind.

## 2. Smart enemy AI (`ai/dinput8.c`)
A 32-bit proxy of `dinput8.dll`. The game loads it from its folder instead of the system one; the DLL forwards `DirectInput8Create` to the real library and starts a worker thread that:

- waits until the game code is decrypted in memory, then replaces entries of virtual-method tables of the enemy AI conditions (attack, fight ring query, combat behaviour, move mode, target query/distance) and of the enemy damage handler with wrappers. Each wrapper calls the original and may change the answer. The expected original address is checked first; if it does not match (a different exe build) that hook is skipped and the line is written to the log.
- reads the Prince's input (left mouse hold, rolls) and the fight ring of the game's ring manager, and changes the ring radii and refuses ring slots to widen the ring, pick the attacker, etc.
- watches the Prince's health component and cuts health pickups (an increase of 3 or more in one step while the readings are continuous).

The patterns (surround, punish, roll trap, roll habit, pressure, answer to a kill, wounded return, shield-first) are state machines in `dinput8.c`; each one logs its events as `PATTERN-NAME: ...` lines in the trace when logging is on.

Addresses are for the Steam executable `Prince of Persia.exe` of 19,447,296 bytes.

## 3. Installer
- `installer/` - Go engine. Without arguments it shows a small local web page (needs a browser); with `--cli install|restore|status|find` it works without a window (options `--no-ai`, `--no-data`, `--malik`, `--reset-ini`, `--game`, `--logfile`).
- `setup/RevivedSands.iss` - Inno Setup wizard that calls the engine in `--cli` mode (it runs before any file of the wizard is copied, so a refusal leaves nothing half-installed).
- `installer/engine_test.go` builds a synthetic game folder from `cfg.json` and tests install, reinstall, restore (byte-identical), Malik switch, foreign `dinput8.dll`, corrupted archive.
