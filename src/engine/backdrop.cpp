#include "engine/backdrop.hpp"

#include "core/units.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace spud::engine {

namespace {

glm::vec3 metres(const float (&units)[3]) {
    return {units::to_metres(units[0]), units::to_metres(units[1]), units::to_metres(units[2])};
}

// A row-major NiMatrix3 (column vectors) as glm's column-major matrix.
glm::mat3 matrix(const float (&m)[9]) {
    glm::mat3 r;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            r[col][row] = m[row * 3 + col];
        }
    }
    return r;
}

} // namespace

// A handful of slots: the logo, re-placed when its scale changes (a slot in use by a frame in flight is never
// rewritten).
Backdrop::Backdrop(render::Renderer& renderer, const std::filesystem::path& cache)
    : m_renderer(renderer), m_instances(renderer.device(), 16), m_fog(cache, [this, &renderer](std::string_view path) -> uint32_t {
          const uint32_t texture = renderer.textures().acquire(path, render::TextureKind::color, 0.0f);
          m_fog_textures.push_back(texture);
          return texture;
      }) {
    // Nothing but the scene: black behind it, no sun disc or glow in the sky, no fog.
    m_environment.sky_zenith = m_environment.sky_horizon = m_environment.sky_ground = glm::vec3(0.0f);
    m_environment.sun_disc = 0.0f;
    m_environment.fog_max = 0.0f;
    // The constants the game gives this scene (captured at the main menu): no sun or ambient, no fog, lit colour limited to 1.
    m_environment.skyrim_inputs = true;
    m_environment.dir_light_color = glm::vec3(0.0f);
    m_environment.fog_param = glm::vec4(5000000.0f, 0.1f, 1.0f, 0.0f);
    m_environment.fog_near_color = m_environment.fog_far_color = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
    m_environment.fog_color = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
    m_environment.output_clamp = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
    m_environment.frame_params = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
}

Backdrop::~Backdrop() {
    release();
    // Deferred frees refer to the instance table.
    m_renderer.drain();
}

void Backdrop::set_image_space(const float* block) {
    render::ImageSpaceData is;
    float* d = &is.eye_adapt_speed;
    for (int i = 0; i < 13; ++i) {
        d[i] = block[i]; // hdr, cinematic, tint amount
    }
    is.tint = glm::vec3(block[13], block[14], block[15]);
    m_environment.sky.image_space = is;
    m_environment.sky.image_space_valid = true;
    m_environment.sky.ui_scene = true;
}

void Backdrop::release() {
    release_model();
    if (m_fog_id) {
        m_fog.stop(m_fog_id);
        m_fog_id = 0;
        m_fog_seconds = -1.0f;
        m_fog_looping = false;
    }
}

void Backdrop::release_model() {
    if (m_mesh != render::k_invalid_mesh) {
        m_renderer.meshes().release(m_mesh);
        m_mesh = render::k_invalid_mesh;
    }
    m_model.clear();
    if (m_slot != ~0u) {
        m_renderer.device().defer([this, s = m_slot] { m_instances.free(s); });
        m_slot = ~0u;
    }
    m_draws.clear();
}

void Backdrop::place(const glm::mat4& transform) {
    if (m_slot != ~0u && transform == m_placed) {
        return;
    }
    const uint32_t slot = m_instances.allocate();
    if (slot == ~0u) {
        return; // all slots still in flight: keep the old placement this frame
    }
    const glm::vec4 local = m_renderer.meshes().sphere(m_mesh);
    render::GpuInstanceData d;
    for (int i = 0; i < 3; ++i) {
        d.rows[i] = glm::vec4(transform[0][i], transform[1][i], transform[2][i], transform[3][i]);
    }
    d.lights[0] = 0u | 1u << 16; // the scene's three lights reach everything in it
    d.lights[1] = 2u | 0xFFFFu << 16;
    d.flags = uint32_t(render::InstanceFade::never) << render::k_instance_fade_shift; // no LOD fades in this scene: MaterialData.y is 1
    const float scale = glm::length(glm::vec3(transform[0]));
    d.sphere = glm::vec4(glm::vec3(transform * glm::vec4(glm::vec3(local), 1.0f)), local.w * scale);
    m_instances.write(slot, d);
    if (m_slot != ~0u) {
        m_renderer.device().defer([this, s = m_slot] { m_instances.free(s); });
    }
    m_slot = slot;
    m_placed = transform;
}

