#pragma once

// The world side of: temp particles, box decals, lights that move, the screen blood and modifiers. What
// plays when is src/game/impacts; this holds what stays and fills the frame with it.

#include "game/projectiles.hpp"
#include "render/gpu_types.hpp"
#include "render/view.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <random>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spud::engine {

class App;

class Vfx {
public:
    explicit Vfx(App& app);

    void temp_particle(std::string_view model, const game::Units3& at, const game::DecalAxes& frame, float scale, float duration);
    void add_decal(const game::DecalRequest& request);
    void add_skin_decal(const game::SkinDecalRequest& request);
    void screen_blood(int count);
    void clear_screen_blood();
    void set_light(uint32_t id, uint32_t light, const game::Units3& at, float alpha);
    void remove_light(uint32_t id);
    // A Light effect's LIGH on an actor: it goes where the actor goes. 0: off.
    void follow_light(uint32_t ref, uint32_t light);
    // Where a target's hit art holds its light (its AttachLight node), game units: the light stands there, not at the head.
    void attach_light(uint32_t ref, const glm::vec3& units) { m_attached_lights[ref] = units; }
    void modifier(uint32_t imad, float strength, bool unique); // or TriggerUnique

    // Once a frame, on the world clock: `dt` 0 holds everything still.
    void update(const render::View& view, float dt);
    std::span<const render::GpuDecal> decals() const { return m_gpu_decals; }
    std::span<const render::GpuSplat> splats() const { return m_gpu_splats; }
    std::span<const uint32_t, 3> splat_textures() const { return std::span<const uint32_t, 3>(m_splat_textures); }

private:
    struct TextureSet {
        uint32_t diffuse = 0, normal = 0, glow = 0;
        bool no_specular = false;
    };
    struct Decal {
        uint32_t ref = 0;
        TextureSet textures;
        glm::vec3 position{0.0f}, axes[3] = {};
        float half_width = 0, half_height = 0, depth = 0; // metres
        float width = 0;                                  // units, as the spacing test reads it
        float cos_angle = 0, shininess = 0;
        glm::vec3 color{1.0f};
        uint32_t flags = 0;
        float uv_scale = 0.25f;
        glm::vec2 uv_offset{0.0f};
        glm::uvec4 lights{0xFFFFFFFFu};
        float age = 0;
    };
    struct TempMesh {
        uint32_t id = 0;
        float left = 0;
    };
    struct Light {
        glm::vec3 position{0.0f}, color{0.0f};
        float radius = 0, alpha = 1;
    };
    struct LightBase {
        glm::vec3 color{0.0f};
        float radius = 0;
    };
    struct Splat {
        render::GpuSplat look;
        float opacity = 0;
    };
    struct SplatCall { // one call's splats share a clock
        float seconds = 0;
        std::vector<Splat> splats;
    };

    const TextureSet& texture_set(uint32_t form);
    const LightBase& light_base(uint32_t form);
    float roll(float low, float high);
    float setting(std::string_view name, float fallback) const;
    void clear();
    void update_decals(const render::View& view, float dt);
    void update_lights(const render::View& view);
    void update_screen_blood(float dt);
    void put(const Decal& d, const render::View& view, float fade);

    App& m_app;
    const void* m_world = nullptr; // what the decals and lights belong to
    std::unordered_map<uint32_t, TextureSet> m_texture_sets;
    std::unordered_map<uint32_t, LightBase> m_light_bases;
    std::unordered_map<uint32_t, uint32_t> m_followers; // actor -> LIGH
    std::deque<Decal> m_decals; // oldest first: while under the cap they don't age
    std::vector<Decal> m_aging;
    std::vector<render::GpuDecal> m_gpu_decals;
    std::unordered_map<uint32_t, glm::vec3> m_attached_lights;
    uint32_t m_decals_this_frame = 0;
    int m_skin_decals_this_frame = 0;
    size_t m_cell_check = 0;
    std::vector<TempMesh> m_temp_meshes;
    uint32_t m_next_mesh = 0xFFB00000;
    std::unordered_map<uint32_t, Light> m_lights;
    std::vector<SplatCall> m_splat_calls;
    std::vector<render::GpuSplat> m_gpu_splats;
    float m_splat_strength = 0;
    uint32_t m_splat_textures[3] = {};
    std::mt19937 m_random{0x5B1Du}; // seeded: scripted runs draw the same blood
};

} // namespace spud::engine
