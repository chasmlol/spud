# Audio

spud plays Skyrim's sounds, music and ambience with Skyrim's own rules: the same files, volumes, attenuation,
positioning, randomisation, reverb, music behaviour and ambience.
This page is how the pieces fit and how to use them.

## Pieces

| Where | What |
|---|---|
| `src/importer/convert/audio_import.cpp` | Reads every audio record of the load order (SNDR, SOUN, SOPM, SNCT, REVB, ASPC, MUSC, MUST, FSTP, FSTS, IPCT, IPDS, MATT, REGN sounds and music, WTHR sounds, WATR, WRLD/LCTN/CELL audio data, sound-emitting references, DOBJ, the audio INI settings) into `<cache>/audio/sound.osd` ([formats/osd.md](formats/osd.md)) |
| `src/importer/convert/audio_codec.cpp` | Sound files: WAV copied, xWMA and .fuz audio decoded and re-encoded as Ogg Vorbis, .fuz/.lip lip data to `.osv` ([formats/osv.md](formats/osv.md)) |
| `src/audio/device.*` | XAudio2 2.9: the game's graph (6-channel mastering voice, 2-channel reverb submix), X3DAudio, reverb, the capture tap |
| `src/audio/sound.*` | One playing sound: volume, pitch, loops and envelopes, 3D and speaker outputs, streaming |
| `src/audio/system.*` | The public API, the audio thread (33 ms passes), categories and fades, the loader thread and clip cache |
| `src/audio/music.*` | The music manager: heap, types, tracks, transitions, ducking |
| `src/audio/ambience.*` | Acoustic spaces and reverb, region sounds, weather sounds, water, the underwater loop, placed emitters, footstep and impact selection |
| `src/audio/hrtf.*`, `src/audio/binaural.*` | Headphone spatial audio: the measured filters, the renderer, and the voice and effect that run it |
| `src/audio/conditions.*` | The condition functions sound and music records use |
| `src/core/game_random.*` | The game's shared random generator, which every audio draw comes from |

Import: `skytool import` imports audio with everything else (`--audio-only` for just audio, `--no-audio`
to skip it). The runtime reads `--audio-cache <dir>`; by default the import cache, or, when only that has the
sounds, `%LOCALAPPDATA%\spud\cache\audio` (the audio-only cache). A
re-import of the records with the files already converted takes seconds.

## Threads

The engine never waits on audio. Calls into `audio::AudioSystem` push commands into a lock-free ring; the audio
thread runs the game's 33 ms pass (commands, fades, 3D, music, ambience, category state), and a loader thread reads
and decodes files into a 256 MB clip cache. Music and voices stream. The main thread only publishes the listener
and the environment once a frame (`App::update_audio`).

## Hooks for other systems

What the rest of the engine calls, and what calls it once those systems exist:

| Call | Caller |
|---|---|
| `set_listener(camera)` | every frame |
| `set_environment(env)` | every frame: world or interior cell, game hour, weather and transition, underwater, water samples |
| `play(descriptor, position)` / `stop` / `set_position` | anything that plays a sound |
| `play_file(path, category, output_model, position)` | **dialogue**: a line's `.ogg` (its lips are the `.osv` beside it) in AudioCategoryVOC (DOBJ 136) with SOMDialogue3DDefault (DOBJ 147) at the speaker, following the speaker with `set_position` |
| `speak(path, position)` | **NPCs**: greetings, chatter and combat lines (not the player's conversation), moved with `set_position`; the speech-highlight rule ducks speakers farther than 250 units by 12 dB while someone nearer speaks |
| `music_add(type)` / `music_remove(type)` | combat (MUSCombat on combat start, remove on end), scripts (`MusicType.Add/Remove`), menus (stats, level up), location cleared, death |
| `footstep(set, gait, tag, material, position, player, first_person)` | **animation**: each footstep annotation (FootLeft, FootRight, FootSprintLeft, JumpUp, JumpDown, ...) with the actor's footstep set (its race/armour's FSTS), the gait, and the ground's material under the foot |
| `collision(material_a, material_b, position, speed)` | **physics**: contact points at or above 60 units/s between two materials |
| `combat_impact(set, material, position, player_attacker, normal_hit)` | **combat**: a weapon hit |
| `impact(set, material, position)` | projectiles, explosions, impact effects |
| `material_from_havok_id(id)` | physics: the MATT whose material ID a Havok shape carries |
| `animation_event(reference, annotation, position, node)` / `move_animation_sounds(reference, position)` | **animation**: every `SoundPlay.<EDID>`, `SoundPlayAt.<EDID>.<node>`, `SoundStop`, `SoundRelease` and `SoundAnimationEnd` annotation or event of a reference's animation (furniture, activators, actors), and the reference's position as it moves |
| `set_menu_mode(open)` / `set_loading_menu(open)` / `set_dialogue(open)` | **UI**: a pausing menu open, the loading screen, the dialogue menu |
| `menu_sliders`, `set_master_volume`, `set_category_volume`, `save_settings` | **UI**: the Audio settings page |

