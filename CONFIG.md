# Settings (`revived.ini`)

The file lies in the game folder next to `Prince of Persia.exe`. Every key has a comment above it. `F5` in game reloads it, `F3` switches the smart AI on and off.
Settings the installer keeps when you update; tick "Reset revived.ini" in the wizard to go back to the defaults (the old file is saved as `revived.ini.bak`).

| Section | What it controls |
|---|---|
| `[patterns]` | Master switch, how long the left mouse must be held to count as a charge, ring grow/shrink times, how many enemies may attack while the ring is wide |
| `[ring_distance]`, `[ring_slots]`, `[scale_by_enemies]` | Distance of the enemy ring and number of places in it, depending on how many enemies fight |
| `[tease]` | The "pulse" while a charge is held: one enemy steps in, teases and may attack |
| `[roll]`, `[roll_fatigue]`, `[roll_lag]` | Roll detection, the trap after chained rolls, roll recovery |
| `[wave]` | After a charge is released only enemies closer than a distance may start an attack |
| `[crowd]` | More attack places when 3+ enemies fight |
| `[wounded]`, `[shield]`, `[mixed]` | Wounded-enemy retreat and return, shield-bearer behaviour, "shield-bearers attack first" rule in mixed crowds |
| `[melee]` | Enemies keep walking in when the charge is not held (anti-spam) |
| `[heal]` | `enabled`, `scale_pct` (10-100): how much of a health pickup is restored. Default 50 |
| `[log]` | `enabled=1` writes `revived_log.txt` / `revived_trace.txt` and turns on developer keys. Public builds default to 0 |
| `[diag]`, `[limit]`, `[slots]`, `[baseline]`, `[reach_gate]`, `[flank]` | Diagnostics and experiments from development; leave as they are |

Sections not present in the file use the defaults built into the DLL.
