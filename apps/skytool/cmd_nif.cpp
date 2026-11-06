#include "commands.hpp"

#include "core/error.hpp"
#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "formats/osm.hpp"
#include "formats/osp.hpp"
#include "importer/convert/collision_import.hpp"
#include "importer/convert/mesh_import.hpp"
#include "importer/legacy/data_source.hpp"
#include "importer/legacy/nif.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <format>
#include <map>
#include <thread>

namespace skytool {

namespace {

using namespace spud::legacy;

// A file on disk if it exists, otherwise a path in the game's merged data view.
std::vector<std::byte> load_nif(const std::string& arg, std::string& source_path) {
    if (std::filesystem::is_regular_file(arg)) {
        source_path = spud::path::normalize(std::filesystem::path(arg).filename().string());
        return spud::read_file(arg);
    }
    source_path = spud::path::with_root(spud::path::normalize(arg), "meshes");
    DataSource data(open_install());
    auto bytes = data.read(source_path);
    if (!bytes) {
        throw spud::Error(std::format("{} not found in the install", source_path));
    }
    return std::move(*bytes);
}

const char* status_name(NifBlockStatus s) {
    switch (s) {
    case NifBlockStatus::parsed:
        return "";
    case NifBlockStatus::opaque:
        return " (opaque)";
    case NifBlockStatus::mismatch:
        return " (MISMATCH)";
    }
    return "";
}

std::string attribute_names(const BSVertexDesc& d) {
    static constexpr std::pair<uint32_t, const char*> k_names[] = {
        {BSVertexDesc::attr_vertex, "pos"},
        {BSVertexDesc::attr_uv, "uv"},
        {BSVertexDesc::attr_uv2, "uv2"},
        {BSVertexDesc::attr_normal, "normal"},
        {BSVertexDesc::attr_tangent, "tangent"},
        {BSVertexDesc::attr_colors, "colour"},
        {BSVertexDesc::attr_skinned, "skin"},
        {BSVertexDesc::attr_land_data, "land"},
        {BSVertexDesc::attr_eye_data, "eye"},
        {BSVertexDesc::attr_instance, "instance"},
        {BSVertexDesc::attr_full_precision, "full"},
    };
    std::string out;
    for (auto [bit, name] : k_names) {
        if (d.has(static_cast<BSVertexDesc::Attribute>(bit))) {
            out += out.empty() ? "" : "|";
            out += name;
        }
    }
    return std::format("{} ({}B)", out, d.vertex_size());
}

std::string transform_text(const NiAVObject& o) {
    std::string s =
        std::format("T=({:.3f},{:.3f},{:.3f})", o.translation.x, o.translation.y, o.translation.z);
    if (o.rotation != glm::mat3(1.0f)) {
        s += " R=[";
        for (int row = 0; row < 3; ++row) {
            s += std::format("{}{:.3f} {:.3f} {:.3f}", row ? "; " : "", o.rotation[0][row],
                             o.rotation[1][row], o.rotation[2][row]);
        }
        s += "]";
    }
    if (o.scale != 1.0f) {
        s += std::format(" S={:.4f}", o.scale);
    }
    return s;
}

class Dumper {
public:
    explicit Dumper(const NifFile& nif) : m_nif(nif), m_seen(nif.blocks.size(), 0) {}

    void header() const {
        const auto& h = m_nif.header;
        std::printf("NIF 20.2.0.7 user %u, BS %u; author \"%s\", process \"%s\", export \"%s\"\n",
                    h.user_version, h.bs_version, h.author.c_str(), h.process_script.c_str(),
                    h.export_script.c_str());
        std::printf("%zu blocks, %zu block types, %zu strings, roots:", m_nif.blocks.size(),
                    h.block_types.size(), h.strings.size());
        for (auto r : m_nif.roots) {
            std::printf(" %d", r);
        }
        std::puts("\n\nblocks:");
        for (size_t i = 0; i < m_nif.blocks.size(); ++i) {
            const auto& b = m_nif.blocks[i];
            std::printf("  %4zu %-32s %8u%s%s%s\n", i,
                        std::string(m_nif.type_name(static_cast<NifRef>(i))).c_str(), b.size,
                        status_name(b.status), b.error.empty() ? "" : ": ", b.error.c_str());
        }
    }

