#include "importer/legacy/nif.hpp"

#include "core/binary.hpp"
#include "core/error.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <type_traits>

namespace spud::legacy {

namespace {

constexpr std::string_view k_signature = "Gamebryo File Format, Version ";

// Rejects element counts that could not fit in what is left of the block, before allocating for them.
uint32_t check_count(const BinaryReader& r, uint64_t count, size_t element_size) {
    if (count * element_size > r.remaining()) {
        throw ParseError(std::format("count {} x {} bytes overruns the block ({} bytes left)", count,
                                     element_size, r.remaining()));
    }
    return static_cast<uint32_t>(count);
}

uint32_t read_count(BinaryReader& r, size_t element_size) {
    return check_count(r, r.read<uint32_t>(), element_size);
}

float half_to_float(uint16_t h) {
    uint32_t sign = static_cast<uint32_t>(h & 0x8000) << 16;
    uint32_t exponent = (h >> 10) & 0x1F;
    uint32_t mantissa = h & 0x3FF;
    uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            // Subnormal: renormalise into a float exponent.
            int e = -1;
            do {
                ++e;
                mantissa <<= 1;
            } while ((mantissa & 0x400) == 0);
            bits = sign | static_cast<uint32_t>(127 - 15 - e) << 23 | (mantissa & 0x3FF) << 13;
        }
    } else if (exponent == 31) {
        bits = sign | 0x7F800000 | mantissa << 13;
    } else {
        bits = sign | (exponent - 15 + 127) << 23 | mantissa << 13;
    }
    return std::bit_cast<float>(bits);
}

template <typename T>
T load(const std::byte* p) {
    T value;
    std::memcpy(&value, p, sizeof(T));
    return value;
}

float unorm8_to_snorm(uint8_t b) {
    return static_cast<float>(b) / 255.0f * 2.0f - 1.0f;
}

// Per-block parsing state: the reader is limited to the block's bytes.
struct BlockParser {
    BinaryReader& r;
    const NifHeader& h;
    std::string_view type;

    uint32_t bsver() const { return h.bs_version; }

    glm::vec2 vec2() { return {r.read<float>(), r.read<float>()}; }
    glm::vec3 vec3() {
        auto x = r.read<float>();
        auto y = r.read<float>();
        auto z = r.read<float>();
        return {x, y, z};
    }
    glm::vec4 vec4() {
        auto x = r.read<float>();
        auto y = r.read<float>();
        auto z = r.read<float>();
        auto w = r.read<float>();
        return {x, y, z, w};
    }

