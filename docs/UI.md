# Game UI

spud's menus are **Skyrim's own interface movies**, played by spud. The importer converts the Flash movies
in the user's `Skyrim - Interface.bsa` into `.osu` files in the cache. A movie player in `src/ui/player` runs them:
Flash 8 / ActionScript 2 with the Scaleform GFx 3.x extensions the game uses. Each menu's engine side in `src/ui`
does what the game does for that menu. So the main menu, the journal, the console, message boxes and the rest
look and behave as in the game, from the user's own data. Nothing of the game is shipped.

The converted movie format is in [formats/osu.md](formats/osu.md).

## Structure

| Piece | Where | Role |
|---|---|---|
| Import | `src/importer/convert/ui_*.cpp` | Movies (shapes and glyphs to triangles, bitmaps decoded, script resolved), translations, font map, control map, string game settings, globals, credits; the Mist Menu's logo models; the loading screens (`interface/loadingscreens.osl`, docs/formats/osl.md) with their models and camera paths. `skytool ui import` (also part of `skytool import`); a movie is converted again when its `.osu` is outdated. |
| Player | `src/ui/player` | One player per open movie, with its own VM and `_global`, as each Scaleform movie view has. A `Library` holds what views share (loaded movies, fonts, translations, game settings, globals). Each frame produces a draw list: meshes, glyphs, masks and layers in stage space. No renderer dependency: `skytool ui play` runs movies headless. |
| Menus | `src/ui/menu.*`, `menus.cpp`, `menus_<group>.cpp` | A `Menu` is one entry of the game's menu stack: movie, scale mode, depth, flags, input context, background alpha, and the GameDelegate calls it handles. The core menus are in `menus.cpp`; the in-game groups (items, magic, hud, stats, world) are in their own files. |
| UI | `src/ui/ui.*` | The menu stack and its message queue, input (keyboard, mouse, text, through the input contexts of `controlmap.txt`), the cursor, automation, captures, and the `UiHost` interface to the engine. |
| Renderer | `src/ui/movie_renderer.*`, `shaders/uimovie*` | The `render::Overlay` that draws the draw lists over the scene. See below. |
| Backdrop | `src/engine/backdrop.*` | The Mist Menu's 3D scene (the logo behind the main menu and loading screens): its own camera, drawn as the frame's scene through the mesh path while the menu is open. |
| UI 3D scenes | `src/ui/scene.*`, `item3d.*`, `starfield.*`, `book3d.*`, `ui_scenes.cpp`, `src/engine/ui_scenes.*` | The other slots of the game's UI 3D scene manager: the item menus' and the Magic Menu's 3D item, the Stats Menu's starfield, the Book Menu's book and its page targets. A menu describes its slot each frame; the overlay draws it at the menu's place, before or after the menu's movie, through the mesh path with depth cleared. `ui item3d`, `ui item3d rotate <a> <b>`, `ui scene <slot>` and `ui page <0-3> <png>` dump and drive them (`ui open StatsMenu|beast` stands for the beast-mode flag); ` compares them with the game's. |
| Engine side | `src/engine/app_menus.cpp` | `ui::UiHost`: New Game and the other menu actions, the console's commands and aliases, settings, saves, player info, the calendar. |

### A frame

`Ui::update` runs once a frame before rendering. While nothing is open it returns at once and `Ui::overlay` is
null: no pass, no barriers, no allocation. Otherwise:

1. It processes the menu messages (open and close, in the game's order).
2. It advances each open movie the way GFx does: the engine's `CurrentTime`, then `Advance(dt)` with at most two
   catch-up frames.
3. It builds each movie's draw list, from the topmost menu with the "draws below" flag down.

The renderer then calls `MovieRenderer::record`:

- **Geometry.** Movie geometry lives in per-movie buffers, written once when a movie is first drawn. Per-draw
  data goes into a per-frame buffer, created with the UI. Drawing-API geometry goes into a per-frame vertex
  buffer.
- **Main pass.** Everything draws into one 4x multisampled layer with a stencil buffer for masks, which is resolved
  and composited over the scene with premultiplied alpha.
- **Layers.** Objects with filters or blend modes draw through the same targets at their place on screen, are
  copied into a layer atlas, filtered there (blur, drop shadow, glow, colour matrix), and composited back.

The movies' bitmaps upload through the renderer's uploader. A bitmap still uploading is drawn from the frame
after it arrives, never stalling a frame. The UI draws the current frame's state only: no history, no temporal
filtering.

### Fonts and text

The fonts are the game's own, from its font libraries (`fonts_en.swf`, ...) through `fontconfig.txt`. Text set by
script is translated through `translate_<language>.txt` the way GFx does. The menus' other texts are the game's
string settings (GMSTs).

### Engine images (`img://`)

Movies ask the engine for images by name: `<img src='img://NAME'>` in text, `loadMovie` / `loadClip` of
`img://NAME`. As the game's image loader does, `player::ImageRegistry` (shared by all views)
holds what the engine registered, and GFx sees each as 64 x 64:
- **Textures** from the cache: the books' drop caps and pictures. The importer converts the PNGs the books name into
  `<cache>/textures/.../name.dds` (RGBA8); the Book Menu registers its text's images while it is open. The movie
  renderer reads and decodes them on the renderer's workers and draws them from the frame after they arrive.
- **Render targets** the engine draws: `Local_Map`, the Map Menu's local map, drawn by the
  renderer as a second view of the world (`render::OffscreenView`, `engine/local_map.*`) while the local map is
  shown, and registered with the movie renderer by name (`Ui::set_render_target_image`).

## Menus

| Menu | Where | Notes |
|---|---|---|
| Main Menu | `menus.cpp` | `startmenu.swf`. Continue and Load are enabled when there are saves; New Game asks first, fades, and starts the game behind the Loading Menu. |
| Mist Menu | `menus_loading.cpp`, `engine/backdrop.*` | The 3D logo behind the main menu (fog simulated in `src/fx`), and the loading screens' models with their camera paths and inspection controls. |
| Loading Menu | `menus_loading.cpp`, `loading_screens.*` | The game's loading screens: chosen by their conditions for the destination, text, level bar, fades. `ui seed <n>` seeds the game the choice draws from; `ui loading <file>` dumps the choice and the camera; `ui loadfor <world> <x> <y> \| interior <cell>` opens it for a destination (options `from <world>`, `new_game`, `location <id>`, `no_screen`, and `quest <id> <stage> <flags>` / `global <id> <value>` to replay a state measured in the game); `ui loadscreen <id>` forces the next screen; `ui drag <dx> <dy>` and `ui stick <0\|1> <x> <y>` drive the inspection controls. |
| Cursor Menu, Fader Menu, MessageBoxMenu, Console, Credits Menu | `menus.cpp` | As in the game. |
| Journal Menu | `menus_journal.cpp` | Esc in a world opens it on the tab it was last on, J on Quests. Settings edit spud's settings live and keep them. |
| Inventory, Container, Barter, Gift | `menus_items.cpp` | As in the game. |
| Magic, Favorites, Tween | `menus_magic.cpp` | As in the game. |
| HUD, Sleep/Wait, Lockpicking, Training | `menus_hud.cpp` | As in the game. |
| Stats, Level Up, Book, Dialogue | `menus_stats.cpp` | As in the game. |
| Map, Crafting, Race Sex, Tutorial, the rest | `menus_world.cpp` | As in the game. |

## Input

Every SDL event goes to `Ui::handle_event` first (through `InputFocus::route`). Keys reach the menus as the game
delivers them:
- Key names, Flash key codes and the user-event bindings come from the game's control map (`controlmap.txt`,
  `keyboard_english.txt`).