    void tree() {
        std::puts("\ntree:");
        for (auto r : m_nif.roots) {
            node(r, 1);
        }
    }

private:
    void line(int depth, const std::string& text) const {
        std::printf("%*s%s\n", depth * 2, "", text.c_str());
    }

    void node(NifRef ref, int depth) {
        if (!m_nif.valid(ref)) {
            return;
        }
        auto type = std::string(m_nif.type_name(ref));
        const auto* av = m_nif.av_object(ref);
        if (!av) {
            line(depth, std::format("[{}] {}", ref, type));
            return;
        }
        if (m_seen[static_cast<size_t>(ref)]++) {
            line(depth, std::format("[{}] {} \"{}\" (again)", ref, type, av->name));
            return;
        }
        line(depth, std::format("[{}] {} \"{}\" flags=0x{:08x}{} {}", ref, type, av->name, av->flags,
                                av->hidden() ? " HIDDEN" : "", transform_text(*av)));
        for (auto e : av->extra_data) {
            extra(e, depth + 2);
        }
        if (av->collision != -1) {
            line(depth + 2, std::format("collision [{}] {}", av->collision, m_nif.type_name(av->collision)));
        }
        if (const auto* n = m_nif.get<NiNode>(ref)) {
            if (n->switch_index) {
                line(depth + 2,
                     std::format("switch index {}{}", *n->switch_index,
                                 n->lod_data != -1 ? std::format(", lod data [{}]", n->lod_data) : ""));
                if (const auto* lod = m_nif.get<NiRangeLODData>(n->lod_data)) {
                    for (size_t i = 0; i < lod->ranges.size(); ++i) {
                        line(depth + 3,
                             std::format("child {}: {} .. {}", i, lod->ranges[i][0], lod->ranges[i][1]));
                    }
                }
            }
            if (type == "BSRangeNode" || type == "BSBlastNode" || type == "BSDamageStage" ||
                type == "BSDebrisNode") {
                line(depth + 2,
                     std::format("range {} .. {}, current {}", n->range[0], n->range[1], n->range[2]));
            }
            for (auto c : n->children) {
                node(c, depth + 1);
            }
        } else if (const auto* s = m_nif.get<BSTriShape>(ref)) {
            std::string text = std::format("{} vertices, {} triangles, {}", s->num_vertices, s->num_triangles,
                                           attribute_names(s->vertex_desc));
            if (s->has_lods) {
                text +=
                    std::format(", LOD sizes {} {} {}", s->lod_sizes[0], s->lod_sizes[1], s->lod_sizes[2]);
            }
            if (s->geometry.positions.empty() && s->num_vertices > 0) {
                text += ", vertex data in skin partition";
            }
            line(depth + 2, text);
            line(depth + 2, std::format("bound ({:.2f},{:.2f},{:.2f}) r={:.2f}", s->bound_center.x,
                                        s->bound_center.y, s->bound_center.z, s->bound_radius));
            skin(s->skin, depth + 2);
            property(s->shader_property, depth + 2);
            property(s->alpha_property, depth + 2);
        } else if (const auto* g = m_nif.get<NiGeometry>(ref)) {
            std::string text = std::format("data [{}] {}", g->data, m_nif.type_name(g->data));
            if (const auto* d = m_nif.get<NiGeometryData>(g->data)) {
                const auto& geo = d->geometry;
                text +=
                    std::format(": {} vertices, {} triangles{}{}{}", geo.positions.size(),
                                geo.triangles.size(), geo.uvs.empty() ? "" : ", uv",
                                geo.normals.empty() ? "" : ", normal", geo.colors.empty() ? "" : ", colour");
            }
            if (g->has_lods) {
                text +=
                    std::format(", LOD sizes {} {} {}", g->lod_sizes[0], g->lod_sizes[1], g->lod_sizes[2]);
            }
            line(depth + 2, text);
            skin(g->skin, depth + 2);
            property(g->shader_property, depth + 2);
            property(g->alpha_property, depth + 2);
        }
    }