// The fog plays AutoPlay, then AutoLoop at its end, turned -180 degrees about Z at the origin.
void Backdrop::update_fog(float dt) {
    if (m_fog_seconds < 0.0f) {
        m_fog.set_soft(false);
        m_fog.update(m_no_world, m_view, 0.0f); // takes the empty world, which would clear what's tracked
        const glm::mat3 turned(glm::rotate(glm::mat4(1.0f), glm::radians(-180.0f), glm::vec3(0, 0, 1)));
        m_fog_id = m_fog.track("meshes/interface/intmenufogparticles.osm", turned, glm::vec3(0.0f), 1.0f);
        m_fog_seconds = 0.0f;
    }
    if (fx::EffectInstance* fog = m_fog.tracked(m_fog_id)) {
        m_fog_seconds += dt;
        if (m_fog_seconds == dt) {
            for (const std::string& name : fog->sequence_names()) {
                fog->deactivate_sequence(name);
            }
            fog->activate_sequence("AutoPlay");
        } else if (!m_fog_looping && m_fog_seconds >= 0.667f) {
            m_fog_looping = true;
            fog->deactivate_sequence("AutoPlay");
            fog->activate_sequence("AutoLoop");
        }
    }
    m_fog.update(m_no_world, m_view, dt);
}

void Backdrop::update(const ui::BackdropScene& scene, float aspect, float dt) {
    // Camera (section 2, "Camera"): at `camera`, looking at `look_at` with Z up, or with the rotation a loading
    // screen's camera path gives it (an NiCamera looks along its X axis, Y up). The frustum is horizontal-locked:
    // half slopes 0.75 t across and 0.75 t x height / width up, t = tan(fov x pi / 1200).
    const glm::vec3 eye = metres(scene.camera);
    glm::vec3 forward = glm::normalize(metres(scene.look_at) - eye);
    glm::vec3 up(0, 0, 1);
    if (scene.camera_rotation_set) {
        const glm::mat3 r = matrix(scene.camera_rotation);
        forward = glm::normalize(r[0]);
        up = glm::normalize(r[1]);
    }
    const float t = std::tan(scene.fov_degrees * std::numbers::pi_v<float> / 1200.0f);
    const float half_up = 0.75f * t / aspect;
    m_view = render::View::make(eye, 0.0f, 0.0f, 2.0f * std::atan(half_up), aspect, units::to_metres(scene.near_units));
    m_view.rotation = glm::lookAtRH(glm::vec3(0), forward, up);
    m_view.view_proj = m_view.projection * m_view.rotation;
    m_view.frustum = render::Frustum::from_view_proj(m_view.view_proj);

    update_fog(dt);

    if (scene.model != m_model) {
        release_model();
        m_model = scene.model;
        if (!m_model.empty()) {
            m_mesh = m_renderer.meshes().acquire(m_model, 0.0f); // ahead of the world streaming in behind the screen
        }
    }
    m_draws.clear();
    m_model_shown = false;
    // Still loading: the model appears when it's in, textures and all.
    if (m_mesh == render::k_invalid_mesh || !m_renderer.meshes().ready(m_mesh) || !m_renderer.meshes().textures_settled(m_mesh)) {
        return;
    }
    // The root as placed: translation, rotation, uniform scale.
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), metres(scene.model_translation));
    transform = transform * glm::mat4(matrix(scene.model_rotation));
    transform = glm::scale(transform, glm::vec3(scene.model_scale));
    place(transform);
    if (m_slot == ~0u) {
        return;
    }
    const glm::vec3 centre = glm::vec3(m_placed * glm::vec4(glm::vec3(m_renderer.meshes().sphere(m_mesh)), 1.0f));
    m_draws.add(m_mesh, m_slot, glm::dot(centre - eye, centre - eye));
    m_model_shown = true;

    // The UI player scene's three lights, attenuated as any point light. Light 0 casts a shadow in the game: not here yet.
    m_lights.clear();
    for (const ui::BackdropLight& l : scene.lights) {
        render::PointLight p;
        p.position = metres(l.position);
        p.radius = units::to_metres(l.radius);
        p.color = glm::vec3(l.color[0], l.color[1], l.color[2]) * l.dimmer;
        p.flags = render::point_light_on | render::point_light_never_fades;
        m_lights.push_back(p);
    }
}

render::SpaceView Backdrop::space_view() const {
    render::SpaceView sv;
    sv.draws = &m_draws;
    sv.instance_table = m_instances.address();
    sv.environment = m_environment;
    sv.lights = m_lights;
    // The fog shows with its textures in: a sprite this near is the whole screen in the loading grey.
    if (std::ranges::all_of(m_fog_textures, [this](uint32_t t) { return m_renderer.textures().settled(t); })) {
        sv.particles = m_fog.frame();
    }
    return sv;
}

} // namespace spud::engine
