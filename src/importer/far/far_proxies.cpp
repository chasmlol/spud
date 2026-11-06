// Region proxies: the large static objects of each square of cells, merged per material and
// simplified. They keep their materials, on low-resolution copies of the same textures, so they light as the real
// objects do and the whole map needs only a small set of textures resident.

#include "importer/far/far_view.hpp"

#include "core/file.hpp"
#include "core/hash.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "formats/dds.hpp"
#include "formats/osm.hpp"
#include "importer/far/texels.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <meshoptimizer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace spud::importer::far {

namespace {

constexpr int32_t k_level1_cells = 4;
constexpr int32_t k_level2_cells = 8;
// Metres. A region's proxy is drawn from about 700 m, where a pixel at 1440p is 0.4 m; level 2 from 3 km, 1.8 m.
constexpr float k_level1_error = 0.3f;
constexpr float k_level2_error = 1.5f;
// Objects smaller than this are under a pixel or two by then (ARCHITECTURE.md: small clutter is left out).
constexpr float k_level1_min_radius = 1.0f;
constexpr float k_level2_min_radius = 4.0f;
constexpr uint32_t k_texture_texels = 256;

uint8_t pack(float v) { return uint8_t(std::clamp((v + 1.0f) * 127.5f + 0.5f, 0.0f, 255.0f)); }
glm::vec3 unpack(const uint8_t* b) { return glm::vec3(b[0], b[1], b[2]) * (2.0f / 255.0f) - 1.0f; }

// The shader features a proxy has no use for at its distance, or no texture left for.
void strip_material(osm::MaterialParams& p) {
    constexpr uint32_t f1 = (1u << 7) | (1u << 10) | (1u << 11) | (1u << 14) | (1u << 17) | (1u << 18) | (1u << 21) |
                            (1u << 28);
    constexpr uint32_t f2 = (1u << 1) | (1u << 2) | (1u << 6) | (1u << 9) | (1u << 14) | (1u << 24) | (1u << 25) |
                            (1u << 26) | (1u << 27) | (1u << 29) | (1u << 31);
    p.skyrim_flags1 &= ~f1;
    p.skyrim_flags2 &= ~f2;
    p.skyrim_shader_type = 0;
    p.flags &= ~(osm::material_glow | osm::material_env_map | osm::material_tree_anim);
}

class MeshCache {
public:
    explicit MeshCache(std::filesystem::path root) : m_root(std::move(root)) {}
    std::shared_ptr<const osm::Mesh> get(const std::string& path) {
        std::lock_guard lock(m_mutex);
        auto& slot = m_meshes[path];
        if (!slot) {
            try {
                slot = std::make_shared<osm::Mesh>(osm::read(read_file(m_root / path)));
            } catch (const std::exception&) {
                slot = std::make_shared<osm::Mesh>();
            }
        }
        return slot;
    }

private:
    std::filesystem::path m_root;
    std::mutex m_mutex;
    std::unordered_map<std::string, std::shared_ptr<const osm::Mesh>> m_meshes;
};

// Low-resolution copies of source textures, written once per path.
class TextureCopies {
public:
    TextureCopies(TexelCache& texels, const std::filesystem::path& cache) : m_texels(texels), m_cache(cache) {}
    std::string copy(const std::string& path, bool keep_alpha) {
        if (path.empty()) {
            return path;
        }
        const std::string out = std::format("far/tex/{:016x}.dds", xxh64(path));
        {
            std::lock_guard lock(m_mutex);
            if (!m_done.insert(out).second) {
                return out;
            }
        }
        const auto level = m_texels.get(path, k_texture_texels);
        if (!level || !*level) {
            return path;
        }
        Canvas canvas(level->width, level->height);
        for (uint32_t y = 0; y < level->height; ++y) {
            for (uint32_t x = 0; x < level->width; ++x) {
                const uint8_t* s = level->at(x, y);
                float* d = canvas.at(x, y);
                for (int c = 0; c < 4; ++c) {
                    d[c] = float(s[c]) * (1.0f / 255.0f);
                }
            }
        }
        write_file(m_cache / out, dds::write(encode(canvas, keep_alpha ? Encoding::bc3 : Encoding::bc1, false, 1)));
        return out;
    }

private:
    TexelCache& m_texels;
    std::filesystem::path m_cache;
    std::mutex m_mutex;
    std::set<std::string> m_done;
};

struct Group {
    osm::Material material;
    std::vector<osm::Vertex> vertices;
    std::vector<uint32_t> indices;
};

// The groups of one proxy mesh, by material.
using Groups = std::map<std::string, Group>;

std::string material_key(const osm::Material& m) {
    std::string key = m.textures[osm::texture_diffuse] + "|" + m.textures[osm::texture_normal] + "|";
    key.append(reinterpret_cast<const char*>(&m.params), sizeof(m.params));
    return key;
}

void add_ref(Groups& groups, const osm::Mesh& mesh, const osw::Ref& ref, const glm::vec3& origin) {
    const glm::mat3 rot = glm::mat3_cast(glm::quat(ref.rotation[3], ref.rotation[0], ref.rotation[1], ref.rotation[2]));
    const glm::vec3 position(ref.position[0], ref.position[1], ref.position[2]);
    for (const osm::Submesh& s : mesh.submeshes) {
        const osm::Material& source = mesh.materials[s.material];
        const osm::MaterialParams& p = source.params;
        if (p.model != osm::ShaderModel::lighting || s.billboard_mode != osm::k_no_billboard ||
            (p.flags & (osm::material_alpha_blend | osm::material_alpha_test | osm::material_decal)) ||
            !(p.flags & osm::material_z_write)) {
            continue;
        }
        osm::Material material;
        material.params = p;
        strip_material(material.params);
        material.textures[osm::texture_diffuse] = source.textures[osm::texture_diffuse];
        material.textures[osm::texture_normal] = source.textures[osm::texture_normal];
        Group& g = groups[material_key(material)];
        if (g.vertices.empty()) {
            g.material = std::move(material);
        }
        glm::mat3 linear;
        glm::vec3 shift;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                linear[c][r] = s.transform[r][c];
            }
            shift[r] = s.transform[r][3];
        }
        const glm::mat3 frame = rot * linear;
        const float frame_scale = glm::length(linear[0]);
        const glm::mat3 unit = frame_scale > 0 ? frame / frame_scale : frame;
        const uint32_t base = uint32_t(g.vertices.size());
        g.vertices.reserve(base + s.vertex_count);
        for (uint32_t i = 0; i < s.vertex_count; ++i) {
            osm::Vertex v = mesh.vertices[s.base_vertex + i];
            const glm::vec3 local(v.position[0], v.position[1], v.position[2]);
            const glm::vec3 world = rot * (ref.scale * (linear * local + shift)) + position - origin;
            const glm::vec3 n = unit * unpack(v.normal);
            const glm::vec3 t = unit * unpack(v.tangent);
            const glm::vec3 b = unit * glm::vec3(v.bitangent_x, float(v.normal[3]) * (2.0f / 255.0f) - 1.0f,
                                                 float(v.tangent[3]) * (2.0f / 255.0f) - 1.0f);
            v.position[0] = world.x;
            v.position[1] = world.y;
            v.position[2] = world.z;
            v.bitangent_x = b.x;
            v.normal[0] = pack(n.x);
            v.normal[1] = pack(n.y);
            v.normal[2] = pack(n.z);
            v.normal[3] = pack(b.y);
            v.tangent[0] = pack(t.x);
            v.tangent[1] = pack(t.y);
            v.tangent[2] = pack(t.z);
            v.tangent[3] = pack(b.z);
            g.vertices.push_back(v);
        }
        for (uint32_t i = 0; i < s.index_count; ++i) {
            g.indices.push_back(base + mesh.indices[s.first_index + i]);
        }
    }
}

