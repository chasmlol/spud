#pragma once

// Refcounted .osm mesh cache. Meshes are read and parsed on workers, their
// vertices/indices land in two shared GPU arenas (so every mesh draws from
// one vertex and one index binding), and each submesh becomes a "draw group"
// the renderer instances. Unreferenced meshes stay cached (LRU) until the
// arenas fill up.

#include "formats/osm.hpp"
#include "formats/osw.hpp"
#include "render/gpu_arena.hpp"
#include "render/gpu_types.hpp"
#include "render/jobs.hpp"
#include "render/pipelines.hpp"
#include "formats/ots.hpp"
#include "render/textures.hpp"
#include "render/uploader.hpp"

#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spud::render {

using MeshId = uint32_t;
inline constexpr MeshId k_invalid_mesh = ~0u;

// Fixed-size records in one host-visible table (materials, UV animations).
template <typename T>
class GpuTable {
public:
    GpuTable(Device& device, uint32_t capacity, const char* name) : m_device(device), m_capacity(capacity) {
        m_buffer = device.create_buffer(VkDeviceSize(capacity) * sizeof(T), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        MemoryUsage::dynamic, Lifetime::asset, name);
    }
    ~GpuTable() { m_device.destroy_buffer(m_buffer); }
    GpuTable(const GpuTable&) = delete;
    GpuTable& operator=(const GpuTable&) = delete;

    uint32_t allocate(const T& value) { // ~0u when full
        uint32_t slot;
        if (!m_free.empty()) {
            slot = m_free.back();
            m_free.pop_back();
        } else if (m_next < m_capacity) {
            slot = m_next++;
        } else {
            return ~0u;
        }
        // New slots are never referenced by frames already in flight, so a direct write is safe.
        std::memcpy(m_buffer.as<T>() + slot, &value, sizeof(T));
        if (slot >= m_copy.size()) {
            m_copy.resize(slot + 1);
        }
        m_copy[slot] = value;
        ++m_used;
        return slot;
    }
    void free(uint32_t slot) { // caller defers until the GPU is done
        m_free.push_back(slot);
        --m_used;
    }
    // From the CPU copy: the table is write-combined device memory, where every read crosses the bus (a directional
    // variant reading its mesh's materials cost frames 1-2.6 ms of streaming time).
    const T& get(uint32_t slot) const { return m_copy[slot]; }
    VkDeviceAddress address() const { return m_buffer.address; }
    uint32_t used() const { return m_used; }

private:
    Device& m_device;
    Buffer m_buffer;
    uint32_t m_capacity;
    uint32_t m_next = 0;
    uint32_t m_used = 0;
    std::vector<uint32_t> m_free;
    std::vector<T> m_copy; // what each slot was written with
};
using MaterialTable = GpuTable<GpuMaterial>;

struct DrawGroup {
    uint32_t index_count = 0;
    uint32_t first_index = 0;  // absolute in the index arena
    int32_t vertex_offset = 0; // absolute in the vertex arena
    uint32_t material = 0;
    uint32_t state = 0;        // DrawState::pack()
    uint32_t state_slot = 0;   // dense index of `state` (MeshManager::states())
    MeshId mesh = k_invalid_mesh;
    uint32_t local = 0; // index among the mesh's groups
    uint32_t submesh = 0;      // the .osm submesh it was made from
    uint32_t shadow = 0;       // ShadowCaster bits when it casts shadows, else 0
    bool blood = false;        // a held weapon's blood: drawn only over itself, by DrawList::add_over
    // Level l > 0: DrawItem::lod. A submesh without level l has the next finer level's range there.
    uint32_t lod_first[osm::k_lod_levels + 1] = {};
    uint32_t lod_count[osm::k_lod_levels + 1] = {};
};

// How a draw group renders into shadow maps.
enum ShadowCaster : uint32_t {
    shadow_caster = 1u << 0,     // casts for point and spot lights
    shadow_two_sided = 1u << 1,  // no culling
    shadow_alpha_test = 1u << 2, // discards by the diffuse alpha
    shadow_sun = 1u << 3,        // casts into the sun's cascades: the game
    shadow_god_ray = 1u << 4,    // casts into the god-ray map
};

