#pragma once

// A Space is one loaded .osw worldspace: its cell grid, the streamed set of
// cells and their placed references, terrain and water, and the environment
// it is lit with. The engine owns spaces; several can be alive at once
// (portals later), and each frame submits (space, view transform) pairs.
//
// References go through a Model -> mesh-LOD lookup, so LOD chains / HLOD
// proxies from the importer slot in without touching the streaming code.

#include "engine/actors.hpp"
#include "engine/grass.hpp"
#include "engine/rotation.hpp"
#include "engine/space_rooms.hpp"

#include "formats/osh.hpp"
#include "formats/osw.hpp"
#include "formats/ots.hpp"
#include "render/far_trees.hpp"
#include "render/instance_table.hpp"
#include "render/light_grid.hpp"
#include "render/renderer.hpp"
#include "render/shadows.hpp"
#include "render/terrain.hpp"
#include "render/view.hpp"

#include <functional>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <span>
#include <vector>

namespace spud::engine {

struct SpaceSettings {
    float lod_pixels = 3.0f; // how far a simplified level may stray on screen before a finer one draws
    float draw_distance = 12.0f; // cells (radius)
    float unload_margin = 2.0f;  // hysteresis in cells
    bool show_disabled = false;
    int grass_cell_radius = 2;  // iGrassCellRadius:Grass
    float fade_seconds = -1.0f; // this frame's play for what fades in with time; below 0, scripted: no fade
    RotationMode rotation_mode = RotationMode::stored;
    double load_budget_ms = 0.25; // main-thread time for cell loads/unloads per frame
    // References whose bounding sphere projects to less than this radius (pixels) are not drawn:
    // they would cover at most a pixel or two, so the image doesn't change. 0 = off.
    float min_pixel_radius = 0.75f;
    float tree_near = 200.0f; // metres: past this a tree with an impostor draws as it; 0: never
    bool tight_pixel_radius = true; // debug: off tests the enclosing sphere instead
    float pixels_per_radian = 1131.0f; // set per frame from the view (render height / (2 tan(fov/2)))
    bool animate_lights = true;        // flicker and pulse (off: lights keep their created dimmer and position)
    bool room_culling = true;          // interiors: draw only what the rooms and portals seen show (debug: off)
    bool occlusion = true;             // occlusion planes hide what is wholly behind them (debug: off)
};

struct SpaceStats {
    uint32_t cells_total = 0;
    uint32_t cells_loaded = 0;
    uint32_t cells_wanted = 0; // within draw distance, not loaded yet
    uint32_t refs_loaded = 0;
    uint32_t refs_ready = 0;   // have an instance slot
    uint32_t models_loaded = 0;
    uint32_t models_pending = 0;
    uint32_t drawn = 0;        // draw items last frame
    uint32_t instance_slots = 0;
};

class Space {
public:
    // Throws on missing/invalid files.
    Space(render::Renderer& renderer, const std::filesystem::path& cache_root, const std::string& edid);
    // An already read space, built off the render thread: its instance table has a slot per reference and `spare`
    // more. Any thread; call finish() on the render thread before anything else.
    Space(render::Renderer& renderer, std::shared_ptr<osw::World> world, uint32_t spare);
    void finish();
    ~Space();
    // The file of a space: worlds/<name>.osw, else interiors/<name>.osw. Throws when neither exists.
    static std::filesystem::path find_file(const std::filesystem::path& cache_root, const std::string& name);
    static std::shared_ptr<osw::World> read_world(const std::filesystem::path& file);
    bool interior() const { return (m_world->flags & osw::world_interior) != 0; }
    // Unloads everything. Destroying the space from k_frames_in_flight frames later on then needs no GPU wait:
    // only interiors, whose terrain has no work in flight.
    void retire();
    // Draws a reference or not (an open door hides its mesh); kept across cell loads.
    void set_ref_hidden(uint32_t form_id, bool hidden);
    void set_refs_hidden(const std::unordered_map<uint32_t, bool>& changes); // many at once: one pass
    // Draws a reference where its body has gone; kept across cell loads.
    void move_ref(uint32_t form_id, const glm::vec3& position, const glm::quat& rotation);
    // A reference's enable state as the game has it, against its record's; the table covers what loads later.
    void set_ref_disabled(uint32_t form_id, bool disabled);
    void set_ref_overrides(const std::unordered_map<uint32_t, bool>* overrides);
    bool ref_loaded(uint32_t form_id) const;
    std::vector<uint32_t> loaded_refs() const;
    // A cell's references came or went (its FormID): the world simulation's timers.
    std::function<void(uint32_t cell, bool loaded)> on_cell;
    // The drawn references of the loaded cells whose bounding sphere the ray reaches within `distance` (metres).
    void refs_on_ray(const glm::vec3& from, const glm::vec3& direction, float distance,
                     std::vector<const osw::Ref*>& out) const;
    // Starts this space's actors (engine/actors.hpp). Main thread.
    void attach_actors(ActorData& data);
    ActorSet* actors() { return m_actors.get(); }
    // Nothing farther than this is drawn (metres; 0 = no limit): an interior's fog clip distance
    void set_view_distance(float metres) { m_view_distance = metres; }
    // Rooms and portals: the room the last build_draws camera was in (-1: none), and how many rooms it saw.
    int32_t camera_room() const { return m_rooms ? m_rooms->camera_room() : -1; }
    uint32_t rooms_visible() const { return m_rooms ? m_rooms->rooms_visible() : 0; }
    bool rooms_outside() const { return !m_rooms || m_rooms->outside_visible(); }
    // The lighting to show: `from` blended towards `to` by `weight` while the camera changes rooms; advances the
    // transition by `dt` seconds.
    struct RoomBlend {
        const osw::CellLighting* from;
        const osw::CellLighting* to;
        float weight;
        float sunlight_scale; // the image space's: it doesn't always follow the two
        uint32_t from_image_space, to_image_space; // the rooms' own (XRMR), 0 for the cell's
    };
    RoomBlend room_blend(float dt);
    Space(const Space&) = delete;
    Space& operator=(const Space&) = delete;

