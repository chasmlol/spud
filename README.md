# Spud

Spud is an open-source C++ engine for open-world RPGs. It currently runs Skyrim Special Edition from your own copy of the game.

- **Bring your own game.** Spud ships no game content. Its importer converts your own install into Spud's native formats, locally.
- **Native formats at runtime.** The original file formats are read only by the importer. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and [docs/formats](docs/formats/).
- **A clear image.** No TAA, no temporal upscaling and no frame generation.

Much of this code was written with AI assistance.

## Building (Windows)

You need Python 3, Visual Studio 2022 or later with C++ tools, CMake 3.28+, Ninja, the Vulkan SDK, and [vcpkg](https://github.com/microsoft/vcpkg) (set `VCPKG_ROOT`, default `C:\vcpkg`).

```
scripts\build.cmd release
```

Binaries land in `build\release\bin`.

Optional: install [sccache](https://github.com/mozilla/sccache) (`winget install Mozilla.sccache`) and repeat builds get much faster.

## Importing your game

```
build\release\bin\skytool.exe import
```

This finds Skyrim SE/AE automatically (or pass `--game <dir>`), reads your load order, and writes the cache to `%LOCALAPPDATA%\spud\cache\skyrim`. Import one world with `--world tamriel`.

## Running

```
launch.bat
```

This builds, imports if needed, and opens the game. To run it yourself: `build\release\bin\spud.exe --world tamriel`.

`skytool` also inspects game data: `bsa list|extract|find|verify`, `esm ...`, `nif ...`. Run it with no arguments for usage.

## Docs

- [Vision](docs/VISION.md) and [roadmap](docs/ROADMAP.md)
- [Architecture](docs/ARCHITECTURE.md) and [native formats](docs/formats/)
- [Rendering](docs/RENDERING.md), [audio](docs/AUDIO.md), [movement](docs/MOVEMENT.md) and [game UI](docs/UI.md)
- [Automation](docs/AUTOMATION.md): scripting the engine, with or without a window

## License

MIT, see [LICENSE](LICENSE). One data file is the exception: `src/audio/hrtf_data.inc`, the headphone HRTF filters, derives from the SADIE II database, University of York, under the Apache License 2.0 ([tools/hrtf/LICENSE-SADIE-II.txt](tools/hrtf/LICENSE-SADIE-II.txt)).

## Legal

Spud is an unofficial, non-commercial fan project. It is not affiliated with, endorsed by or sponsored by Bethesda Softworks, ZeniMax Media or Microsoft. The Elder Scrolls and Skyrim are trademarks of ZeniMax Media, used here only to say which game Spud works with.

Spud includes no game content. You need your own legal copy of the game to use it.

If you hold rights in anything here and have a concern, please open an issue and it will be dealt with promptly.