// One shape of a skinned model, parallel to MeshManager::groups(id): where its data lies in the vertex arena, and its
// bones.
struct SkinnedShape {
    uint64_t vertices = 0; // byte offsets into the arena
    uint64_t skin = 0;
    uint64_t morph = 0;    // 0: not weight-morphed
    uint32_t vertex_count = 0;
    uint32_t first_bone = 0; // into SkinnedModel::bones
    uint32_t bone_count = 0;
    uint32_t first_part = 0;
    uint32_t part_count = 0;
    uint32_t index_count = 0;
    bool axes = false;       // model-space normals (render::k_skin_axes)
    uint32_t type = 0;       // the lighting shader type
};
struct SkinnedModel {
    std::vector<SkinnedShape> shapes;
    std::vector<std::string> shape_names; // parallel to `shapes`
    std::vector<osm::SkinBone> bones;
    std::vector<uint16_t> parts;
    std::vector<uint32_t> part_indices;
    std::string attach; // the skeleton node the model names
};

// The game draws a few blend-factor pairs as others. NiAlphaProperty
// factors: 0 ONE, 1 ZERO, 2 SRC_COLOR, 4 DEST_COLOR, 6 SRC_ALPHA, 7 INV_SRC_ALPHA, 9 INV_DEST_ALPHA.
void game_blend(uint32_t src, uint32_t dst, uint32_t& out_src, uint32_t& out_dst);
// A head part's morphs (.ofm) in the vertex arena. Heads share them, and they stay once loaded.
struct FaceMorphs {
    uint32_t vertex_count = 0;
    std::vector<std::string> names;
    std::vector<float> scales;
    std::vector<uint64_t> deltas; // per morph: byte offset into the arena
    int32_t find(std::string_view name) const; // case blind; -1 when it isn't there
};

struct MeshStats {
    uint32_t live = 0;
    uint32_t ready = 0;
    uint32_t loading = 0;
    uint32_t failed = 0;
    uint32_t referenced = 0;
    uint64_t vertex_bytes = 0;
    uint64_t index_bytes = 0;
    uint64_t vertex_capacity = 0;
    uint64_t index_capacity = 0;
};

class MeshManager {
public:
    MeshManager(Device& device, Uploader& uploader, JobSystem& jobs, TextureManager& textures,
                std::filesystem::path root, VkDeviceSize vertex_capacity, VkDeviceSize index_capacity);
    ~MeshManager();

    MeshId acquire(std::string_view path, float priority);
    void release(MeshId id);

