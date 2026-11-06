# Rendering principles

## Image clarity: no temporal techniques

spud is an old-school engine underneath its modern GPU-driven speed: **the picture as a whole is never smeared across frames.** No TAA softness, no ghosting trails behind moving objects, no image that only sharpens once you stop moving. This is part of the engine's identity, not a setting.

**The rule: the final image is never built from previous final images.** Individual effects may use temporal techniques internally (see below), but nothing blends, reprojects or reconstructs the whole frame from earlier frames.

### Banned

Not as defaults, not as player options:

- TAA in any form, and TAA-based upscalers (DLSS Super Resolution, FSR 2 and later, XeSS, TSR).
- Frame generation and interpolation.
- Any full-screen history resolve: reprojecting or blending the previous final image into the current one.
- Sub-pixel camera jitter, which only exists to feed TAA.
- Checkerboard or interleaved rendering of the whole frame.

Watch for hidden reliance on TAA. Many techniques from modern references assume a TAA pass will clean them up afterwards: noisy few-sample SSAO, dithered LOD fades, stochastic transparency. We don't have that pass, so their noise or patterns would show. Either pick a technique that is clean on its own, or give the effect its own temporal filtering (allowed, see below).

### Allowed: temporal techniques inside individual effects

No approval needed. Effects can keep state from frame to frame when that makes them look or perform better:

| Kind | Examples |
|---|---|
| Simulation state | Water ripples and waves, rain splashes, snow deformation, particle state |
| Effect-local accumulation | Volumetric fog/clouds, AO, reflections or shadows filtered over a few frames inside their own buffers |
| Eye adaptation | Skyrim's HDR exposure easing when you leave a cave |
| Caches | Shadow maps of static geometry redrawn only when invalidated |

Conditions:

- **Contained.** The state lives in the effect's own resources, never in the final colour target, and the effect's output is composited into a freshly rendered frame.
- **Declared.** Each such GPU resource is created as `Lifetime::simulation` or `Lifetime::cache` and listed in the cross-frame allowlist in `src/render/device.cpp`, with a line in the table below. The list exists so every piece of cross-frame state is visible, not as a gate. An unlisted one throws at startup.
- **Settles.** With the camera and world still, the effect converges and stops changing. No permanent ghosting.
- **Testable.** In scripted runs the effect is deterministic (fixed timestep, fixed seed) so the same pose always renders the same.

| Allowlisted resource | Kind | Notes |
|---|---|---|
| `eye adaptation` | Eye adaptation | The two 2x2 adapted-luminance targets of the HDR chain: each frame moves at least 1/256 towards the scene's luminance and never past it, so it settles exactly when the scene is still; reset on a forced weather change. |
| `precipitation occlusion` | Cache | The offscreen target of the precipitation occlusion map: only its depth is read, in the frame that draws it. |
| `skin decals` | Actors | Skinned blood: each decal's triangles, as indices into its actor's posed vertices and the decal's coordinates. Made once per hit, gone after 31 s; it holds geometry, not an image |
| *focus weight* (CPU state, no GPU resource, so not in the allowlist) | Eye model | `Sky::m_dof_weight`, the depth of field's centre weight: falls when the camera turns or moves, eases back over 1.5 s of stillness. One float, settled by then |
| `water history` | Simulation state | The water's own image: water pixels only, a ping-pong pair, blended per pixel towards the last image, reprojected by camera. Off with the water's clock stopped and under `hold_effect_state`, so a still pose is exact. |
| `water reflection` | Cache | The reflection cube: two faces a frame, stopped under water; settles three frames after the camera stops. |
| `water flow` | Cache | The flow maps of the cells round the camera: static data, rewritten when a cell comes in. |
| `water ripples` | Simulation state | The wading ripples: 40 Hz, clear after 10 s without a ripple. |
| `local map` | Cache | The Map Menu's local map: redrawn every frame the local map is shown, sampled by the UI in the same frame; while the movie fades the local map out it shows the last picture. Never part of the scene's colour target. |

### Anti-aliasing, the non-temporal way

- **MSAA** (4x default) for geometry edges.
- **`centroid` on what a shader can't afford to extrapolate** (colours, blend weights, fog, normals, positions; not texture coordinates, whose derivatives pick the mip). Under MSAA a triangle covering a sample but not the pixel's centre is otherwise shaded outside itself: on a sliver of land one pixel reached 248 and bloom made a white ball of it, in a frame's time, wherever land or objects changed level. Lit colour is also floored at 0, since a negative slips past the composite's limit.
- **Alpha-to-coverage** for alpha-tested foliage, so trees and grass don't shimmer. Lighting materials take it from the edge only: above the threshold they are solid, as the game's test is, or a texture with low alpha shows the scene through (the Vampire Lord's body). It is sub-pixel coverage that's identical every frame, so it isn't dithered transparency.
- **Specular anti-aliasing** from normal variance (Toksvig-style) and correct mipmaps, so shiny and detailed surfaces don't sparkle. It conserves energy: the highlight it widens is dimmed by (n' + 8) / (n + 8), or rough water turns into a white sheen.
- Optional spatial post-AA (SMAA 1x) only if it proves necessary. Spatial only.

MSAA counts against the frame budget like any other feature.

### Determinism

The same world, settings and camera pose must give the **same image, bit for bit**, however the camera got there (allowlisted effect state aside, once it has settled). In practice:

- What is drawn (cells, objects, terrain LOD) is chosen from the current camera only. Streaming may keep extra data resident (hysteresis) and may plan a little wider than it draws, but resident-but-out-of-range data is never drawn.
- Draw submission order never depends on load order or memory slots. Overlapping and blended surfaces must resolve the same way every time.
- LOD transitions are decided per frame from the current view. Geometry LOD doesn't fade across frames: it switches where the change is below a pixel.
