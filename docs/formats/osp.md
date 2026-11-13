# .osp — physics shapes (kind `PHYS`, version 3)

Layout: [`src/formats/osp.hpp`](../../src/formats/osp.hpp). One file per mesh that has solid collision, next to its render mesh: `meshes/foo/bar.nif` → `meshes/foo/bar.osp` (and `bar.osm`). A mesh without an `.osp` has nothing to collide with.

| Chunk | Contents |
|---|---|
| `INFO` | bounds (min/max, metres), body/shape/vertex/index counts |
| `BODY` | `Body[]` — 16 bytes: mass, friction, restitution, motion (fixed / keyframed / dynamic), layer |
| `SHAP` | `Shape[]` — 80 bytes: type, Skyrim material and layer, body, convex radius, centre, radius, rotation, extents, vertex and index ranges |
| `VERT` | `float[3]` vertices, metres (hull points and mesh triangles) |
| `INDX` | `u32[]` mesh triangles, relative to the shape's `first_vertex` |
| `RANK` | optional (version 2): `u32` per mesh triangle, in `INDX` order — the place Havok's MOPP walk visits the triangle at, `0xFFFFFFFF` where unknown |
| `NODE` | optional (version 3): `BodyNode[]`, one per body — 32 bytes: the node's name (offset into `STRS`), the rigid body's position and rotation (quaternion xyzw) in mesh space |
| `JOIN` | optional (version 3): `Joint[]` — 132 bytes: type, the two bodies, each one's pivot and three axes in mesh space, limits, friction, length |
| `BSXF` | optional (version 3): `u32`, the root's `BSXFlags` |
| `STRS` | with `NODE`: the node names |

## Semantics

- Everything is in **mesh space, metres**, like `.osm`: NIF node transforms, `bhkRigidBodyT` and `bhkTransformShape` transforms and the Havok scale (69.99124 Skyrim units per Havok unit) are baked in. A reference places the whole file with its own transform and uniform scale.
- Shape types:
  - `box`: `centre`, `rotation` (quaternion xyzw), `extents` = half extents.
  - `sphere`: `centre`, `radius`.
  - `capsule`, `cylinder`: the segment from `centre` to `extents` (both points), `radius`.
  - `convex`: the hull of `vertex_count` points from `first_vertex`.
  - `mesh`: `index_count / 3` triangles. Vertices are welded (0.2 mm), so neighbouring triangles share edges, and degenerate triangles are dropped.
- `convex_radius` is Havok's shell around boxes, cylinders, convex hulls and mesh triangles (the NIF shape's `radius`; for meshes the compressed or packed mesh shape's). Havok's collision surface is the shape grown by it, which the character controller uses. Files written before mesh radii were recorded have 0 there for meshes.
- `layer` is the Skyrim collision layer (`SkyrimLayer` in niftools nif.xml: 1 static, 2 anim static, 4 clutter, 9 trees, 13 terrain, 15 non-collidable, 31 stair helper, ...) and `material` the `SkyrimHavokMaterial` ID. They are kept per shape for footsteps, impacts and per-system filtering. Triangle soups are split into one `mesh` shape per (body, layer, material).
- `Body.node`, 0 in most files, is 1 + the `.osk` node of an animated object's body (`skinned/<model>.osp`); the body's shapes are then in that node's space. It takes up what was a reserved field, so no version changed.
- `Body.motion` comes from the Havok rigid body: `keyframed` for animated bodies (motion system keyframed, or the anim-static layer), `dynamic` for bodies with mass, `fixed` otherwise. The engine currently treats every body as fixed.
- `RANK` (version 2) gives each mesh triangle its place in the order Havok collects contacts in: the MOPP code's leaves walked depth first, first child first, each MOPP of a file ranking after the previous one (rank = MOPP number × 2^20 + leaf number). The character controller's contact filter depends on that order. Files without a MOPP have no `RANK` chunk; version 1 files have none.
- `NODE` (version 3): each body's rigid body frame in mesh space and the name of the node whose collision object holds it. Scripts address a model's bodies by node (`Game.AddHavokBallAndSocketConstraint`), and a model of several dynamic bodies is simulated body by body from these frames.
- `JOIN` (version 3): the NIF's constraints between the model's bodies (`bhkBallAndSocketConstraint`, `bhkHingeConstraint`, `bhkLimitedHingeConstraint`, `bhkRagdollConstraint`, `bhkStiffSpringConstraint`, `bhkPrismaticConstraint`), with pivots and axes taken from each body's own space into mesh space. By type: a hinge's axes are the hinge axis then two perpendicular ones (limited hinge: `limits` 0 and 1 the angle range, radians); a ragdoll joint's twist, plane and motor axes (`limits`: cone max, plane min, plane max, twist min, twist max); a prismatic joint's sliding, rotation and plane axes (`limits` 0 and 1 the distance range, metres); a stiff spring's `length` in metres. Motors aren't kept.
- `BSXF` (version 3): `BSXFlags` of the root (2 Havok, 8 complex, 0x80 articulated, ...).
- Not exported: phantoms (`bhkSPCollisionObject`, triggers and bumpers), ragdoll bones (`bhkBlendCollisionObject`), the MOPP code itself (only the order it gives, `RANK`) and welding data (the engine builds its own acceleration structures), constraint motors.
- Destructible meshes contribute the intact stage only, switch nodes their active child. Collision ignores the hidden (`APP_CULLED`) flag, as the game does.

## Source blocks

Read by the importer from SE and LE NIFs (layouts per niftools nif.xml, checked against each block's size):

- `bhkCollisionObject` → `bhkRigidBody` / `bhkRigidBodyT` → shape tree.
- `bhkMoppBvTreeShape` → `bhkCompressedMeshShape` → `bhkCompressedMeshShapeData` (chunks with quantised vertices, strips and triangle lists, big triangles; per-chunk material and layer).
- `bhkListShape`, `bhkTransformShape`, `bhkConvexTransformShape`.
- `bhkBoxShape`, `bhkSphereShape`, `bhkCapsuleShape`, `bhkCylinderShape`, `bhkConvexVerticesShape`.
- LE: `bhkPackedNiTriStripsShape` → `hkPackedNiTriStripsData` (sub shapes give material and layer by vertex range). Vanilla SE data has none; the layout is untested against real files.
- `bhkNiTriStripsShape` → `NiTriStripsData` (Oblivion-era, in Skyrim units; 30 vanilla meshes).
