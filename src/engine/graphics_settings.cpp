#include "engine/graphics_settings.hpp"

#include "engine/app_internal.hpp"

#include <algorithm>
#include <format>

namespace spud::engine {

const std::vector<GraphicsRow>& graphics_rows() {
    // Presets: the install's Low.ini, Medium.ini, High.ini, Ultra.ini. The game's FXAA and TAA rows go to MSAA.
    static const std::vector<GraphicsRow> rows = {
        {k_graphics_detail_id, "graphics.detail", "Detail", {"Low", "Medium", "High", "Ultra", "Custom"}, {0, 1, 2, 3}},
        {101, "graphics.antialiasing", "Antialiasing (after a restart)", {"Off", "MSAA 2x", "MSAA 4x", "MSAA 8x"}, {0, 1, 2, 2}},
        {102, "graphics.shadow_quality", "Shadow Quality", {"Low", "High", "Ultra", "Extreme"}, {0, 1, 1, 2}},
        {103, "graphics.shadow_distance", "Shadow Distance", {"Low", "High", "Ultra", "Extreme", "Maximum"}, {0, 0, 1, 2}},
        {104, "graphics.object_detail", "Object Detail", {"Low", "Medium", "High", "Ultra", "Extreme"}, {0, 1, 2, 3}},
        {105, "graphics.grass_detail", "Grass Detail", {"Low", "Medium", "Ultra", "Extreme"}, {0, 1, 2, 2}},
        {106, "graphics.distant_detail", "Distant Object Detail", {"Low", "Medium", "High", "Ultra", "Extreme", "Maximum"}, {0, 1, 2, 3}},
        {107, "graphics.decals", "Decal Quantity", {"None", "Medium", "Ultra"}, {0, 1, 2, 2}},
        {108, "graphics.volumetric", "Volumetric Lighting", {}, {0, 1, 1, 1}},
        {109, "graphics.lens_flare", "Lens Flare", {}, {0, 1, 1, 1}},
        {110, "graphics.reflections", "Screen Space Reflections", {}, {0, 1, 1, 1}},
    };
    return rows;
}

const GraphicsRow* graphics_row(int id) {
    for (const GraphicsRow& r : graphics_rows()) {
        if (r.id == id) {
            return &r;
        }
    }
    return nullptr;
}

void App::prepare_graphics_settings() {
    for (const GraphicsRow& r : graphics_rows()) {
        m_user.add_hidden(r.key, std::format("{}", r.preset[3]));
    }
}

std::vector<ui::GraphicsOption> App::ui_graphics_options() {
    std::vector<ui::GraphicsOption> out;
    for (const GraphicsRow& r : graphics_rows()) {
        ui::GraphicsOption o;
        o.id = r.id;
        o.text = r.label;
        o.value = m_user.number(r.key);
        o.default_value = r.preset[3];
        o.options.assign(r.steps.begin(), r.steps.end());
        out.push_back(std::move(o));
    }
    return out;
}

void App::graphics_option_change(int id, double value) {
    const GraphicsRow* row = graphics_row(id);
    if (!row) {
        return;
    }
    const auto& rows = graphics_rows();
    const int step = int(value + 0.5);
    if (id == k_graphics_detail_id) {
        if (step >= k_graphics_custom) {
            return; // Custom is what the rows say, not a choice of its own
        }
        for (const GraphicsRow& r : rows) {
            m_user.set(r.key, std::format("{}", id == r.id ? step : r.preset[size_t(step)]));
        }
    } else {
        m_user.set(row->key, std::format("{}", step));
        // The Detail row names the preset the rows match, else Custom.
        int detail = k_graphics_custom;
        for (int p = 0; p < 4 && detail == k_graphics_custom; ++p) {
            if (std::ranges::all_of(rows, [&](const GraphicsRow& r) {
                    return r.id == k_graphics_detail_id || int(m_user.number(r.key)) == r.preset[size_t(p)];
                })) {
                detail = p;
            }
        }
        m_user.set(rows[0].key, std::format("{}", detail));
    }
    apply_graphics_settings();
}

void App::apply_graphics_settings() {
    const auto step = [this](int id, size_t count) {
        return std::min(size_t(std::max(int(m_user.number(graphics_row(id)->key)), 0)), count - 1);
    };
    render::RenderSettings& rs = m_render_settings;
    rs.shadows.resolution = k_shadow_resolutions[step(102, k_shadow_resolutions.size())];
    m_renderer->set_shadow_resolution(rs.shadows.resolution);
    rs.shadows.distance = k_shadow_distances[step(103, k_shadow_distances.size())];
    const ObjectDetail& objects = k_object_detail[step(104, k_object_detail.size())];
    rs.lod_fade_out_mult_objects = objects.objects;
    rs.lod_fade_out_mult_items = objects.items;
    m_user.set("fLODFadeOutMultObjects:LOD", std::format("{}", std::min(objects.objects, 30.0f)));
    m_user.set("fLODFadeOutMultItems:LOD", std::format("{}", std::min(objects.items, 10.0f)));
    m_user.set("fLODFadeOutMultActors:LOD", std::format("{}", std::min(objects.actors, 15.0f)));
    const GrassDetail& grass = k_grass_detail[step(105, k_grass_detail.size())];
    rs.grass_fade_start = grass.fade_start;
    m_space_settings.grass_cell_radius = grass.cell_radius;
    m_user.set("fGrassStartFadeDistance:Grass", std::format("{}", std::min(grass.fade_start, 7000.0f)));
    if (!m_opt.draw_distance) {
        set_setting("draw_distance", std::format("{}", k_distant_detail[step(106, k_distant_detail.size())]));
    }
    m_max_decals = k_decal_steps[step(107, k_decal_steps.size())];
    rs.volumetric = m_user.number(graphics_row(108)->key) != 0;
    rs.lens_flare = m_user.number(graphics_row(109)->key) != 0;
    rs.reflections = m_user.number(graphics_row(110)->key) != 0;
}

} // namespace spud::engine
