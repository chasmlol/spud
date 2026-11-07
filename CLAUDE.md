# Spud: notes for agents

Spud is an open-source C++23 Vulkan engine for open-world RPGs. It currently runs Skyrim Special Edition from the player's own copy of the game, with only native formats at runtime. Read [docs/VISION.md](docs/VISION.md), [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and [docs/ROADMAP.md](docs/ROADMAP.md) before larger changes.

## Build and run

Heavy work goes through the machine gate (`tools/gate/gate.py`), so several sessions can share one PC. Use the scripts:

- `scripts\build.cmd release` (from Bash: `cmd //c "scripts\\build.cmd release"`)
- `scripts\spud.cmd ...` and `scripts\skytool.cmd ...`, not the `.exe` files directly
- `scripts\gate.cmd status` shows who holds the machine
- `scripts\audit.cmd`: the image audit, once before merging rendering or streaming work
- `scripts\perf.cmd`: perf runs, only in a chat doing performance work

Import the game with `scripts\skytool.cmd import`. It writes to `%LOCALAPPDATA%\spud\cache\skyrim`. `launch.bat` builds and opens the game.

Run the engine headless so nothing pops up on the desktop: `scripts\spud.cmd --headless --run script.osc`. Check visual changes with a screenshot and look at the PNG. See [docs/AUTOMATION.md](docs/AUTOMATION.md).

## Working here

- Work on a branch, commit normally, merge the latest `main` in, then merge to `main`.
- Use the smallest check that proves the change: one screenshot or one headless script. Don't run full suites unless asked.
- Known problems are GitHub issues on this repo. Close one when it's fixed, and keep issue text plain.
- Push only when the user asks.

## Rules

- **Never commit game data** or anything converted from it, including screenshots. Test output goes in a scratch folder or the cache.
- **Our own words only:** code, comments, docs, commits and issues describe what the engine does. Never put addresses or internal names from the game's binaries in them, or decompiled code.
- **Only the importer (`src/importer`) knows Skyrim's formats.** The engine loads Spud formats only (`src/formats`). Native formats are versioned: change `src/formats/*.hpp` and `docs/formats/*.md` together and bump the version.
- **No TAA:** the final image is never built from previous frames. See [docs/RENDERING.md](docs/RENDERING.md). TAA, temporal upscalers, frame generation, full-screen history and camera jitter are banned, even as options. Temporal state inside one effect is fine.
- **Match the real game.** Values meant to match Skyrim need evidence from the game's data or a measurement, not a guess.
- **No IO, parsing or pipeline creation on the render thread.**

## Code style

C++23, MSVC, 4-space indent, 110 columns (`.clang-format`). Namespaces under `spud::`. snake_case functions and variables, PascalCase types, `m_` members, `#pragma once`. Parse with `spud::BinaryReader` and log with `spud::log`. Comments explain why, not what. Units are metres, Z up, X east, Y north.