// One proxy mesh from its groups, each simplified to `error` metres.
osm::Mesh build_mesh(Groups& groups, float error, TextureCopies& textures) {
    osm::Mesh out;
    for (auto& [key, g] : groups) {
        if (g.indices.empty()) {
            continue;
        }
        std::vector<uint32_t> indices(g.indices.size());
        // Across UV seams too: the seams of a rock are worth less than its triangles from here. Pieces under the error
        // go altogether.
        const size_t count = meshopt_simplify(indices.data(), g.indices.data(), g.indices.size(),
                                              &g.vertices[0].position[0], g.vertices.size(), sizeof(osm::Vertex), 0, error,
                                              meshopt_SimplifyErrorAbsolute | meshopt_SimplifyPermissive |
                                                  meshopt_SimplifyPrune,
                                              nullptr);
        indices.resize(count);
        if (indices.empty()) {
            continue;
        }
        std::vector<uint32_t> remap(g.vertices.size());
        const size_t unique =
            meshopt_optimizeVertexFetchRemap(remap.data(), indices.data(), indices.size(), g.vertices.size());
        osm::Submesh s{};
        s.first_index = uint32_t(out.indices.size());
        s.index_count = uint32_t(indices.size());
        s.base_vertex = uint32_t(out.vertices.size());
        s.vertex_count = uint32_t(unique);
        s.material = uint32_t(out.materials.size());
        out.vertices.resize(out.vertices.size() + unique);
        meshopt_remapVertexBuffer(out.vertices.data() + s.base_vertex, g.vertices.data(), g.vertices.size(),
                                  sizeof(osm::Vertex), remap.data());
        meshopt_remapIndexBuffer(indices.data(), indices.data(), indices.size(), remap.data());
        meshopt_optimizeVertexCache(indices.data(), indices.data(), indices.size(), unique);
        out.indices.insert(out.indices.end(), indices.begin(), indices.end());
        out.submeshes.push_back(s);
        // A region spans kilometres of distance, and what is a pixel at its near edge is a quarter of one at its far edge:
        // the same chain as any mesh's (section 7), each level measured from the original, so it draws at a pixel of error.
        out.lods.resize(out.submeshes.size() * osm::k_lod_levels);
        uint32_t previous = s.index_count;
        for (uint32_t l = 0; l < osm::k_lod_levels; ++l) {
            std::vector<uint32_t> level(indices.size());
            float reached = 0.0f;
            const size_t kept = meshopt_simplify(level.data(), indices.data(), indices.size(),
                                                 &out.vertices[s.base_vertex].position[0], unique, sizeof(osm::Vertex), 0,
                                                 error * float((2u << l) - 1u),
                                                 meshopt_SimplifyErrorAbsolute | meshopt_SimplifyPermissive |
                                                     meshopt_SimplifyPrune,
                                                 &reached);
            if (kept == 0 || float(kept) > 0.85f * float(previous)) {
                continue;
            }
            meshopt_optimizeVertexCache(level.data(), level.data(), kept, unique);
            osm::SubmeshLod& lod = out.lods[(out.submeshes.size() - 1) * osm::k_lod_levels + l];
            lod.first_index = uint32_t(out.indices.size());
            lod.index_count = uint32_t(kept);
            lod.error = error + reached;
            out.indices.insert(out.indices.end(), level.begin(), level.begin() + std::ptrdiff_t(kept));
            previous = uint32_t(kept);
        }
        osm::Material m = g.material;
        m.textures[osm::texture_diffuse] = textures.copy(m.textures[osm::texture_diffuse], false);
        m.textures[osm::texture_normal] = textures.copy(m.textures[osm::texture_normal], true);
        out.materials.push_back(std::move(m));
    }
    osm::compute_bounds(out);
    return out;
}

