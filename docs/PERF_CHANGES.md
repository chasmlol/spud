# Overnight performance changes

Every change on this branch is one commit that `git revert <commit>` undoes alone. Nothing here is merged: pick what you want.

Measured with `scripts\perf.cmd --tour`, 12 sections of the real game. Run-to-run noise is about 10%.

## Where it ended

All from clean builds with sccache out (`eb1e3bf`, the commit from `main`), three tours each:

| Tour | Before (`c733f4b`) | Now (`eb4e78e`) |
|---|---|---|
| Average fps | 261-266 | **372-377** |
| Frames over 3.33 ms | 23,400-24,031 | **3,442-3,819** |
| GPU mean | 1.71-1.79 ms | 1.74-1.75 ms |
| Late ticks per run | 0-1 | 0-1, and 41 once in an earlier set (a stall, see below) |

Please check first, as none of this has run in a real window: resize, minimise, alt-tab, vsync, and going in and out of buildings.

## Why tonight's baseline isn't the 502-524 fps in the brief

The tour fell to about 266 fps because a bare present got slower: `calibration` takes 0.72-0.80 ms, where all 72 stored results from 4 and 5 November took 0.17 ms.

**It's the machine, not the code.** The code from 5 November (`253814a9`), rebuilt and run tonight, takes 0.72 ms. So do builds from 6 November at 12:10 and 17:50, and this branch.