    void extra(NifRef ref, int depth) const {
        auto type = std::string(m_nif.type_name(ref));
        if (const auto* i = m_nif.get<NiIntegerExtraData>(ref)) {
            line(depth, std::format("[{}] {} \"{}\" = 0x{:x}", ref, type, i->name, i->value));
        } else if (const auto* s = m_nif.get<NiStringExtraData>(ref)) {
            line(depth, std::format("[{}] {} \"{}\" = \"{}\"", ref, type, s->name, s->value));
        } else if (const auto* b = m_nif.get<BSBound>(ref)) {
            line(depth,
                 std::format("[{}] {} \"{}\" centre ({:.2f},{:.2f},{:.2f}) half ({:.2f},{:.2f},{:.2f})", ref,
                             type, b->name, b->center.x, b->center.y, b->center.z, b->half_extents.x,
                             b->half_extents.y, b->half_extents.z));
        } else {
            line(depth, std::format("[{}] {}", ref, type));
        }
    }

    void skin(NifRef ref, int depth) const {
        if (ref == -1) {
            return;
        }
        std::string text = std::format("skin [{}] {}", ref, m_nif.type_name(ref));
        if (const auto* s = m_nif.get<NiSkinInstance>(ref)) {
            text += std::format(": {} bones, partition [{}]", s->bones.size(), s->skin_partition);
            if (const auto* p = m_nif.get<NiSkinPartition>(s->skin_partition)) {
                size_t tris = 0;
                for (const auto& part : p->partitions) {
                    tris += part.triangles.size();
                }
                text += std::format(" {} partitions, {} vertices, {} triangles, {}", p->partitions.size(),
                                    p->geometry.positions.size(), tris, attribute_names(p->vertex_desc));
            }
        }
        line(depth, text);
    }

    void property(NifRef ref, int depth) const {
        if (ref == -1) {
            return;
        }
        auto type = std::string(m_nif.type_name(ref));
        if (const auto* s = m_nif.get<BSLightingShaderProperty>(ref)) {
            line(depth, std::format("[{}] {} \"{}\" type {} flags1=0x{:08x} flags2=0x{:08x} clamp {}", ref,
                                    type, s->name, s->shader_type, s->flags1, s->flags2, s->texture_clamp));
            line(depth + 2,
                 std::format("alpha {} gloss {} spec ({:.2f},{:.2f},{:.2f})x{} emissive ({:.2f},{:.2f},"
                             "{:.2f})x{} uv off ({},{}) scale ({},{}) le {} {}",
                             s->alpha, s->glossiness, s->specular_color.r, s->specular_color.g,
                             s->specular_color.b, s->specular_strength, s->emissive_color.r,
                             s->emissive_color.g, s->emissive_color.b, s->emissive_multiple, s->uv_offset.x,
                             s->uv_offset.y, s->uv_scale.x, s->uv_scale.y, s->lighting_effect1,
                             s->lighting_effect2));
            if (const auto* t = m_nif.get<BSShaderTextureSet>(s->texture_set)) {
                for (size_t i = 0; i < t->textures.size(); ++i) {
                    if (!t->textures[i].empty()) {
                        line(depth + 2, std::format("texture {}: {}", i, t->textures[i]));
                    }
                }
            }
        } else if (const auto* e = m_nif.get<BSEffectShaderProperty>(ref)) {
            line(depth, std::format("[{}] {} \"{}\" flags1=0x{:08x} flags2=0x{:08x} clamp {}", ref, type,
                                    e->name, e->flags1, e->flags2, e->texture_clamp));
            line(depth + 2, std::format("base ({:.2f},{:.2f},{:.2f},{:.2f})x{} falloff ({},{},{},{}) soft {}",
                                        e->base_color.r, e->base_color.g, e->base_color.b, e->base_color.a,
                                        e->base_color_scale, e->falloff.x, e->falloff.y, e->falloff.z,
                                        e->falloff.w, e->soft_falloff_depth));
            line(depth + 2, std::format("source: {}", e->source_texture));
            line(depth + 2, std::format("greyscale: {}", e->greyscale_texture));
        } else if (const auto* a = m_nif.get<NiAlphaProperty>(ref)) {
            line(depth,
                 std::format("[{}] {} flags=0x{:04x} threshold {}", ref, type, a->flags, a->threshold));
        } else {
            line(depth, std::format("[{}] {}", ref, type));
        }
    }

