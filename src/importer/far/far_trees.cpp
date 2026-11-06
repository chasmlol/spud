// Tree impostors: each tree model drawn from many directions into an atlas of albedo, coverage and normals
// and every placed tree.

#include "importer/far/far_view.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "formats/dds.hpp"
#include "formats/osm.hpp"
#include "importer/far/texels.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <map>
#include <thread>

namespace spud::importer::far {

namespace {

constexpr uint32_t k_frames = 8;       // views across the hemisphere's octahedron
constexpr uint32_t k_frame_texels = 128;
constexpr uint32_t k_supersample = 2;
constexpr uint32_t k_diffuse_texels = 512;

// The view through frame (i, j): the hemi-octahedral map, with corners on the grid (impostor.vert has the inverse).
glm::vec3 frame_direction(uint32_t i, uint32_t j) {
    const float u = float(i) / float(k_frames - 1) * 2.0f - 1.0f;
    const float v = float(j) / float(k_frames - 1) * 2.0f - 1.0f;
    const float x = (u + v) * 0.5f, y = (u - v) * 0.5f;
    return glm::normalize(glm::vec3(x, y, 1.0f - std::abs(x) - std::abs(y)));
}

struct Basis {
    glm::vec3 right, up;
};

Basis frame_basis(const glm::vec3& d) {
    const glm::vec3 axis = std::abs(d.z) > 0.999f ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1);
    const glm::vec3 right = glm::normalize(glm::cross(axis, d));
    return {right, glm::cross(d, right)};
}

glm::vec3 unpack(const uint8_t* b) { return glm::vec3(b[0], b[1], b[2]) * (2.0f / 255.0f) - 1.0f; }

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
    glm::vec3 tint;
};

struct Part {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::shared_ptr<const Texels> diffuse;
    float threshold = -1.0f; // alpha test, < 0 for none
    bool two_sided = false;
};

struct Pixel {
    glm::vec3 albedo{0.0f};
    glm::vec3 normal{0.0f};
    float depth = -1e30f;
    bool hit = false;
};

void draw_part(const Part& part, const glm::vec3& centre, float radius, const glm::vec3& dir, const Basis& basis,
               std::vector<Pixel>& pixels, int size) {
    const float scale = float(size) * 0.5f / radius;
    struct Projected {
        glm::vec2 p;
        float depth;
    };
    std::vector<Projected> screen(part.vertices.size());
    for (size_t i = 0; i < part.vertices.size(); ++i) {
        const glm::vec3 q = part.vertices[i].position - centre;
        // Rows run down the atlas with the frame's up axis: v = 0 is the frame's bottom.
        screen[i] = {glm::vec2(glm::dot(q, basis.right), glm::dot(q, basis.up)) * scale + float(size) * 0.5f, glm::dot(q, dir)};
    }
    for (size_t t = 0; t + 2 < part.indices.size(); t += 3) {
        const uint32_t ia = part.indices[t], ib = part.indices[t + 1], ic = part.indices[t + 2];
        const glm::vec2 a = screen[ia].p, b = screen[ib].p, c = screen[ic].p;
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::abs(area) < 1e-8f) {
            continue;
        }
        const int x0 = std::max(int(std::floor(std::min({a.x, b.x, c.x}))), 0);
        const int y0 = std::max(int(std::floor(std::min({a.y, b.y, c.y}))), 0);
        const int x1 = std::min(int(std::ceil(std::max({a.x, b.x, c.x}))), size - 1);
        const int y1 = std::min(int(std::ceil(std::max({a.y, b.y, c.y}))), size - 1);
        const Vertex &va = part.vertices[ia], &vb = part.vertices[ib], &vc = part.vertices[ic];
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const glm::vec2 p(float(x) + 0.5f, float(y) + 0.5f);
                const float w1 = ((p.x - a.x) * (c.y - a.y) - (p.y - a.y) * (c.x - a.x)) / area;
                const float w2 = ((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)) / area;
                const float w0 = 1.0f - w1 - w2;
                if (w0 < 0 || w1 < 0 || w2 < 0) {
                    continue;
                }
                const float depth = w0 * screen[ia].depth + w1 * screen[ib].depth + w2 * screen[ic].depth;
                Pixel& px = pixels[size_t(y) * size + x];
                if (px.hit && depth <= px.depth) {
                    continue;
                }
                const glm::vec2 uv = w0 * va.uv + w1 * vb.uv + w2 * vc.uv;
                std::array<float, 4> d = {0.5f, 0.5f, 0.5f, 1.0f};
                if (part.diffuse && *part.diffuse) {
                    d = part.diffuse->sample(uv.x, uv.y, true);
                }
                if (d[3] <= part.threshold) {
                    continue;
                }
                glm::vec3 n = glm::normalize(w0 * va.normal + w1 * vb.normal + w2 * vc.normal);
                if (part.two_sided && glm::dot(n, dir) < 0.0f) {
                    n = -n;
                }
                px.albedo = glm::vec3(d[0], d[1], d[2]) * (w0 * va.tint + w1 * vb.tint + w2 * vc.tint);
                px.normal = n;
                px.depth = depth;
                px.hit = true;
            }
        }
    }
}