**When:** after the 07:03 tour on 6 November (524 fps, which a slow present couldn't reach) and before 21:30. There was no reboot in that time. Note that running an old build says nothing about when the machine changed.

**How it's slow.** A `DxgKrnl` trace shows every headless present is `D3DKMT_PM_REDIRECTED_BLT`: a real copy through the compositor, not a flip. Whether it was always a blit isn't known, as no trace from a good day exists.

| Checked, read-only | Result |
|---|---|
| Vulkan layers (OBS, RTSS, `VK_LAYER_NV_present`, Optimus, AMD), each off for one process, confirmed gone in the loader's log | No change: 0.82 ms |
| NVIDIA settings: present method, vsync, low latency, frame limit, power, in the global profile and for `spud.exe` and `opensky.exe` | None set. The IDs were from memory, so this isn't airtight |
| More swapchain images, or presenting from a second queue | No help, or worse |
| Defender during tours | 1.8% of one core |
| Remote desktop | No: console session |
| The Claude app | Its GPU process presents 360 times a second, the display's refresh, and 765 render packets a second, with this session busy or idle. **Unproven either way:** it didn't go quiet, so there was no clean run to compare |

Changes inside the time window, none shown to matter: 27 driver resets (`nvlddmkm` 153); Claude app restarted 12:57 (its binary dates from 3 November); Steam started 13:00, rewriting the Vulkan layer key at 13:01; NVIDIA's settings database written 18:12; GameDVR key 19:02; Settings opened 19:41. Also three `stornvme` 129 resets of a disk.

### What to try, cheapest first

Each ends with `scripts\perf.cmd --scenario calibration`, looking for 0.17 ms:

1. **Quit the Claude app and any other GPU apps**, then run it from a plain terminal. This tests the top suspect directly.
2. **Reboot.** Nothing has cleared the driver since those 27 resets, and a reset might leave it on a slower path.
3. **`scripts\perf.cmd --tour --windowed`.** A shown window can flip, so play may not have this cost at all. If windowed is much faster, the 372 fps here understates the game, and the slowdown only affects headless measuring.
4. Unplug or disable the virtual displays (SudoMaker, Virtual Desktop) for one run.
5. In NVIDIA's control panel, see what changed at 18:12.

## The device loss: three causes found and fixed

There were three separate bugs. All were tested with the fix in and out, on clean builds, except that the first was measured before the clean builds.

| Cause | Commit | Without it | With it |
|---|---|---|---|
| Actors behind a door posed between ticks, reading what was never uploaded | `c733f4b` | 2 of 6 passes through the Bannered Mare door lost the device | 12 of 12 clean |
| Skinning ran twice with a door in view, so a tick could overwrite rows the GPU was reading | `bce725a` | 2 of 2 door stress runs lost it, after 32 and 2 crossings, with only this commit reverted | 92 and 39 crossings on the same route, no loss |
| A device wait raced the upload thread's submit, the old "write at address 0" crash | `8fc6111` | 5 of 27 audits lost it, `main` itself 2 of 14 | 25 of 25 audits clean |

The first two are what #50 describes. The third isn't new, and comes from a device wait during a big load, such as a resolution change. So I expect these crashes to be much rarer, but not gone: the last two mechanisms are inferred from fix-in and fix-out runs, and the machine's other resets weren't all accounted for. If it happens again, the log's `gpu:` lines are what's needed.

## Changes

| Commit | What it does | Gain | Visual effect |
|---|---|---|---|
| `7d6cb64` | Tour: `set player_invulnerable 1`. Arriving in the dungeon after the flight took 100 health as a fall, which killed the player and ended every full tour at the Main Menu, so the tour couldn't finish on `main`. | The tour finishes. No fps change. | None in play: a script setting only |
| `c733f4b` | **Fixes the first cause of the GPU device loss behind #50.** A space behind a door stepped its actors on every frame, so skinning read bones and face morphs that were never uploaded. | Before: 2 of 6 passes through the Bannered Mare door lost the device. After: 12 of 12 clean, then 3 full tours clean. | NPCs seen through a door are now posed once per tick. First-person, NPC and doorway shots look right. May also help #25, not checked |
| `1c65a8a` | **Presents from its own thread.** `vkQueuePresentKHR` blocked the main thread, so the GPU ran out of work. A thread now presents and takes the next image; the main thread needs it only at the final blit. | Tour **266-268 to 359-360 fps**, frames over 3.33 ms about 24,000 to 5,200, late ticks 0 in all three runs. GPU time unchanged. | None. **Not tested in a real window:** resize, minimise, alt-tab and vsync. Please check those first. |
| `2f745c0` | **Presents once the frame is drawn.** The driver holds a lock through a present until the frame is finished, and the main thread stopped at it in `vkCmdEndRendering`. The present thread now waits for the frame's fence first. Revert this before `1c65a8a`. | Tour **359-360 to 371-373 fps**, frames over about 5,200 to 4,250, p99 4.44 to 4.17 ms, 0 late ticks. A present 2.2 to 0.76 ms, the main thread's wait 1.4 to 0.05 ms. | None |
| `912f916` | **Third person starts at the eyes (#13).** The first frames after the first switch rendered from the world's origin, which is the view from across the map. | Bug fix. Not measured for fps. The freeze in #13 didn't reproduce with a warm file cache (first switch 11.3 ms, second 4.7 ms; first talk 13 ms, #9), and a cold disk wasn't tested. The E prompt wasn't tested. | For up to one tick after switching, the view stays at the eyes. |
| `bce725a` | **Skins once a frame: the second cause of the device loss.** With a door in view a tick took two of the three sets of bone rows, and the next tick could write over a set the GPU was still reading. | Tour frames over about 4,250 to 3,750, `door` section 551 to about 340, 372-373 fps, 0 late ticks. | None seen: door, NPC and first-person shots look right |
| `25f3526` | Arriving actors were listed twice, so two workers grounded the same actor at once. Now once. | **None:** `arrivals` 14.5 ms before, 14.8 ms after. It removes a data race, nothing more. | None |
| `94d364e` | Uploads skip empty copies, which validation called invalid. | None: it's cleanup. It was tried for the cold start crash and didn't fix it. | None |
| `8fc6111` | **Third cause of the device loss:** device waits now hold the upload queue too. | See the table above. | None |
| `eb1e3bf` | `main`'s commit taking sccache out, cherry-picked. Not a performance change. | Builds are real. A clean build takes about 2 minutes. | None |
| `bb66ee1` | **Loading screens no longer hang for 30 s.** A world's effects were only taken in by `WorldFx::update`, which doesn't run behind a loading screen, so the screen waited for itself until its timeout. | 60 random travels: 14 took 30 s, 474 s in all. After: none over 25 s in 360, median 0.5 s. | None |
| `eb4e78e` | **Crash on travel to another world.** The water system spotted a new world by address alone, and a new world can land at the old one's. Its cells then indexed flow maps that aren't there. | One crash in 54 travels, at exactly flow map 358 of a null array. After: 360 of 360 clean. One crash isn't many, so that's good evidence, not proof. | None |
| `9783922` | Retired interiors wait for their own uploads before they are freed. They skip the drain a normal teardown does, and town doors preload interiors, so the oldest can still be uploading. | Found by reading the code, not from a crash, so no measured gain. Door stress: 82 crossings, past the 12 interior limit on 27, no loss. | None |

## What MSAA 4x costs

Measured only, it stays on. One tour each with a temporary override, not committed:

| Samples | GPU mean | fps | Frames over 3.33 ms |
|---|---|---|---|
| 4x | 1.75 ms | 372 | 4,094 |
| 2x | 1.61 ms | 391 | 3,213 |
| 1x | 1.50 ms | 410 | 2,201 |

So 4x costs about 0.25 ms of GPU, 14%, and about 10% of fps. Forest 2.59 ms against 2.22. Most of it is `composite`, 0.08 ms, then `opaque` 0.04, `sky` 0.04 and `image_space` 0.04.

## Not built, and why

These two need your decision. Both are measured.

**A visibility buffer on forest.** Two temporary experiments with the real shaders:

| Forest, `opaque` + `alpha_test` | ms |
|---|---|
| Today | 1.13 |
| Geometry and alpha test only, no lighting (a screenshot confirmed it) | 0.60 |
| Extra cost of lighting only what's finally visible | 0.12 |

So about three quarters of the lighting in forest goes on fragments that get covered, and the best a visibility buffer could reach is about 0.72 ms, a saving near 0.4 ms. It won't reach that: its resolve loses hardware interpolation and the specialised shaders, which were worth 1.37 to 1.01 ms when they came in, and 4x MSAA adds edge samples. My guess at the real saving is 0 to 0.2 ms, in forest only, for a rewrite covering every material variant. Drawing twice instead, which needs none of that, is a clear loss: 2.96 ms against 2.55 for the whole frame.

**The rest of the tick off the main thread.** Tick frames average about 2.85 ms against 2.68 ms for all frames, and about 1 frame in 6 is a tick frame, so the ceiling is about 1% of average fps. The main thread spends about half of each frame waiting for the GPU. It would shorten spikes like `arrivals`, but it means making AI, Papyrus and quests safe against the renderer, and tonight's crashes were races of just that kind.

## What's left in the frame

Frames now take about the GPU's time plus the 0.76 ms present. So the biggest gain left is finding out why that cost went from 0.17 ms. Most frames over 3.33 ms are forest and flight, where the two add up past the line.

About 25 frames in a tour of 50,000 go over 8 ms, each named by `SPUD_TRACE_SLOW=8`:

| Spike | Size | Note |
|---|---|---|
| `space update` on arriving after travel, with `roster` | up to 25 ms | t=114 in `night` |
| `ai` > `arrivals`, grounding actors after a journey | up to 15 ms | Parallel already. Probably first-use navmesh decoding, not measured |
| `papyrus` > `quests` | 7-12 ms | About 9 a tour |
| The UI scripts' full garbage collection | about 12 ms | About every 18 s. Incremental collection needs write barriers in the script VM |
| `greetings`, first use | 10 ms | Once |

## Tried and dropped

- **More swapchain images** (5 and 8): no change. The engine did get them: the log shows 8.
- **A spare image taken ahead**, so the main thread never waits at the blit: 469 fps on the dungeon part against 460, but frames came in short and long pairs, and frames over 3.33 ms went from about 7 to 2,340. Dropped: it's worse on what the tour measures. What it showed is that the GPU sits idle for about the present's 0.76 ms even then, so that cost looks unhideable from here, and only getting the 0.17 ms floor back would remove it.
- **Presenting from a second queue:** the graphics family has 16 queues that can present. On the dungeon part, 434 fps with the fence wait and 412 without, against 460 on the main queue, and `calibration` went from 0.76 to 0.99 ms. Dropped.

## Found, not changed

- **Stale builds from sccache:** fixed on `main` in `55d9fd3`. Numbers measured here before that were all re-measured on clean builds. `-DSCCACHE=SCCACHE-NOTFOUND`, which I suggested, never disabled it. A branch still needs that commit.
- **`travel` keeps a fall in progress.** It moves the player through `Character::set_position`, which keeps the fall start, where `place()` clears it. That's the likely reason for the fatal landing above, though only the 100 damage itself was measured. Whether the game clears it on a loading screen needs checking in the game.
- **Long stalls, still unexplained:** three tours of about 30 had them, up to 222 ms with 41 late ticks, in the main thread's `streaming` and `update` zones. The worst sat at t=79-86 in `flight`, the same place as the brief's 197 ms. Not explained by a cold file cache: a tour with most of the cache evicted had none, worst frame 41 ms. Ten sampled tours in a row didn't catch one either. They aren't from the present thread as far as I can tell, as the 197 ms stall predates it.
- **A zero-size `vkCmdCopyBuffer`**: validation reports `VUID-VkBufferCopy-size-01988` twice in the tour's first part. **`vkDestroyDevice`** also reports a pipeline and a layout left over at exit, not checked against the old build.
- **First visits are bound by the disk, not the engine's workers.** Warm, a travel takes about 0.3-1.7 s. With the file cache evicted, 8 workers took 17.5 and 41.3 s for the same 8 travels and 14 workers 17.2 and 18.9 s, so more workers don't help, and the limit of half the cores stays. Cold first visits usually take 4-5.5 s. Two were far slower and aren't explained: 17 s once, and a 27.6 s Tamriel load once.
- **`C:\spudold`** is a temporary export of old code, left behind. A safety check stopped me deleting it, so please do.
- **Defender isn't the cause of the slow present:** 1.8% of one core during three tours.
- **The control port sends plain text as `result`** (`"result": started`), which isn't valid JSON.
- **#9 and #13's freeze didn't reproduce**, warm or cold. With `forms.osf` and the actor data evicted, the first third-person switch took 4 ms and the first talk 36 ms, though another job may have warmed some of it again first. Those runs use scripts, not the real menu in a real window, so neither issue should be closed on this.
- **A space never entered has no time step**, so its actors don't step or pose when seen through a door until the player has been inside. Not looked at.