    const NifFile& m_nif;
    std::vector<uint8_t> m_seen;
};

void print_summary(const spud::osm::Mesh& mesh) {
    std::printf("%zu submeshes, %zu vertices, %zu triangles, %zu materials\n", mesh.submeshes.size(),
                mesh.vertices.size(), mesh.indices.size() / 3, mesh.materials.size());
    std::printf("bounds (%.3f, %.3f, %.3f) .. (%.3f, %.3f, %.3f) m\n", mesh.bounds_min[0], mesh.bounds_min[1],
                mesh.bounds_min[2], mesh.bounds_max[0], mesh.bounds_max[1], mesh.bounds_max[2]);
    for (size_t i = 0; i < mesh.submeshes.size(); ++i) {
        const auto& s = mesh.submeshes[i];
        float lo[3] = {1e30f, 1e30f, 1e30f};
        float hi[3] = {-1e30f, -1e30f, -1e30f};
        for (uint32_t v = s.base_vertex; v < s.base_vertex + s.vertex_count && v < mesh.vertices.size();
             ++v) {
            for (int k = 0; k < 3; ++k) {
                lo[k] = std::min(lo[k], mesh.vertices[v].position[k]);
                hi[k] = std::max(hi[k], mesh.vertices[v].position[k]);
            }
        }
        std::printf("  submesh %zu: %u vertices, %u triangles, material %u, (%.2f, %.2f, %.2f) .. (%.2f, "
                    "%.2f, %.2f)\n",
                    i, s.vertex_count, s.index_count / 3, s.material, lo[0], lo[1], lo[2], hi[0], hi[1],
                    hi[2]);
    }
    for (size_t i = 0; i < mesh.materials.size(); ++i) {
        const auto& m = mesh.materials[i];
        const auto& p = m.params;
        std::printf("  material %zu: %s type %u flags1=0x%08x flags2=0x%08x flags=0x%03x", i,
                    p.model == spud::osm::ShaderModel::effect ? "effect" : "lighting", p.skyrim_shader_type,
                    p.skyrim_flags1, p.skyrim_flags2, p.flags);
        if (p.flags & (spud::osm::material_alpha_test | spud::osm::material_alpha_blend)) {
            std::printf(" blend %u/%u test func %u ref %u", p.blend_src, p.blend_dst, p.alpha_func,
                        p.alpha_threshold);
        }
        std::puts("");
        for (size_t t = 0; t < spud::osm::k_texture_slots; ++t) {
            if (!m.textures[t].empty()) {
                std::printf("    texture %zu: %s\n", t, m.textures[t].c_str());
            }
        }
    }
}

const char* shape_type_name(spud::osp::ShapeType t) {
    switch (t) {
    case spud::osp::ShapeType::box: return "box";
    case spud::osp::ShapeType::sphere: return "sphere";
    case spud::osp::ShapeType::capsule: return "capsule";
    case spud::osp::ShapeType::cylinder: return "cylinder";
    case spud::osp::ShapeType::convex: return "convex";
    case spud::osp::ShapeType::mesh: return "mesh";
    }
    return "?";
}

void print_collision(const spud::osp::Collision& c) {
    std::printf("%zu bodies, %zu shapes, %zu vertices, %zu triangles\n", c.bodies.size(), c.shapes.size(),
                c.vertices.size() / 3, c.indices.size() / 3);
    if (!c.ranks.empty()) {
        const auto ranked = std::ranges::count_if(c.ranks, [](uint32_t r) { return r != 0xFFFFFFFFu; });
        std::printf("MOPP collection order: %zu of %zu triangles ranked\n", static_cast<size_t>(ranked), c.ranks.size());
    }
    if (c.shapes.empty()) {
        return;
    }
    std::printf("bounds (%.3f, %.3f, %.3f) .. (%.3f, %.3f, %.3f) m\n", c.bounds_min[0], c.bounds_min[1],
                c.bounds_min[2], c.bounds_max[0], c.bounds_max[1], c.bounds_max[2]);
    for (size_t i = 0; i < c.bodies.size(); ++i) {
        const auto& b = c.bodies[i];
        std::printf("  body %zu: layer %u, motion %u, mass %.2f, friction %.2f, restitution %.2f\n", i, b.layer,
                    static_cast<unsigned>(b.motion), b.mass, b.friction, b.restitution);
    }
    for (size_t i = 0; i < c.shapes.size(); ++i) {
        const auto& s = c.shapes[i];
        std::printf("  shape %zu: %s body %u layer %u material %08x", i, shape_type_name(s.type), s.body, s.layer,
                    s.material);
        switch (s.type) {
        case spud::osp::ShapeType::box:
            std::printf(" centre (%.3f, %.3f, %.3f) half (%.3f, %.3f, %.3f) q (%.3f, %.3f, %.3f, %.3f)", s.centre[0],
                        s.centre[1], s.centre[2], s.extents[0], s.extents[1], s.extents[2], s.rotation[0],
                        s.rotation[1], s.rotation[2], s.rotation[3]);
            break;
        case spud::osp::ShapeType::sphere:
            std::printf(" centre (%.3f, %.3f, %.3f) r %.3f", s.centre[0], s.centre[1], s.centre[2], s.radius);
            break;
        case spud::osp::ShapeType::capsule:
        case spud::osp::ShapeType::cylinder:
            std::printf(" (%.3f, %.3f, %.3f) - (%.3f, %.3f, %.3f) r %.3f", s.centre[0], s.centre[1], s.centre[2],
                        s.extents[0], s.extents[1], s.extents[2], s.radius);
            break;
        case spud::osp::ShapeType::convex:
            std::printf(" %u points, radius %.4f", s.vertex_count, s.convex_radius);
            break;
        case spud::osp::ShapeType::mesh:
            std::printf(" %u vertices, %u triangles", s.vertex_count, s.index_count / 3);
            break;
        }
        std::puts("");
    }
}

// Collapses the numbers in a message so similar errors group together.
std::string error_group(std::string_view message) {
    std::string out;
    for (size_t i = 0; i < message.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(message[i]))) {
            while (i + 1 < message.size() && std::isdigit(static_cast<unsigned char>(message[i + 1]))) {
                ++i;
            }
            out += '#';
        } else {
            out += message[i];
        }
    }
    return out;
}