Out of scope here, owned by other workstreams: choosing dialogue lines (dialogue plays the line's `.ogg` through
`play_file` and drives the lips from the `.osv`), and the animation timing of footstep events.

## Settings

The Audio menu's levels persist in `%LOCALAPPDATA%\spud\settings.ini`, section `[audio]`, in percent:

```
[audio]
master = 100
effects = 100
footsteps = 80
voice = 80
music = 50
```

The keys are the game's Audio menu: the master volume and every category flagged "Should Appear on Menu" (vanilla:
AudioCategorySFX, FST, VOCGeneral, MUS; categories added by plugins appear under their lower-cased EditorID).
Defaults are the game's: 100 % master and each category's UNAM. The file belongs to the engine's user settings
(`src/engine/user_settings.*`, docs/UI.md): the audio system reads no file; the engine adds these keys from the
loaded sound categories and passes the levels on (`App::apply_audio_settings`). The Journal's Audio page (System >
Settings) and the F1 panel's sliders edit the same keys. Only interactive sessions read and write the file, or any
session given `--settings <file>`.

## Headphone spatial audio

True HRTF rendering for headphones, an upgrade over the game. It is off by default, and off means the game's graph
exactly: the voices below don't exist. Turn it on with **Headphone Spatial Audio** on the Journal's Audio page or in the
F1 panel (`hrtf` under `[audio]` in settings.ini); it applies to sounds already playing.

"HRTF" in the game's output models only names X3DAudio's speaker panning, which has no height: a sound 45 degrees up
pans exactly like one in front. Here, each positioned sound gets filters measured on a dummy head, which is what gives
direction above, below, in front and behind.

```
source voice --(gain, distance filter)--> mono submix, HRTF effect --> mastering voice FL, FR
             --(reverb send)------------> reverb submix, as ever
```

- **What goes through it:** sounds whose output model uses the game's "HRTF" panning and that have a position. Sounds
  on the listener, defined-speaker output models, music and UI keep the game's mix.
- **The game's rules still apply:** distance curve, distance low-pass, reverb send, volumes and fades are all
  unchanged. The level comes from X3DAudio on a one-speaker layout, so it holds the curve without the panning: on 5.1,
  a sound straight above, below or within 1 m spreads over five speakers and drops 7 dB, which is about speakers.
- **Within 1 m** the sound blends to the centre of the head, as direction stops meaning anything (X3DAudio's inner
  radius does the same on speakers). It keeps its level there, where the game's spread loses some: +1.6 dB at 50 units,
  +3.7 dB at 30, identical from 70 units out.
- **Cost:** about 0.1 % of one core per audible sound, on XAudio2's thread; a filter runs only when there is something
  to hear, so loops out of range cost nothing. The audio thread only sends each sound's direction, every 33 ms pass.
