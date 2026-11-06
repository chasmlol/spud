#pragma once

// Flash shapes -> triangles. Each fill style becomes one mesh (its edges linked into closed contours with the
// fill on one side, tessellated non-zero), each line style one mesh (stroked with round joins and caps, the
// Flash default). Coordinates stay in the source's units; the caller scales.

#include "importer/legacy/swf.hpp"

#include <cstdint>
#include <vector>

namespace spud::importer {

struct TriMesh {
    std::vector<float> xy; // pairs
    std::vector<uint32_t> indices;
};

struct ShapeMeshes {
    struct Part {
        bool stroke;
        uint32_t style; // index into Shape::fills or Shape::lines
        TriMesh mesh;
    };
    std::vector<Part> parts; // draw order: per style group, fills then strokes
};

// tolerance: maximum distance of the flattened curve from the true curve, in source units.
ShapeMeshes tessellate_shape(const legacy::swf::Shape& shape, float tolerance);

} // namespace spud::importer