    bool ready(MeshId id) const { return m_entries[id].state == State::ready; }
    bool textures_settled(MeshId id) const;
    bool failed(MeshId id) const { return m_entries[id].state == State::failed; }
    // Local-space bounding sphere (valid when ready).
    const glm::vec4& sphere(MeshId id) const { return m_entries[id].sphere; }
    // Local-space sphere of the model's root as the original engine merges it (osm::Mesh::fade_bound), else the
    // bounding sphere: what LOD fades and light reach measure from.
    const glm::vec4& fade_sphere(MeshId id) const { return m_entries[id].fade_sphere; }
    // Metres each simplified level strays from the full mesh, worst submesh; < 0: the level doesn't exist.
    const std::array<float, osm::k_lod_levels>& lod_error(MeshId id) const { return m_entries[id].lod_error; }
    std::span<const uint32_t> groups(MeshId id) const { return m_entries[id].groups; }
    // The groups a reference draws: its directional variant's where it has one (their own materials and pipelines,
    // parallel to the mesh's), else the mesh's.
    std::span<const uint32_t> groups(MeshId id, uint32_t variant) const {
        return variant < m_variant_groups.size() && !m_variant_groups[variant].empty() ? m_variant_groups[variant]
                                                                                        : m_entries[id].groups;
    }
    // A copy of a ready mesh's materials as a placed reference with a directional material has them (projected UV,
    // snow). Cached per mesh and base object; lives as long as the mesh.
    uint32_t directional_variant(MeshId id, const osw::DirectionalMaterial& d, const ots::Swap* swap = nullptr);
    uint32_t variant_material(uint32_t variant, uint32_t local) const { return m_variants[variant][local]; }
    // The multi-pass directional material's second pass (section 11.2.3): one material per eligible group (lighting,
    // no alpha blending or testing, not a decal), k_no_material for the others.
    uint32_t multipass_variant(MeshId id, const osw::DirectionalMaterial& d, const osw::DirectionalPass& pass);
    static constexpr uint32_t k_no_material = ~0u;
    // A skinned model's shapes (empty for a static mesh; valid when ready).
    const SkinnedModel* skinned_model(MeshId id) const { return m_entries[id].skinned.get(); }
    // Starts loading a head part's morphs, by cache path, and says which they are. An empty path is none.
    uint32_t acquire_face_morphs(const std::string& path);
    bool face_morphs_settled(uint32_t id) const; // loaded, or never will be
    const FaceMorphs* face_morphs(uint32_t id) const; // null until loaded, or when they can't be
    // A posed copy of a ready skinned mesh for one actor: its own vertices and materials, drawn like any variant.
    // `ranges` says which runs of each of the mesh's groups it draws (first index, count, within the group); `edit`
    // changes each drawn group's material. `targets` receives each group's vertices (byte offsets into the arena, 0
    // for a group left out). k_no_material when the arena is full. The caller holds the mesh until
    // free_skinned_variant.
    using IndexRuns = std::vector<std::pair<uint32_t, uint32_t>>;
    uint32_t skinned_variant(MeshId id, const std::function<void(uint32_t, IndexRuns&)>& ranges,
                             const std::function<void(uint32_t, GpuMaterial&)>& edit, std::vector<uint64_t>& targets);
    void free_skinned_variant(uint32_t variant);
    // The INI values the snow bits depend on (bEnableImprovedSnow:Display).
    void set_improved_snow(bool on) { m_improved_snow = on; }
    const DrawGroup& group(uint32_t g) const { return m_groups[g]; }
    uint32_t group_count() const { return static_cast<uint32_t>(m_groups.size()); }
    // Every distinct DrawState seen so far, indexed by DrawGroup::state_slot. Only grows.
    std::span<const uint32_t> states() const { return m_states; }

    // A new material slot holding draw group `g`'s material as changed by `edit` (the UI 3D scenes fade a shape or
    // show a render target on it), for DrawItem::material; ~0u when the table is full. The caller frees it with
    // free_material once no frame in flight reads it (Device::defer).
    template <typename F>
    uint32_t copy_material(uint32_t g, F&& edit) {
        GpuMaterial m = m_materials.get(m_groups[g].material);
        edit(m);
        return m_materials.allocate(m);
    }
    void free_material(uint32_t slot) { m_materials.free(slot); }
    const GpuMaterial& group_material(uint32_t g) const { return m_materials.get(m_groups[g].material); }
    // A record of the frame's animated materials that the caller writes itself, for GpuMaterial::uv_anim.
    uint32_t allocate_material_block();
    void set_material_block(uint32_t block, const GpuMaterialAnim& values) { m_anims[block].base = values; }
    void free_material_block(uint32_t block);

    const GpuArena& vertices() const { return m_vertices; }
    const GpuArena& indices() const { return m_indices; }
    VkDeviceAddress materials_address() const { return m_materials.address(); }
    // Animated materials: the number of GpuMaterialAnim records a frame needs, and
    // their values at the clock `time`, UV values at `previous_time` (the game shows them one frame late). Records of
    // evicted meshes are left unwritten.
    uint32_t material_anim_count() const { return static_cast<uint32_t>(m_anims.size()); }
    void write_material_anims(float time, float previous_time, GpuMaterialAnim* out) const;

    // Render thread, once per frame: schedules loads and evicts.
    void update();
    size_t busy() const { return m_queued + m_in_flight; }
    MeshStats stats() const;

private:
    enum class State : uint8_t { free, queued, loading, ready, failed };

