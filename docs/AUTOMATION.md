# Automation: headless runs and scripting

spud can be driven entirely by scripts, with or without a window, so tools and tests use the engine the same way a player would, without anyone at the keyboard.

## Modes

| Mode | How | Use |
|---|---|---|
| Windowed | default | Playing, manual checks, checks that include the desktop compositor. Run interactively (no `--control`, `--world`, `--cell` or `--pos`) it opens the main menu ([UI.md](UI.md)). |
| **Headless** | `--headless` (with `--run`, `--start`, `--control`, `--bench` or `--screenshot`) | Scripts and tests. Nothing on the user's desktop: no visible window, no taskbar entry, no focus change. |

Headless renders and presents exactly like windowed: same passes, same settings, every frame presented at the requested resolution. The difference is that the window is never shown. It is created hidden as a tool window. It presents with a mode that never waits on the display (immediate, else mailbox). Its size follows `--width`/`--height` and the `resolution` command, whatever the desktop.

Everything that works windowed (scripts, screenshots, bench, control) works headless.

## One command language

Bench scenarios, test scripts and live control all use the same line-based commands. On top of the scenario commands:

| Command | Effect |
|---|---|
| `cell <x> <y> [+z]` | Jump to the centre of exterior cell (x, y), `+z` units above the terrain (default +2000) |
| `wait_loaded [timeout s]` | Block until all streaming for the current view is complete |
| `wait_frames <n>` | Render n frames. Animation time stands still in a script, so fades and whatever else the sky times stop too: step it with `set animation_time` (a jump over 1 s counts as none). Script time runs on rendered frames, not `move` ticks: a `Wait` needs `wait_frames` |
| `screenshot <file.png>` | Save the current frame (pre-UI) |
| `screenshot_hdr <file.npy>` | Save the HDR scene before tonemapping (and before refraction) as float16 H x W x 4|
| `screenshot_shadow <file.npy>` | Save the sun's shadow cascades of the next frame as uint16 layers x size x size (the game's D16 depth)) |
| `screenshot_ui <file.png>` | Save the current frame with the game UI composited ([UI.md](UI.md)) |
| `spawn <x> <y> <z> <heading> <mesh>` | Draw a cache mesh that no reference places (e.g. `meshes/actors/character/character assets/eyesmale.osm`) at a position in Skyrim units, heading in degrees: render-path tests |
| `state <file.json>` | Write camera pose (metres and Skyrim units), cell, worldspace, loaded and pending counts, last frame's timings, `actors`: the actors that exist here (reference, NPC, position), `gpu_median_ms`: per-pass GPU medians over the last 64 frames, steady enough for A/B comparisons, and `sun_cascades`: each cascade's rows per metre, near and far in units |
| `effect <x> <y> <z> <heading> <model>` | Play a one-shot effect (`meshes/effects/watersplash.osm`): particles and animated shapes, arguments as `spawn`. It steps with the world clock, so a script moves `animation_time` to see it run |
| `set <name> <value>` | Change a runtime setting (`draw_distance`, `wireframe`, `show_disabled`, `time_of_day`, `clock_run` (real seconds through the game's calendar at once), `fast_travel` (a map marker's hex FormID, as `Game.FastTravel`), `autosave_age` (minutes off the last save's time), `weather`, `fov_horizontal`, `point_lights`, `animate_lights`, `light_fade_start`, `tree_wind` (as the game's `stswm`; 0 stills tree leaves), `draw_blended` (0: leave out blended draws except blended decals, as the game's lit target before its deferred composite has them), `draw_decals` (0 with `draw_blended 0`: leave out the blended decals too, the game's target just before its decal pass), `blend_background` (>= 0: clear the colour to this value, depth kept, and draw over it the blended draws other than blended decals, or with `draw_blended 0` the blended decals alone; renders over 0 and 1 predict them over any background), `sun_shadow` (1: everything in the sun's shadow, as the game's shader applies a shadow mask of 0: direct diffuse and specular off, rim, soft and back lighting kept; for comparing the pixels the game shadows), `shadows` (0: no shadow maps or masks, and shadow-casting lights give no light, as when none is chosen), `animation_time` (the clock of scripted runs, 0 by default: NIF controllers, leaves, light animation; one frame back is 1/60 s earlier), `fov`, `mouse_sensitivity`, `invert_y`, ...). Unknown names are an error. |
| `set fog_off 0\|1` | The console's `tfog`: the weather's fog off, for comparing with the game |
| `set room_culling 0\|1`, `set occlusion 0\|1` | Rooms and portals, and occlusion planes with the land horizon, off or on, for comparing frames |
| `set hold_actors 0\|1` | Actors take one fixed step of their graphs and stand still: what they do is game state, like the hour |
| `set player_live 0\|1` | A headless session's player ticks on real time between commands, as when playing, instead of only on scripted ticks: for what only shows with the live tick (view changes, riding) |
| `set player_invulnerable 0\|1` | Nothing hurts the player, blows, falls and drowning alike: the tour keeps it, so a death can't end the run at the Main Menu |
| `set door_preload 0\|1` | Whether a load in an interior also makes the exterior worlds its doors lead to (off: they are built on the way, to measure that) |
| `set debug_view <view>` | `lit` (default), `albedo`, `normals`, `lighting`;: `material` (r shader type, +100 for effects, 200 for landscape; g material flags; b 1 when alpha tested) and `form` (the reference's FormID as r + g * 2048 + b * 2048^2; landscape is all ones; a is the distance from the camera in metres; blended effects show what lies behind) and `technique` and `uv` (the fractional texture coordinate exactly: u = (r + g / 1024) / 256, v = (b + a / 1024) / 256) and `shadow` |
| `door <sub>...` | Load doors: `list` (JSON: doors here, sizes, destination spaces), `open <formid\|nearest>`, `close <formid\|nearest\|all>`, `activate` (the Activate key: the door the camera points at, within reach), `rooms` (JSON: the doors between rooms here, nearest first), `go <formid\|nearest>` (opens it and stands the free camera a metre in front), `through` (steps through; the next frame crosses), `auto on\|off` (whether the free camera opens doors it comes near; also `set door_auto_open`). A `fly` path that crosses a door is carried through with the camera; later absolute coordinates name the space the camera is in then. |
| `ui <sub> ...` | Drive the game UI: `open`, `close`, `back`, `activate`, `select`, `key`, `type`, `console`, `state`, `reload` ([UI.md](UI.md#automation)) |
| `papyrus <Script.Function>` | Call a global Papyrus function with no arguments; what it logs appears in the engine's log. `papyrus new_game` binds the quests' scripts and starts the start-game quests |
| `papyrus quest:<EditorID>:<stage>`, `papyrus global:<hex>:<value>`, `papyrus new_game` | Start a quest at a stage, set a global, start the new game's quests. |
| `game character native <Class> <form> <Name> [i:1 r:1.5 b:1 Class:form]` | Calls a Papyrus native or method on a form through the VM (form 0: a global function of the class); the result is logged as `native <Name> =...` |
| `actor <reference\|player> event <name>`, `actor ... idle <IDLE EditorID>`, `actor ... set <variable> <value>` | Sends an animation event, plays an idle, or sets a graph variable on an actor that exists here (`state`'s `actors`); logs whether it was taken |
| `camera free <x> <y> <z> <heading> <pitch>` / `camera player` | Watch from a fixed pose in units and degrees: the player stays where it is and, in third person, shows. `camera player` goes back |
| `actor <reference\|player> action <AACT EditorID>` | Performs an action through the idle tree, as the attack key does: `ActionRightPowerAttack` picks a kill move if one applies |
| `set camera_orbit <degrees>` | The camera carried clockwise around the walking player's body, for a shot from the side |
| `actor <reference|player> blood set|add|clear` | The blood on its weapon, as the game's `SetBloodParam` console command |
| `actor <reference\|player> graph` | Logs the names of the graph nodes running now |
| `projectile <PROJ EditorID or form (hex)> <heading> <pitch> [SPEL EditorID]` | Launches a projectile from the camera (degrees, positive pitch looks down), by the player when on foot, with the ammo that flies as that record, or the spell (form 0: its own projectile); logs where each projectile ends |
| `travel <space\|-> <x> <y> <z> <heading> <pitch>` | The player (or free camera) moved there behind a real loading screen; ends with it |
| `attack left\|right\|voice <seconds>` | That hand's Attack/Block button held, as the mouse button would be, or the Shout key |
| `set auto_message <button>` | Answer every message box with that button (below 0: they wait for a player) |
| `quit` | Exit |

The player on foot ([MOVEMENT.md](MOVEMENT.md)):

| Command | Effect |
|---|---|
| `mode walk\|fly` | Walk (the player drops from under the camera) or fly the noclip camera (the player stays where it is) |
| `player <x> <y> <z> <yaw> <pitch>` | Walk from here: feet position, `z` "+N" = N units above the terrain. In a bench scenario's leading lines it is the start, settled before recording. |
| `move <dir> <s> [walk\|run\|sprint] [jump]` | Hold a direction for `s` seconds of 60 Hz ticks (default run; `jump` on the first tick). `dir`: `forward`, `back`, `left`, `right`, `forward_left`, `forward_right`, `back_left`, `back_right`, `none` (stand). Completes when the ticks have run. |
| `gamepad <lx> <ly> <rx> <ry> <s> [lt v] [rt v] [button...]` | Hold a pad state for `s` seconds virtual XInput pad: stick deflections -1..1 (y up = forward), buttons `a` `b` `x` `y` `lb` `rb` `ls` `rs` `start` `back` `up` `down` `left` `right` mapped through controlmap.txt's gamepad column. The player reads it one tick later, as the game polls the pad at the start of a frame. |
| `jump` | Jump (if standing): one tick |
| `ai on\|off` | The console's ToggleAI: with AI processing off the session clock stops, so regeneration delays stop counting down |
| `process_time <s>` | Set the session clock regeneration measures time with |
| `breath <s>` | Set the breath left under water, so a drowning test needn't wait out the whole breath |
| `watch <form id>... \| off` | References (hex form IDs) whose collision body positions the next traces record (`<file>.refs.csv`) |
| `trace <file.csv>` / `trace off` | Record every tick (position, velocity, ground state) and write the CSV; `<file>.contacts.csv` beside it holds the character proxy's manifold each tick |

In a script, the player only moves on scripted ticks: between `move` commands time stands still for it while collision keeps streaming, so the same script gives the same result at any frame rate. Movement follows the camera's heading when the `move` starts (`spin` turns it). While walking, the camera stays at the player's eyes: `fly`, `teleport`, `start` and `cell` only move it in fly mode; use `player` to move the player. `wait_loaded` also waits for the collision around the player. `state` adds `mode`, `player` (latest tick: position, velocity, `on_ground`, `waiting`, ground layer and material, `ticks`) and `physics` (cells, bodies, shapes). `set` takes `player_accel_time`, `player_decel_time`, `player_air_control`, `player_jump_height`, `player_gravity`, `player_air_gravity_mult` and `player_eye_height` for tuning.

Later systems add their own commands: `talk <ref>`, `attack <ref>`, `enter <door>`, and so on. An unknown command is always an error, never silently ignored.

## Batch: run a script and exit

```
spud.exe --headless --run checks.osc [--width 1920 --height 1080]
```

Runs every line of the script in order, then exits with 0. It exits non-zero, with the failing line in the message, on any error. For example, to check a visual change:

```
cell 5 -2 +3000
wait_loaded
screenshot C:/.../scratch/whiterun.png
state C:/.../scratch/whiterun.json
```

`--bench <file>` is batch mode plus timing capture.

Scripts start in the world as before. `--menu` starts them in the main menu instead, as an interactive run would,
so UI tests can drive it (`ui activate New Game`,...); ` runs the UI test suite this way. Scripts
never read or write the player's settings file unless `--settings <file>` names one.

The HUD replays its recording (the `hud` capture) only in scripted runs: `--run`, `--start`, `--control`, `--bench`
and anything headless. `--hud-recording` and `--no-hud-recording` force either way.

## Start: run a script, then keep the session

```
spud.exe --windowed --start menu.osc [--width 1264 --height 711 --window-pos 1288 31]
```

Runs the script's lines from the first frame on, as `--run` would, then carries on as the session it is: windowed,
the window is the player's (the saved settings are read and written as in any interactive session); with
`--control`, the client's.
- While the script runs, every frame is the fixed 1/60 s step `--run` uses, so its `wait_frames` mean the same
  whatever the display's refresh and the menus' animations advance with them. After its last line the session
  runs on real time.
- Inline results (`state -`) go to stdout. A failing line is logged and drops the rest of the script; the window
  stays the player's and spud exits with 1 when it is closed.
- Headless (`--headless --start`), a failing line ends the session with 1, and a script that ends with `quit` exits
  with 0.
- `--start` doesn't combine with `--run`, `--screenshot` or `--bench`.

`--window-pos <x> <y>` puts the window's client area at that desktop position (windowed only; the window is shown
once it is there).

## Live control: a session that stays up

```
spud.exe --headless --control 47110
```

This listens on `127.0.0.1:47110` (localhost only) for newline-delimited JSON:

```
→ {"id": 1, "line": "cell 5 -2 +3000"}
← {"id": 1, "ok": true}
→ {"id": 2, "line": "state -"}
← {"id": 2, "ok": true, "result": { "camera": { ... }, "cell": [5, -2], ... }}
```

Each request carries one command line, and responses come back in order. A file argument of `-` returns the result inline. This avoids reloading the world for every check during long investigations.

## The player's items

`game <sub>` drives the item systems. `game start` makes the game's state (a New Game or walking
does too; without it the menus replay captures).

| Command | Effect |
|---|---|
| `game additem <form> [n]` | Adds to the player's inventory |
| `game equip <form> [hand]` | The Inventory Menu's selection (0 right, 1 left) |
| `game skill <actor value> <level>` | Sets a skill |
| `game ready <spell>` | The spell in both of the player's hands with the weapon out: casting art and light |
| `game cast <spell> <target>` | A spell's effects land on a reference from the player, with no cast: what they show on it |
| `game open <ref>` | What Activate does on a reference |
| `game barter <npc>`, `game pickpocket <npc>` | The vendor's Barter Menu, or a pickpocket's Container Menu |
| `actor <killer> killmove <victim>:<pa_ event>` | That kill move, past the tree's conditions |
| `game var <form> <name> [value]` | A variable of the scripts on a form, or a number or bool set |
| `game stubs` | The Papyrus natives the game declares and nothing here answers |
| `game refs [base]` | The first references the player could act on here; with a base form, every reference of it and where |
| `game target [ref]`, `game card <form>`, `game player`, `game inventory` | The crosshair's target, an item's card, the player's numbers and things |
| `game ai <space> [high]` | The agents in a WRLD or interior CELL, or only those near the player: package, place, goal, path, detection, activity |
| `game ride <ref> [tamed]\|off` | The Activate key on that horse or, with `tamed`, a dragon as if Bend Will had taken it; `off` is the Activate key in the saddle |
| `game fly <ref> orbit\|hover\|land` | Enables a flier and orders it around the player |
| `game watch [ref\|walking\|using] [units]` | Puts the player that far behind an actor (250; negative is in front), and prints their clips. `--timescale` or `set time_scale` turns activities over faster |
| `game path <space> x y z x y z` | A navmesh path, Skyrim units |
| `game seen on\|off` | Whether actors around see the player, standing in for AI's detection |
| `game bounty [faction gold]`, `game fine <faction> [out]`, `game jail <faction>`, `game serve`, `game steal <ref> <form>` | Bounties, paying, going to jail, serving time, a theft report |