struct Example {
    size_t count = 0;
    std::string path;
    std::string message;
};

struct FolderStats {
    size_t ok = 0;
    size_t empty = 0;
    size_t failed = 0;
    size_t partial = 0; // converted, but some block could not be decoded
};

struct ScanStats {
    size_t ok = 0;
    size_t empty = 0;
    size_t failed = 0;
    size_t with_warnings = 0;
    size_t submeshes = 0;
    size_t vertices = 0;
    size_t triangles = 0;
    std::map<std::string, Example> errors;
    std::map<std::string, Example> warnings;
    // Block type -> count + first file it appears in.
    std::map<std::string, Example> opaque;
    std::map<std::string, Example> mismatch;
    std::map<std::string, Example> parsed;
    std::map<std::string, FolderStats> folders;
    std::vector<std::string> empty_files;
    std::map<std::string, Example> versions; // "user/bs" version pair
    size_t with_collision = 0;
    size_t collision_only = 0;
    size_t collision_shapes[6] = {};
    size_t collision_triangles = 0;
    std::map<std::string, Example> collision_warnings;
    // Collision reaching more than 0.5 m and 25% past the render bounds: a scale or layout mistake.
    std::map<std::string, Example> collision_outside;

    static void note(std::map<std::string, Example>& m, const std::string& key, const std::string& path,
                     const std::string& message) {
        auto& e = m[key];
        if (e.count++ == 0) {
            e.path = path;
            e.message = message;
        }
    }

    void merge(const ScanStats& o) {
        ok += o.ok;
        empty += o.empty;
        failed += o.failed;
        with_warnings += o.with_warnings;
        submeshes += o.submeshes;
        vertices += o.vertices;
        triangles += o.triangles;
        empty_files.insert(empty_files.end(), o.empty_files.begin(), o.empty_files.end());
        with_collision += o.with_collision;
        collision_only += o.collision_only;
        for (int i = 0; i < 6; ++i) {
            collision_shapes[i] += o.collision_shapes[i];
        }
        collision_triangles += o.collision_triangles;
        for (auto [mine, theirs] : {std::pair{&errors, &o.errors}, std::pair{&warnings, &o.warnings},
                                    std::pair{&mismatch, &o.mismatch}, std::pair{&opaque, &o.opaque},
                                    std::pair{&parsed, &o.parsed}, std::pair{&versions, &o.versions},
                                    std::pair{&collision_warnings, &o.collision_warnings},
                                    std::pair{&collision_outside, &o.collision_outside}}) {
            for (const auto& [k, v] : *theirs) {
                auto& e = (*mine)[k];
                if (e.count == 0) {
                    e.path = v.path;
                    e.message = v.message;
                }
                e.count += v.count;
            }
        }
        for (const auto& [k, v] : o.folders) {
            auto& f = folders[k];
            f.ok += v.ok;
            f.empty += v.empty;
            f.failed += v.failed;
            f.partial += v.partial;
        }
    }
};

