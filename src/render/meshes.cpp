#include "render/meshes.hpp"

#include "core/path.hpp"
#include "core/file.hpp"
#include "core/log.hpp"
#include "formats/ofm.hpp"
#include "formats/osm.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace spud::render {

namespace {

// The static part of Skyrim's lighting technique id for a material, as the game
// builds it from the NIF's shader flags. Light counts, shadows and the distance
// fades are per frame.
// The single pass's TextureProj (11.4.1): the fixed top-down basis, projected UV = (y, -x), up = z.
void set_fixed_projection(GpuMaterial& g) {
    const float rows[3][4] = {{0, 1, 0, 0}, {-1, 0, 0, 0}, {0, 0, 1, 0}};
    std::memcpy(g.projection, rows, sizeof rows);
}

uint32_t lighting_technique(const osm::MaterialParams& p, bool improved_snow) {
    const uint64_t f = uint64_t(p.skyrim_flags1) | (uint64_t(p.skyrim_flags2) << 32);
    auto has = [&](int bit) { return (f >> bit) & 1; };
    uint32_t t = 0;
    // Projected UV and snow. LOD materials (flags 33, 34, 46, 63) take neither snow
    // bit; bEnableImprovedSnow gates both.
    const bool lod = has(33) || has(34) || has(46) || has(63);
    if (has(23)) {
        t |= 1u << 15;
    }
    if (has(23) && has(30) && !lod && improved_snow) {
        t |= 1u << 19;
    }
    if (((has(60) && !has(61)) || has(30)) && !lod && improved_snow) {
        t |= (1u << 21) | (1u << 9);
    }
    if (has(32 + 5) && (p.flags & osm::material_vertex_colors)) {
        t |= 1u << 0;
    }
    if (has(1)) {
        t |= 1u << 1;
    }
    if (has(12)) {
        t |= 1u << 2;
    }
    if (has(0) || has(32 + 9)) {
        t |= 1u << 9;
    }
    if (has(32 + 25)) {
        t |= 1u << 10;
    }
    if (has(32 + 26)) {
        t |= 1u << 11;
    }
    if (has(32 + 27)) {
        t |= 1u << 12;
    }
    if (has(32 + 21)) {
        t |= 1u << 16;
    }
    if (has(32 + 8)) {
        t |= 1u << 22;
    }
    // Type: later rules override earlier ones.
    uint32_t type = 0;
    if (has(7)) type = 1;
    if (has(32 + 6)) type = 2;
    if (has(11) && !has(28)) type = 3;
    if (has(10)) type = 4;
    if (has(21)) type = 5;
    if (has(18)) type = 6;
    if (has(28)) type = 7;
    if (has(14)) type = 8;
    if (has(32 + 14)) type = 19;
    if (has(32 + 1)) type = 18;
    if (has(32 + 2)) type = 13;
    if (has(32 + 31)) type = 15;
    if (has(32 + 24)) type = 11;
    if (has(32 + 29)) type = 12;
    if (has(32 + 9) && has(23)) type = 14;
    if (has(17)) type = 16;
    // SetupTechnique: parallax occlusion renders as type 0 (no type-7 shaders in 1.7.104); LOD land noise as 9.
    if (type == 7) type = 0;
    if (type == 18) type = 9;
    return t | (type << 24);
}

// The effect shader's technique bits for a material, as the game sets them from the
// NIF. Particle, skinning and membrane bits never apply to imported statics.
uint32_t effect_technique(const osm::MaterialParams& p, const osm::Material& m) {
    const uint32_t f1 = p.skyrim_flags1, f2 = p.skyrim_flags2;
    const bool blend = (p.flags & osm::material_alpha_blend) != 0;
    const bool test = (p.flags & osm::material_alpha_test) != 0;
    const bool greyscale = !m.textures[1].empty();
    uint32_t t = 0;
    if ((f2 >> 5 & 1) && (p.flags & osm::material_vertex_colors)) t |= 0x1;
    if (!m.textures[osm::texture_diffuse].empty()) t |= 0x42;
    if (f1 >> 6 & 1) t |= 0x110;
    if (blend && p.blend_dst == 0) t |= 0x400;
    if (blend && p.blend_dst == 2) t |= 0x800;
    if (blend && p.blend_src == 4 && p.blend_dst == 7) t |= 0x400000;
    if (f2 >> 17 & 1) t |= 0x4000;
    if (f2 >> 30 & 1) t |= 0x10000;
    if (f1 >> 30 & 1) t |= 0x40000;
    if ((f1 >> 4 & 1) && greyscale) t |= 0x80000;
    if ((f1 >> 5 & 1) && greyscale) t |= 0x100000;
    if (f2 >> 13 & 1) t |= 0x1000000;
    if ((!blend || test) && (f1 >> 31 & 1) && (f2 & 1)) t |= 0x4000000;
    return t;
}

} // namespace

void game_blend(uint32_t src, uint32_t dst, uint32_t& out_src, uint32_t& out_dst) {
    if ((src == 6 && dst == 0) || (src == 0 && dst == 0) || (src == 6 && dst == 9)) {
        out_src = 6, out_dst = 0; // SRC_ALPHA, ONE
    } else if ((src == 1 && dst == 2) || (src == 4 && dst == 1)) {
        out_src = 4, out_dst = 1; // DEST_COLOR, ZERO
    } else if (src == 4 && dst == 7) {
        out_src = 4, out_dst = 7;
    } else {
        out_src = 6, out_dst = 7; // SRC_ALPHA, INV_SRC_ALPHA (and pairs the game leaves to the previous draw)
    }
}

// ---- MaterialTable ---------------------------------------------------------

// ---- MeshManager -----------------------------------------------------------

struct MeshManager::Parsed {
    glm::vec4 sphere{0, 0, 0, 0};
    std::vector<std::array<osm::SubmeshLod, osm::k_lod_levels>> lods; // parallel to submeshes
    glm::vec4 fade_sphere{0, 0, 0, 0};
    ArenaRange vertices;
    ArenaRange indices;
    std::vector<osm::Submesh> submeshes;
    std::vector<uint32_t> submesh_index; // each kept submesh's index in the file
    std::vector<osm::Material> materials;
    ArenaRange skin_vertices;
    ArenaRange skin_morph;
    std::shared_ptr<SkinnedModel> skinned; // shapes parallel to `submeshes`
};

MeshManager::MeshManager(Device& device, Uploader& uploader, JobSystem& jobs, TextureManager& textures,
                         std::filesystem::path root, VkDeviceSize vertex_capacity, VkDeviceSize index_capacity)
    : m_device(device), m_uploader(uploader), m_jobs(jobs), m_textures(textures), m_root(std::move(root)),
      m_vertices(device, vertex_capacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 "mesh vertices"),
      m_indices(device, index_capacity, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "mesh indices"),
      m_materials(device, 1u << 18, "materials") {
    log::info("meshes: {} MiB vertex arena, {} MiB index arena", vertex_capacity >> 20, index_capacity >> 20);
}

MeshManager::~MeshManager() = default;

MeshId MeshManager::allocate_entry() {
    if (!m_free_entries.empty()) {
        const MeshId id = m_free_entries.back();
        m_free_entries.pop_back();
        m_entries[id] = Entry{};
        return id;
    }
    m_entries.emplace_back();
    return static_cast<MeshId>(m_entries.size() - 1);
}