- **The output device:** the result is two channels in FL and FR. On a device set to 5.1 or 7.1 they reach the front
  left and right. Virtual surround (Windows Sonic, Dolby Atmos for Headphones, a headset's 7.1 mode) must be off, or it
  processes the sound a second time.

**The data** is subject D1 (Neumann KU100 dummy head) of the [SADIE II database](https://zenodo.org/records/12092466),
University of York, Apache-2.0 ([licence](../tools/hrtf/LICENSE-SADIE-II.txt)): Armstrong, Thresh, Murphy, Kearney,
Applied Sciences 8(11):2029, 2018. `tools/hrtf/build_hrtf.py` reduces its 9,201 measurements (48 kHz, the full sphere) to
the 306 directions in `src/audio/hrtf_data.inc`, about 9 degrees apart: diffuse-field equalised so timbre stays close to
the game's, minimum phase, 64 taps, with the interaural delay stored apart so directions interpolate cleanly. Each
filter is blended from the four nearest measurements. Other device rates resample the filters once, when the option is
first turned on.

Measured in the engine against the raw database, with a headless capture of one fixed sound per direction, at 48 kHz:

| What | Result |
|---|---|
| Level against the game's mix | -0.1 dB in front, -0.8 dB 45 degrees up or down, +0.2 to +0.8 dB on the diagonals |
| Where the game's panning dips | +2.7 to +4.3 dB behind and at the sides, +6 dB straight up and down: the game's dip is gone, not HRTF being louder |
| Interaural level difference | Equal to the filters' to 0.1 dB |
| Interaural time difference against the raw impulse responses, same source | Within 3 us on the median plane, 44 us at the sides (820 us there). What's left is a single delay standing in for a phase that varies with frequency |
| Height cues: spectrum up, down, front-up, front-down and back, each against front | Equal to the filters' within 0.1 dB up to 10 kHz, within 0.7 dB above |
| A head turned 90 degrees | The sound moves to the correct ear |
| A voice line through SOMDialogue3DDefault at 193, 728, 1411 and 2500 units | Within 0.2 to 0.6 dB of the game's mix at each, falling the same way (-20.5 and -20.6 dB at 2500); the distance low-pass takes the same off the top (-5.07 and -5.02 dB) |
| A real NPC greeting (Carlotta, `game talk`, 2.6 m) | Reaches the filters, with nothing in the centre channel |
| Turning at 300 degrees a second, or snapping 180 degrees, with a sound playing | The sound swings the right way round, and the energy above 17 kHz, a test for clicks, is no higher than holding still (-44 dB bursts) |
| Other device rates | Filters at 44.1 kHz within 0.12 dB of 48 kHz on average; the delay is unchanged |

Bright sounds peak up to 3 dB higher at the near ear than on the game's mix, because an ear really does hear them
louder. Neither path has a limiter.

These measurements show the engine renders the data faithfully, not how convincing it sounds: that depends on the
listener's head, and a generic head can't suit everyone. Front and back confusion is the usual limit.

## Scripts and automation

`audio ...` lines in scripts (`--run`, `--startup`, `--control`; docs/AUTOMATION.md):

| Command | What it does |
|---|---|
| `audio play <sound> [x y z \| here]` | Play an SNDR (EditorID or FormID), at a position in Skyrim units or 2D; the result is the sound's id |
| `audio playrel <sound> dx dy dz` | Play relative to the listener (Skyrim units, world axes) |
| `audio playfile <cache path> <category\|none> <output model\|none> [x y z \| here]` | Play a file without a descriptor, as dialogue does |
| `audio speak <cache path> [x y z \| here]` | Non-dialogue speech at a speaker (speech highlight by distance) |
| `audio stop <id>` | Stop a sound `audio play` started |
| `audio music add\|remove <type>` / `audio music stop` | Music events |
| `audio volume master\|<category> <percent>` | Master or a category's menu volume |
| `audio hrtf on\|off` | Headphone spatial audio for this session, leaving the saved setting alone |
| `audio env hour <h>` / `weather <id\|none>` / `interior <cell\|none>` / `underwater <0\|1>` | What the audio rules see (`interior` lets audio run in an interior cell before interiors render) |
| `audio footstep <set\|default> <gait> <tag> <material> [x y z\|here] [player\|player1st]` | A footstep event |
| `audio impact <set> <material> [x y z\|here]` / `audio collision <ma> <mb> <speed m/s> [pos]` | Impact and collision events |
| `audio menu\|loading\|dialogue <0\|1>` | Menu mode, loading screen, dialogue ducking |
| `audio capture <file.wav> <seconds> [async]` / `audio capture_wait` | Record the 6-channel mix (before the device) to a float WAV |
| `audio state <file\|->` | Audio menu values, active sounds (flags, static, fade and distance attenuation, volume, distance), music heap and track, ambience state, as JSON |
| `audio animevent <form> <annotation> [x y z \| here] [node x y z]` | Send a reference's animation sound annotation (`SoundPlay.OBJSawMillLoad03Drop`), as its animation would |
| `audio seed <n>` | Seed the game's shared generator (clock-seeded at startup, like the game's) for a repeatable run |
| `audio save` | Write the Audio menu values to the settings file (interactive sessions, or `--settings`) |

Headless runs are silent unless `--sound`; captures record either way. `--mute` silences a windowed run.