- A key is looked up through the stack of input contexts (gameplay, Menu Mode for menus that pause, then each open
  menu's own context).
- Bound keys arrive as the menus' user events (Accept, Cancel, ...), the rest as raw key codes.

While a menu that pauses the game is open, `InputFocus::game` is false and the game must not read the keyboard
or mouse.

## Console

`~` opens the game's console movie over whatever is showing. It runs spud's command language
([AUTOMATION.md](AUTOMATION.md)): `cell`, `teleport`, `world`, `set`, `state -`, ... Up and Down recall earlier
lines; Esc or `~` close it. Skyrim-style aliases:

| Alias | Does |
|---|---|
| `cow <world> <x> <y>` | `world <world>`, then `cell <x> <y> +128` |
| `coc <cell>` | Not available: exterior cell EditorIDs are not in the import cache (an importer step) |
| `tfc` | Toggle free-fly mouse look |
| `tm` | Toggle the debug overlay and HUD |
| `fov <deg>`, `sucsm <m/s>` | `set fov`, `set speed` |
| `getpos`, `player.getpos` | Camera position in Skyrim units, heading and pitch |
| `qqq` | Quit at once |
| `help`, `clear` | |

## Settings

The Journal's System > Settings pages (Gameplay, Display, Audio, Graphics) show the game's own options and write spud's
settings live (`src/engine/user_settings.*`, the Skyrim settings by their game names, such as
`fMouseHeadingSensitivity:Controls`). They are kept in `%LOCALAPPDATA%\spud\settings.ini`.
- Only interactive sessions read and write that file.
- `--run`, `--bench`, `--control` and headless runs use the defaults, so they stay reproducible.
- `--settings <file>` uses another file.
- **Graphics** is spud's page, a fourth entry the movie doesn't have (`JournalMenu::add_graphics_page`, `src/engine/graphics_settings.*`). Detail's Low, Medium, High and Ultra are the install's `Low.ini`, `Medium.ini`, `High.ini` and `Ultra.ini`; Ultra is the default and what scripted runs always use. Rows go above Ultra where spud can: Shadow Quality (8192), Shadow Distance (16000, 24000), Object Detail (fades 45, 15, 25), Grass Detail (fade 10000, cell radius 3), Distant Object Detail (16 and 20 cells). Antialiasing is MSAA only, after a restart; the game's FXAA and TAA aren't offered. Distant Object Detail's steps are `uLargeRefLODGridSize` + 1 cells: spud's reading, as its draw distance is a radius of full objects.

## Startup

Run interactively with none of `--world`, `--cell`, `--pos`, and spud opens the main menu. New Game loads
`new_game.world` at `new_game.position` (settings.ini) behind the loading screen. `--menu` and `--no-menu` force
either way.

## Automation and tests

| Command | Effect |
|---|---|
| `ui open <menu>` | Open a menu by the game's name or an alias (`main_menu`, `journal`, `pause`, `console`, `credits`, `loading`, `fader`, `mist`, ...) |
| `ui open <menu>\|<data>` | Open it with show data, what the game passes with the show message: `Crafting Menu\|forge` (the station), `Tutorial Menu\|HBMM` (the help message's default-object key), `TitleSequence Menu\|sequence1` |
| `ui mouse <x> <y>` | Move the cursor to that pixel |
| `ui mousebutton <0\|1\|2> down\|up`, `ui mousemove <dx> <dy>` | A mouse button pressed or released through the normal input path (its bound user event to the top menu's input handler, press and release, and the movie's button), and the mouse's own motion to the top menu's handler |
| `ui close [<menu>\|all]`, `ui back` | Close a menu, everything, or what Esc does |
| `ui activate <text>`, `ui select <text>` | Click or highlight the object showing that text in the top menu |
| `ui pad <hex>` | A pad button by its hardware value in `gamepad.txt`, pressed and let go, through the normal input path |
| `ui key <name> [n\|down\|up]` | Key presses through the normal input path (the game's key names); `down` / `up` hold a key across frames |
| `ui userevent <event>` | A user event for the top menu as the game posts it |
| `ui type <text>`, `ui console <line>` | Text into the focused field; a console line |
| `ui state <file\|->` | Open menus, texts on screen, script errors, focus, hover, timings |
| `ui dump <menu>[:<movie>] <file\|->` | A menu's display list as JSON (`Book Menu:Book`: the pages' movie): where they aren't the default the colour transform (`cxform`: multipliers, then offsets 0..1), the filter count and the blend mode, which script can't read on a text field |
| `ui get <menu>[:<movie>]\|<path>` | A movie variable as ActionScript converts it to a string (`Book Menu:Book` is the Book Menu's second movie; `movie:NAME` a debug movie) |
| `ui set <menu>[:<movie>]\|<path> <value>` | A value into a movie variable: JSON when it parses as JSON (`" "` is a one-space string), else the text as a string |
| `ui call <menu>[:<movie>]\|<path> [args]` | A method with number, `true` / `false` or `"string"` arguments; an object result as `name=value` pairs (`getLineMetrics`, `getCharBoundaries`) |
| `ui localmap <file\|->` | The Map Menu's local map camera (Skyrim units), whether its land is drawn, and the loaded area |
| `ui localmap_area <x0> <y0> <z0> <x1> <y1> <z1>\|off` | The loaded area's bounds instead of the attached cells', to replay one measured in the game; `set grids_to_load N` sets `uGridsToLoad` (default 5) |
| `ui movie <name> [scale mode]`, `ui movie close` | Play any movie on its own (debugging) |
| `ui reload` | Reload the movies |
| `screenshot_ui <file>` | Like `screenshot` but after the UI pass (`screenshot` stays pre-UI) |

- **Traces.** `SPUD_UI_TRACE=1` logs every delegate call between movies and engine, `=text` every text a script
  sets, `=calls:<a>|<b>` the method calls (with their primitive arguments) whose name or target contains a text,
  `=names` every `instanceN` handed out with the member a display-list search was looking for.
- **Headless movie runs.** `skytool ui play <movie>|--all` runs movies headless and reports script errors.
  `skytool ui disasm` lists a movie's script.