// "meshes/architecture/whiterun/x.nif" -> "meshes/architecture"
std::string folder_of(const std::string& p) {
    auto first = p.find('/');
    if (first == std::string::npos) {
        return p;
    }
    auto second = p.find('/', first + 1);
    return second == std::string::npos ? p.substr(0, first) : p.substr(0, second);
}

void scan_one(const DataSource& data, const std::string& p, ScanStats& stats) {
    auto& folder = stats.folders[folder_of(p)];
    try {
        auto bytes = data.read(p);
        if (!bytes) {
            throw spud::Error("unreadable");
        }
        auto nif = read_nif(*bytes);
        ScanStats::note(stats.versions,
                        std::format("user {}, BS {}", nif.header.user_version, nif.header.bs_version), p, {});
        bool partial = false;
        for (size_t i = 0; i < nif.blocks.size(); ++i) {
            const auto& b = nif.blocks[i];
            auto type = std::string(nif.type_name(static_cast<NifRef>(i)));
            switch (b.status) {
            case NifBlockStatus::parsed:
                ScanStats::note(stats.parsed, type, p, {});
                break;
            case NifBlockStatus::opaque:
                ScanStats::note(stats.opaque, type, p, {});
                break;
            case NifBlockStatus::mismatch:
                ScanStats::note(stats.mismatch, type, p, b.error);
                partial = true;
                break;
            }
        }
        auto result = spud::importer::import_nif(nif, p);
        // Exercise the writer too, so a scan covers the whole conversion.
        (void)spud::osm::write(result.mesh);
        if (!result.warnings.empty()) {
            ++stats.with_warnings;
            for (const auto& w : result.warnings) {
                ScanStats::note(stats.warnings, error_group(w), p, w);
            }
        }
        auto collision = spud::importer::import_collision(nif, p);
        (void)spud::osp::write(collision.collision);
        for (const auto& w : collision.warnings) {
            ScanStats::note(stats.collision_warnings, error_group(w), p, w);
        }
        if (const auto& c = collision.collision; !c.shapes.empty()) {
            ++stats.with_collision;
            stats.collision_only += result.mesh.submeshes.empty();
            for (const auto& sh : c.shapes) {
                ++stats.collision_shapes[static_cast<size_t>(sh.type)];
            }
            stats.collision_triangles += c.indices.size() / 3;
            if (!result.mesh.submeshes.empty()) {
                const auto& m = result.mesh;
                float worst = 0;
                for (int k = 0; k < 3; ++k) {
                    const float size = std::max(m.bounds_max[k] - m.bounds_min[k], 0.01f);
                    const float over =
                        std::max(m.bounds_min[k] - c.bounds_min[k], c.bounds_max[k] - m.bounds_max[k]);
                    if (over > 0.5f && over > 0.25f * size) {
                        worst = std::max(worst, over);
                    }
                }
                if (worst > 0) {
                    ScanStats::note(stats.collision_outside, folder_of(p), p,
                                    std::format("{:.2f} m outside the render bounds", worst));
                }
            }
        }
        if (partial) {
            ++folder.partial;
        }
        if (result.mesh.submeshes.empty()) {
            ++stats.empty;
            ++folder.empty;
            stats.empty_files.push_back(p);
        } else {
            ++stats.ok;
            ++folder.ok;
            stats.submeshes += result.mesh.submeshes.size();
            stats.vertices += result.mesh.vertices.size();
            stats.triangles += result.mesh.indices.size() / 3;
        }
    } catch (const std::exception& e) {
        ++stats.failed;
        ++folder.failed;
        ScanStats::note(stats.errors, error_group(e.what()), p, e.what());
    }
}

