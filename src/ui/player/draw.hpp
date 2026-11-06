#pragma once

// What the player hands the renderer each frame: meshes in stage space, with masks (stencil) and layers
// (filters, blend modes that need their own target) as brackets around the items they apply to.

#include "ui/player/display.hpp"

#include <vector>

namespace spud::ui::player {

struct DrawItem {
    enum class Kind : uint8_t {
        mesh,        // a shape mesh: movie->meshes[index]
        glyph,       // a font glyph: movie->glyphs[index], filled with `color`
        rect,        // a solid rectangle in local space (text field border/background): rect, color
        mask_begin,  // the following items up to mask_content are the mask's geometry
        mask_content,// mask geometry done: draw what follows inside it
        mask_end,    // remove the innermost mask (its geometry follows again, up to mask_done)
        mask_done,
        layer_begin, // the items up to layer_end render into a layer, then filters and blend apply
        layer_end,
        triangles,   // `count` vertices from DrawList::vertices at `first`, a triangle list filled with `color`
        bitmap,      // the bitmap character movie->characters[index] stretched over `rect` (local space), smoothed
        image,       // the engine image `index` of the library's ImageRegistry (img://) stretched over `rect`
    };
    Kind kind = Kind::mesh;
    const MovieDef* movie = nullptr;
    uint32_t index = 0;
    Matrix2D transform; // local -> stage pixels
    ColorXform cxform;
    uint32_t color = 0xFFFFFFFFu; // glyphs and rects (RGBA8, straight alpha)
    float rect[4] = {0, 0, 0, 0}; // x0, y0, x1, y1
    uint32_t first = 0, count = 0; // triangles
    // Layers
    osu::Blend blend = osu::Blend::normal;
    const osu::Filter* filters = nullptr;
    uint32_t filter_count = 0;
    const float* filter_values = nullptr;
    float bounds[4] = {0, 0, 0, 0}; // stage-space bounds of a layer's content
    // Scale-9 shapes: vertex -> `inner` (placement in the clip) -> the grid remap -> `transform` (the clip's).
    bool scale9 = false;
    Matrix2D inner;
    float s9_bounds[4] = {0, 0, 0, 0}; // the clip's content bounds, clip space
    float s9_grid[4] = {0, 0, 0, 0};   // the grid's inner rectangle, clip space
    float s9_scale[2] = {1, 1};        // the clip's scale on screen
};

struct DrawList {
    std::vector<DrawItem> items;
    std::vector<osu::Vertex> vertices; // the triangles items' geometry, local space
    float stage_width = 1280, stage_height = 720;
    void clear() {
        items.clear();
        vertices.clear();
    }
};

} // namespace spud::ui::player