    const osw::World& world() const { return *m_world; }
    std::shared_ptr<const osw::World> world_ptr() const { return m_world; } // shared with physics
    const std::string& edid() const { return m_world->edid; }
    float cell_size() const { return m_world->cell_size; }

    // Main thread, once per frame: streaming within budget.
    void update(const glm::vec3& camera, const glm::vec3& forward, const SpaceSettings& settings,
                const render::TerrainSettings& terrain);
    // Culls the loaded references against the view and fills the draw list.
    void build_draws(const render::View& view, const SpaceSettings& settings) { build_draws(view, settings, m_draws, true); }
    // The view through a door that leads back into this space, with its own draw list.
    render::SpaceView portal_space_view(const render::View& view, const SpaceSettings& settings);
    // The shadow casters of each sun cascade: the references whose bound reaches the
    // cascade's light-space box, culled like build_draws otherwise.
    void build_shadow_draws(const render::View& view, const render::SunShadows& sun,
                            const render::ShadowSettings& shadows, const SpaceSettings& settings);
    const render::DrawList& shadow_draws(uint32_t cascade) const { return m_shadow_draws[cascade]; }
    // This frame's shadow lights: in world order the first that pass the view and distance
    // tests, as many as the mask's channels `sun` leaves, with their casters.
    void build_shadow_lights(const render::View& view, const render::SunShadows& sun,
                             const render::ShadowSettings& shadows, float lod_adjust, bool interior,
                             const SpaceSettings& settings);
    std::span<const render::ShadowLight> shadow_lights() const { return m_shadow_lights; }
    // A door within this space: its far frame's shadows are built between these two, beside the main frame's.
    void begin_portal_shadows();
    void end_portal_shadows();
    const render::DrawList& portal_shadow_draws(uint32_t cascade) const { return m_portal_shadow_draws[cascade]; }
    std::span<const render::ShadowLight> portal_shadow_lights() const { return m_portal_shadow_lights; }
    const render::DrawList& portal_shadow_light_draws(uint32_t light) const { return m_portal_shadow_light_draws[light]; }
    std::span<const render::PointLight> portal_point_lights() const { return m_portal_point_lights; }
    const render::DrawList& shadow_light_draws(uint32_t light) const { return m_shadow_light_draws[light]; }
    // Once per frame after update: the placed lights' state. `time` is the
    // frame's time in seconds; lights near the camera animate by the time since the previous call.
    void update_lights(const glm::vec3& camera, float time, const SpaceSettings& settings);
    // Weathers whose Effect Lighting colours lights (region emittance), and that colour for the current hour.
    const std::vector<uint32_t>& emittance_weathers() const { return m_emittance_weathers; }
    void set_emittance(uint32_t weather, const glm::vec3& colour);
    const std::vector<render::PointLight>& point_lights() const { return m_point_lights; }
    // What moves through the world this frame, in the slots after World::lights (render::k_moving_lights at most).
    void set_moving_lights(std::span<const render::PointLight> lights);
    // The lights reaching a sphere, as an instance's list.
    void light_list(const glm::vec3& centre, float radius, uint32_t out[4]) const { m_light_grid->list(centre, radius, false, out); }
    // A mesh no reference places, drawn at a position (metres) with a heading (radians): render-path tests.
    void spawn(const std::string& mesh, const glm::vec3& position, float heading, uint32_t id = 0);
    // ... turned and scaled as a reference is.
    void spawn(const std::string& mesh, const glm::vec3& position, const glm::quat& rotation, float scale, uint32_t id);
    // Takes a spawned object out again, by the id it was spawned with.
    void despawn(uint32_t id);
    // Moves a spawned object: `rotation` holds its scale.
    void move_spawned(uint32_t id, const glm::vec3& position, const glm::mat3& rotation);
    void set_spawned_hidden(uint32_t id, bool hidden);
    // Its mesh has loaded, or never will.
    bool spawned_ready(uint32_t id) const;
    // Sky statics: every base any weather lists, once; then which ones the current and outgoing
    // weather show and their colour (Sky Statics).
    void set_sky_static_bases(std::span<const uint32_t> bases);
    // Alternate textures by base object; the map outlives the space.
    void set_texture_swaps(const std::unordered_map<uint32_t, ots::Swap>* swaps) { m_texture_swaps = swaps; }
    void show_sky_statics(std::span<const uint32_t> current, std::span<const uint32_t> outgoing,
                          const glm::vec3& colour);
    // The weather's sky model (aurora), centred on `camera`, at `fade`; "" removes it.
    void set_sky_model(const std::string& mesh, const glm::vec3& camera, float fade);
    // Rain's ripple mesh over each given water cell, at a strength of 0 to 1.
    void set_rain_ripples(std::span<const glm::vec3> positions, float strength);
    // One-shot effects' mesh shapes (engine/world_fx.hpp): drawn by the next build_draws.
    struct FxShape {
        std::string model;
        uint32_t submesh;
        glm::mat3 rotation;
        glm::vec3 position;
        glm::vec4 base_color;
        glm::vec4 uv;
        bool animated = false;
    };
    void set_fx_shapes(std::span<const FxShape> shapes);
    // Shapes of doors that are moving, this frame: a submesh of a model at a pose.
    struct DoorShape {
        std::string model;  // the .osm
        uint32_t submesh;
        glm::mat3 rotation; // with scale
        glm::vec3 position; // metres
        uint32_t leaf = 0;  // a leaf past its doorway: index + 1 into the door lights
    };
    void set_door_shapes(std::span<const DoorShape> shapes);
    // A door hung from the other side of its doorway (App::turn_doors): its shapes while it stands shut, turned about
    // `axis`, a point on the doorway's upright. Its reference isn't drawn.
    struct TurnedDoor {
        uint32_t form_id = 0;
        glm::vec3 axis{0.0f};
        std::vector<DoorShape> shapes;
    };
    void set_turned_doors(std::vector<TurnedDoor> doors) {
        m_turned_doors = std::move(doors);
        m_doors_turned = true;
    }
    bool doors_turned() const { return m_doors_turned; }
    std::span<const TurnedDoor> turned_doors() const { return m_turned_doors; }
    const TurnedDoor* turned_door(uint32_t form_id) const {
        for (const TurnedDoor& d : m_turned_doors) {
            if (d.form_id == form_id) {
                return &d;
            }
        }
        return nullptr;
    }
    void set_door_lights(std::span<const render::DoorLight> lights) { m_door_lights.assign(lights.begin(), lights.end()); }
    // The references of the loaded cells in the cell rectangle [x0, x1] x [y0, y1] that `view` sees, into `out`
    // (a second view: the local map); no pixel-size culling. With `lo` / `hi`, also those cells' bounds, terrain
    // included (metres).
    void build_area_draws(const render::View& view, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                          render::DrawList& out) const;
    bool area_bounds(int32_t x0, int32_t y0, int32_t x1, int32_t y1, glm::vec3& lo, glm::vec3& hi) const;