// Raw bytes of one block, for working out layouts.
void hex_dump(const NifFile& nif, std::span<const std::byte> bytes, NifRef ref) {
    if (!nif.valid(ref)) {
        throw spud::Error(std::format("no block {}", ref));
    }
    const auto& b = nif.blocks[static_cast<size_t>(ref)];
    std::printf("block %d %s: %u bytes at offset %u%s%s%s\n", ref, std::string(nif.type_name(ref)).c_str(),
                b.size, b.offset, status_name(b.status), b.error.empty() ? "" : ": ", b.error.c_str());
    for (uint32_t i = 0; i < b.size; i += 16) {
        std::printf("%6u:", i);
        for (uint32_t j = i; j < i + 16 && j < b.size; ++j) {
            std::printf(" %02x", static_cast<unsigned>(bytes[b.offset + j]));
        }
        std::puts("");
    }
}

int scan(Args args, const std::vector<std::string>& pos) {
    auto prefix = pos.size() >= 2 ? spud::path::normalize(pos[1]) : std::string("meshes/");
    size_t limit = std::stoul(option(args, "--limit").value_or("0"));
    unsigned threads = static_cast<unsigned>(
        std::stoul(option(args, "--threads")
                       .value_or(std::to_string(std::max(1u, std::thread::hardware_concurrency())))));
    bool verbose = has_flag(args, "--warnings");

    DataSource data(open_install());
    auto files = data.list(prefix, option(args, "--ext").value_or(".nif"));
    if (limit > 0 && files.size() > limit) {
        files.resize(limit);
    }
    spud::log::info("scanning {} NIFs under \"{}\" on {} threads", files.size(), prefix, threads);
    auto start = std::chrono::steady_clock::now();

    std::atomic<size_t> next{0};
    std::vector<ScanStats> per_thread(threads);
    std::vector<std::jthread> workers;
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            for (size_t i; (i = next.fetch_add(1)) < files.size();) {
                scan_one(data, files[i], per_thread[t]);
            }
        });
    }
    workers.clear();
    ScanStats stats;
    for (const auto& s : per_thread) {
        stats.merge(s);
    }
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    std::printf("\nfolders (ok / empty / failed / partial):\n");
    for (const auto& [name, f] : stats.folders) {
        std::printf("  %-36s %6zu %6zu %6zu %6zu\n", name.c_str(), f.ok, f.empty, f.failed, f.partial);
    }
    std::printf("\nversions:\n");
    for (const auto& [v, e] : stats.versions) {
        std::printf("  %8zu  %-20s e.g. %s\n", e.count, v.c_str(), e.path.c_str());
    }
    if (!stats.errors.empty()) {
        std::printf("\nerrors:\n");
        for (const auto& [group, e] : stats.errors) {
            std::printf("  %6zu  %s\n          e.g. %s: %s\n", e.count, group.c_str(), e.path.c_str(),
                        e.message.c_str());
        }
    }
    if (!stats.mismatch.empty()) {
        std::printf("\nsize mismatches by block type:\n");
        for (const auto& [type, e] : stats.mismatch) {
            std::printf("  %6zu  %-32s e.g. %s: %s\n", e.count, type.c_str(), e.path.c_str(),
                        e.message.c_str());
        }
    }
    std::printf("\nopaque (unknown) block types:\n");
    std::vector<std::pair<size_t, std::string>> opaque;
    for (const auto& [type, e] : stats.opaque) {
        opaque.emplace_back(e.count, type);
    }
    std::ranges::sort(opaque, std::greater{});
    for (const auto& [n, type] : opaque) {
        std::printf("  %8zu  %-40s e.g. %s\n", n, type.c_str(), stats.opaque[type].path.c_str());
    }
    std::printf("\nparsed block types:\n");
    for (const auto& [type, e] : stats.parsed) {
        std::printf("  %8zu  %-40s e.g. %s\n", e.count, type.c_str(), e.path.c_str());
    }
    std::printf("\nwarnings (%zu files with warnings):\n", stats.with_warnings);
    std::vector<std::pair<size_t, const Example*>> warnings;
    for (const auto& [group, e] : stats.warnings) {
        warnings.emplace_back(e.count, &e);
    }
    std::ranges::sort(warnings, [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < warnings.size() && (verbose || i < 40); ++i) {
        std::printf("  %6zu  e.g. %s: %s\n", warnings[i].first, warnings[i].second->path.c_str(),
                    warnings[i].second->message.c_str());
    }
    std::printf("\ncollision: %zu files (%zu with nothing to draw); %zu boxes, %zu spheres, %zu capsules, %zu "
                "cylinders, %zu convex, %zu meshes (%zu triangles)\n",
                stats.with_collision, stats.collision_only, stats.collision_shapes[0], stats.collision_shapes[1],
                stats.collision_shapes[2], stats.collision_shapes[3], stats.collision_shapes[4],
                stats.collision_shapes[5], stats.collision_triangles);
    for (const auto& [group, e] : stats.collision_warnings) {
        std::printf("  %6zu  e.g. %s: %s\n", e.count, e.path.c_str(), e.message.c_str());
    }
    if (!stats.collision_outside.empty()) {
        std::printf("collision outside the render bounds, by folder:\n");
        for (const auto& [folder, e] : stats.collision_outside) {
            std::printf("  %6zu  %-36s e.g. %s: %s\n", e.count, folder.c_str(), e.path.c_str(), e.message.c_str());
        }
    }
    if (has_flag(args, "--empty")) {
        std::ranges::sort(stats.empty_files);
        std::printf("\nempty (nothing renderable):\n");
        for (const auto& p : stats.empty_files) {
            std::printf("  %s\n", p.c_str());
        }
    }
    std::printf(
        "\n%zu files: %zu ok, %zu empty, %zu failed; %zu submeshes, %zu vertices, %zu triangles; %.1f s\n",
        files.size(), stats.ok, stats.empty, stats.failed, stats.submeshes, stats.vertices, stats.triangles,
        seconds);
    return stats.failed == 0 ? 0 : 1;
}

} // namespace