// Spreads colour into the empty texels around what was drawn, so filtering and block compression don't pull in black.
void dilate(Canvas& canvas, uint32_t x0, uint32_t y0, uint32_t size, int passes) {
    for (int pass = 0; pass < passes; ++pass) {
        std::vector<std::array<float, 3>> fill;
        std::vector<std::pair<uint32_t, uint32_t>> at;
        for (uint32_t y = y0; y < y0 + size; ++y) {
            for (uint32_t x = x0; x < x0 + size; ++x) {
                if (canvas.at(x, y)[3] > 0.0f || canvas.at(x, y)[0] + canvas.at(x, y)[1] + canvas.at(x, y)[2] > 0.0f) {
                    continue;
                }
                float sum[3] = {0, 0, 0};
                int n = 0;
                for (int j = -1; j <= 1; ++j) {
                    for (int i = -1; i <= 1; ++i) {
                        const int nx = int(x) + i, ny = int(y) + j;
                        if (nx < int(x0) || ny < int(y0) || nx >= int(x0 + size) || ny >= int(y0 + size)) {
                            continue;
                        }
                        const float* t = canvas.at(uint32_t(nx), uint32_t(ny));
                        if (t[0] + t[1] + t[2] > 0.0f) {
                            sum[0] += t[0];
                            sum[1] += t[1];
                            sum[2] += t[2];
                            ++n;
                        }
                    }
                }
                if (n > 0) {
                    fill.push_back({sum[0] / float(n), sum[1] / float(n), sum[2] / float(n)});
                    at.emplace_back(x, y);
                }
            }
        }
        for (size_t i = 0; i < fill.size(); ++i) {
            float* t = canvas.at(at[i].first, at[i].second);
            t[0] = fill[i][0];
            t[1] = fill[i][1];
            t[2] = fill[i][2];
        }
    }
}

bool bake_model(const std::filesystem::path& cache, const std::string& mesh_path, TexelCache& texels,
                const std::string& stem, osh::TreeImpostor& out) {
    const osm::Mesh mesh = osm::read(read_file(cache / mesh_path));
    std::vector<Part> parts;
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const osm::Submesh& s : mesh.submeshes) {
        const osm::Material& m = mesh.materials[s.material];
        if (m.params.model != osm::ShaderModel::lighting || s.billboard_mode != osm::k_no_billboard) {
            continue;
        }
        Part part;
        part.diffuse = texels.get(m.textures[osm::texture_diffuse], k_diffuse_texels);
        part.two_sided = (m.params.flags & osm::material_two_sided) != 0;
        if (m.params.flags & osm::material_alpha_test) {
            part.threshold = float(m.params.alpha_threshold) / 255.0f;
        }
        glm::mat3 linear;
        glm::vec3 shift;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                linear[c][r] = s.transform[r][c];
            }
            shift[r] = s.transform[r][3];
        }
        part.vertices.resize(s.vertex_count);
        const bool tint = (m.params.flags & osm::material_vertex_colors) != 0;
        for (uint32_t i = 0; i < s.vertex_count; ++i) {
            const osm::Vertex& v = mesh.vertices[s.base_vertex + i];
            Vertex& o = part.vertices[i];
            o.position = linear * glm::vec3(v.position[0], v.position[1], v.position[2]) + shift;
            o.normal = glm::normalize(linear * unpack(v.normal));
            o.uv = glm::vec2(v.uv[0], v.uv[1]) * glm::vec2(m.params.uv_scale[0], m.params.uv_scale[1]) +
                   glm::vec2(m.params.uv_offset[0], m.params.uv_offset[1]);
            o.tint = tint ? glm::vec3(v.color[0], v.color[1], v.color[2]) * (1.0f / 255.0f) : glm::vec3(1.0f);
            lo = glm::min(lo, o.position);
            hi = glm::max(hi, o.position);
        }
        part.indices.assign(mesh.indices.begin() + s.first_index, mesh.indices.begin() + s.first_index + s.index_count);
        parts.push_back(std::move(part));
    }
    if (parts.empty()) {
        return false;
    }
    const glm::vec3 centre = (lo + hi) * 0.5f;
    float radius = 0.0f, lateral = 0.0f;
    for (const Part& p : parts) {
        for (const Vertex& v : p.vertices) {
            radius = std::max(radius, glm::length(v.position - centre));
            lateral = std::max(lateral, glm::length(glm::vec2(v.position - centre)));
        }
    }
    constexpr uint32_t atlas = k_frames * k_frame_texels;
    constexpr int size = int(k_frame_texels * k_supersample);
    Canvas colour(atlas, atlas), normal(atlas, atlas);
    std::vector<Pixel> pixels(size_t(size) * size);
    for (uint32_t j = 0; j < k_frames; ++j) {
        for (uint32_t i = 0; i < k_frames; ++i) {
            const glm::vec3 dir = frame_direction(i, j);
            const Basis basis = frame_basis(dir);
            std::fill(pixels.begin(), pixels.end(), Pixel{});
            for (const Part& p : parts) {
                draw_part(p, centre, radius, dir, basis, pixels, size);
            }
            for (uint32_t y = 0; y < k_frame_texels; ++y) {
                for (uint32_t x = 0; x < k_frame_texels; ++x) {
                    glm::vec3 a(0.0f), n(0.0f);
                    uint32_t hits = 0;
                    for (uint32_t sy = 0; sy < k_supersample; ++sy) {
                        for (uint32_t sx = 0; sx < k_supersample; ++sx) {
                            const Pixel& px = pixels[size_t(y * k_supersample + sy) * size + x * k_supersample + sx];
                            if (px.hit) {
                                a += px.albedo;
                                n += px.normal;
                                ++hits;
                            }
                        }
                    }
                    float* c = colour.at(i * k_frame_texels + x, j * k_frame_texels + y);
                    float* m = normal.at(i * k_frame_texels + x, j * k_frame_texels + y);
                    if (hits > 0) {
                        a /= float(hits);
                        const float nl = glm::length(n);
                        n = nl > 0 ? n / nl : glm::vec3(0, 0, 1);
                        c[0] = a.r;
                        c[1] = a.g;
                        c[2] = a.b;
                        c[3] = float(hits) / float(k_supersample * k_supersample);
                        m[0] = n.x * 0.5f + 0.5f;
                        m[1] = n.y * 0.5f + 0.5f;
                        m[2] = n.z * 0.5f + 0.5f;
                        m[3] = c[3];
                    }
                }
            }
            dilate(colour, i * k_frame_texels, j * k_frame_texels, k_frame_texels, 4);
            dilate(normal, i * k_frame_texels, j * k_frame_texels, k_frame_texels, 4);
        }
    }
    out.colour = stem + ".dds";
    out.normal = stem + "_n.dds";
    out.frames = k_frames;
    out.centre[0] = centre.x;
    out.centre[1] = centre.y;
    out.centre[2] = centre.z;
    out.radius = radius;
    out.lateral = radius > 0 ? lateral / radius : 1.0f;
    write_file(cache / out.colour, dds::write(encode(colour, Encoding::bc3, true, 1)));
    write_file(cache / out.normal, dds::write(encode(downsample(normal, true), Encoding::bc1, true, 1)));
    return true;
}

} // namespace