    // Stored row by row (m11 m12 m13 m21 ...); glm is column-major.
    glm::mat3 matrix33() {
        glm::mat3 m(1.0f);
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                m[col][row] = r.read<float>();
            }
        }
        return m;
    }

    // Index into the header string table.
    std::string string_ref() {
        auto index = r.read<int32_t>();
        if (index < 0) {
            return {};
        }
        if (static_cast<size_t>(index) >= h.strings.size()) {
            throw ParseError(
                std::format("string index {} out of range ({} strings)", index, h.strings.size()));
        }
        return h.strings[static_cast<size_t>(index)];
    }

    // u32 length + characters (no terminator).
    std::string sized_string() {
        auto len = r.read<uint32_t>();
        check_count(r, len, 1);
        return r.read_string(len);
    }

    std::vector<NifRef> refs() {
        auto count = read_count(r, 4);
        return r.read_array<NifRef>(count);
    }

    void object_net(NiObjectNET& o) {
        o.name = string_ref();
        o.extra_data = refs();
        o.controller = r.read<int32_t>();
    }

    void av_object(NiAVObject& o) {
        object_net(o);
        o.flags = r.read<uint32_t>();
        o.translation = vec3();
        o.rotation = matrix33();
        o.scale = r.read<float>();
        o.collision = r.read<int32_t>();
    }

    NiNode node() {
        NiNode n;
        av_object(n);
        n.children = refs();
        n.effects = refs();
        if (type == "NiSwitchNode" || type == "NiLODNode") {
            n.switch_flags = r.read<uint16_t>();
            n.switch_index = r.read<uint32_t>();
            if (type == "NiLODNode") {
                n.lod_data = r.read<int32_t>();
            }
        } else if (type == "BSTreeNode") {
            refs(); // bones 1
            refs(); // bones
        } else if (type == "BSMultiBoundNode") {
            n.multi_bound = r.read<int32_t>();
            r.skip(4); // culling mode
        } else if (type == "BSOrderedNode") {
            const glm::vec3 centre = vec3();
            n.alpha_sort_bound = glm::vec4(centre, r.read<float>());
            n.static_bound = r.read<uint8_t>() != 0;
        } else if (type == "BSValueNode") {
            r.skip(4 + 1); // value, value flags
        } else if (type == "NiBillboardNode") {
            n.billboard_mode = r.read<uint16_t>();
        } else if (type == "BSRangeNode" || type == "BSBlastNode" || type == "BSDamageStage" ||
                   type == "BSDebrisNode") {
            n.range = {r.read<uint8_t>(), r.read<uint8_t>(), r.read<uint8_t>()};
        } else if (type == "BSMasterParticleSystem") {
            r.skip(2); // max emitter objects
            refs();    // particle systems
        } else if (type == "NiSortAdjustNode") {
            r.skip(4); // sorting mode
        }
        return n;
    }

    // Decodes `count` packed vertices laid out by `desc`.
    NifGeometry vertices(BSVertexDesc desc, uint32_t count) {
        NifGeometry g;
        uint32_t stride = desc.vertex_size();
        if (count == 0) {
            return g;
        }
        if (stride == 0) {
            throw ParseError("vertex data with a zero vertex size");
        }
        auto require = [&](bool present, uint32_t offset, uint32_t size, const char* what) {
            if (present && offset + size > stride) {
                throw ParseError(
                    std::format("{} at offset {} exceeds the {}-byte vertex", what, offset, stride));
            }
            return present;
        };
        // SE positions are always 3 floats (+ bitangent x) at offset 0, whatever the full precision flag
        // says; half-float positions only exist from Fallout 4 on.
        bool has_position = require(desc.has(BSVertexDesc::attr_vertex), 0, 16, "position");
        bool has_uv = require(desc.has(BSVertexDesc::attr_uv), desc.uv_offset(), 4, "uv");
        bool has_normal = require(desc.has(BSVertexDesc::attr_normal), desc.normal_offset(), 4, "normal");
        bool has_color = require(desc.has(BSVertexDesc::attr_colors), desc.color_offset(), 4, "colour");
        bool has_tangent = require(desc.has(BSVertexDesc::attr_tangent), desc.tangent_offset(), 4, "tangent");
        bool has_skin = require(desc.has(BSVertexDesc::attr_skinned), desc.skinning_offset(), 12, "skinning");

        auto bytes = r.take(static_cast<size_t>(check_count(r, count, stride)) * stride);
        if (has_position) {
            g.positions.resize(count);
        }
        if (has_skin) {
            g.bone_weights.resize(count);
            g.bone_indices.resize(count);
        }
        if (has_uv) {
            g.uvs.resize(count);
        }
        if (has_normal) {
            g.normals.resize(count);
            g.packed_normals.resize(count);
        }
        if (has_tangent) {
            g.packed_tangents.resize(count);
        }
        if (has_position) {
            g.bitangent_x.resize(count);
        }
        if (has_color) {
            g.colors.resize(count);
        }
        for (uint32_t i = 0; i < count; ++i) {
            const std::byte* v = bytes.data() + static_cast<size_t>(i) * stride;
            if (has_position) {
                g.positions[i] = {load<float>(v), load<float>(v + 4), load<float>(v + 8)};
                g.bitangent_x[i] = load<float>(v + 12);
            }
            if (has_uv) {
                const std::byte* p = v + desc.uv_offset();
                g.uvs[i] = {half_to_float(load<uint16_t>(p)), half_to_float(load<uint16_t>(p + 2))};
            }
            if (has_normal) {
                const std::byte* p = v + desc.normal_offset();
                g.normals[i] = {unorm8_to_snorm(load<uint8_t>(p)), unorm8_to_snorm(load<uint8_t>(p + 1)),
                                unorm8_to_snorm(load<uint8_t>(p + 2))};
                g.packed_normals[i] = {load<uint8_t>(p), load<uint8_t>(p + 1), load<uint8_t>(p + 2),
                                       load<uint8_t>(p + 3)};
            }
            if (has_tangent) {
                const std::byte* p = v + desc.tangent_offset();
                g.packed_tangents[i] = {load<uint8_t>(p), load<uint8_t>(p + 1), load<uint8_t>(p + 2),
                                        load<uint8_t>(p + 3)};
            }
            if (has_color) {
                const std::byte* p = v + desc.color_offset();
                g.colors[i] = {load<uint8_t>(p), load<uint8_t>(p + 1), load<uint8_t>(p + 2),
                               load<uint8_t>(p + 3)};
            }
            if (has_skin) {
                // 4 half-float weights, then 4 byte bone indices.
                const std::byte* p = v + desc.skinning_offset();
                g.bone_weights[i] = {half_to_float(load<uint16_t>(p)), half_to_float(load<uint16_t>(p + 2)),
                                     half_to_float(load<uint16_t>(p + 4)),
                                     half_to_float(load<uint16_t>(p + 6))};
                g.bone_indices[i] = {load<uint8_t>(p + 8), load<uint8_t>(p + 9), load<uint8_t>(p + 10),
                                     load<uint8_t>(p + 11)};
            }
        }
        return g;
    }

    NiTransform transform() {
        NiTransform t;
        t.rotation = matrix33();
        t.translation = vec3();
        t.scale = r.read<float>();
        return t;
    }

    NiSkinData skin_data() {
        NiSkinData d;
        d.skin_transform = transform();
        auto num_bones = read_count(r, 70);
        bool has_weights = r.read<uint8_t>() != 0;
        d.bones.resize(num_bones);
        for (auto& b : d.bones) {
            b.skin_to_bone = transform();
            b.bound_center = vec3();
            b.bound_radius = r.read<float>();
            uint32_t n = r.read<uint16_t>();
            if (has_weights) {
                check_count(r, n, 6);
                b.weights.resize(n);
                for (auto& [vertex, weight] : b.weights) {
                    vertex = r.read<uint16_t>();
                    weight = r.read<float>();
                }
            }
        }
        return d;
    }

    std::vector<std::array<uint16_t, 3>> triangles(uint32_t count) {
        check_count(r, count, 6);
        return r.read_array<std::array<uint16_t, 3>>(count);
    }

    BSTriShape tri_shape() {
        BSTriShape s;
        av_object(s);
        s.bound_center = vec3();
        s.bound_radius = r.read<float>();
        s.skin = r.read<int32_t>();
        s.shader_property = r.read<int32_t>();
        s.alpha_property = r.read<int32_t>();
        s.vertex_desc.raw = r.read<uint64_t>();
        s.num_triangles = r.read<uint16_t>(); // u32 from Fallout 4 on
        s.num_vertices = r.read<uint16_t>();
        auto data_size = r.read<uint32_t>();
        if (data_size > 0) {
            uint64_t expected =
                uint64_t{s.num_vertices} * s.vertex_desc.vertex_size() + uint64_t{s.num_triangles} * 6;
            if (data_size != expected) {
                throw ParseError(std::format("data size {} != {} vertices x {} + {} triangles x 6", data_size,
                                             s.num_vertices, s.vertex_desc.vertex_size(), s.num_triangles));
            }
            s.geometry = vertices(s.vertex_desc, s.num_vertices);
            s.geometry.triangles = triangles(s.num_triangles);
        }
        if (bsver() == k_bs_version_se) {
            auto particle_size = r.read<uint32_t>();
            if (particle_size > 0) {
                // Half-precision positions, then normals (3 halves each per vertex), then triangles. Not
                // drawn; NiPSysMeshEmitter samples it.
                check_count(r, uint64_t{s.num_vertices} * 12 + uint64_t{s.num_triangles} * 6, 1);
                auto halves = r.read_array<uint16_t>(size_t{s.num_vertices} * 6);
                s.particle_positions.resize(s.num_vertices);
                s.particle_normals.resize(s.num_vertices);
                for (uint32_t i = 0; i < s.num_vertices; ++i) {
                    for (int k = 0; k < 3; ++k) {
                        s.particle_positions[i][k] = half_to_float(halves[size_t{i} * 3 + k]);
                        s.particle_normals[i][k] = half_to_float(halves[size_t{s.num_vertices} * 3 + i * 3 + k]);
                    }
                }
                s.particle_triangles = triangles(s.num_triangles);
            }
        }
        if (type == "BSMeshLODTriShape") {
            s.has_lods = true;
            for (auto& lod : s.lod_sizes) {
                lod = r.read<uint32_t>();
            }
        } else if (type == "BSSubIndexTriShape") {
            if (bsver() == k_bs_version_se) {
                auto segments = read_count(r, 9);
                r.skip(size_t{segments} * 9); // flags u8, start index, triangle count
            } else if (data_size > 0) {
                throw ParseError("BSSubIndexTriShape is only decoded for BS version 100");
            }
        } else if (type == "BSDynamicTriShape") {
            // Facegen: positions are a separate float4 array that the game updates for morphs.
            auto dynamic_size = r.read<uint32_t>();
            auto count = check_count(r, dynamic_size / 16, 16);
            if (dynamic_size % 16 != 0) {
                throw ParseError(std::format("dynamic data size {} is not a multiple of 16", dynamic_size));
            }
            if (count > 0) {
                auto positions = r.read_array<glm::vec4>(count);
                s.geometry.positions.resize(count);
                s.geometry.bitangent_x.resize(count);
                for (uint32_t i = 0; i < count; ++i) {
                    s.geometry.positions[i] = glm::vec3(positions[i]);
                    s.geometry.bitangent_x[i] = positions[i].w;
                }
                s.dynamic = true;
            }
        }
        return s;
    }

    NiGeometry geometry() {
        NiGeometry g;
        av_object(g);
        g.data = r.read<int32_t>();
        g.skin = r.read<int32_t>();
        // Material data: names (string refs) and extra data per material, then the active one.
        auto materials = read_count(r, 8);
        r.skip(size_t{materials} * 8);
        r.skip(4); // active material
        r.skip(1); // material needs update
        g.shader_property = r.read<int32_t>();
        g.alpha_property = r.read<int32_t>();
        if (type == "BSLODTriShape") {
            g.has_lods = true;
            for (auto& lod : g.lod_sizes) {
                lod = r.read<uint32_t>();
            }
        } else if (type == "BSSegmentedTriShape") {
            auto segments = read_count(r, 9);
            r.skip(size_t{segments} * 9);
        }
        return g;
    }

    NiGeometryData geometry_data() {
        NiGeometryData d;
        auto& g = d.geometry;
        r.skip(4); // group id
        uint32_t count = r.read<uint16_t>();
        r.skip(2); // keep flags, compress flags
        if (r.read<uint8_t>()) {
            check_count(r, count, 12);
            g.positions = r.read_array<glm::vec3>(count);
        }
        // Bethesda data flags: bit 0 has UVs, bit 12 has tangents.
        auto data_flags = r.read<uint16_t>();
        r.skip(4); // material CRC
        if (r.read<uint8_t>()) {
            check_count(r, count, 12);
            g.normals = r.read_array<glm::vec3>(count);
            if (data_flags & 0x1000) {
                check_count(r, count, 24);
                g.tangents = r.read_array<glm::vec3>(count);
                g.bitangents = r.read_array<glm::vec3>(count);
            }
        }
        d.bound_center = vec3();
        d.bound_radius = r.read<float>();
        if (r.read<uint8_t>()) {
            check_count(r, count, 16);
            auto colors = r.read_array<glm::vec4>(count);
            g.colors.resize(count);
            for (uint32_t i = 0; i < count; ++i) {
                auto c = glm::clamp(colors[i], 0.0f, 1.0f) * 255.0f + 0.5f;
                g.colors[i] = glm::u8vec4(c);
            }
        }
        if (data_flags & 0x1) {
            check_count(r, count, 8);
            g.uvs = r.read_array<glm::vec2>(count);
        }
        r.skip(2); // consistency flags
        r.skip(4); // additional data
        uint32_t num_triangles = r.read<uint16_t>();
        if (type == "NiTriShapeData") {
            r.skip(4); // number of triangle points
            if (r.read<uint8_t>()) {
                g.triangles = triangles(num_triangles);
            }
            uint32_t match_groups = r.read<uint16_t>();
            for (uint32_t i = 0; i < match_groups; ++i) {
                uint32_t n = r.read<uint16_t>();
                r.skip(size_t{n} * 2);
            }
        } else {
            // NiTriStripsData
            uint32_t strips = r.read<uint16_t>();
            check_count(r, strips, 2);
            auto lengths = r.read_array<uint16_t>(strips);
            if (r.read<uint8_t>()) {
                g.triangles.reserve(num_triangles);
                for (auto len : lengths) {
                    check_count(r, len, 2);
                    auto points = r.read_array<uint16_t>(len);
                    for (size_t i = 2; i < points.size(); ++i) {
                        // Strips alternate winding; odd triangles are flipped back.
                        std::array<uint16_t, 3> t = {points[i - 2], points[i - 1], points[i]};
                        if (i % 2 == 1) {
                            std::swap(t[1], t[2]);
                        }
                        if (t[0] != t[1] && t[1] != t[2] && t[0] != t[2]) {
                            g.triangles.push_back(t);
                        }
                    }
                }
            }
        }
        return d;
    }

    NiSkinInstance skin_instance() {
        NiSkinInstance s;
        s.data = r.read<int32_t>();
        s.skin_partition = r.read<int32_t>();
        s.skeleton_root = r.read<int32_t>();
        s.bones = refs();
        if (type == "BSDismemberSkinInstance") {
            auto partitions = read_count(r, 4);
            for (uint32_t i = 0; i < partitions; ++i) {
                r.skip(2); // part flag
                s.body_parts.push_back(r.read<uint16_t>());
            }
        }
        return s;
    }

    NiSkinPartition skin_partition() {
        NiSkinPartition s;
        auto num_partitions = read_count(r, 10);
        if (bsver() == k_bs_version_se) {
            auto data_size = r.read<uint32_t>();
            auto vertex_size = r.read<uint32_t>();
            s.vertex_desc.raw = r.read<uint64_t>();
            if (data_size > 0) {
                if (vertex_size == 0 || data_size % vertex_size != 0 ||
                    vertex_size != s.vertex_desc.vertex_size()) {
                    throw ParseError(std::format("skin partition data size {} / vertex size {} (desc {})",
                                                 data_size, vertex_size, s.vertex_desc.vertex_size()));
                }
                s.geometry = vertices(s.vertex_desc, data_size / vertex_size);
            }
        }
        s.partitions.resize(num_partitions);
        for (auto& p : s.partitions) {
            uint32_t num_vertices = r.read<uint16_t>();
            uint32_t num_triangles = r.read<uint16_t>();
            uint32_t num_bones = r.read<uint16_t>();
            uint32_t num_strips = r.read<uint16_t>();
            uint32_t weights_per_vertex = r.read<uint16_t>();
            check_count(r, num_bones, 2);
            p.bones = r.read_array<uint16_t>(num_bones);
            if (r.read<uint8_t>()) {
                check_count(r, num_vertices, 2);
                p.vertex_map = r.read_array<uint16_t>(num_vertices);
            }
            if (r.read<uint8_t>()) {
                r.skip(check_count(r, uint64_t{num_vertices} * weights_per_vertex, 4) * size_t{4});
            }
            check_count(r, num_strips, 2);
            auto strip_lengths = r.read_array<uint16_t>(num_strips);
            std::vector<std::array<uint16_t, 3>> local;
            if (r.read<uint8_t>()) {
                if (num_strips == 0) {
                    local = triangles(num_triangles);
                } else {
                    for (auto len : strip_lengths) {
                        check_count(r, len, 2);
                        auto points = r.read_array<uint16_t>(len);
                        for (size_t i = 2; i < points.size(); ++i) {
                            std::array<uint16_t, 3> t = {points[i - 2], points[i - 1], points[i]};
                            if (i % 2 == 1) {
                                std::swap(t[1], t[2]);
                            }
                            if (t[0] != t[1] && t[1] != t[2] && t[0] != t[2]) {
                                local.push_back(t);
                            }
                        }
                    }
                }
            }
            if (r.read<uint8_t>()) {
                r.skip(check_count(r, uint64_t{num_vertices} * weights_per_vertex, 1)); // bone indices
            }
            r.skip(2); // LOD level, global VB
            if (bsver() == k_bs_version_se) {
                r.skip(8); // vertex desc (same as the partition block's)
                p.triangles = triangles(num_triangles);
                if (!local.empty() && local != p.triangles) {
                    throw ParseError("partition triangle copy differs from its triangles");
                }
            } else {
                p.triangles.reserve(local.size());
                for (auto t : local) {
                    for (auto& i : t) {
                        if (i >= p.vertex_map.size()) {
                            throw ParseError(std::format("partition vertex {} outside its vertex map", i));
                        }
                        i = p.vertex_map[i];
                    }
                    p.triangles.push_back(t);
                }
            }
        }
        return s;
    }

    BSLightingShaderProperty lighting_shader() {
        BSLightingShaderProperty s;
        s.shader_type = r.read<uint32_t>(); // Skyrim stores the shader type ahead of the NiObjectNET fields
        object_net(s);
        s.flags1 = r.read<uint32_t>();
        s.flags2 = r.read<uint32_t>();
        s.uv_offset = vec2();
        s.uv_scale = vec2();
        s.texture_set = r.read<int32_t>();
        s.emissive_color = vec3();
        s.emissive_multiple = r.read<float>();
        s.texture_clamp = r.read<uint32_t>();
        s.alpha = r.read<float>();
        s.refraction_strength = r.read<float>();
        s.glossiness = r.read<float>();
        s.specular_color = vec3();
        s.specular_strength = r.read<float>();
        s.lighting_effect1 = r.read<float>();
        s.lighting_effect2 = r.read<float>();
        auto params = [&](size_t n) {
            for (size_t i = 0; i < n; ++i) {
                s.type_params[i] = r.read<float>();
            }
        };
        switch (s.shader_type) {
        case 1: // environment map: scale
            params(1);
            break;
        case 5: // skin tint colour
        case 6: // hair tint colour
            params(3);
            break;
        case 7: // parallax occlusion: max passes, scale
            params(2);
            break;
        case 11: // multilayer parallax: thickness, refraction scale, inner uv scale xy, env map strength
            params(5);
            break;
        case 14: // sparkle snow
            params(4);
            break;
        case 16: // eye: cubemap scale, left reflection centre, right reflection centre
            params(7);
            break;
        default:
            break;
        }
        return s;
    }

    BSEffectShaderProperty effect_shader() {
        BSEffectShaderProperty s;
        object_net(s);
        s.flags1 = r.read<uint32_t>();
        s.flags2 = r.read<uint32_t>();
        s.uv_offset = vec2();
        s.uv_scale = vec2();
        s.source_texture = sized_string();
        s.texture_clamp = r.read<uint8_t>();
        s.lighting_influence = r.read<uint8_t>();
        s.env_map_min_lod = r.read<uint8_t>();
        r.skip(1);
        s.falloff = vec4();
        s.base_color = vec4();
        s.base_color_scale = r.read<float>();
        s.soft_falloff_depth = r.read<float>();
        s.greyscale_texture = sized_string();
        return s;
    }

    BSSkyShaderProperty sky_shader() {
        BSSkyShaderProperty s;
        object_net(s);
        s.flags1 = r.read<uint32_t>();
        s.flags2 = r.read<uint32_t>();
        s.uv_offset = vec2();
        s.uv_scale = vec2();
        s.source_texture = sized_string();
        s.sky_object_type = r.read<uint32_t>();
        return s;
    }

    BSShaderTextureSet texture_set() {
        BSShaderTextureSet s;
        auto count = read_count(r, 4);
        s.textures.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            s.textures.push_back(sized_string());
        }
        return s;
    }

    NiAlphaProperty alpha_property() {
        NiAlphaProperty a;
        object_net(a);
        a.flags = r.read<uint16_t>();
        a.threshold = r.read<uint8_t>();
        return a;
    }

    NiIntegerExtraData integer_extra_data() {
        NiIntegerExtraData e;
        e.name = string_ref();
        e.value = r.read<uint32_t>();
        return e;
    }

    NiStringExtraData string_extra_data() {
        NiStringExtraData e;
        e.name = string_ref();
        e.value = string_ref();
        return e;
    }

    NiCamera camera() {
        NiCamera c;
        av_object(c);
        r.read<uint16_t>(); // camera flags
        c.frustum_left = r.read<float>();
        c.frustum_right = r.read<float>();
        c.frustum_top = r.read<float>();
        c.frustum_bottom = r.read<float>();
        c.frustum_near = r.read<float>();
        c.frustum_far = r.read<float>();
        r.read<uint8_t>(); // orthographic
        for (int i = 0; i < 5; ++i) {
            r.read<float>(); // the viewport, LOD adjust
        }
        r.read<int32_t>();  // scene
        r.read<uint32_t>(); // screen polygons
        r.read<uint32_t>(); // screen textures
        return c;
    }

    BSFurnitureMarker furniture_marker() {
        BSFurnitureMarker m;
        m.name = string_ref();
        const uint32_t count = r.read<uint32_t>();
        m.positions.resize(count);
        for (FurnitureMarkerPosition& p : m.positions) {
            p.offset = vec3();
            p.heading = r.read<float>();
            p.animation = r.read<uint16_t>();
            p.entry = r.read<uint16_t>();
        }
        return m;
    }

    BSBound bound() {
        BSBound b;
        b.name = string_ref();
        b.center = vec3();
        b.half_extents = vec3();
        return b;
    }

    NiRangeLODData range_lod_data() {
        NiRangeLODData d;
        d.center = vec3();
        auto count = read_count(r, 8);
        d.ranges = r.read_array<std::array<float, 2>>(count);
        return d;
    }

    // ---- Havok ------------------------------------------------------------------------------------------

    HavokFilter havok_filter() {
        HavokFilter f;
        f.layer = r.read<uint8_t>();
        f.flags_and_part = r.read<uint8_t>();
        f.group = r.read<uint16_t>();
        return f;
    }

    // bhkWorldObjCInfoProperty: data, size, capacity and flags; runtime-only.
    void cinfo_property() { r.skip(12); }

    bhkCollisionObject collision_object() {
        bhkCollisionObject c;
        c.target = r.read<int32_t>();
        c.flags = r.read<uint16_t>();
        c.body = r.read<int32_t>();
        if (type == "bhkBlendCollisionObject") {
            r.skip(8); // heir gain, velocity gain
        }
        return c;
    }

    bhkRigidBody rigid_body() {
        bhkRigidBody b;
        // bhkWorldObject
        b.shape = r.read<int32_t>();
        b.filter = havok_filter();
        r.skip(4);       // bhkWorldObjCInfo: unused
        r.skip(1 + 3);   // broad phase type, unused
        cinfo_property();
        // bhkRigidBodyCInfo (Skyrim): the response and filter are repeated around runtime-only words.
        r.skip(4);       // collision response, unused, process contact callback delay
        r.skip(4);       // unknown
        havok_filter();  // copy of the body's filter
        r.skip(4 + 4);   // unused, unknown
        r.skip(4);       // collision response, unused, callback delay (copy)
        b.translation = vec4();
        b.rotation = vec4();
        r.skip(16 + 16); // linear and angular velocity
        for (int row = 0; row < 3; ++row) { // inertia tensor
            const glm::vec4 v = vec4();
            b.inertia[row] = v[row];
        }
        r.skip(16);      // centre of mass
        b.mass = r.read<float>();
        b.linear_damping = r.read<float>();
        b.angular_damping = r.read<float>();
        r.skip(4 + 4);   // time factor, gravity factor
        b.friction = r.read<float>();
        r.skip(4);       // rolling friction multiplier
        b.restitution = r.read<float>();
        b.max_linear_velocity = r.read<float>();
        b.max_angular_velocity = r.read<float>();
        r.skip(4);       // penetration depth
        b.motion_system = r.read<uint8_t>();
        r.skip(1 + 1);   // enable deactivation, solver deactivation
        b.quality_type = r.read<uint8_t>();
        r.skip(16);      // auto remove level, response modifier flags, shape keys, unused
        b.constraints = refs();
        b.body_flags = r.read<uint16_t>();
        return b;
    }

    bhkJoint joint(bool hinge) {
        bhkJoint j;
        j.hinge = hinge;
        r.skip(16);      // entity count, the two entities, priority
        r.skip(8 * 16);  // each body's axes and pivot
        if (hinge) {
            j.hinge_min = r.read<float>();
            j.hinge_max = r.read<float>();
        } else {
            j.cone_max = r.read<float>();
            j.plane_min = r.read<float>();
            j.plane_max = r.read<float>();
            j.twist_min = r.read<float>();
            j.twist_max = r.read<float>();
        }
        j.max_friction = r.read<float>();
        r.skip(1);       // motor type: none in these files
        return j;
    }

    bhkMoppBvTreeShape mopp_shape() {
        bhkMoppBvTreeShape s;
        s.shape = r.read<int32_t>();
        r.skip(12); // unused
        s.shape_scale = r.read<float>();
        auto mopp_size = r.read<uint32_t>();
        r.skip(12 + 4); // MOPP origin, scale
        r.skip(1);      // build type
        s.code = r.read_array<uint8_t>(check_count(r, mopp_size, 1));
        return s;
    }

    bhkCompressedMeshShape compressed_mesh_shape() {
        bhkCompressedMeshShape s;
        s.target = r.read<int32_t>();
        r.skip(4); // user data
        s.radius = r.read<float>();
        r.skip(4); // unknown
        s.scale = vec4();
        r.skip(4 + 16); // radius copy, scale copy
        s.data = r.read<int32_t>();
        return s;
    }

    template <typename T>
    std::vector<T> counted_array(size_t element_size) {
        auto count = read_count(r, element_size);
        return r.read_array<T>(count);
    }

    bhkCompressedMeshShapeData compressed_mesh_data() {
        bhkCompressedMeshShapeData d;
        d.bits_per_index = r.read<uint32_t>();
        d.bits_per_w_index = r.read<uint32_t>();
        d.w_index_mask = r.read<uint32_t>();
        d.index_mask = r.read<uint32_t>();
        d.error = r.read<float>();
        d.aabb_min = vec4();
        d.aabb_max = vec4();
        r.skip(1 + 1); // welding type, material type
        for (int i = 0; i < 3; ++i) {
            counted_array<uint32_t>(4); // 32/16/8-bit material tables, unused by Skyrim
        }
        auto num_materials = read_count(r, 8);
        d.materials.resize(num_materials);
        for (auto& m : d.materials) {
            m.material = r.read<uint32_t>();
            m.filter = havok_filter();
        }
        r.skip(4); // named materials
        auto num_transforms = read_count(r, 32);
        d.transforms.resize(num_transforms);
        for (auto& t : d.transforms) {
            t.translation = vec4();
            t.rotation = vec4();
        }
        d.big_vertices = counted_array<glm::vec4>(16);
        auto num_big = read_count(r, 12);
        d.big_triangles.resize(num_big);
        for (auto& t : d.big_triangles) {
            t.vertices = {r.read<uint16_t>(), r.read<uint16_t>(), r.read<uint16_t>()};
            t.material = r.read<uint32_t>();
            t.welding = r.read<uint16_t>();
        }
        auto num_chunks = read_count(r, 36);
        d.chunks.resize(num_chunks);
        for (auto& c : d.chunks) {
            c.translation = vec4();
            c.material = r.read<uint32_t>();
            r.skip(2); // reference, always 0xFFFF
            c.transform = r.read<uint16_t>();
            c.vertices = counted_array<uint16_t>(2); // count is in components
            if (c.vertices.size() % 3 != 0) {
                throw ParseError(std::format("chunk with {} vertex components", c.vertices.size()));
            }
            c.indices = counted_array<uint16_t>(2);
            c.strips = counted_array<uint16_t>(2);
            counted_array<uint16_t>(2); // welding info
        }
        r.skip(4); // convex pieces, always 0
        return d;
    }

    bhkConvexVerticesShape convex_vertices_shape() {
        bhkConvexVerticesShape s;
        s.material = r.read<uint32_t>();
        s.radius = r.read<float>();
        cinfo_property(); // vertices
        cinfo_property(); // normals
        s.vertices = counted_array<glm::vec4>(16);
        s.planes = counted_array<glm::vec4>(16);
        return s;
    }

    bhkBoxShape box_shape() {
        bhkBoxShape s;
        s.material = r.read<uint32_t>();
        s.radius = r.read<float>();
        r.skip(8);
        s.half_extents = vec3();
        r.skip(4);
        return s;
    }

    bhkSphereShape sphere_shape() {
        bhkSphereShape s;
        s.material = r.read<uint32_t>();
        s.radius = r.read<float>();
        return s;
    }

    bhkCapsuleShape capsule_shape() {
        bhkCapsuleShape s;
        s.material = r.read<uint32_t>();
        s.radius = r.read<float>();
        r.skip(8);
        s.a = vec3();
        r.skip(4); // radius at a (same as the shape radius)
        s.b = vec3();
        r.skip(4); // radius at b
        return s;
    }

    bhkCylinderShape cylinder_shape() {
        bhkCylinderShape s;
        s.material = r.read<uint32_t>();
        s.radius = r.read<float>();
        r.skip(8);
        s.a = glm::vec3(vec4());
        s.b = glm::vec3(vec4());
        s.cylinder_radius = r.read<float>();
        r.skip(12);
        return s;
    }

    bhkListShape list_shape() {
        bhkListShape s;
        s.shapes = refs();
        s.material = r.read<uint32_t>();
        cinfo_property(); // child shapes
        cinfo_property(); // child filters
        auto n = read_count(r, 4);
        s.filters.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            s.filters.push_back(havok_filter());
        }
        return s;
    }

    bhkTransformShape transform_shape() {
        bhkTransformShape s;
        s.shape = r.read<int32_t>();
        s.material = r.read<uint32_t>();
        s.radius = r.read<float>();
        r.skip(8);
        // hkTransform: three rotation columns, then the translation (each a Vector4).
        for (int col = 0; col < 4; ++col) {
            s.transform[col] = vec4();
        }
        return s;
    }

    bhkPackedNiTriStripsShape packed_tri_strips_shape() {
        bhkPackedNiTriStripsShape s;
        r.skip(4 + 4); // user data, unused
        s.radius = r.read<float>();
        r.skip(4);
        s.scale = vec4();
        r.skip(4 + 16); // radius copy, scale copy
        s.data = r.read<int32_t>();
        return s;
    }

    hkPackedNiTriStripsData packed_tri_strips_data() {
        hkPackedNiTriStripsData d;
        auto num_triangles = read_count(r, 8);
        d.triangles.resize(num_triangles);
        for (auto& t : d.triangles) {
            t = {r.read<uint16_t>(), r.read<uint16_t>(), r.read<uint16_t>()};
            r.skip(2); // welding info
        }
        auto num_vertices = r.read<uint32_t>();
        bool compressed = r.read<uint8_t>() != 0;
        check_count(r, num_vertices, compressed ? 6 : 12);
        d.vertices.resize(num_vertices);
        for (auto& v : d.vertices) {
            if (compressed) {
                v = {half_to_float(r.read<uint16_t>()), half_to_float(r.read<uint16_t>()),
                     half_to_float(r.read<uint16_t>())};
            } else {
                v = vec3();
            }
        }
        uint32_t num_sub_shapes = r.read<uint16_t>();
        check_count(r, num_sub_shapes, 12);
        d.sub_shapes.resize(num_sub_shapes);
        for (auto& s : d.sub_shapes) {
            s.filter = havok_filter();
            s.vertex_count = r.read<uint32_t>();
            s.material = r.read<uint32_t>();
        }
        return d;
    }

    bhkNiTriStripsShape tri_strips_shape() {
        bhkNiTriStripsShape s;
        s.material = r.read<uint32_t>();
        s.radius = r.read<float>();
        r.skip(20); // unused
        r.skip(4);  // grow by
        s.scale = vec4();
        s.strips = refs();
        auto n = read_count(r, 4);
        s.filters.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            s.filters.push_back(havok_filter());
        }
        return s;
    }

    // ---- Particle systems. Layouts: niftools nif.xml for 20.2.0.7 / BS 100, cross-checked against the
    // game's LoadBinary functions. SE only; LE particle blocks stay opaque.

    bool flag() { return r.read<uint8_t>() != 0; }

    NiParticleSystem particle_system() {
        NiParticleSystem s;
        av_object(s);
        s.bound_center = vec3();
        s.bound_radius = r.read<float>();
        s.skin = r.read<int32_t>();
        s.shader_property = r.read<int32_t>();
        s.alpha_property = r.read<int32_t>();
        s.vertex_desc.raw = r.read<uint64_t>();
        for (auto& d : s.lod_distances) {
            d = r.read<uint16_t>();
        }
        s.data = r.read<int32_t>();
        s.world_space = flag();
        s.modifiers = refs();
        return s;
    }

    NiPSysData psys_data() {
        NiPSysData d;
        r.skip(4); // group id
        d.max_particles = r.read<uint16_t>();
        r.skip(2);  // keep flags, compress flags
        r.skip(1);  // has vertices: positions are runtime state, never streamed for SE
        r.skip(2);  // data flags
        r.skip(4);  // material CRC
        r.skip(1);  // has normals
        d.bound_center = vec3();
        d.bound_radius = r.read<float>();
        d.has_colors = flag();
        r.skip(2); // consistency flags
        r.skip(4); // additional data
        d.has_radii = flag();
        d.num_active = r.read<uint16_t>();
        d.has_sizes = flag();
        d.has_rotations = flag();
        d.has_rotation_angles = flag();
        d.has_rotation_axes = flag();
        d.has_texture_indices = flag();
        auto offsets = read_count(r, 16);
        d.subtexture_offsets = r.read_array<glm::vec4>(offsets);
        d.aspect_ratio = r.read<float>();
        d.aspect_flags = r.read<uint16_t>();
        for (auto& f : d.speed_to_aspect) {
            f = r.read<float>();
        }
        d.has_rotation_speeds = flag();
        return d;
    }

    void modifier_base(NiPSysModifierBase& m) {
        m.name = string_ref();
        m.order = r.read<uint32_t>();
        m.target = r.read<int32_t>();
        m.active = flag();
    }

    template <typename T>
    T modifier() {
        T m;
        modifier_base(m);
        return m;
    }

    NiPSysAgeDeathModifier age_death() {
        auto m = modifier<NiPSysAgeDeathModifier>();
        m.spawn_on_death = flag();
        m.spawn_modifier = r.read<int32_t>();
        return m;
    }

    BSPSysLODModifier lod_modifier() {
        auto m = modifier<BSPSysLODModifier>();
        m.begin_distance = r.read<float>();
        m.end_distance = r.read<float>();
        m.end_emit_scale = r.read<float>();
        m.end_size = r.read<float>();
        return m;
    }

    NiPSysMeshEmitter mesh_emitter() {
        auto m = modifier<NiPSysMeshEmitter>();
        m.speed = r.read<float>();
        m.speed_variation = r.read<float>();
        m.declination = r.read<float>();
        m.declination_variation = r.read<float>();
        m.planar_angle = r.read<float>();
        m.planar_angle_variation = r.read<float>();
        m.initial_color = vec4();
        m.initial_radius = r.read<float>();
        m.radius_variation = r.read<float>();
        m.life_span = r.read<float>();
        m.life_span_variation = r.read<float>();
        m.meshes = refs();
        m.velocity_type = r.read<uint32_t>();
        m.emission_type = r.read<uint32_t>();
        m.emission_axis = vec3();
        return m;
    }

    NiPSysVolumeEmitter volume_emitter(VolumeShape shape) {
        auto m = modifier<NiPSysVolumeEmitter>();
        m.speed = r.read<float>();
        m.speed_variation = r.read<float>();
        m.declination = r.read<float>();
        m.declination_variation = r.read<float>();
        m.planar_angle = r.read<float>();
        m.planar_angle_variation = r.read<float>();
        m.initial_color = vec4();
        m.initial_radius = r.read<float>();
        m.radius_variation = r.read<float>();
        m.life_span = r.read<float>();
        m.life_span_variation = r.read<float>();
        m.emitter_object = r.read<int32_t>();
        m.shape = shape;
        const int extents = shape == VolumeShape::sphere ? 1 : shape == VolumeShape::box ? 3 : 2;
        for (int i = 0; i < extents; ++i) {
            m.extents[i] = r.read<float>();
        }
        return m;
    }

    NiPSysSpawnModifier spawn_modifier() {
        auto m = modifier<NiPSysSpawnModifier>();
        m.num_generations = r.read<uint16_t>();
        m.percentage_spawned = r.read<float>();
        m.min_spawned = r.read<uint16_t>();
        m.max_spawned = r.read<uint16_t>();
        m.speed_variation = r.read<float>();
        m.direction_variation = r.read<float>();
        m.life_span = r.read<float>();
        m.life_span_variation = r.read<float>();
        return m;
    }

    BSPSysSimpleColorModifier color_modifier() {
        auto m = modifier<BSPSysSimpleColorModifier>();
        m.fade_in = r.read<float>();
        m.fade_out = r.read<float>();
        m.color1_start = r.read<float>();
        m.color1_end = r.read<float>();
        m.color2_start = r.read<float>();
        m.color2_end = r.read<float>();
        for (auto& c : m.colors) {
            c = vec4();
        }
        return m;
    }

    NiPSysRotationModifier rotation_modifier() {
        auto m = modifier<NiPSysRotationModifier>();
        m.speed = r.read<float>();
        m.speed_variation = r.read<float>();
        m.angle = r.read<float>();
        m.angle_variation = r.read<float>();
        m.random_speed_sign = flag();
        m.random_axis = flag();
        m.axis = vec3();
        return m;
    }

    // nif.xml types "Start Frame" as uint; the game reads all seven fields as floats.
    BSPSysSubTexModifier subtex_modifier() {
        auto m = modifier<BSPSysSubTexModifier>();
        m.start_frame = r.read<float>();
        m.start_frame_fudge = r.read<float>();
        m.end_frame = r.read<float>();
        m.loop_start_frame = r.read<float>();
        m.loop_start_frame_fudge = r.read<float>();
        m.frame_count = r.read<float>();
        m.frame_count_fudge = r.read<float>();
        return m;
    }

    BSPSysScaleModifier scale_modifier() {
        auto m = modifier<BSPSysScaleModifier>();
        auto n = read_count(r, 4);
        m.scales = r.read_array<float>(n);
        return m;
    }

    NiPSysGravityModifier gravity_modifier() {
        auto m = modifier<NiPSysGravityModifier>();
        m.gravity_object = r.read<int32_t>();
        m.axis = vec3();
        m.decay = r.read<float>();
        m.strength = r.read<float>();
        m.force_type = r.read<uint32_t>();
        m.turbulence = r.read<float>();
        m.turbulence_scale = r.read<float>();
        m.world_aligned = flag();
        return m;
    }

    NiPSysDragModifier drag_modifier() {
        auto m = modifier<NiPSysDragModifier>();
        m.parent = r.read<int32_t>();
        m.axis = vec3();
        m.percentage = r.read<float>();
        m.range = r.read<float>();
        m.range_falloff = r.read<float>();
        return m;
    }

    NiPSysBoundUpdateModifier bound_update_modifier() {
        auto m = modifier<NiPSysBoundUpdateModifier>();
        m.update_skip = r.read<uint16_t>();
        return m;
    }

    // ---- Animation.

    void time_controller(NiTimeController& c) {
        c.next = r.read<int32_t>();
        c.flags = r.read<uint16_t>();
        c.frequency = r.read<float>();
        c.phase = r.read<float>();
        c.start_time = r.read<float>();
        c.stop_time = r.read<float>();
        c.target = r.read<int32_t>();
    }

    static bool is_modifier_ctlr(std::string_view t) {
        static constexpr std::string_view k_types[] = {
            "NiPSysModifierActiveCtlr",      "NiPSysEmitterInitialRadiusCtlr", "NiPSysEmitterLifeSpanCtlr",
            "NiPSysEmitterSpeedCtlr",        "NiPSysEmitterDeclinationCtlr",   "NiPSysEmitterDeclinationVarCtlr",
            "NiPSysEmitterPlanarAngleCtlr",  "NiPSysEmitterPlanarAngleVarCtlr", "NiPSysGravityStrengthCtlr",
            "NiPSysInitialRotSpeedCtlr",     "NiPSysInitialRotSpeedVarCtlr",   "NiPSysInitialRotAngleCtlr",
            "NiPSysInitialRotAngleVarCtlr",
        };
        return std::ranges::find(k_types, t) != std::end(k_types);
    }

    static bool is_shader_ctlr(std::string_view t) {
        return t == "BSEffectShaderPropertyFloatController" || t == "BSEffectShaderPropertyColorController" ||
               t == "BSLightingShaderPropertyFloatController" || t == "BSLightingShaderPropertyColorController";
    }

    static bool is_controller_type(std::string_view t) {
        return is_modifier_ctlr(t) || is_shader_ctlr(t) || t == "NiPSysEmitterCtlr" || t == "BSPSysMultiTargetEmitterCtlr" ||
               t == "NiPSysUpdateCtlr" ||
               t == "NiMultiTargetTransformController";
    }

    NiTimeController controller() {
        NiTimeController c;
        time_controller(c);
        if (type == "NiPSysUpdateCtlr") {
            return c;
        }
        if (type == "NiMultiTargetTransformController") {
            uint32_t n = r.read<uint16_t>();
            check_count(r, n, 4);
            c.extra_targets = r.read_array<NifRef>(n);
            return c;
        }
        c.interpolator = r.read<int32_t>(); // NiSingleInterpController
        if (is_shader_ctlr(type)) {
            c.controlled_variable = r.read<uint32_t>();
            return c;
        }
        c.modifier_name = string_ref();
        if (type == "NiPSysEmitterCtlr" || type == "BSPSysMultiTargetEmitterCtlr") {
            c.visibility_interpolator = r.read<int32_t>();
        }
        if (type == "BSPSysMultiTargetEmitterCtlr") {
            r.skip(2 + 4); // max emitters, master particle system
        }
        return c;
    }

    NiControllerManager controller_manager() {
        NiControllerManager m;
        time_controller(m.base);
        m.cumulative = flag();
        m.sequences = refs();
        m.object_palette = r.read<int32_t>();
        return m;
    }

    NiControllerSequence controller_sequence() {
        NiControllerSequence s;
        s.name = string_ref();
        auto n = read_count(r, 29);
        r.skip(4); // array grow by
        s.blocks.resize(n);
        for (auto& b : s.blocks) {
            b.interpolator = r.read<int32_t>();
            b.controller = r.read<int32_t>();
            b.priority = r.read<uint8_t>();
            b.node_name = string_ref();
            b.property_type = string_ref();
            b.controller_type = string_ref();
            b.controller_id = string_ref();
            b.interpolator_id = string_ref();
        }
        s.weight = r.read<float>();
        s.text_keys = r.read<int32_t>();
        s.cycle_type = r.read<uint32_t>();
        s.frequency = r.read<float>();
        s.start_time = r.read<float>();
        s.stop_time = r.read<float>();
        s.manager = r.read<int32_t>();
        s.accum_root_name = string_ref();
        uint32_t notes = r.read<uint16_t>();
        check_count(r, notes, 4);
        s.anim_notes = r.read_array<NifRef>(notes);
        return s;
    }

    NiInterpolator interpolator() {
        NiInterpolator i;
        if (type == "NiFloatInterpolator") {
            i.float_value = r.read<float>();
            i.data = r.read<int32_t>();
        } else if (type == "NiBoolInterpolator" || type == "NiBoolTimelineInterpolator") {
            i.bool_value = r.read<uint8_t>();
            i.data = r.read<int32_t>();
        } else {
            // NiBlendInterpolator
            i.blend_flags = r.read<uint8_t>();
            i.array_size = r.read<uint8_t>();
            i.weight_threshold = r.read<float>();
            if ((i.blend_flags & 1) == 0) {
                // Not manager controlled: the blend state is streamed too (rebuilt at runtime).
                r.skip(4 + 4 * 4);
                r.skip(check_count(r, i.array_size, 17) * size_t{17});
            }
            if (type == "NiBlendFloatInterpolator") {
                i.float_value = r.read<float>();
            } else {
                i.bool_value = r.read<uint8_t>();
            }
        }
        return i;
    }

    NiFloatData float_data() {
        NiFloatData d;
        auto n = read_count(r, 8);
        if (n == 0) {
            return d;
        }
        d.key_type = r.read<uint32_t>();
        d.keys.resize(n);
        for (auto& k : d.keys) {
            k.time = r.read<float>();
            k.value = r.read<float>();
            if (d.key_type == 2) {
                k.forward = r.read<float>();
                k.backward = r.read<float>();
            } else if (d.key_type == 3) {
                for (auto& f : k.tbc) {
                    f = r.read<float>();
                }
            }
        }
        return d;
    }

    NiTransformInterpolator transform_interpolator() {
        NiTransformInterpolator t;
        t.translation = vec3();
        for (int i = 0; i < 4; ++i) {
            t.rotation[i] = r.read<float>(); // w, x, y, z as stored
        }
        t.scale = r.read<float>();
        t.data = r.read<int32_t>();
        return t;
    }

    NiTransformData transform_data() {
        NiTransformData d;
        const uint32_t rotations = r.read<uint32_t>();
        if (rotations > 0) {
            d.rotation_type = r.read<uint32_t>();
            if (d.rotation_type == 4) {
                for (auto& axis : d.xyz) {
                    axis = float_data();
                }
            } else {
                check_count(r, rotations, d.rotation_type == 3 ? 32 : 20);
                d.rotation_keys.resize(rotations);
                for (auto& k : d.rotation_keys) {
                    k.time = r.read<float>();
                    for (int i = 0; i < 4; ++i) {
                        k.value[i] = r.read<float>(); // w, x, y, z as stored
                    }
                    if (d.rotation_type == 3) {
                        for (auto& f : k.tbc) {
                            f = r.read<float>();
                        }
                    }
                }
            }
        }
        const uint32_t translations = r.read<uint32_t>();
        if (translations > 0) {
            d.translation_type = r.read<uint32_t>();
            check_count(r, translations, 16);
            d.translation_keys.resize(translations);
            for (auto& k : d.translation_keys) {
                k.time = r.read<float>();
                k.value = vec3();
                if (d.translation_type == 2) {
                    k.forward = vec3();
                    k.backward = vec3();
                } else if (d.translation_type == 3) {
                    for (auto& f : k.tbc) {
                        f = r.read<float>();
                    }
                }
            }
        }
        d.scales = float_data();
        return d;
    }

    NiBoolData bool_data() {
        NiBoolData d;
        auto n = read_count(r, 5);
        if (n == 0) {
            return d;
        }
        d.key_type = r.read<uint32_t>();
        d.keys.resize(n);
        for (auto& [time, value] : d.keys) {
            time = r.read<float>();
            value = r.read<uint8_t>();
            if (d.key_type == 2) {
                r.skip(2); // forward, backward
            } else if (d.key_type == 3) {
                r.skip(12); // tension, bias, continuity
            }
        }
        return d;
    }

    NiTextKeyExtraData text_keys() {
        NiTextKeyExtraData t;
        t.name = string_ref();
        auto n = read_count(r, 8);
        t.keys.resize(n);
        for (auto& [time, text] : t.keys) {
            time = r.read<float>();
            text = string_ref();
        }
        return t;
    }

    NiDefaultAVObjectPalette object_palette() {
        NiDefaultAVObjectPalette p;
        p.scene = r.read<int32_t>();
        auto n = read_count(r, 8);
        p.objects.resize(n);
        for (auto& [name, ref] : p.objects) {
            name = sized_string();
            ref = r.read<int32_t>();
        }
        return p;
    }

    BSBehaviorGraphExtraData behavior_graph() {
        BSBehaviorGraphExtraData b;
        b.name = string_ref();
        b.graph = string_ref();
        b.controls_base_skeleton = flag();
        return b;
    }

    NifBlockData parse_particles() {
        if (bsver() != k_bs_version_se) {
            return std::monostate{};
        }
        if (type == "NiParticleSystem") {
            return particle_system();
        }
        if (type == "NiPSysData") {
            return psys_data();
        }
        if (type == "NiPSysAgeDeathModifier") {
            return age_death();
        }
        if (type == "BSPSysLODModifier") {
            return lod_modifier();
        }
        if (type == "NiPSysMeshEmitter") {
            return mesh_emitter();
        }
        if (type == "NiPSysSphereEmitter") {
            return volume_emitter(VolumeShape::sphere);
        }
        if (type == "NiPSysBoxEmitter") {
            return volume_emitter(VolumeShape::box);
        }
        if (type == "NiPSysCylinderEmitter") {
            return volume_emitter(VolumeShape::cylinder);
        }
        if (type == "NiPSysSpawnModifier") {
            return spawn_modifier();
        }
        if (type == "BSPSysSimpleColorModifier") {
            return color_modifier();
        }
        if (type == "NiPSysRotationModifier") {
            return rotation_modifier();
        }
        if (type == "BSPSysSubTexModifier") {
            return subtex_modifier();
        }
        if (type == "BSPSysScaleModifier") {
            return scale_modifier();
        }
        if (type == "NiPSysGravityModifier") {
            return gravity_modifier();
        }
        if (type == "NiPSysDragModifier") {
            return drag_modifier();
        }
        if (type == "NiPSysPositionModifier") {
            return modifier<NiPSysPositionModifier>();
        }
        if (type == "NiPSysBoundUpdateModifier") {
            return bound_update_modifier();
        }
        if (is_controller_type(type)) {
            return controller();
        }
        if (type == "NiControllerManager") {
            return controller_manager();
        }
        if (type == "NiControllerSequence") {
            return controller_sequence();
        }
        if (type == "NiFloatInterpolator" || type == "NiBoolInterpolator" || type == "NiBoolTimelineInterpolator" ||
            type == "NiBlendFloatInterpolator" || type == "NiBlendBoolInterpolator") {
            return interpolator();
        }
        if (type == "NiFloatData") {
            return float_data();
        }
        if (type == "NiTransformInterpolator") {
            return transform_interpolator();
        }
        if (type == "NiTransformData" || type == "NiKeyframeData") {
            return transform_data();
        }
        if (type == "NiBoolData") {
            return bool_data();
        }
        if (type == "NiTextKeyExtraData") {
            return text_keys();
        }
        if (type == "NiDefaultAVObjectPalette") {
            return object_palette();
        }
        if (type == "BSBehaviorGraphExtraData") {
            return behavior_graph();
        }
        return std::monostate{};
    }

    NifBlockData parse() {
        if (is_nif_node_type(type)) {
            return node();
        }
        if (type == "BSTriShape" || type == "BSMeshLODTriShape" || type == "BSSubIndexTriShape" ||
            type == "BSDynamicTriShape") {
            return tri_shape();
        }
        if (type == "NiTriShape" || type == "NiTriStrips" || type == "BSLODTriShape" ||
            type == "BSSegmentedTriShape") {
            return geometry();
        }
        if (type == "NiTriShapeData" || type == "NiTriStripsData") {
            return geometry_data();
        }
        if (type == "NiSkinInstance" || type == "BSDismemberSkinInstance") {
            return skin_instance();
        }
        if (type == "NiSkinPartition") {
            return skin_partition();
        }
        if (type == "NiSkinData") {
            return skin_data();
        }
        if (type == "BSLightingShaderProperty") {
            return lighting_shader();
        }
        if (type == "BSEffectShaderProperty") {
            return effect_shader();
        }
        if (type == "BSSkyShaderProperty") {
            return sky_shader();
        }
        if (type == "BSShaderTextureSet") {
            return texture_set();
        }
        if (type == "NiAlphaProperty") {
            return alpha_property();
        }
        if (type == "BSXFlags" || type == "NiIntegerExtraData") {
            return integer_extra_data();
        }
        if (type == "NiStringExtraData") {
            return string_extra_data();
        }
        if (type == "BSBound") {
            return bound();
        }
        if (type == "NiCamera") {
            return camera();
        }
        if (type == "BSFurnitureMarkerNode" || type == "BSFurnitureMarker") {
            return furniture_marker();
        }
        if (type == "NiRangeLODData") {
            return range_lod_data();
        }
        if (type == "bhkCollisionObject" || type == "bhkSPCollisionObject" || type == "bhkPCollisionObject" ||
            type == "bhkBlendCollisionObject") {
            return collision_object();
        }
        if (type == "bhkRigidBody" || type == "bhkRigidBodyT") {
            return rigid_body();
        }
        if (type == "bhkRagdollConstraint" || type == "bhkLimitedHingeConstraint") {
            return joint(type == "bhkLimitedHingeConstraint");
        }
        if (type == "bhkMoppBvTreeShape") {
            return mopp_shape();
        }
        if (type == "bhkCompressedMeshShape") {
            return compressed_mesh_shape();
        }
        if (type == "bhkCompressedMeshShapeData") {
            return compressed_mesh_data();
        }
        if (type == "bhkConvexVerticesShape") {
            return convex_vertices_shape();
        }
        if (type == "bhkBoxShape") {
            return box_shape();
        }
        if (type == "bhkSphereShape") {
            return sphere_shape();
        }
        if (type == "bhkCapsuleShape") {
            return capsule_shape();
        }
        if (type == "bhkCylinderShape") {
            return cylinder_shape();
        }
        if (type == "bhkListShape") {
            return list_shape();
        }
        if (type == "bhkTransformShape" || type == "bhkConvexTransformShape") {
            return transform_shape();
        }
        if (type == "bhkPackedNiTriStripsShape") {
            return packed_tri_strips_shape();
        }
        if (type == "hkPackedNiTriStripsData") {
            return packed_tri_strips_data();
        }
        if (type == "bhkNiTriStripsShape") {
            return tri_strips_shape();
        }
        if (type == "BSMultiBound") {
            return BSMultiBound{r.read<int32_t>()};
        }
        if (type == "BSMultiBoundAABB" || type == "BSMultiBoundOBB") {
            BSMultiBoundShape s;
            s.center = vec3();
            s.extent = vec3();
            if (type == "BSMultiBoundOBB") {
                matrix33(); // rotation: the bound's sphere doesn't depend on it
            }
            return s;
        }
        if (type == "BSMultiBoundSphere") {
            BSMultiBoundShape s;
            s.center = vec3();
            s.radius = r.read<float>();
            s.sphere = true;
            return s;
        }
        // Shader-property controllers, float interpolators and their keys, in every NIF version: the materials'
        // own animation reads them from all meshes. The particle and animation blocks
        // in parse_particles() decode the same types for SE files, where these come first.
        if (is_shader_ctlr(type)) {
            return controller();
        }
        if (type == "BSNiAlphaPropertyTestRefController") {
            BSNiAlphaPropertyTestRefController c;
            time_controller(c.base);
            c.base.interpolator = r.read<int32_t>();
            return c;
        }
        if (type == "NiFloatInterpolator") {
            return interpolator();
        }
        if (type == "NiFloatData") {
            return float_data();
        }
        if (type == "NiPoint3Interpolator") {
            NiPoint3Interpolator i;
            i.value = {r.read<float>(), r.read<float>(), r.read<float>()};
            i.data = r.read<int32_t>();
            return i;
        }
        if (type == "NiPosData") {
            NiPosData d;
            const uint32_t count = r.read<uint32_t>();
            if (count > 0) {
                d.key_type = r.read<uint32_t>();
                check_count(r, count, 16);
                for (auto& c : d.keys) {
                    c.resize(count);
                }
                for (uint32_t k = 0; k < count; ++k) {
                    const float t = r.read<float>();
                    float v[3], fwd[3] = {}, bwd[3] = {}, tbc[3] = {};
                    for (float& x : v) x = r.read<float>();
                    if (d.key_type == 2) {
                        for (float& x : fwd) x = r.read<float>();
                        for (float& x : bwd) x = r.read<float>();
                    } else if (d.key_type == 3) {
                        for (float& x : tbc) x = r.read<float>();
                    }
                    for (int c = 0; c < 3; ++c) {
                        d.keys[c][k] = d.key_type == 3 ? std::array<float, 5>{t, v[c], tbc[0], tbc[1], tbc[2]}
                                                       : std::array<float, 5>{t, v[c], fwd[c], bwd[c], 0.0f};
                    }
                }
            }
            return d;
        }
        return parse_particles();
    }
};