int cmd_nif(Args args) {
    auto pos = positionals(args, {"--limit", "--threads", "--block", "--ext"});
    if (pos.empty()) {
        std::puts(
            "nif dump <path-or-file> [--block N]   header, blocks, node tree, shapes and materials\n"
            "                                      (--block N: hex of one block)\n"
            "nif convert <path-or-file> <out.osm>  convert, write, read back and summarise\n"
            "nif scan [prefix] [--limit N] [--threads N] [--ext .nif] [--warnings] [--empty]\n"
            "                                      convert every .nif under prefix (default meshes/) in\n"
            "                                      parallel; --warnings lists all warning groups, --empty\n"
            "                                      the files with nothing renderable");
        return 1;
    }
    const auto& sub = pos[0];
    if (sub == "dump" && pos.size() >= 2) {
        std::string source;
        auto bytes = load_nif(pos[1], source);
        auto nif = read_nif(bytes);
        if (auto block = option(args, "--block")) {
            hex_dump(nif, bytes, std::stoi(*block));
            return 0;
        }
        std::printf("%s (%zu bytes)\n", source.c_str(), bytes.size());
        Dumper dumper(nif);
        dumper.header();
        dumper.tree();
        auto result = spud::importer::import_nif(nif, source);
        std::puts("\nconverted:");
        print_summary(result.mesh);
        for (const auto& w : result.warnings) {
            spud::log::warn("{}", w);
        }
        auto collision = spud::importer::import_collision(nif, source);
        std::puts("\ncollision:");
        print_collision(collision.collision);
        for (const auto& w : collision.warnings) {
            spud::log::warn("collision: {}", w);
        }
        return 0;
    }
    if (sub == "convert" && pos.size() >= 3) {
        std::string source;
        auto bytes = load_nif(pos[1], source);
        auto result = spud::importer::import_nif(bytes, source);
        for (const auto& w : result.warnings) {
            spud::log::warn("{}: {}", source, w);
        }
        auto file = spud::osm::write(result.mesh);
        spud::write_file(pos[2], file);
        spud::log::info("wrote {} bytes to {}", file.size(), pos[2]);
        auto back = spud::osm::read(spud::read_file(pos[2]));
        print_summary(back);
        std::printf("%zu textures:\n", result.textures.size());
        for (const auto& t : result.textures) {
            std::printf("  %s\n", t.c_str());
        }
        return 0;
    }
    if (sub == "scan") {
        return scan(args, pos);
    }
    spud::log::error("unknown nif subcommand");
    return 1;
}

} // namespace skytool
