# Changelog

## 1.1.3.11 (Beta) - first public release
- Installer: Inno Setup wizard (English/Russian) around a Go engine; installs/removes the timing patch and the AI library independently, keeps your `revived.ini`, backs up a foreign `dinput8.dll`, restores the original game files on uninstall.
- AI library 3.11: public build writes no log files (`[log] enabled=1` turns them on); developer keys F4/F6-F9 only with logging on.
- Health pickups restore 50% (`[heal] scale_pct`); pickups before the first hit are cut too (fixed in 3.11).
- Shield mode only while the charged attack is held; ring squeeze; wounded-enemy ring fix; melee push when the charge is not held.
- Patterns: surround ring on charged-attack hold, punish after release, roll trap, roll habit, pressure when the Prince backs off, answer to a kill, wounded enemy returns angrier.
- Timing patch: faster soldier/sergeant wind-up, strike, recovery, turns, get-up and strafe; damage 10 -> 15; kick domino limited to 3 targets; Prince recovers faster; Prince without hair; optional Malik model (experimental).