    render::SpaceView space_view() const;
    render::Environment& environment() { return m_environment; }
    render::Terrain& terrain() { return *m_terrain; }
    const osh::FarView* far_view() const { return m_far.get(); }

    std::optional<float> terrain_height(float x, float y) const;
    // The LTEX that covers most of the land vertex nearest a point, 0 for
    // none: the layer with the largest weight, unless the base's weight, 1 minus the layers', is at least that.
    uint32_t dominant_land_texture(float x, float y) const;
    // The height of the one land vertex the game's triangle lookup (no flags) returns for a
    // point: the corner of its triangle off the square's diagonal. The game's water audio samples land this
    // way.
    std::optional<float> land_vertex_height(float x, float y) const;
    glm::vec2 centre() const; // middle of the cell grid, metres
    bool has_cell(int32_t x, int32_t y) const;
    // Streaming of the current view not finished (cells, models, terrain ring).
    void set_actor_blend(float blend) { m_actor_blend = blend; } // how far a frame is between two world ticks
    bool busy() const;
    SpaceStats stats() const;

private:
    struct LoadedRef {
        uint32_t ref;              // index into World::refs
        uint32_t model;            // index into m_models (World::meshes)
        uint32_t instance = ~0u;   // instance-table slot once drawable
        uint32_t variant = render::k_no_variant; // material variant (directional material), once drawable
        uint32_t second = render::k_no_variant;  // the multi-pass directional material's second pass
        glm::vec4 sphere{0};       // world bounding sphere
        float pixel_radius = 0.0f; // the radius the sub-pixel test uses: the model's own bound, tighter than the box's
        std::array<float, osm::k_lod_levels> lod_error{-1.0f, -1.0f, -1.0f}; // metres at this scale; < 0: no such level
        bool disabled = false;
        bool impostor = false;     // the far view has it as an impostor
        bool tree = false;         // a TREE base: BSTreeNode's shadow rule
        bool sky_static = false;   // its base is a sky static: the weather decides whether it shows
        bool hidden = false;            // set_ref_hidden
        bool occludable = false;        // osw::ref_occludable
        bool occludable_far = false;    // osw::ref_occludable_far
        uint32_t emittance_weather = 0; // drawn with a region's emittance colour, kept current (update_emittance)
        uint32_t group = Rooms::k_cell; // where the rooms put it (space_rooms.hpp)
    };
    struct LoadedCell {
        uint32_t cell = ~0u; // index into World::cells, ~0u = free slot
        uint32_t generation = 0;
        std::vector<LoadedRef> refs;
        glm::vec3 lo{1e30f}, hi{-1e30f};
        uint32_t ready = 0;
        uint32_t waiting = 0; // references whose model is still loading
    };
    struct RefHandle {
        uint32_t slot;
        uint32_t generation;
        uint32_t index;
    };
    // V0: one LOD per model; select() is the seam for importer-generated LOD chains.
    struct Model {
        uint32_t refs = 0;
        render::MeshId lod0 = render::k_invalid_mesh;
        bool pending = false;
        std::vector<RefHandle> waiting;
        render::MeshId select(float /*distance_sq*/) const { return lod0; }
    };