    struct Entry {
        std::string path;
        State state = State::free;
        uint32_t refs = 0;
        float priority = 0.0f;
        glm::vec4 sphere{0, 0, 0, 0};
        glm::vec4 fade_sphere{0, 0, 0, 0};
        std::array<float, osm::k_lod_levels> lod_error{-1.0f, -1.0f, -1.0f};
        ArenaRange vertices;
        ArenaRange indices;
        std::vector<uint32_t> groups;
        std::vector<uint32_t> materials;
        std::vector<uint32_t> anims; // material animation records
        std::vector<osm::MaterialParams> params; // per group, for variants
        std::vector<std::pair<uint32_t, uint32_t>> variants; // (base object, variant id)
        std::vector<TextureId> textures;
        ArenaRange skin_vertices; // a skinned model: its weights and its light vertices
        ArenaRange skin_morph;
        std::shared_ptr<const SkinnedModel> skinned;
        bool in_lru = false;
        bool arena_full = false; // failed for want of room: the next acquire loads it again
        std::list<MeshId>::iterator lru;
    };

    struct Parsed; // worker -> render thread hand-off

    struct QueueItem {
        float priority;
        uint64_t sequence;
        MeshId id;
        bool operator<(const QueueItem& o) const {
            if (priority != o.priority) {
                return priority > o.priority;
            }
            return sequence > o.sequence;
        }
    };

    MeshId allocate_entry();
    void load_job(MeshId id, std::string path, float priority);
    void finish_load(MeshId id, Parsed& parsed);
    void fail_load(MeshId id, const std::string& error);
    void touch_lru(MeshId id);
    void evict(MeshId id);
    uint32_t allocate_group();
    uint32_t state_slot(uint32_t state); // interned into states()

    Device& m_device;
    Uploader& m_uploader;
    JobSystem& m_jobs;
    TextureManager& m_textures;
    std::filesystem::path m_root;
    GpuArena m_vertices;
    GpuArena m_indices;
    MaterialTable m_materials;
    // A material whose controllers animate it: its own values and what the animated ones combine with.
    struct MaterialAnimation {
        GpuMaterialAnim base{};
        osm::MaterialParams params;
        uint32_t type = 0; // lighting shader type
        bool effect = false;
        bool palette_color = false; // effect: the shader applies the base colour scale itself
        bool refraction = false;
        bool live = false;
        bool block = false; // written by its owner: `base` as it stands
        std::vector<osm::MaterialController> controllers;
    };
    std::vector<MaterialAnimation> m_anims;
    std::vector<uint32_t> m_free_anims;

    std::vector<Entry> m_entries;
    std::vector<MeshId> m_free_entries;
    std::unordered_map<std::string, MeshId> m_by_path;
    std::list<MeshId> m_lru;
    std::priority_queue<QueueItem> m_queue;
    uint64_t m_sequence = 0;
    size_t m_queued = 0;
    size_t m_in_flight = 0;
    size_t m_max_in_flight = 128;
    uint32_t m_ready_count = 0;
    uint32_t m_failed_count = 0;

    std::vector<DrawGroup> m_groups;
    std::vector<uint32_t> m_free_groups;
    std::vector<uint32_t> m_states;
    std::unordered_map<uint32_t, uint32_t> m_state_slots;

    std::mutex m_fail_mutex;
    std::vector<std::pair<MeshId, std::string>> m_failures;
    std::unordered_set<std::string> m_logged;

    std::vector<std::vector<uint32_t>> m_variants; // material per group of the variant's mesh
    std::vector<std::vector<uint32_t>> m_variant_groups; // directional variants: their group per group of the mesh
    std::vector<uint32_t> m_free_variants;
    std::unordered_map<uint32_t, ArenaRange> m_skinned_variants; // their vertices
    struct FaceMorphEntry {
        std::shared_ptr<const FaceMorphs> morphs;
        std::atomic<bool> settled{false};
    };
    std::deque<FaceMorphEntry> m_face_morphs;
    std::unordered_map<std::string, uint32_t> m_face_morph_ids;
    bool m_improved_snow = true;
};

} // namespace spud::render
