# Overnight performance changes

Every change on this branch is one commit that `git revert <commit>` undoes alone. Nothing here is merged: pick what you want.

Measured with `scripts\perf.cmd --tour`, 12 sections of the real game. Run-to-run noise is about 10%.

## Read this first: tonight's baseline is not the 502-524 fps in the brief

Three tours on 2026-11-06 at about 22:20, with both fixes below: **266-268 fps, about 24,000 frames over 3.33 ms, GPU mean 1.71 ms.** Compare changes with these numbers, not with the earlier ones.

- The GPU is no slower. The main thread is blocked in `vkQueuePresentKHR`, inside `win32u.dll`: 61% of its samples in the tour's first part. The `record` zone includes present, which is why it shows 3.15 ms against 0.64 ms in the 07:03 report.
- `calibration`, a clear and present only, takes 0.74 ms tonight. It took 0.17 ms in every one of the 72 stored results from 4 and 5 November.
- It isn't tonight's code: `opensky.exe` from 17:50 gets 376 fps on the dungeon part tonight, Spud 374. The 07:03 report had 724.
- Cause not found. No stored result falls between 5 November 05:52 and tonight, apart from that 07:03 tour. Nothing in the Windows Update log looks related.

## Changes

| Commit | What it does | Gain | Visual effect |
|---|---|---|---|
| `7d6cb64` | Tour: `set player_invulnerable 1`. Arriving in the dungeon after the flight took 100 health as a fall, which killed the player and ended every full tour at the Main Menu, so the tour couldn't finish on `main`. | The tour finishes. No fps change. | None in play: a script setting only |
| `c733f4b` | **Fixes the GPU device loss, which is also #50.** A space behind a door stepped its actors on every frame, so skinning read bones and face morphs that were never uploaded. | Before: 2 of 6 passes through the Bannered Mare door lost the device. After: 12 of 12 clean, then 3 full tours clean. | NPCs seen through a door are now posed once per tick. First-person, NPC and doorway shots look right. May also help #25, not checked |
| `1c65a8a` | **Presents from its own thread.** `vkQueuePresentKHR` blocked the main thread, so the GPU ran out of work. A thread now presents and takes the next image; the main thread needs it only at the final blit. | Tour **266-268 to 359-360 fps**, frames over 3.33 ms about 24,000 to 5,200, late ticks 0 in all three runs. GPU time unchanged. | None. **Not tested in a real window:** resize, minimise, alt-tab and vsync. Please check those first. |

## Found, not changed

- **Header edits don't rebuild in worktrees.** With the shared sccache from `b3505c6`, a cache hit replays the first checkout's include paths into Ninja's dependencies, so they point at `C:\spud\src`. Seen in this worktree and in `combat`, `doors`, `gameplay` and `player`. An edited `renderer.hpp` left stale objects here, and the exe crashed at startup. This branch was built with `-DSCCACHE=SCCACHE-NOTFOUND`, which took 2 min 51 s from clean. Fixing it properly means giving up sharing the cache between checkouts, so that's your call.
- **`travel` keeps a fall in progress.** It moves the player through `Character::set_position`, which keeps the fall start, where `place()` clears it. That's the likely reason for the fatal landing above, though only the 100 damage itself was measured. Whether the game clears it on a loading screen needs checking in the game.
- **One tour in eight on the present thread had stalls of 60-168 ms**, all in the main thread's `streaming` and `update` zones, 77 late ticks. Not seen in the other seven or the three baseline tours. Cause not found.
- **A zero-size `vkCmdCopyBuffer`**: validation reports `VUID-VkBufferCopy-size-01988` twice in the tour's first part. **`vkDestroyDevice`** also reports a pipeline and a layout left over at exit, not checked against the old build.
- **A space never entered has no time step**, so its actors don't step or pose when seen through a door until the player has been inside. Not looked at.