    // A light's animation state; reset when its cell loads, as the game recreates it.
    struct LightState {
        float phase[3] = {0.0f, 0.0f, 0.0f};
        float factor = 1.0f;      // dimmer multiplier
        glm::vec3 offset{0.0f};   // movement in the light's parent space (metres)
        bool loaded = false;
        std::mt19937 random;      // flicker steps (the game draws them from its global generator)
    };
    void animate_light(const osw::Light& light, LightState& state, float dt);

    void build_draws(const render::View& view, const SpaceSettings& settings, render::DrawList& out, bool main);
    void load_cell(uint32_t cell_index, float priority);
    void unload_slot(uint32_t slot);
    uint32_t sky_static_flags(uint32_t flags, uint32_t base) const;
    void write_actor_instances();
    void actor_draws(const render::View& view, render::DrawList& out, bool main);
    void finalize_ref(LoadedCell& cell, LoadedRef& r);
    void process_ready_models(double budget_ms);
    void rebuild_instances();
    int32_t cell_lookup(int32_t x, int32_t y) const;

    render::Renderer& m_renderer;
    std::shared_ptr<osw::World> m_world;
    // The far view (space_far.cpp).
    struct FarProxy {
        render::MeshId mesh = render::k_invalid_mesh;
        uint32_t instance = ~0u;
        uint32_t variant = render::k_no_variant;
    };
    struct TreeBlock {
        glm::vec3 lo, hi;
        uint32_t first, last; // FarView::tree_cells
    };
    void far_init();
    int32_t far_square(int32_t x, int32_t y) const; // index into m_far_full, or -1
    // What decides whether a cell streams: squares of cells go together, so a proxy can stand in for all of them.
    float stream_distance(int32_t x, int32_t y, const glm::vec2& camera_cells) const;
    void far_mark_squares(const glm::vec2& camera_cells, float max_distance);
    void far_update(const glm::vec3& camera, const SpaceSettings& settings, const render::TerrainSettings& terrain);
    size_t far_busy() const;
    void far_draw_region(const render::View& view, const osh::Region& r, render::DrawList& out, float pixels_per_radian);
    void far_draws(const render::View& view, render::DrawList& out, float pixels_per_radian);
    void far_tree_runs(const render::View& view);