std::string read_short_string(BinaryReader& r) {
    auto len = r.read<uint8_t>();
    return r.read_string(len);
}

std::string read_sized_string(BinaryReader& r) {
    auto len = r.read<uint32_t>();
    check_count(r, len, 1);
    return r.read_string(len);
}

} // namespace

bool is_nif_node_type(std::string_view type) {
    static constexpr std::string_view k_node_types[] = {
        "NiNode",
        "BSFadeNode",
        "BSLeafAnimNode",
        "BSTreeNode",
        "BSMultiBoundNode",
        "BSOrderedNode",
        "BSValueNode",
        "NiBillboardNode",
        "NiSwitchNode",
        "NiLODNode",
        "BSRangeNode",
        "BSBlastNode",
        "BSDamageStage",
        "BSDebrisNode",
        "BSMasterParticleSystem",
        "NiSortAdjustNode",
        "RootCollisionNode",
        "AvoidNode",
        "NiBSAnimationNode",
        "NiBSParticleNode",
    };
    return std::ranges::find(k_node_types, type) != std::end(k_node_types);
}

std::string_view NifFile::type_name(NifRef ref) const {
    if (!valid(ref)) {
        return {};
    }
    return header.block_types[blocks[static_cast<size_t>(ref)].type];
}

const NiAVObject* NifFile::av_object(NifRef ref) const {
    if (!valid(ref)) {
        return nullptr;
    }
    return std::visit(
        [](const auto& d) -> const NiAVObject* {
            if constexpr (std::is_base_of_v<NiAVObject, std::decay_t<decltype(d)>>) {
                return &d;
            } else {
                return nullptr;
            }
        },
        blocks[static_cast<size_t>(ref)].data);
}