void bake_trees(const osw::World& w, TexelCache& texels, const std::filesystem::path& cache, osh::FarView& view,
                unsigned threads) {
    // A model's impostor is shared by every world: the first world to need it bakes it.
    std::map<uint32_t, uint32_t> impostor_of_mesh;
    std::vector<uint32_t> models;
    for (const osw::Ref& r : w.refs) {
        if (r.tree != osw::k_none && !(r.flags & osw::ref_initially_disabled) && r.mesh < w.meshes.size() &&
            impostor_of_mesh.emplace(r.mesh, osh::k_none).second) {
            models.push_back(r.mesh);
        }
    }
    std::vector<osh::TreeImpostor> impostors(models.size());
    std::vector<char> baked(models.size(), 0);
    std::atomic<size_t> next{0};
    auto work = [&] {
        for (size_t i = next++; i < models.size(); i = next++) {
            const std::string& path = w.meshes[models[i]];
            const auto slash = path.find_last_of('/');
            const std::string name = path.substr(slash + 1, path.size() - slash - 5); // without ".osm"
            try {
                baked[i] = bake_model(cache, path, texels, "far/trees/" + name, impostors[i]);
            } catch (const std::exception& e) {
                log::warn("far view: {}: {}", path, e.what());
            }
        }
    };
    {
        std::vector<std::jthread> pool;
        for (unsigned t = 0; t < threads; ++t) {
            pool.emplace_back(work);
        }
    }
    for (size_t i = 0; i < models.size(); ++i) {
        if (baked[i]) {
            impostor_of_mesh[models[i]] = uint32_t(view.impostors.size());
            view.impostors.push_back(std::move(impostors[i]));
        }
    }
    for (const osw::Cell& c : w.cells) {
        const uint32_t first = uint32_t(view.trees.size());
        const uint64_t end = std::min<uint64_t>(uint64_t(c.first_ref) + c.ref_count, w.refs.size());
        for (uint64_t i = c.first_ref; i < end; ++i) {
            const osw::Ref& r = w.refs[i];
            if (r.tree == osw::k_none || (r.flags & osw::ref_initially_disabled)) {
                continue;
            }
            const auto it = impostor_of_mesh.find(r.mesh);
            if (it == impostor_of_mesh.end() || it->second == osh::k_none) {
                continue;
            }
            osh::TreeInstance t{};
            std::copy_n(r.position, 3, t.position);
            t.scale = r.scale;
            std::copy_n(r.rotation, 4, t.rotation);
            t.impostor = it->second;
            t.form_id = r.form_id;
            view.trees.push_back(t);
        }
        if (view.trees.size() > first) {
            view.tree_cells.push_back({c.x, c.y, first, uint32_t(view.trees.size()) - first});
        }
    }
    log::info("far view: {}: {} tree impostors, {} trees", w.edid, view.impostors.size(), view.trees.size());
}

} // namespace spud::importer::far
