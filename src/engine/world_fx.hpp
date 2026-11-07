#pragma once

// Particle systems in the world: the references whose models hold one, simulated by
// src/fx while the camera is near, and one-shot effects such as a water splash. No rendering here: frame() is what the
// renderer draws.

#include "formats/osw.hpp"
#include "fx/effect.hpp"
#include "render/gpu_types.hpp"
#include "render/view.hpp"

#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spud {

class WorldFx {
public:
    using TextureResolver = std::function<uint32_t(std::string_view path)>;

    WorldFx(std::filesystem::path cache, TextureResolver textures);

    // Once a frame. `dt` is the world clock's step: 0 holds everything still.
    void update(const osw::World& world, const render::View& view, float dt);
    bool busy() const { return m_world_effects.valid(); } // a world's effects are still being read
    // Behind a loading screen, where update() doesn't run: takes in a world's effects once they're read. Without it busy()
    // stays true, and the screen waits for itself until it times out.
    void settle(const osw::World& world);
    // A one-shot effect at a position (metres), gone when its particles are.
    void spawn(std::string_view model, const glm::vec3& position, float heading);
    // Whether a model has particle systems: only their effects run here.
    bool has_effect(std::string_view model);
    // loaded off the frame, so it shows a frame or more late, and gone at the
    // longest stop - start of its controllers, at least 0.1 s; `duration` without any.
    void spawn_temp(std::string_view model, const glm::mat3& rotation, const glm::vec3& position, float scale, float duration);

    // An effect that moves (16): started at a pose, moved, stopped. Particles it has left in world space stay where they are
    // until the effect goes, and go with it. 0 is no effect.
    uint32_t track(std::string_view model, const glm::mat3& rotation, const glm::vec3& position, float scale);
    bool tracked_ready(uint32_t id) const; // running, or its model turned out to have nothing to run
    void move(uint32_t id, const glm::mat3& rotation, const glm::vec3& position);
    void hide(uint32_t id, bool hidden);
    std::function<bool(uint32_t form)> ref_disabled; // null: the world's own flags
    void stop(uint32_t id);
    fx::EffectInstance* tracked(uint32_t id); // null until it runs, and when its model has nothing to run

    // The ADDN records: index to model. A model's AddOnNode<index> nodes play them at the node (master particle systems).
    void set_addons(std::unordered_map<uint32_t, std::string> models) { m_addons = std::move(models); }

    // What the named sequence of a model holds: the time of a text key and its length, 0 for none.
    float text_key_time(std::string_view model, std::string_view sequence, std::string_view key);
    float sequence_length(std::string_view model, std::string_view sequence);
    // The file's spelling of a sequence: the game's names don't go by case. "": no such sequence.
    std::string sequence_named(std::string_view model, std::string_view sequence);

    // The UI's scene: GetRenderPasses clears the soft effect there (measured: technique 0x180043).
    void set_soft(bool on) { m_soft = on; }

    const render::ParticleFrame* frame() const { return m_frame.draws.empty() ? nullptr : &m_frame; }

    // Particles worked out elsewhere (ShaderParticles), for the next update to draw.
    struct Batch {
        std::string texture, palette;
        uint32_t technique = 0, blend_src = 6, blend_dst = 0;
        float colour_scale = 1;
        glm::vec3 centre{0.0f}; // metres
        std::vector<render::GpuParticle> particles;
    };
    std::vector<Batch>& batches() { return m_batches; }

    // The one-shot effects' mesh shapes this frame, which Space draws: submesh of the model at a pose, at an alpha.
    struct Shape {
        std::string model; // the .osm
        uint32_t submesh;
        glm::mat3 rotation; // with scale
        glm::vec3 position; // metres
        glm::vec4 base_color; // the effect material as the effect's controllers leave it
        glm::vec4 uv;         // offset, scale
        bool animated = false; // base_color and uv apply: the effect runs this material
    };
    std::span<const Shape> shapes() const { return m_shapes; }

private:
    struct Emitter {
        uint32_t ref;
        std::shared_ptr<const fx::Effect> effect;
    };
    struct Live {
        std::unique_ptr<fx::EffectInstance> instance;
        float seconds = 0.0f;
        bool one_shot = false;
        float life = 0.0f;
        float lifetime = 6.0f;
        float scale = 1.0f;
        float emit_for = -1.0f; // a master system's: it stops emitting then, and goes when its particles have
        std::string model;
        glm::mat3 root_rotation{1.0f};
        glm::vec3 root_position{0.0f};
        // Per submesh: the shape's node at rest, the transform the .osm's vertices are already in.
        std::vector<glm::mat4> rest_inverse;
    };

    // Waiting for its model: the pose it asked for, or was moved to since.
    std::vector<Batch> m_batches;
    std::unordered_map<std::string, uint32_t> m_batch_textures;
    struct Request {
        std::string model;
        glm::mat3 rotation{1.0f};
        glm::vec3 position{0.0f};
        float scale = 1.0f, duration = 0.0f;
        uint32_t tracked = 0; // else a temp particle
        bool hidden = false;
        float emit_for = -1.0f;
    };
    // An addon node's effect, following its parent.
    struct Child {
        uint32_t id = 0;
        glm::mat3 rotation{1.0f};
        glm::vec3 position{0.0f}; // metres at scale 1
        uint32_t node = ~0u;      // the addon node in the parent, when it has one by name
        uint32_t addon = ~0u;     // its ADDN index
    };
    struct Tracked {
        uint32_t id = 0;
        Live live;
        bool hidden = false;
        float scale = 1.0f;
        std::vector<Child> children;
    };

    void set_world(const osw::World& world);
    std::shared_ptr<const fx::Effect> effect(const std::string& osm_path);
    void load(const std::string& osm_path);
    void start_requests();
    void place_addons(const fx::Effect& e, const Request& r, std::vector<Child>* children);
    Live place(const std::shared_ptr<const fx::Effect>& e, const std::string& model, const glm::mat3& rotation,
               const glm::vec3& position, float scale);
    Live start(const std::shared_ptr<const fx::Effect>& e, uint32_t seed, const glm::mat3& rotation, const glm::vec3& position,
               float scale);
    void collect(const fx::EffectInstance& instance, const render::View& view);
    void collect_shapes(const Live& l);

    std::filesystem::path m_cache;
    bool m_soft = true;
    TextureResolver m_textures;
    const osw::World* m_world = nullptr;
    std::string m_world_edid;
    std::unordered_map<std::string, std::shared_ptr<const fx::Effect>> m_effects;
    std::future<std::vector<std::shared_ptr<const fx::Effect>>> m_world_effects; // by the world's mesh index
    void finish_world();
    std::vector<Emitter> m_emitters;
    std::unordered_map<uint32_t, Live> m_live; // by index in m_emitters
    std::vector<Live> m_one_shots;
    std::unordered_map<std::string, std::future<std::shared_ptr<const fx::Effect>>> m_loading;
    std::vector<Request> m_requests;
    std::vector<Request> m_addon_requests; // made while the requests are walked
    std::vector<Tracked> m_tracked;
    uint32_t m_next_tracked = 1;
    std::unordered_map<uint32_t, std::string> m_addons;
    std::unordered_map<const ose::EffectMaterial*, glm::uvec2> m_material_textures; // texture, palette slots
    std::vector<render::GpuParticle> m_particles;
    std::vector<render::GpuParticleSystem> m_systems;
    std::vector<render::ParticleDraw> m_draws;
    std::vector<Shape> m_shapes;
    render::ParticleFrame m_frame;
};

} // namespace spud