    std::shared_ptr<const osh::FarView> m_far;
    std::unique_ptr<render::FarTrees> m_far_trees;
    std::unique_ptr<GrassField> m_grass;
    int32_t m_far_grid_min[2] = {0, 0};
    int32_t m_far_squares[2] = {0, 0};
    std::vector<uint8_t> m_far_full;    // per square: its cells are loaded and draw themselves
    std::vector<uint32_t> m_far_marked; // squares set in m_far_full
    std::vector<FarProxy> m_far_proxies;
    std::vector<uint64_t> m_far_pending; // proxy << 32 | region
    std::vector<uint32_t> m_far_top;     // level 2 regions
    std::vector<uint32_t> m_far_orphans; // level 1 regions without one
    std::vector<std::vector<uint32_t>> m_far_children;
    std::vector<std::pair<glm::vec3, glm::vec3>> m_far_tree_bounds;
    std::vector<TreeBlock> m_far_tree_blocks;
    std::vector<VkDrawIndirectCommand> m_far_tree_runs;
    float m_far_range = 0.0f; // metres: where level 2 takes over
    std::unordered_set<uint32_t> m_far_tree_forms; // the references the far view has impostors of
    float m_tree_near = 0.0f; // this frame's SpaceSettings::tree_near, once impostors are ready
    glm::vec3 m_far_plan_pos{0.0f};
    float m_far_planned_range = -1.0f;
    std::unique_ptr<render::Terrain> m_terrain;
    render::InstanceTable m_instances;
    float m_actor_blend = 1.0f;
    render::DrawList m_draws;
    render::DrawList m_shadow_draws[render::k_max_cascades];
    std::vector<uint32_t> m_shadow_candidates; // World::lights indices of the shadow-casting lights, in world order
    std::vector<render::ShadowLight> m_shadow_lights;
    render::DrawList m_shadow_light_draws[render::k_max_shadow_lights];
    render::DrawList m_portal_draws;
    render::DrawList m_portal_shadow_draws[render::k_max_cascades];
    std::vector<render::ShadowLight> m_portal_shadow_lights;
    render::DrawList m_portal_shadow_light_draws[render::k_max_shadow_lights];
    std::vector<render::PointLight> m_portal_point_lights;
    std::vector<uint32_t> m_main_light_shadows; // the main frame's choice while the door's is made
    render::Environment m_environment;
    std::unique_ptr<render::LightGrid> m_light_grid;
    std::vector<LightState> m_light_state;           // parallel to World::lights
    std::vector<render::PointLight> m_point_lights; // this frame's, parallel to World::lights
    std::vector<uint32_t> m_emittance_weathers;
    std::unordered_map<uint32_t, glm::vec3> m_emittance;
    bool m_emittance_changed = false;
    float m_light_time = -1.0f;
    struct Spawned {
        uint32_t id = 0;
        render::MeshId mesh = render::k_invalid_mesh;
        glm::vec3 position{0.0f};
        float heading = 0.0f;
        uint32_t instance = ~0u;
        glm::vec4 sphere{0.0f};
        glm::mat3 rotation{1.0f}; // with scale
        bool moved = false;       // the instance is out of date
        bool hidden = false;
    };
    std::vector<Spawned> m_spawned;
    const std::unordered_map<uint32_t, ots::Swap>* m_texture_swaps = nullptr;
    struct MovedRef {
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        uint32_t slot = ~0u; // where it is loaded, while that holds
        uint32_t generation = 0;
        uint32_t index = 0;
    };
    std::unordered_map<uint32_t, MovedRef> m_moved;
    std::unordered_set<uint32_t> m_sky_static_bases, m_sky_static_current, m_sky_static_outgoing;
    std::string m_sky_model_path;
    Spawned m_sky_model;
    render::MeshId m_ripple_mesh = render::k_invalid_mesh;
    std::vector<uint32_t> m_ripple_instances;
    std::vector<glm::vec3> m_ripple_positions;
    float m_ripple_strength = 0.0f;
    std::vector<FxShape> m_fx_shapes;
    struct DrawnDoorShape {
        render::MeshId mesh = render::k_invalid_mesh;
        uint32_t group = 0;
        uint32_t instance = ~0u;
        glm::vec4 sphere{0.0f};
    };
    std::vector<DrawnDoorShape> m_door_shapes;
    std::vector<render::DoorLight> m_door_lights;
    struct DoorSlot {
        render::GpuInstanceData data;
        uint32_t slot = ~0u;
    };
    std::vector<DoorSlot> m_door_slots; // this frame's shapes
    std::vector<TurnedDoor> m_turned_doors;
    bool m_doors_turned = false;
    std::vector<uint32_t> m_fx_instances;
    std::vector<uint32_t> m_fx_materials;
    std::unordered_map<std::string, render::MeshId> m_fx_meshes;
    float m_sky_model_fade = 0.0f;
    const std::unordered_map<uint32_t, bool>* m_ref_overrides = nullptr;
    std::unordered_set<uint32_t> m_hidden; // reference FormIDs not drawn
    std::unique_ptr<ActorSet> m_actors;
    std::unique_ptr<Rooms> m_rooms;       // interiors with room markers
    std::unique_ptr<Occluders> m_occluders;
    float m_view_distance = 0.0f;
    int32_t m_room_from = -1, m_room_to = -1; // the camera's rooms, as lighting sees them
    float m_room_timer = -1.0f;               // the transition's seconds, < 0: none running
    float m_room_sunlight = -1.0f;            // the image space installed by the last blend, < 0: none yet
    void init(bool render_thread);