NifFile read_nif(std::span<const std::byte> data) {
    BinaryReader r(data);
    NifFile file;
    auto& h = file.header;

    std::string line;
    for (;;) {
        auto c = r.read<char>();
        if (c == '\n') {
            break;
        }
        line.push_back(c);
        if (line.size() > 128) {
            throw ParseError("not a NIF file (no header line)");
        }
    }
    if (!line.starts_with(k_signature)) {
        throw ParseError("not a NIF file");
    }
    h.version = r.read<uint32_t>();
    if (h.version != k_nif_version) {
        throw ParseError(std::format("unsupported NIF version {}", line.substr(k_signature.size())));
    }
    if (r.read<uint8_t>() != 1) {
        throw ParseError("big-endian NIF files are not supported");
    }
    h.user_version = r.read<uint32_t>();
    auto num_blocks = r.read<uint32_t>();
    h.bs_version = r.read<uint32_t>();
    if (h.bs_version != k_bs_version_se && h.bs_version != k_bs_version_le) {
        throw ParseError(
            std::format("unsupported BS version {} (user version {})", h.bs_version, h.user_version));
    }
    h.author = read_short_string(r);
    h.process_script = read_short_string(r);
    h.export_script = read_short_string(r);

    uint32_t num_types = r.read<uint16_t>();
    check_count(r, num_types, 4);
    h.block_types.reserve(num_types);
    for (uint32_t i = 0; i < num_types; ++i) {
        h.block_types.push_back(read_sized_string(r));
    }
    check_count(r, num_blocks, 6);
    auto type_index = r.read_array<uint16_t>(num_blocks);
    auto block_sizes = r.read_array<uint32_t>(num_blocks);

    auto num_strings = read_count(r, 4);
    r.skip(4); // max string length
    h.strings.reserve(num_strings);
    for (uint32_t i = 0; i < num_strings; ++i) {
        h.strings.push_back(read_sized_string(r));
    }
    auto num_groups = read_count(r, 4);
    r.skip(size_t{num_groups} * 4);

    file.blocks.resize(num_blocks);
    size_t offset = r.tell();
    for (uint32_t i = 0; i < num_blocks; ++i) {
        auto& block = file.blocks[i];
        // Bit 15 flags a PhysX-related variant in some versions; the low bits are the type.
        block.type = type_index[i] & 0x7FFF;
        if (block.type >= h.block_types.size()) {
            throw ParseError(
                std::format("block {} has type index {} of {}", i, block.type, h.block_types.size()));
        }
        if (block_sizes[i] > data.size() - offset) {
            throw ParseError(std::format("block {} ({}) of {} bytes at offset {} overruns the file", i,
                                         h.block_types[block.type], block_sizes[i], offset));
        }
        block.offset = static_cast<uint32_t>(offset);
        block.size = block_sizes[i];
        BinaryReader br(data.subspan(offset, block.size));
        BlockParser parser{br, h, h.block_types[block.type]};
        try {
            block.data = parser.parse();
            if (std::holds_alternative<std::monostate>(block.data)) {
                block.status = NifBlockStatus::opaque;
            } else if (br.remaining() != 0) {
                block.status = NifBlockStatus::mismatch;
                block.error = std::format("decoded {} of {} bytes", br.tell(), block.size);
                block.data = std::monostate{};
            } else {
                block.status = NifBlockStatus::parsed;
            }
        } catch (const ParseError& e) {
            block.status = NifBlockStatus::mismatch;
            block.error = e.what();
            block.data = std::monostate{};
        }
        std::string_view type_name = h.block_types[block.type];
        if ((type_name.ends_with("Controller") || type_name.ends_with("Ctlr") ||
             type_name == "NiControllerManager") &&
            block.size >= 4) {
            BinaryReader nr(data.subspan(offset, 4));
            block.next_controller = nr.read<int32_t>();
        }
        offset += block.size;
    }

    r.seek(offset);
    auto num_roots = read_count(r, 4);
    file.roots = r.read_array<NifRef>(num_roots);
    return file;
}

} // namespace spud::legacy