MeshId MeshManager::acquire(std::string_view path, float priority) {
    std::string key(path);
    if (auto it = m_by_path.find(key); it != m_by_path.end()) {
        Entry& e = m_entries[it->second];
        if (e.refs++ == 0 && e.in_lru) {
            m_lru.erase(e.lru);
            e.in_lru = false;
        }
        if (e.state == State::queued && priority < e.priority) {
            e.priority = priority;
            m_queue.push({priority, m_sequence++, it->second});
        }
        if (e.state == State::failed && e.arena_full) {
            e.arena_full = false;
            e.state = State::queued;
            e.priority = priority;
            --m_failed_count;
            ++m_queued;
            m_queue.push({priority, m_sequence++, it->second});
        }
        return it->second;
    }
    const MeshId id = allocate_entry();
    Entry& e = m_entries[id];
    e.path = std::move(key);
    e.state = State::queued;
    e.refs = 1;
    e.priority = priority;
    m_by_path.emplace(e.path, id);
    m_queue.push({priority, m_sequence++, id});
    ++m_queued;
    return id;
}

void MeshManager::release(MeshId id) {
    Entry& e = m_entries[id];
    if (e.refs == 0) {
        return;
    }
    if (--e.refs == 0 && (e.state == State::ready || e.state == State::failed)) {
        touch_lru(id);
    }
}

void MeshManager::touch_lru(MeshId id) {
    Entry& e = m_entries[id];
    if (e.in_lru) {
        m_lru.erase(e.lru);
    }
    m_lru.push_back(id);
    e.lru = std::prev(m_lru.end());
    e.in_lru = true;
}

void MeshManager::update() {
    {
        std::vector<std::pair<MeshId, std::string>> failures;
        {
            std::lock_guard lock(m_fail_mutex);
            failures.swap(m_failures);
        }
        for (auto& [id, error] : failures) {
            fail_load(id, error);
        }
    }

    while (m_in_flight < m_max_in_flight && !m_queue.empty()) {
        const QueueItem item = m_queue.top();
        m_queue.pop();
        Entry& e = m_entries[item.id];
        if (e.state != State::queued || item.priority != e.priority) {
            continue;
        }
        --m_queued;
        if (e.refs == 0) {
            m_by_path.erase(e.path);
            e = Entry{};
            m_free_entries.push_back(item.id);
            continue;
        }
        e.state = State::loading;
        ++m_in_flight;
        m_jobs.submit(item.priority,
                      [this, id = item.id, path = e.path, p = item.priority] { load_job(id, path, p); });
    }

    // Keep the arenas from filling with meshes nobody uses; capped per frame.
    int budget = 64;
    while (!m_lru.empty() && budget-- > 0 &&
           (m_lru.size() > 4096 || m_vertices.used() > m_vertices.capacity() * 7 / 10 ||
            m_indices.used() > m_indices.capacity() * 7 / 10)) {
        evict(m_lru.front());
    }
}

void MeshManager::load_job(MeshId id, std::string path, float priority) {
    (void)priority;
    try {
        const auto full = m_root / path;
        std::error_code ec;
        if (!std::filesystem::exists(full, ec)) {
            throw Error("file not found");
        }
        auto mesh = osm::read(read_file(full));

        auto parsed = std::make_shared<Parsed>();
        // Drop submeshes that would read out of bounds: a bad index must never reach the GPU.
        for (const auto& sm : mesh.submeshes) {
            const bool ranges_ok = uint64_t(sm.first_index) + sm.index_count <= mesh.indices.size() &&
                                   uint64_t(sm.base_vertex) + sm.vertex_count <= mesh.vertices.size() &&
                                   sm.material < mesh.materials.size() && sm.index_count % 3 == 0;
            bool ok = ranges_ok;
            for (uint32_t i = 0; ok && i < sm.index_count; ++i) {
                ok = mesh.indices[sm.first_index + i] < sm.vertex_count;
            }
            if (ok && sm.index_count > 0) {
                parsed->submeshes.push_back(sm);
                parsed->submesh_index.push_back(static_cast<uint32_t>(&sm - mesh.submeshes.data()));
                std::array<osm::SubmeshLod, osm::k_lod_levels> lods{};
                if (mesh.lods.size() == mesh.submeshes.size() * osm::k_lod_levels) {
                    for (uint32_t l = 0; l < osm::k_lod_levels; ++l) {
                        const osm::SubmeshLod& lod = mesh.lods[size_t(&sm - mesh.submeshes.data()) * osm::k_lod_levels + l];
                        bool good = uint64_t(lod.first_index) + lod.index_count <= mesh.indices.size();
                        for (uint32_t i = 0; good && i < lod.index_count; ++i) {
                            good = mesh.indices[lod.first_index + i] < sm.vertex_count;
                        }
                        if (good) {
                            lods[l] = lod;
                        }
                    }
                }
                parsed->lods.push_back(lods);
            } else if (!ok) {
                log::warn("meshes: {}: dropping invalid submesh", path);
            }
        }
        parsed->materials = std::move(mesh.materials);

        const glm::vec3 lo(mesh.bounds_min[0], mesh.bounds_min[1], mesh.bounds_min[2]);
        const glm::vec3 hi(mesh.bounds_max[0], mesh.bounds_max[1], mesh.bounds_max[2]);
        parsed->sphere = glm::vec4((lo + hi) * 0.5f, glm::length(hi - lo) * 0.5f);
        parsed->fade_sphere = mesh.fade_bound[3] != 0.0f ? glm::vec4(mesh.fade_bound[0], mesh.fade_bound[1],
                                                                     mesh.fade_bound[2], mesh.fade_bound[3])
                                                         : parsed->sphere;

        const size_t vbytes = mesh.vertices.size() * sizeof(osm::Vertex);
        const size_t ibytes = mesh.indices.size() * sizeof(uint32_t);
        if (parsed->submeshes.empty() || vbytes == 0 || ibytes == 0) {
            parsed->submeshes.clear();
            // Nothing to upload; finish through the uploader anyway to keep one completion path.
        } else {
            parsed->vertices = m_vertices.allocate(vbytes, sizeof(osm::Vertex));
            parsed->indices = m_indices.allocate(ibytes, sizeof(uint32_t));
            const bool skinned = !mesh.skin.shapes.empty();
            const size_t sbytes = mesh.skin.vertices.size() * sizeof(osm::SkinVertex);
            const size_t mbytes = mesh.skin.morph.size() * sizeof(osm::Vertex);
            if (skinned) {
                parsed->skin_vertices = m_vertices.allocate(sbytes, 16);
                if (mbytes) {
                    parsed->skin_morph = m_vertices.allocate(mbytes, 16);
                }
            }
            if (!parsed->vertices || !parsed->indices || (skinned && !parsed->skin_vertices) ||
                (mbytes && !parsed->skin_morph)) {
                m_vertices.free(parsed->vertices);
                m_indices.free(parsed->indices);
                m_vertices.free(parsed->skin_vertices);
                m_vertices.free(parsed->skin_morph);
                throw Error("mesh arenas are full");
            }
        }

        UploadRequest req;
        if (!parsed->submeshes.empty()) {
            const size_t sbytes = mesh.skin.vertices.size() * sizeof(osm::SkinVertex);
            const size_t mbytes = mesh.skin.morph.size() * sizeof(osm::Vertex);
            req.data.resize(vbytes + ibytes + sbytes + mbytes);
            std::memcpy(req.data.data(), mesh.vertices.data(), vbytes);
            std::memcpy(req.data.data() + vbytes, mesh.indices.data(), ibytes);
            req.buffer_regions.push_back({m_vertices.buffer().buffer, parsed->vertices.offset, 0, vbytes});
            req.buffer_regions.push_back({m_indices.buffer().buffer, parsed->indices.offset, vbytes, ibytes});
            if (parsed->skin_vertices) {
                std::memcpy(req.data.data() + vbytes + ibytes, mesh.skin.vertices.data(), sbytes);
                req.buffer_regions.push_back(
                    {m_vertices.buffer().buffer, parsed->skin_vertices.offset, vbytes + ibytes, sbytes});
                auto model = std::make_shared<SkinnedModel>();
                model->bones = std::move(mesh.skin.bones);
                model->parts = std::move(mesh.skin.parts);
                model->part_indices = std::move(mesh.skin.part_indices);
                model->attach = std::move(mesh.skin.attach);
                for (size_t k = 0; k < parsed->submeshes.size(); ++k) {
                    const auto& sm = parsed->submeshes[k];
                    const auto& shape = mesh.skin.shapes[parsed->submesh_index[k]];
                    SkinnedShape s;
                    s.vertices = parsed->vertices.offset + uint64_t(sm.base_vertex) * sizeof(osm::Vertex);
                    s.skin = parsed->skin_vertices.offset + uint64_t(sm.base_vertex) * sizeof(osm::SkinVertex);
                    s.vertex_count = sm.vertex_count;
                    s.first_bone = shape.first_bone;
                    s.bone_count = shape.bone_count;
                    s.first_part = shape.first_part;
                    s.part_count = shape.part_count;
                    s.index_count = sm.index_count;
                    if (parsed->skin_morph) {
                        s.morph = parsed->skin_morph.offset + uint64_t(sm.base_vertex) * sizeof(osm::Vertex);
                    }
                    model->shapes.push_back(s);
                    const size_t source = parsed->submesh_index[k];
                    model->shape_names.push_back(source < mesh.skin.shape_names.size() ? mesh.skin.shape_names[source]
                                                                                       : std::string());
                }
                parsed->skinned = std::move(model);
            }
            if (parsed->skin_morph) {
                std::memcpy(req.data.data() + vbytes + ibytes + sbytes, mesh.skin.morph.data(), mbytes);
                req.buffer_regions.push_back(
                    {m_vertices.buffer().buffer, parsed->skin_morph.offset, vbytes + ibytes + sbytes, mbytes});
            }
        }
        req.on_complete = [this, id, parsed] { finish_load(id, *parsed); };
        m_uploader.submit(std::move(req));
    } catch (const std::exception& ex) {
        std::lock_guard lock(m_fail_mutex);
        m_failures.emplace_back(id, ex.what());
    }
}