    std::vector<int32_t> m_grid; // world cell index by (x - min, y - min)
    std::vector<float> m_land_floor; // lowest land height by grid position, -inf where there is no land
    // Whether the land hides a box: every sight line to its top runs under some cell's lowest land on the way.
    bool land_hides(const glm::vec3& eye, const glm::vec3& lo, const glm::vec3& hi) const;
    int32_t m_grid_min[2] = {0, 0};
    int32_t m_grid_size[2] = {0, 0};

    std::vector<LoadedCell> m_slots;
    // Loaded slots ordered by cell index. Draws are emitted in this order rather than slot order (which
    // depends on load history), so overlapping and blended surfaces resolve the same way every time.
    std::vector<uint32_t> m_draw_order;
    bool m_draw_order_dirty = true;
    // This frame's: nearest cell first, so the depth test rejects what near cells cover. It depends only on the pose.
    std::vector<std::pair<float, uint32_t>> m_near_order;
    std::vector<uint32_t> m_free_slots;
    std::vector<int32_t> m_slot_of_cell; // World::cells index -> slot, -1 = not loaded
    std::vector<Model> m_models;
    std::vector<uint32_t> m_pending_models;

    RotationMode m_rotation_mode = RotationMode::stored;
    int32_t m_cam_cell[2] = {INT32_MIN, INT32_MIN};
    float m_last_distance = -1;
    glm::vec2 m_plan_pos{1e30f}; // camera (in cells) the current load/unload plan was made from
    std::vector<std::pair<float, uint32_t>> m_wanted; // (priority, cell) not yet loaded, nearest first
    size_t m_wanted_next = 0;
    std::vector<uint32_t> m_to_unload;
    uint32_t m_loaded_count = 0;
    uint32_t m_refs_loaded = 0;
    uint32_t m_refs_ready = 0;
    bool m_logged_full = false;
    bool m_retired = false;
};

} // namespace spud::engine