struct RegionWork {
    int32_t x, y, cells, level;
    std::vector<uint32_t> refs; // indices into World::refs
    osh::Region region{};
    std::vector<std::pair<std::string, uint32_t>> meshes; // path, directional
};

} // namespace

void bake_proxies(const osw::World& w, TexelCache& texels, const std::filesystem::path& cache, osh::FarView& view,
                  unsigned threads) {
    MeshCache meshes(cache);
    TextureCopies textures(texels, cache);
    const std::string edid = path::to_lower(w.edid);
    const float cs = w.cell_size;
    // Bounds of each model, for leaving out what is too small.
    std::map<uint32_t, float> radius_of_mesh;
    // World::directional_materials has one entry per base object: the same material under many names merges here.
    std::map<std::string, uint32_t> directional_by_content;
    std::vector<uint32_t> directional_class(w.directional_materials.size());
    for (uint32_t i = 0; i < w.directional_materials.size(); ++i) {
        osw::DirectionalMaterial d = w.directional_materials[i];
        d.base_form_id = 0;
        const std::string key(reinterpret_cast<const char*>(&d), sizeof(d));
        directional_class[i] = directional_by_content.emplace(key, i).first->second;
    }
    std::map<std::pair<int32_t, int32_t>, RegionWork> squares[2];
    const int32_t x0 = view.cell_min[0], y0 = view.cell_min[1];
    auto square = [](int32_t v, int32_t origin, int32_t size) { return origin + (v - origin) / size * size; };
    for (const osw::Cell& c : w.cells) {
        const uint64_t end = std::min<uint64_t>(uint64_t(c.first_ref) + c.ref_count, w.refs.size());
        for (uint64_t i = c.first_ref; i < end; ++i) {
            const osw::Ref& r = w.refs[i];
            if (!(r.flags & osw::ref_occludable) || (r.flags & osw::ref_initially_disabled) || r.mesh >= w.meshes.size()) {
                continue;
            }
            auto it = radius_of_mesh.find(r.mesh);
            if (it == radius_of_mesh.end()) {
                const auto mesh = meshes.get(w.meshes[r.mesh]);
                const glm::vec3 lo(mesh->bounds_min[0], mesh->bounds_min[1], mesh->bounds_min[2]);
                const glm::vec3 hi(mesh->bounds_max[0], mesh->bounds_max[1], mesh->bounds_max[2]);
                it = radius_of_mesh.emplace(r.mesh, glm::length(hi - lo) * 0.5f).first;
            }
            const float radius = it->second * r.scale;
            if (radius < k_level1_min_radius) {
                continue;
            }
            const int32_t sx1 = square(c.x, x0, k_level1_cells), sy1 = square(c.y, y0, k_level1_cells);
            RegionWork& one = squares[0][{sx1, sy1}];
            one.x = sx1, one.y = sy1, one.cells = k_level1_cells, one.level = 1;
            one.refs.push_back(uint32_t(i));
            if (radius >= k_level2_min_radius) {
                const int32_t sx2 = square(c.x, x0, k_level2_cells), sy2 = square(c.y, y0, k_level2_cells);
                RegionWork& two = squares[1][{sx2, sy2}];
                two.x = sx2, two.y = sy2, two.cells = k_level2_cells, two.level = 2;
                two.refs.push_back(uint32_t(i));
            }
        }
    }
    std::vector<RegionWork*> work;
    for (auto& level : squares) {
        for (auto& [key, r] : level) {
            work.push_back(&r);
        }
    }
    std::atomic<size_t> next{0};
    std::atomic<uint64_t> triangles_in{0}, triangles_out[2] = {0, 0};
    auto run = [&] {
        for (size_t i = next++; i < work.size(); i = next++) {
            RegionWork& r = *work[i];
            const glm::vec3 origin((float(r.x) + float(r.cells) * 0.5f) * cs, (float(r.y) + float(r.cells) * 0.5f) * cs, 0.0f);
            const float error = r.level == 1 ? k_level1_error : k_level2_error;
            std::map<uint32_t, Groups> by_directional;
            for (uint32_t ri : r.refs) {
                const osw::Ref& ref = w.refs[ri];
                const auto mesh = meshes.get(w.meshes[ref.mesh]);
                const uint32_t directional =
                    ref.directional < directional_class.size() ? directional_class[ref.directional] : osw::k_none;
                add_ref(by_directional[directional], *mesh, ref, origin);
            }
            glm::vec3 lo(1e30f), hi(-1e30f);
            uint32_t part = 0;
            for (auto& [directional, groups] : by_directional) {
                for (const auto& [key, g] : groups) {
                    triangles_in += g.indices.size() / 3;
                }
                osm::Mesh mesh = build_mesh(groups, error, textures);
                if (mesh.indices.empty()) {
                    continue;
                }
                triangles_out[r.level - 1] += mesh.indices.size() / 3;
                lo = glm::min(lo, glm::vec3(mesh.bounds_min[0], mesh.bounds_min[1], mesh.bounds_min[2]));
                hi = glm::max(hi, glm::vec3(mesh.bounds_max[0], mesh.bounds_max[1], mesh.bounds_max[2]));
                const std::string path = std::format("far/{}/r{}_{}_{}_{}.osm", edid, r.level, r.x, r.y, part++);
                write_file(cache / path, osm::write(mesh));
                r.meshes.emplace_back(path, directional);
            }
            r.region.x = r.x;
            r.region.y = r.y;
            r.region.level = uint32_t(r.level);
            r.region.error = error;
            for (int c = 0; c < 3; ++c) {
                r.region.lo[c] = lo[c] + origin[c];
                r.region.hi[c] = hi[c] + origin[c];
                r.region.centre[c] = origin[c];
            }
            r.region.parent = osh::k_none;
        }
    };
    {
        std::vector<std::jthread> pool;
        for (unsigned t = 0; t < threads; ++t) {
            pool.emplace_back(run);
        }
    }
    std::map<std::pair<int32_t, int32_t>, uint32_t> level2_index;
    for (const RegionWork* r : work) { // level 1 first, then level 2
        if (r->meshes.empty()) {
            continue;
        }
        osh::Region region = r->region;
        region.first_proxy = uint32_t(view.proxies.size());
        region.proxy_count = uint32_t(r->meshes.size());
        for (const auto& [path, directional] : r->meshes) {
            view.proxies.push_back({uint32_t(view.meshes.size()), directional == osw::k_none ? osh::k_none : directional});
            view.meshes.push_back(path);
        }
        if (r->level == 2) {
            level2_index[{r->x, r->y}] = uint32_t(view.regions.size());
        }
        view.regions.push_back(region);
    }
    for (osh::Region& region : view.regions) {
        if (region.level == 1) {
            const auto it = level2_index.find({square(region.x, x0, k_level2_cells), square(region.y, y0, k_level2_cells)});
            region.parent = it != level2_index.end() ? it->second : osh::k_none;
        }
    }
    log::info("far view: {}: {} regions, {} proxies, {} thousand triangles to {} at level 1, {} at level 2", w.edid,
              view.regions.size(), view.proxies.size(), triangles_in.load() / 1000, triangles_out[0].load() / 1000,
              triangles_out[1].load() / 1000);
}

} // namespace spud::importer::far
