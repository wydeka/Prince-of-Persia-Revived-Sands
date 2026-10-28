# Prince of Persia: The Revived Sands (the demo)

by **wydeka** · Discord: https://discord.gg/HRFtZ2sNJs

A difficulty mod for *Prince of Persia: The Forgotten Sands* (PC, Steam). Fights against human enemies get harder **without raising their health**: the difficulty comes from timings and encounter tempo.

> **This is a demo.** The mod covers the game only up to the moment the army of King Solomon is summoned. Later levels are unchanged.

Unofficial fan mod. Not affiliated with or endorsed by Ubisoft. Use it only with a copy of the game you own, at your own risk.

## What changes

- Soldiers and shield-bearers wind up, strike, turn, strafe and get up from knockdowns faster, idle less, and are stunned for a shorter time after hits, kicks, rolls and push-backs.
- Soldier and sergeant damage is slightly raised in a few settings.
- The Prince recovers faster after hits, stumbles and knockdowns. His look is standard.

## Install

1. Close the game.
2. Download `RevivedSands_Installer.exe` from the [Releases](../../releases) page and run it (Python is not needed).
3. The installer opens a local page in your browser (an address like `127.0.0.1`; no internet is used). Check the game folder and press **Install mod**. If a "Restart as administrator" button appears, press it.

Press **Restore everything** to return the original files at any time. Fallback: Steam → game properties → Installed files → Verify integrity of game files.

The optional checkbox **"Use Malik's model instead of the Prince (experimental)"** is **off by default**. Malik's model only works on levels where Malik exists; elsewhere the game crashes. It is your choice.

Windows SmartScreen or antivirus software may complain about an unsigned `.exe` ("More info" → "Run anyway").

## How it works

The installer changes numbers (animation lengths and a few settings) inside `DataPC.forge`, `DataPC_actors.forge` and `DataPC_01-03.forge`. It first checks that your files are the original Steam version and writes nothing otherwise. Save files are untouched.

## Building

See [BUILD.txt](BUILD.txt). The patch data (`cfg.json`) is not stored in this repository because it contains fragments of the game's files; it ships inside the released installer.

## License

Installer source code: MIT (see [LICENSE](LICENSE)). The license does not cover the game or any Ubisoft property.

---

# Русская версия

Мод сложности для *Prince of Persia: The Forgotten Sands* (ПК, Steam). Бои с людьми-врагами становятся сложнее **без повышения их здоровья**: сложность строится на таймингах и темпе стычек.

> **Это демо.** Мод сделан только до момента призыва армии царя Соломона. Дальнейшие уровни не изменялись.

Неофициальный фан-мод, не связан с Ubisoft. Используйте только с копией игры, которой вы владеете, на свой риск.

**Что меняется.** Солдаты и щитоносцы быстрее замахиваются, ударяют, разворачиваются, обходят и встают после нокдауна, меньше стоят на месте и короче оглушены после ударов, пинков, переката и отбрасывания. Урон солдат и сержантов слегка поднят. Принц быстрее приходит в себя после ударов, спотыканий и нокдауна.

**Установка.** Закройте игру, скачайте `RevivedSands_Installer.exe` на странице [Releases](../../releases), запустите, проверьте папку игры и нажмите **Install mod**. Кнопка **Restore everything** возвращает оригинал. Запасной путь: Steam → «Проверить целостность файлов игры».

Галочка **«Use Malik's model instead of the Prince (experimental)»** по умолчанию выключена: модель Малика работает только на уровнях, где он есть, иначе игра вылетает. Выбор за вами.