int32_t FaceMorphs::find(std::string_view name) const {
    for (size_t i = 0; i < names.size(); ++i) {
        if (std::ranges::equal(names[i], name, [](char a, char b) { return std::tolower(a) == std::tolower(b); })) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

uint32_t MeshManager::acquire_face_morphs(const std::string& path) {
    if (path.empty()) {
        return ~0u;
    }
    if (const auto it = m_face_morph_ids.find(path); it != m_face_morph_ids.end()) {
        return it->second;
    }
    const uint32_t id = static_cast<uint32_t>(m_face_morphs.size());
    m_face_morphs.emplace_back();
    m_face_morph_ids.emplace(path, id);
    m_jobs.submit(1.0f, [this, id, path] {
        FaceMorphEntry& entry = m_face_morphs[id];
        try {
            const auto file = ofm::read(read_file(m_root / path));
            const size_t bytes = file.deltas.size() * sizeof(int16_t);
            const ArenaRange range = m_vertices.allocate(bytes, 16);
            if (!range) {
                throw Error("mesh arenas are full");
            }
            auto morphs = std::make_shared<FaceMorphs>();
            morphs->vertex_count = file.vertex_count;
            morphs->names = file.names;
            morphs->scales = file.scales;
            const uint64_t stride = uint64_t(file.vertex_count) * 4 * sizeof(int16_t);
            for (size_t m = 0; m < file.names.size(); ++m) {
                morphs->deltas.push_back(range.offset + m * stride);
            }
            UploadRequest req;
            req.data.resize(bytes);
            std::memcpy(req.data.data(), file.deltas.data(), bytes);
            req.buffer_regions.push_back({m_vertices.buffer().buffer, range.offset, 0, bytes});
            req.on_complete = [&entry, morphs] {
                entry.morphs = morphs;
                entry.settled = true;
            };
            m_uploader.submit(std::move(req));
        } catch (const std::exception& e) {
            log::warn("meshes: {}: {}", path, e.what());
            entry.settled = true;
        }
    });
    return id;
}

bool MeshManager::textures_settled(MeshId id) const {
    return std::ranges::all_of(m_entries[id].textures, [this](TextureId t) { return m_textures.settled(t); });
}

bool MeshManager::face_morphs_settled(uint32_t id) const { return id >= m_face_morphs.size() || m_face_morphs[id].settled; }

const FaceMorphs* MeshManager::face_morphs(uint32_t id) const {
    return id < m_face_morphs.size() && m_face_morphs[id].settled ? m_face_morphs[id].morphs.get() : nullptr;
}

uint32_t MeshManager::allocate_group() {
    if (!m_free_groups.empty()) {
        const uint32_t g = m_free_groups.back();
        m_free_groups.pop_back();
        return g;
    }
    m_groups.emplace_back();
    return static_cast<uint32_t>(m_groups.size() - 1);
}

void MeshManager::finish_load(MeshId id, Parsed& parsed) {
    Entry& e = m_entries[id];
    --m_in_flight;
    e.sphere = parsed.sphere;
    e.fade_sphere = parsed.fade_sphere;
    e.vertices = parsed.vertices;
    e.indices = parsed.indices;
    e.skin_vertices = parsed.skin_vertices;
    e.skin_morph = parsed.skin_morph;

    const float tex_priority = e.priority + 0.5f;
    std::vector<GpuMaterial> base(parsed.materials.size());
    std::vector<uint32_t> material_states(parsed.materials.size(), 0);
    std::vector<uint32_t> material_shadow(parsed.materials.size(), 0);
    std::vector<uint8_t> material_blood(parsed.materials.size(), 0);
    const bool weapon = parsed.skinned && path::to_lower(parsed.skinned->attach).starts_with("weapon");
    for (size_t i = 0; i < parsed.materials.size(); ++i) {
        const auto& m = parsed.materials[i];
        const auto& p = m.params;
        const bool effect = p.model == osm::ShaderModel::effect;
        GpuMaterial& g = base[i];
        auto take = [&](uint32_t slot, TextureKind kind) {
            const TextureId t = m_textures.acquire(m.textures[slot], kind, tex_priority);
            e.textures.push_back(t);
            return t;
        };
        // An empty texture-set slot gets the engine's default texture for that slot (section 4.3).
        auto take_or = [&](uint32_t slot, TextureKind kind, TextureId fallback) {
            return m.textures[slot].empty() ? fallback : take(slot, kind);
        };
        for (auto& t : g.tex) {
            t = m_textures.black();
        }
        g.tex[gtex_diffuse] = take(osm::texture_diffuse, TextureKind::color);
        if (effect) {
            g.tex[gtex_normal] = m_textures.flat_normal();
            // SkyrimShaderPropertyFlags1: 4 greyscale->palette colour, 5 ->palette alpha, 6 use falloff.
            const uint32_t f1 = p.skyrim_flags1;
            g.effect_flags = ((f1 >> 6) & 1 ? k_effect_falloff : 0) | ((f1 >> 4) & 1 ? k_effect_palette_color : 0) |
                             ((f1 >> 5) & 1 ? k_effect_palette_alpha : 0);
            g.palette = (g.effect_flags & (k_effect_palette_color | k_effect_palette_alpha)) &&
                                !m.textures[1].empty()
                            ? take(1, TextureKind::color)
                            : m_textures.white();
            if (!(g.effect_flags & (k_effect_palette_color | k_effect_palette_alpha)) || m.textures[1].empty()) {
                g.effect_flags &= ~(k_effect_palette_color | k_effect_palette_alpha);
            }
            for (int k = 0; k < 4; ++k) {
                g.falloff[k] = p.falloff[k];
            }
            g.technique = effect_technique(p, m);
            g.effect[0] = float(p.lighting_influence) / 255.0f; // LightingInfluence.x
            g.effect[1] = p.soft_falloff_depth > 0.0f ? p.soft_falloff_depth : 100.0f;
        } else {
            // Texture registers per material type, section 4.
            g.technique = lighting_technique(p, m_improved_snow);
            set_fixed_projection(g);
            if (g.technique & (1u << 15)) {
                // A NIF's own flag 23 keeps the property's constructor values (11.2).
                g.projected_color[0] = 0.6f;
                g.projected_color[1] = 0.7f;
                g.projected_color[2] = 0.8f;
                g.projected_color[3] = 1.0f;
            }
            const uint32_t type = g.technique >> 24;
            const uint32_t t = g.technique;
            const TextureId def_normal = m_textures.flat_normal(); // (128, 128, 255)
            const TextureId black = m_textures.black();
            if (m.textures[osm::texture_diffuse].empty()) {
                g.tex[gtex_diffuse] = def_normal;
            }
            g.tex[gtex_normal] = take(osm::texture_normal, TextureKind::normal);
            if (t & ((1u << 10) | (1u << 11))) {
                g.tex[gtex_rim_soft] = take_or(osm::texture_glow, TextureKind::color, def_normal);
            }
            if (t & (1u << 12)) {
                g.tex[gtex_back] = take_or(osm::texture_backlight, TextureKind::color, def_normal);
            }
            if ((t & (1u << 2)) && (t & (1u << 9))) {
                g.tex[gtex_specular] = take_or(osm::texture_backlight, TextureKind::color, black);
            }
            switch (type) {
            case 1: // environment map: an empty mask stays empty (EnvmapData.y = 0)
                g.tex[gtex_env] = take(osm::texture_environment, TextureKind::cube);
                g.tex[gtex_env_mask] = take(osm::texture_env_mask, TextureKind::color);
                break;
            case 16: // eye
                g.tex[gtex_env] = take(osm::texture_environment, TextureKind::cube);
                g.tex[gtex_env_mask] = take_or(osm::texture_env_mask, TextureKind::color, def_normal);
                break;
            case 2: g.tex[gtex_glow] = take(osm::texture_glow, TextureKind::color); break;
            case 3: g.tex[gtex_height] = take_or(osm::texture_height, TextureKind::color, black); break;
            case 4: { // face: t3 = slot 6, t4 = slot 3, t12 = slot 2
                g.tex[gtex_height] = take_or(osm::texture_inner_layer, TextureKind::color, m_textures.grey());
                const TextureId detail = m_textures.facegen_detail();
                g.tex[gtex_env] = take_or(osm::texture_height, TextureKind::color, detail);
                g.tex[gtex_rim_soft] = take_or(osm::texture_glow, TextureKind::color, black);
                break;
            }
            case 11: // multilayer parallax
                g.tex[gtex_inner] = take_or(osm::texture_inner_layer, TextureKind::color, def_normal);
                g.tex[gtex_env] = take(osm::texture_environment, TextureKind::cube);
                g.tex[gtex_env_mask] = take_or(osm::texture_env_mask, TextureKind::color, def_normal);
                break;
            default: break;
            }
            // SpecularColor and glossiness are written whether or not this material has bit 9: a reference's snow
            // can add it (11.3).
            for (int c = 0; c < 3; ++c) {
                g.specular[c] = p.specular_color[c] * p.specular_strength;
            }
            // FinishSetupTextures: a specular material (flags1.0) with glossiness <= 0 gets 1.
            g.specular[3] = (p.skyrim_flags1 & 1u) && p.glossiness <= 0.0f ? 1.0f : p.glossiness;
            g.effect[0] = p.lighting_effect1;
            g.effect[1] = p.lighting_effect2;
            if (type == 1 || type == 16 || type == 11) {
                g.effect[2] = type == 11 ? p.type_params[4] : p.type_params[0]; // EnvmapData.x: scale or strength
                // EnvmapData.y: whether the material has a mask texture, which eyes and multilayer always do.
                g.effect[3] = type != 1 || !m.textures[osm::texture_env_mask].empty() ? 1.0f : 0.0f;
            }
            if (type == 11) {
                for (int c = 0; c < 4; ++c) {
                    g.layer[c] = p.type_params[c];
                }
                g.extra[0] = p.type_params[4];
            }
            if (type == 5 || type == 6) {
                for (int c = 0; c < 3; ++c) {
                    g.tint[c] = p.type_params[c];
                }
            }
            if (type == 14) {
                for (int c = 0; c < 4; ++c) {
                    g.sparkle[c] = p.type_params[c];
                }
            }
            if (type == 16) {
                // LeftEyeCenter and RightEyeCenter in the shape's space (section 15.2), in metres like the vertices.
                for (int c = 0; c < 3; ++c) {
                    g.layer[c] = p.type_params[1 + c] * 0.0142875f;
                    g.tint[c] = p.type_params[4 + c] * 0.0142875f;
                }
            }
        }
        g.flags = p.flags | (effect ? k_material_effect_bit : 0) | ((p.skyrim_flags2 >> 3) & 1 ? k_material_no_fade : 0) |
                  ((p.skyrim_flags1 >> 29) & 1 ? k_material_external_emittance : 0) |
                  (p.skyrim_flags2 & ((1u << 3) | (1u << 9)) ? k_material_no_decal_fade : 0);
        // The shadow mask: the game (flags1.8), the decal flags (flags1.26, 27) or
        // The game (flags2.7), never on LOD (flags2.1, 2.2, 2.31).
        const bool lod = (p.skyrim_flags2 & ((1u << 1) | (1u << 2) | (1u << 31))) != 0;
        if (!effect && !lod &&
            ((p.skyrim_flags1 & ((1u << 8) | (1u << 26) | (1u << 27))) != 0 || (p.skyrim_flags2 & (1u << 7)) != 0)) {
            g.flags |= k_material_receive_shadows;
        }
        // int(threshold x property alpha) / 255; the test always keeps alpha >= ref.
        g.alpha_threshold = float(int(float(p.alpha_threshold) * p.alpha)) / 255.0f;
        g.sampler = std::min<uint32_t>(p.texture_clamp, 3);
        if (!effect && (p.skyrim_flags1 & (1u << 20))) {
            // Localmap_Hide_Secret: FinishSetupTextures replaces the diffuse with black and wraps it.
            g.tex[gtex_diffuse] = m_textures.black();
            g.sampler = 3;
        }
        g.alpha_func = p.alpha_func;
        g.uv[0] = p.uv_offset[0];
        g.uv[1] = p.uv_offset[1];
        g.uv[2] = p.uv_scale[0];
        g.uv[3] = p.uv_scale[1];
        for (int c = 0; c < 4; ++c) {
            // Pre-multiplied by the scale except in palette-colour mode, where the shader applies the scale itself.
            const bool palette_color = effect && (g.technique & 0x80000);
            g.base_color[c] = effect ? p.base_color[c] * (c < 3 && !palette_color ? p.base_color_scale : 1.0f) : 1.0f;
        }
        g.extra[1] = p.base_color_scale;
        for (int c = 0; c < 3; ++c) {
            g.emit[c] = p.emissive_color[c] * p.emissive_multiple;
        }
        // MaterialData.z, or an effect's PropertyColor.a: its property alpha, the material's base colour
        // alpha: an effect's alpha has that factor twice (10.5.2).
        g.emit[3] = effect ? p.base_color[3] : p.alpha;

        // The world-map bit (flags2.23): the overlay's textures, which the import puts in these slots.
        const bool world_map = !effect && ((p.skyrim_flags2 >> 23) & 1) != 0;
        if (world_map) {
            g.tex[gtex_env_mask] = take(osm::texture_env_mask, TextureKind::normal);
            g.tex[gtex_inner] = take(osm::texture_inner_layer, TextureKind::normal);
            g.tex[gtex_back] = take(osm::texture_backlight, TextureKind::color);
        }

        DrawState s;
        const bool blend = (p.flags & osm::material_alpha_blend) != 0;
        const bool test = (p.flags & osm::material_alpha_test) != 0;
        // Refraction (flags1 bit 15 or 2): no lit pass at all, only the refraction pass (section 13).
        const bool refraction = !effect && (p.skyrim_flags1 & 0x8004) != 0;
        if (effect) {
            s.program = blend ? program_effect_blend : test ? program_effect_alpha_test : program_effect_opaque;
        } else if (refraction) {
            s.program = program_refraction;
        } else if (world_map) {
            s.program = p.skyrim_shader_type == 18 ? program_world_map_land : program_world_map_object;
        } else {
            s.program = blend ? program_mesh_blend : test ? program_mesh_alpha_test : program_mesh_opaque;
        }
        uint32_t bs = 6, bd = 7;
        game_blend(p.blend_src, p.blend_dst, bs, bd);
        s.blend_src = static_cast<uint8_t>(bs);
        s.blend_dst = static_cast<uint8_t>(bd);
        s.two_sided = (p.flags & osm::material_two_sided) != 0;
        s.decal = (p.flags & osm::material_decal) != 0;
        s.z_write = (p.flags & osm::material_z_write) != 0;
        s.z_test = (p.flags & osm::material_z_test) != 0;
        if (refraction) {
            s.z_write = false; // tested against the scene's depth, never written
            s.z_test = true;
            g.flags |= k_material_refraction | ((p.skyrim_flags1 >> 16) & 1 ? k_material_fire_refraction : 0) |
                       ((p.skyrim_flags1 >> 13) & 1 ? k_material_refraction_clamp : 0);
            g.extra[2] = p.refraction_strength; // RefractionPower
        }
        material_states[i] = s.pack();
        // Shadow casters (5, 10): lighting materials, whatever their blending, alpha-tested when the material is. Point and
        // spot lights take them all, the sun's cascades those with the game (flags1.9), and the god-ray map the rest
        // if fully opaque, unblended and no decal.
        material_shadow[i] = 0;
        // The walk of the game: the Decal flag and not flags2 bit 3.
        material_blood[i] = weapon && (p.skyrim_flags1 >> 26 & 1) && !(p.skyrim_flags2 >> 3 & 1);
        if (!effect && !refraction) {
            const bool cast = (p.skyrim_flags1 & (1u << 9)) != 0;
            material_shadow[i] = shadow_caster | (s.two_sided ? shadow_two_sided : 0u) | (test ? shadow_alpha_test : 0u) |
                                 (cast ? shadow_sun : 0u);
            if (!cast && !lod && !s.decal && !is_blend_program(s.program) && p.alpha >= 1.0f) {
                material_shadow[i] |= shadow_god_ray;
            }
        }
    }

    // Animated materials (16.5): one record per material with controllers, shared by its submeshes.
    std::vector<uint32_t> material_anims(parsed.materials.size(), ~0u);
    for (size_t i = 0; i < parsed.materials.size(); ++i) {
        const auto& m = parsed.materials[i];
        if (m.controllers.empty()) {
            continue;
        }
        uint32_t record;
        if (!m_free_anims.empty()) {
            record = m_free_anims.back();
            m_free_anims.pop_back();
        } else {
            record = static_cast<uint32_t>(m_anims.size());
            m_anims.emplace_back();
        }
        MaterialAnimation& a = m_anims[record];
        const GpuMaterial& g = base[i];
        std::copy_n(g.uv, 4, a.base.uv);
        std::copy_n(g.specular, 4, a.base.specular);
        std::copy_n(g.emit, 4, a.base.emit);
        std::copy_n(g.effect, 4, a.base.effect);
        std::copy_n(g.base_color, 4, a.base.base_color);
        std::copy_n(g.falloff, 4, a.base.falloff);
        std::copy_n(g.extra, 4, a.base.extra);
        a.base.alpha_threshold = g.alpha_threshold;
        a.params = m.params;
        a.effect = m.params.model == osm::ShaderModel::effect;
        a.type = a.effect ? 0u : (g.technique >> 24) & 0x3Fu;
        a.palette_color = a.effect && (g.technique & 0x80000);
        a.refraction = (g.flags & k_material_refraction) != 0;
        a.controllers = m.controllers;
        a.live = true;
        material_anims[i] = record;
        e.anims.push_back(record);
    }

    const uint32_t vbase = static_cast<uint32_t>(e.vertices.offset / sizeof(osm::Vertex));
    const uint32_t ibase = static_cast<uint32_t>(e.indices.offset / sizeof(uint32_t));
    for (size_t si = 0; si < parsed.submeshes.size(); ++si) {
        const auto& sm = parsed.submeshes[si];
        // One GPU material per submesh: it carries the submesh's shape transform.
        GpuMaterial gm = base[sm.material];
        std::memcpy(gm.xform, sm.transform, sizeof(gm.xform));
        std::memcpy(gm.billboard, sm.billboard, sizeof(gm.billboard));
        gm.billboard_mode = sm.billboard_mode;
        if (sm.billboard_mode != ~0u) {
            gm.flags |= k_material_billboard;
        }
        gm.uv_anim = material_anims[sm.material];
        const uint32_t slot = m_materials.allocate(gm);
        if (slot == ~0u) {
            continue;
        }
        e.materials.push_back(slot);
        e.params.push_back(parsed.materials[sm.material].params);
        const uint32_t g = allocate_group();
        DrawGroup& dg = m_groups[g];
        dg.index_count = sm.index_count;
        dg.first_index = ibase + sm.first_index;
        dg.lod_first[0] = dg.first_index;
        dg.lod_count[0] = dg.index_count;
        for (uint32_t l = 0; l < osm::k_lod_levels; ++l) {
            const osm::SubmeshLod& lod = parsed.lods[si][l];
            dg.lod_first[l + 1] = lod.index_count > 0 ? ibase + lod.first_index : dg.lod_first[l];
            dg.lod_count[l + 1] = lod.index_count > 0 ? lod.index_count : dg.lod_count[l];
            if (lod.index_count > 0) {
                e.lod_error[l] = std::max(e.lod_error[l], lod.error);
            }
        }
        dg.vertex_offset = static_cast<int32_t>(vbase + sm.base_vertex);
        dg.material = slot;
        DrawState s = DrawState::unpack(material_states[sm.material]);
        if (s.program == program_mesh_opaque || s.program == program_mesh_alpha_test) {
            s.variant = mesh_variant(gm.technique, gm.flags); // the narrowest specialised pipeline that covers it
        }
        dg.state = s.pack();
        dg.state_slot = state_slot(dg.state);
        dg.shadow = material_blood[sm.material] ? 0 : material_shadow[sm.material];
        dg.blood = material_blood[sm.material] != 0;
        dg.mesh = id;
        dg.local = static_cast<uint32_t>(e.groups.size());
        dg.submesh = parsed.submesh_index[si];
        e.groups.push_back(g);
        if (parsed.skinned) {
            parsed.skinned->shapes[si].axes = (gm.technique & (1u << 2)) != 0; // model-space normals
            parsed.skinned->shapes[si].type = gm.technique >> 24;
        }
    }
    e.skinned = std::move(parsed.skinned);
    e.state = State::ready;
    ++m_ready_count;
    if (e.refs == 0) {
        touch_lru(id);
    }
}

void MeshManager::fail_load(MeshId id, const std::string& error) {
    Entry& e = m_entries[id];
    --m_in_flight;
    // Full arenas are a passing state: what's unused goes, and the mesh asks again, or at its next acquire. A body that
    // lost its gauntlets to this kept them lost through every change of gear.
    e.arena_full = error.find("arenas are full") != std::string::npos;
    if (e.arena_full) {
        for (int budget = 256; budget > 0 && !m_lru.empty(); --budget) {
            evict(m_lru.front());
        }
    }
    e.state = State::failed;
    ++m_failed_count;
    if (m_logged.insert(e.path).second) {
        log::warn("meshes: cannot load {}: {}", e.path, error);
    }
    if (e.refs == 0) {
        touch_lru(id);
    }
}

void MeshManager::evict(MeshId id) {
    Entry& e = m_entries[id];
    if (e.in_lru) {
        m_lru.erase(e.lru);
        e.in_lru = false;
    }
    if (e.state == State::ready) {
        --m_ready_count;
    } else if (e.state == State::failed) {
        --m_failed_count;
    }
    for (auto t : e.textures) {
        m_textures.release(t);
    }
    // Draw groups, materials and arena ranges may still be read by frames in flight.
    std::vector<uint32_t> materials = std::move(e.materials);
    std::vector<uint32_t> groups = e.groups;
    for (const auto& [base, variant] : e.variants) {
        if (variant < m_variant_groups.size()) {
            for (size_t k = 0; k < m_variant_groups[variant].size() && k < e.groups.size(); ++k) {
                if (m_variant_groups[variant][k] != e.groups[k]) {
                    groups.push_back(m_variant_groups[variant][k]);
                }
            }
            m_variant_groups[variant].clear();
        }
        // A variant shares the mesh's own material where it changes nothing; only its own slots are freed here.
        for (size_t k = 0; k < m_variants[variant].size() && k < materials.size(); ++k) {
            if (m_variants[variant][k] != materials[k] && m_variants[variant][k] != k_no_material) {
                materials.push_back(m_variants[variant][k]);
            }
        }
        m_variants[variant].clear();
        m_free_variants.push_back(variant);
    }
    m_device.defer([this, groups = std::move(groups), materials = std::move(materials),
                    anims = std::move(e.anims), v = e.vertices, i = e.indices, sv = e.skin_vertices, sm = e.skin_morph] {
        for (auto a : anims) {
            m_anims[a] = MaterialAnimation{};
            m_free_anims.push_back(a);
        }
        for (auto g : groups) {
            m_groups[g] = DrawGroup{};
            m_free_groups.push_back(g);
        }
        for (auto m : materials) {
            m_materials.free(m);
        }
        m_vertices.free(v);
        m_indices.free(i);
        m_vertices.free(sv);
        m_vertices.free(sm);
    });
    m_by_path.erase(e.path);
    e = Entry{};
    m_free_entries.push_back(id);
}

uint32_t MeshManager::skinned_variant(MeshId id, const std::function<void(uint32_t, IndexRuns&)>& ranges,
                                      const std::function<void(uint32_t, GpuMaterial&)>& edit,
                                      std::vector<uint64_t>& targets) {
    Entry& e = m_entries[id];
    targets.assign(e.groups.size(), 0);
    const ArenaRange range = m_vertices.allocate(e.vertices.size, sizeof(osm::Vertex));
    if (!range) {
        return k_no_material;
    }
    uint32_t variant;
    if (!m_free_variants.empty()) {
        variant = m_free_variants.back();
        m_free_variants.pop_back();
    } else {
        variant = static_cast<uint32_t>(m_variants.size());
        m_variants.emplace_back();
    }
    if (m_variant_groups.size() <= variant) {
        m_variant_groups.resize(variant + 1);
    }
    m_skinned_variants[variant] = range;
    auto& groups = m_variant_groups[variant];
    groups.clear();
    const int32_t shift = static_cast<int32_t>(range.offset / sizeof(osm::Vertex)) -
                          static_cast<int32_t>(e.vertices.offset / sizeof(osm::Vertex));
    IndexRuns runs;
    for (uint32_t k = 0; k < e.groups.size(); ++k) {
        runs.clear();
        ranges(k, runs);
        if (runs.empty()) {
            continue;
        }
        const DrawGroup source = m_groups[e.groups[k]];
        GpuMaterial g = m_materials.get(source.material);
        g.extra[1] = 1.0f;
        const uint32_t technique = g.technique;
        edit(k, g);
        const uint32_t slot = m_materials.allocate(g);
        if (slot == ~0u) {
            continue;
        }
        m_variants[variant].push_back(slot);
        DrawState state = DrawState::unpack(source.state);
        if (g.technique != technique && (state.program == program_mesh_opaque || state.program == program_mesh_alpha_test)) {
            state.variant = mesh_variant(g.technique, g.flags);
        }
        const uint32_t packed = state.pack();
        const uint32_t packed_slot = state_slot(packed);
        for (const auto& [first, count] : runs) {
            const uint32_t group = allocate_group();
            DrawGroup& copy = m_groups[group];
            copy = source;
            copy.material = slot;
            copy.state = packed;
            copy.state_slot = packed_slot;
            copy.vertex_offset = source.vertex_offset + shift;
            copy.first_index = source.first_index + first;
            copy.index_count = count;
            groups.push_back(group);
        }
        targets[k] = e.skinned->shapes[k].vertices + (range.offset - e.vertices.offset);
    }
    return variant;
}

void MeshManager::free_skinned_variant(uint32_t variant) {
    auto it = m_skinned_variants.find(variant);
    if (it == m_skinned_variants.end()) {
        return;
    }
    // A frame in flight may still draw them.
    m_device.defer([this, variant, range = it->second, groups = std::move(m_variant_groups[variant]),
                    materials = std::move(m_variants[variant])] {
        for (auto g : groups) {
            m_groups[g] = DrawGroup{};
            m_free_groups.push_back(g);
        }
        for (auto m : materials) {
            m_materials.free(m);
        }
        m_vertices.free(range);
        m_variants[variant].clear();
        m_free_variants.push_back(variant);
    });
    m_variant_groups[variant].clear();
    m_skinned_variants.erase(it);
}

uint32_t MeshManager::multipass_variant(MeshId id, const osw::DirectionalMaterial& d, const osw::DirectionalPass& pass) {
    Entry& e = m_entries[id];
    const uint32_t key = d.base_form_id | 0x80000000u; // never a variant of the single-pass kind (FormIDs < 2^31 here)
    for (const auto& [base, variant] : e.variants) {
        if (base == key) {
            return variant;
        }
    }
    // MIS: (falloff scale, falloff bias, 1 / noise UV scale, cos(max angle)), the cosine forced to 0 when
    // |cos| <= 0.01 (11.2.3); ProjectedUVParams as 11.4.2 and the multi-pass ProjectedUVParams2.
    float c = std::cos(d.max_angle * 0.017453292f);
    if (std::abs(c) <= 0.01f) {
        c = 0.0f;
    }
    // TextureProj: the game of d = -projection vector, rows (c, b, d) (11.4.1).
    glm::vec3 dir = -glm::vec3(d.projection[0], d.projection[1], d.projection[2]);
    dir = glm::length(dir) > 0.0f ? glm::normalize(dir) : glm::vec3(0, 0, 1);
    const glm::vec3 ad = glm::abs(dir);
    glm::vec3 p0 = ad.x <= ad.y && ad.x <= ad.z ? glm::vec3(0.0f, -dir.z, dir.y)
                   : ad.y <= ad.z                ? glm::vec3(-dir.z, 0.0f, dir.x)
                                                 : glm::vec3(-dir.y, dir.x, 0.0f);
    const glm::vec3 a = glm::normalize(p0);
    const glm::vec3 b = glm::normalize(glm::cross(a, dir));
    const glm::vec3 cc = glm::normalize(glm::cross(b, dir));
    const TextureId diffuse = m_textures.acquire(pass.diffuse, TextureKind::color, 1.0f);
    e.textures.push_back(diffuse);
    TextureId back = m_textures.black();
    const bool back_lit = (pass.flags2 >> 27) & 1;
    if (back_lit) {
        back = m_textures.acquire(pass.back_light, TextureKind::color, 1.0f);
        e.textures.push_back(back);
    }
    std::vector<uint32_t> materials(e.groups.size(), k_no_material);
    for (size_t k = 0; k < e.groups.size(); ++k) {
        const DrawGroup& dg = m_groups[e.groups[k]];
        const osm::MaterialParams& p = e.params[k];
        if (p.model == osm::ShaderModel::effect ||
            (p.flags & (osm::material_alpha_blend | osm::material_alpha_test | osm::material_decal)) ||
            (p.skyrim_flags1 & 0x8004)) {
            continue;
        }
        const GpuMaterial own = m_materials.get(dg.material);
        GpuMaterial g{};
        g.uv_anim = ~0u;
        // Technique (11.2.3 table): type 14 + projected UV + specular, vertex colours from the object's mesh, snow from
        // the MATO with improved snow (the clone never has flag 61), back lighting from the DNAM's flags2.27.
        g.technique = (14u << 24) | (1u << 15) | (1u << 9) | (own.technique & 1u) | (back_lit ? 1u << 12 : 0u) |
                      ((d.flags & osw::directional_mato_snow) && m_improved_snow ? 1u << 21 : 0u);
        g.flags = osm::material_z_write | osm::material_z_test |
                  (own.flags & (osm::material_two_sided | k_material_billboard));
        g.alpha_func = 4;
        for (auto& t : g.tex) {
            t = m_textures.black();
        }
        g.tex[0] = diffuse;
        g.tex[1] = own.tex[1]; // the object's normal map
        g.tex[8] = back;
        g.sampler = 3;
        g.uv[0] = pass.uv_offset[0];
        g.uv[1] = pass.uv_offset[1];
        g.uv[2] = pass.uv_scale[0];
        g.uv[3] = pass.uv_scale[1];
        for (int i = 0; i < 3; ++i) {
            g.specular[i] = pass.specular[i];
            g.emit[i] = pass.emissive[i];
        }
        g.specular[3] = pass.glossiness;
        g.emit[3] = pass.alpha;
        g.effect[0] = own.effect[0];
        g.effect[1] = own.effect[1];
        std::copy_n(pass.sparkle, 4, g.sparkle);
        std::memcpy(g.xform, own.xform, sizeof g.xform);
        std::memcpy(g.billboard, own.billboard, sizeof g.billboard);
        g.billboard_mode = own.billboard_mode;
        g.projected[0] = (1.0f - c) * d.falloff_scale;
        g.projected[2] = 1.0f / d.noise_uv_scale;
        g.projected[3] = (1.0f - c) * d.falloff_bias + c;
        g.projected_color[0] = d.normal_dampener;
        g.projected_color[1] = std::max(1.0f / d.material_uv_scale, 1e-4f);
        for (int i = 0; i < 3; ++i) {
            g.projection[0][i] = cc[i];
            g.projection[1][i] = b[i];
            g.projection[2][i] = dir[i];
        }
        g.base_color[0] = g.base_color[1] = g.base_color[2] = g.base_color[3] = 1.0f;
        const uint32_t slot = m_materials.allocate(g);
        if (slot != ~0u) {
            materials[k] = slot;
        }
    }
    uint32_t variant;
    if (!m_free_variants.empty()) {
        variant = m_free_variants.back();
        m_free_variants.pop_back();
    } else {
        variant = static_cast<uint32_t>(m_variants.size());
        m_variants.emplace_back();
    }
    m_variants[variant] = std::move(materials);
    e.variants.emplace_back(key, variant);
    return variant;
}

uint32_t MeshManager::state_slot(uint32_t state) {
    auto [it, inserted] = m_state_slots.try_emplace(state, static_cast<uint32_t>(m_states.size()));
    if (inserted) {
        m_states.push_back(state);
    }
    return it->second;
}

uint32_t MeshManager::directional_variant(MeshId id, const osw::DirectionalMaterial& d, const ots::Swap* swap) {
    Entry& e = m_entries[id];
    for (const auto& [base, variant] : e.variants) {
        if (base == d.base_form_id) {
            return variant;
        }
    }
    // The game set these flags on the reference's lighting properties
    // (11.2): 23 projected UV, 60 the MATO's snow, 30 the STAT's snow flag. The multi-pass MATO leaves the
    // object's own pass alone and adds a second one (11.2.3).
    const bool projected = (d.flags & osw::directional_projected) != 0;
    const float c = std::cos(d.max_angle * 0.017453292f);
    std::vector<uint32_t> materials(e.groups.size(), 0);
    for (size_t k = 0; k < e.groups.size(); ++k) {
        const DrawGroup& dg = m_groups[e.groups[k]];
        materials[k] = dg.material;
        osm::MaterialParams p = e.params[k];
        if (p.model == osm::ShaderModel::effect) {
            continue;
        }
        if (projected) {
            p.skyrim_flags1 |= 1u << 23;
            if (d.flags & osw::directional_mato_snow) {
                p.skyrim_flags2 |= 1u << (60 - 32);
            }
        }
        if (d.flags & osw::directional_base_snow) {
            p.skyrim_flags1 |= 1u << 30;
        }
        GpuMaterial g = m_materials.get(dg.material);
        const uint32_t technique = lighting_technique(p, m_improved_snow);
        // Alternate textures: the slots the model's material samples take the set's (docs/formats/ots.md).
        bool swapped = false;
        for (const ots::Shape& shape : swap ? std::span(swap->shapes) : std::span<const ots::Shape>{}) {
            if (shape.submesh != dg.submesh) {
                continue;
            }
            const auto take = [&](uint32_t slot, TextureKind kind, uint32_t& out) {
                if (!shape.textures[slot].empty()) {
                    const TextureId t = m_textures.acquire(shape.textures[slot], kind, e.priority);
                    e.textures.push_back(t);
                    out = t;
                    swapped = true;
                }
            };
            take(osm::texture_diffuse, TextureKind::color, g.tex[gtex_diffuse]);
            take(osm::texture_normal, TextureKind::normal, g.tex[gtex_normal]);
            if (g.tex[gtex_glow] != m_textures.black()) {
                take(osm::texture_glow, TextureKind::color, g.tex[gtex_glow]);
            }
            if (g.tex[gtex_env_mask] != m_textures.black()) {
                take(osm::texture_env_mask, TextureKind::color, g.tex[gtex_env_mask]);
            }
        }
        if (technique == g.technique && !swapped) {
            continue; // unchanged: share the mesh's own material
        }
        g.technique = technique;
        if (projected) {
            // The game (11.4).
            g.projected[0] = (1.0f - c) * d.falloff_scale;
            g.projected[1] = 0.0f;
            g.projected[2] = 1.0f / d.noise_uv_scale;
            g.projected[3] = (1.0f - c) * d.falloff_bias + c;
            for (int i = 0; i < 3; ++i) {
                g.projected_color[i] = d.color[i];
            }
            g.projected_color[3] = 1.0f;
        }
        const uint32_t slot = m_materials.allocate(g);
        if (slot != ~0u) {
            materials[k] = slot;
        }
    }
    uint32_t variant;
    if (!m_free_variants.empty()) {
        variant = m_free_variants.back();
        m_free_variants.pop_back();
    } else {
        variant = static_cast<uint32_t>(m_variants.size());
        m_variants.emplace_back();
    }
    // The references with this variant draw their own groups where the material changed: the same geometry with the
    // variant's material and the specialised pipeline that covers it (pipelines.hpp, MeshVariant). The mesh's own
    // groups keep theirs, so what any reference draws never depends on which variants were made.
    std::vector<uint32_t> groups(e.groups.size());
    for (size_t k = 0; k < e.groups.size(); ++k) {
        groups[k] = e.groups[k];
        if (materials[k] == m_groups[e.groups[k]].material) {
            continue;
        }
        DrawGroup copy = m_groups[e.groups[k]];
        copy.material = materials[k];
        DrawState s = DrawState::unpack(copy.state);
        if (s.program == program_mesh_opaque || s.program == program_mesh_alpha_test) {
            const GpuMaterial m = m_materials.get(materials[k]);
            s.variant = mesh_variant(m.technique, m.flags);
            copy.state = s.pack();
            copy.state_slot = state_slot(copy.state);
        }
        groups[k] = allocate_group();
        m_groups[groups[k]] = copy;
    }
    if (m_variant_groups.size() < m_variants.size()) {
        m_variant_groups.resize(m_variants.size());
    }
    m_variant_groups[variant] = std::move(groups);
    m_variants[variant] = std::move(materials);
    e.variants.emplace_back(d.base_form_id, variant);
    return variant;
}

MeshStats MeshManager::stats() const {
    MeshStats s;
    s.live = static_cast<uint32_t>(m_by_path.size());
    s.ready = m_ready_count;
    s.failed = m_failed_count;
    s.loading = static_cast<uint32_t>(m_queued + m_in_flight);
    s.referenced = s.live - static_cast<uint32_t>(m_lru.size());
    s.vertex_bytes = m_vertices.used();
    s.index_bytes = m_indices.used();
    s.vertex_capacity = m_vertices.capacity();
    s.index_capacity = m_indices.capacity();
    return s;
}

// ---- Animated materials

namespace {

// The game for an APP_TIME controller: frequency and phase, the cycle type (0 loop,
// 1 reverse, 2 clamp), then a clamp to [start, stop] and play-backwards (flag 0x10).
float controller_time(const osm::MaterialController& c, float t) {
    const float lo = c.start, hi = c.stop, span = hi - lo;
    float s = c.frequency * t + c.phase;
    const uint32_t cycle = (c.flags >> 1) & 3u;
    if (cycle == 0) {
        if (span > 0.0f) {
            float m = std::fmod(s - lo, span);
            s = lo + (m < 0.0f ? m + span : m);
        } else {
            s = lo;
        }
    } else if (cycle == 1 && span > 0.0f) {
        float m = std::fmod(s, 2.0f * span); // the engine doesn't subtract start first
        if (m < 0.0f) {
            m += 2.0f * span;
        }
        s = m <= span ? lo + m : lo + 2.0f * span - m;
    }
    s = s > hi ? hi : std::max(lo, s);
    if (c.flags & 0x10) {
        s = hi - (s - lo);
    }
    return s;
}

// before the first key the first interval extrapolates, past the last key the last value holds;
// linear, quadratic (Hermite with the left key's outgoing and the right key's incoming tangent, per whole interval)
// or constant (steps at the right key) between keys.
float key_value(const osm::MaterialController& c, float t) {
    const auto& k = c.keys;
    if (k.size() <= 1) {
        return k.empty() ? c.value : k[0][1];
    }
    for (size_t i = 1; i < k.size(); ++i) {
        if (t <= k[i][0] || i + 1 == k.size()) {
            if (t > k[i][0]) {
                return k[i][1];
            }
            const auto& k0 = k[i - 1];
            const auto& k1 = k[i];
            const float u = (t - k0[0]) / (k1[0] - k0[0]);
            if (c.key_type == 5) {
                return u >= 1.0f ? k1[1] : k0[1];
            }
            if (c.key_type == 2) {
                const float d = k1[1] - k0[1], m0 = k0[3], m1 = k1[2];
                return k0[1] + u * (m0 + u * ((3.0f * d - 2.0f * m0 - m1) + u * (m0 + m1 - 2.0f * d)));
            }
            return (1.0f - u) * k0[1] + u * k1[1];
        }
    }
    return k.back()[1];
}

} // namespace

uint32_t MeshManager::allocate_material_block() {
    uint32_t record;
    if (!m_free_anims.empty()) {
        record = m_free_anims.back();
        m_free_anims.pop_back();
    } else {
        record = static_cast<uint32_t>(m_anims.size());
        m_anims.emplace_back();
    }
    m_anims[record] = {};
    m_anims[record].live = m_anims[record].block = true;
    return record;
}

void MeshManager::free_material_block(uint32_t block) {
    m_anims[block].live = m_anims[block].block = false;
    m_free_anims.push_back(block);
}

void MeshManager::write_material_anims(float time, float previous_time, GpuMaterialAnim* out) const {
    for (size_t r = 0; r < m_anims.size(); ++r) {
        const MaterialAnimation& a = m_anims[r];
        if (!a.live) {
            continue;
        }
        if (a.block) {
            out[r] = a.base;
            continue;
        }
        float v[osm::k_material_variable_count];
        uint32_t set = 0;
        for (const auto& c : a.controllers) {
            // UV values are written into the material slot the renderer reads next frame (16.5).
            const float t = c.variable <= osm::uv_scale_v ? previous_time : time;
            v[c.variable] = key_value(c, controller_time(c, t));
            set |= 1u << c.variable;
        }
        auto has = [&](uint32_t var) { return (set >> var) & 1u; };
        auto get = [&](uint32_t var, float fallback) { return has(var) ? v[var] : fallback; };
        const osm::MaterialParams& p = a.params;
        GpuMaterialAnim g = a.base;
        for (uint32_t i = 0; i < 4; ++i) {
            g.uv[i] = get(osm::uv_offset_u + i, g.uv[i]);
        }
        const float alpha = get(osm::var_alpha, p.alpha);
        if (a.effect) {
            const float scale = get(osm::var_base_color_scale, p.base_color_scale);
            for (int c = 0; c < 3; ++c) {
                g.base_color[c] = get(osm::var_base_r + c, p.base_color[c]) * (a.palette_color ? 1.0f : scale);
            }
            g.base_color[3] = has(osm::var_base_alpha) ? std::max(v[osm::var_base_alpha], 0.0f) : g.base_color[3];
            g.emit[3] = g.base_color[3]; // the property alpha follows the material's (SetMaterialAlpha)
            g.extra[1] = scale;
            // The keys are in degrees; the material keeps the cosine.
            constexpr float k_deg = 3.14159265f / 180.0f;
            if (has(osm::var_falloff_start)) {
                g.falloff[0] = std::cos(v[osm::var_falloff_start] * k_deg);
            }
            if (has(osm::var_falloff_stop)) {
                g.falloff[1] = std::cos(v[osm::var_falloff_stop] * k_deg);
            }
            g.falloff[2] = get(osm::var_falloff_start_opacity, g.falloff[2]);
            g.falloff[3] = get(osm::var_falloff_stop_opacity, g.falloff[3]);
        } else {
            const float strength = get(osm::var_specular_strength, p.specular_strength);
            const float multiple = get(osm::var_emissive_multiple, p.emissive_multiple);
            for (int c = 0; c < 3; ++c) {
                g.specular[c] = get(osm::var_specular_r + c, p.specular_color[c]) * strength;
                g.emit[c] = get(osm::var_emissive_r + c, p.emissive_color[c]) * multiple;
            }
            g.specular[3] = get(osm::var_glossiness, g.specular[3]);
            g.emit[3] = alpha;
            if (a.type == 1 && has(osm::var_env_map_scale)) {
                g.effect[2] = v[osm::var_env_map_scale];
            }
            if (a.refraction && has(osm::var_refraction_strength)) {
                g.extra[2] = v[osm::var_refraction_strength];
            }
        }
        // The game with the animated threshold (an integer, 16.5) and alpha.
        const float threshold = has(osm::var_alpha_test_ref)
                                    ? std::trunc(std::clamp(v[osm::var_alpha_test_ref], 0.0f, 255.0f))
                                    : float(p.alpha_threshold);
        if (has(osm::var_alpha_test_ref) || has(osm::var_alpha)) {
            g.alpha_threshold = float(int(threshold * alpha)) / 255.0f;
        }
        out[r] = g;
    }
}

} // namespace spud::render
