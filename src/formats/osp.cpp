#include "formats/osp.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace spud::osp {

namespace {

constexpr auto k_info = container::fourcc("INFO");
constexpr auto k_body = container::fourcc("BODY");
constexpr auto k_shap = container::fourcc("SHAP");
constexpr auto k_vert = container::fourcc("VERT");
constexpr auto k_indx = container::fourcc("INDX");
constexpr auto k_rank = container::fourcc("RANK");

struct Info {
    float bounds_min[3];
    float bounds_max[3];
    uint32_t body_count;
    uint32_t shape_count;
    uint32_t vertex_count;
    uint32_t index_count;
};

} // namespace

void compute_bounds(Collision& c) {
    float lo[3], hi[3];
    for (int i = 0; i < 3; ++i) {
        lo[i] = std::numeric_limits<float>::max();
        hi[i] = std::numeric_limits<float>::lowest();
    }
    auto add = [&](const float* p, float pad) {
        for (int i = 0; i < 3; ++i) {
            lo[i] = std::min(lo[i], p[i] - pad);
            hi[i] = std::max(hi[i], p[i] + pad);
        }
    };
    for (const auto& s : c.shapes) {
        switch (s.type) {
        case ShapeType::box: {
            // Rotated box: the AABB half size along each axis is |R| * extents.
            const float x = s.rotation[0], y = s.rotation[1], z = s.rotation[2], w = s.rotation[3];
            const float m[3][3] = {
                {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)},
            };
            for (int i = 0; i < 3; ++i) {
                const float h = std::abs(m[i][0]) * s.extents[0] + std::abs(m[i][1]) * s.extents[1] +
                                std::abs(m[i][2]) * s.extents[2] + s.convex_radius;
                lo[i] = std::min(lo[i], s.centre[i] - h);
                hi[i] = std::max(hi[i], s.centre[i] + h);
            }
            break;
        }
        case ShapeType::sphere:
            add(s.centre, s.radius);
            break;
        case ShapeType::capsule:
        case ShapeType::cylinder:
            add(s.centre, s.radius);
            add(s.extents, s.radius);
            break;
        case ShapeType::convex:
        case ShapeType::mesh:
            for (uint32_t v = s.first_vertex; v < s.first_vertex + s.vertex_count && v * 3 + 2 < c.vertices.size();
                 ++v) {
                add(&c.vertices[size_t(v) * 3], s.type == ShapeType::convex ? s.convex_radius : 0.0f);
            }
            break;
        }
    }
    if (c.shapes.empty()) {
        std::fill(lo, lo + 3, 0.0f);
        std::fill(hi, hi + 3, 0.0f);
    }
    std::copy(lo, lo + 3, c.bounds_min);
    std::copy(hi, hi + 3, c.bounds_max);
}

std::vector<std::byte> write(const Collision& c) {
    Info info{};
    std::copy(std::begin(c.bounds_min), std::end(c.bounds_min), info.bounds_min);
    std::copy(std::begin(c.bounds_max), std::end(c.bounds_max), info.bounds_max);
    info.body_count = static_cast<uint32_t>(c.bodies.size());
    info.shape_count = static_cast<uint32_t>(c.shapes.size());
    info.vertex_count = static_cast<uint32_t>(c.vertices.size() / 3);
    info.index_count = static_cast<uint32_t>(c.indices.size());

    container::Writer w(k_kind, k_version);
    w.add_struct(k_info, info);
    w.add_array(k_body, c.bodies);
    w.add_array(k_shap, c.shapes);
    w.add_array(k_vert, c.vertices);
    w.add_array(k_indx, c.indices);
    if (!c.ranks.empty()) {
        w.add_array(k_rank, c.ranks);
    }
    return w.finish();
}

Collision read(std::span<const std::byte> file) {
    container::Reader r(file, k_kind);
    if (r.version() != 1 && r.version() != k_version) {
        throw ParseError("unsupported .osp version " + std::to_string(r.version()));
    }
    const auto info = r.read_struct<Info>(k_info);
    Collision c;
    std::copy(std::begin(info.bounds_min), std::end(info.bounds_min), c.bounds_min);
    std::copy(std::begin(info.bounds_max), std::end(info.bounds_max), c.bounds_max);
    c.bodies = r.read_array<Body>(k_body);
    c.shapes = r.read_array<Shape>(k_shap);
    c.vertices = r.read_array<float>(k_vert);
    c.indices = r.read_array<uint32_t>(k_indx);
    if (r.find(k_rank)) {
        c.ranks = r.read_array<uint32_t>(k_rank);
        if (c.ranks.size() * 3 != c.indices.size()) {
            throw ParseError(".osp RANK chunk does not hold one rank per triangle");
        }
    }
    if (c.vertices.size() % 3 != 0) {
        throw ParseError(".osp vertex chunk is not a whole number of vertices");
    }
    const size_t vertex_count = c.vertices.size() / 3;
    for (const auto& s : c.shapes) {
        if (s.body >= c.bodies.size()) {
            throw ParseError(".osp shape refers to a missing body");
        }
        if (s.type == ShapeType::convex || s.type == ShapeType::mesh) {
            if (uint64_t(s.first_vertex) + s.vertex_count > vertex_count) {
                throw ParseError(".osp shape vertices out of range");
            }
        }
        if (s.type == ShapeType::mesh) {
            if (s.index_count % 3 != 0 || uint64_t(s.first_index) + s.index_count > c.indices.size()) {
                throw ParseError(".osp shape indices out of range");
            }
            for (uint32_t i = s.first_index; i < s.first_index + s.index_count; ++i) {
                if (c.indices[i] >= s.vertex_count) {
                    throw ParseError(".osp triangle index out of range");
                }
            }
        }
    }
    return c;
}

} // namespace spud::osp
