#include "engine/ref_effects.hpp"

#include "core/path.hpp"
#include "core/units.hpp"
#include "engine/space.hpp"
#include "game/items.hpp"
#include "render/meshes.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace spud::engine {

namespace {

using forms::Form;
using forms::Value;

constexpr std::string_view k_hit_sequence = "SpecialIdle_HitEffect";
constexpr uint32_t k_shape_bit = 0x40000000; // Space::spawn's ids: apart from the projectiles'

enum ShaderFlag : uint32_t {
    shader_no_membrane = 0x1,
    shader_palette_color = 0x2,
    shader_palette_alpha = 0x4,
    shader_no_particles = 0x8,
    shader_edge_inverse = 0x10,
    shader_skin_only = 0x20,
    shader_ignore_alpha = 0x40,
    shader_project_uvs = 0x80,
    shader_ignore_base_alpha = 0x100,
    shader_lighting = 0x200,
    shader_no_weapons = 0x400,
};

float real(const Form& f, std::string_view path, float fallback = 0) {
    const Value* v = f.get(path);
    return !v ? fallback : v->kind == Value::Kind::real ? float(v->real) : float(v->integer);
}

uint32_t integer(const Form& f, std::string_view path) {
    const Value* v = f.get(path);
    return v ? uint32_t(v->integer) : 0;
}

glm::vec3 colour(const Form& f, const std::string& path) {
    return glm::vec3(real(f, path + ".r"), real(f, path + ".g"), real(f, path + ".b")) / 255.0f;
}

std::string texture(const Form& f, std::string_view field) {
    const Value* v = f.get(field);
    return v && !v->text.empty() ? path::with_root(path::normalize(v->text), "textures") : std::string();
}

EffectShader::Ramp ramp(const Form& f, const std::string& name) {
    EffectShader::Ramp r;
    r.in = real(f, "DATA." + name + "_alpha_fade_in_time");
    r.full = real(f, "DATA." + name + "_full_alpha_time");
    r.out = real(f, "DATA." + name + "_alpha_fade_out_time");
    r.ratio = real(f, "DATA." + name + "_full_alpha_ratio");
    r.persistent = real(f, "DATA." + name + "_persistent_alpha_ratio");
    r.pulse_amplitude = real(f, "DATA." + name + "_alpha_pulse_amplitude");
    r.pulse_frequency = real(f, "DATA." + name + "_alpha_pulse_frequency");
    return r;
}

// The table, from the EFSH's numbering to NiAlphaProperty's.
uint32_t blend_factor(uint32_t efsh) {
    constexpr uint32_t k_table[] = {1, 1, 0, 2, 3, 6, 7, 8, 9, 4, 5, 10};
    return k_table[std::min<uint32_t>(efsh, 11)];
}

float step_ramp(const EffectShader::Ramp& r, float previous, float t, float dt, bool finishing, float min_rate) {
    const float hold = r.in + r.full;
    if (finishing && t > hold) {
        if (r.out <= 0.0f) {
            return 0.0f;
        }
        return std::max(previous - std::max((r.persistent != 0.0f ? r.persistent : r.ratio) / r.out, min_rate) * dt, 0.0f);
    }
    if (t < r.in) {
        return std::min(previous + r.ratio * dt / r.in, r.ratio);
    }
    if (t < hold) {
        return r.ratio;
    }
    if (t < hold + r.out) {
        return std::max(previous - std::abs(r.ratio - r.persistent) * dt / r.out, r.persistent);
    }
    return r.persistent;
}

// The piecewise-linear keys of the game.
template <typename T>
T keyed(const std::array<float, 3>& times, const std::array<T, 3>& keys, float t) {
    t = t <= times[2] ? std::max(times[0], t) : times[2];
    int i = 0;
    while (i < 2 && t > times[i]) {
        ++i;
    }
    const int a = i ? i - 1 : 0;
    if (times[a] == times[i]) {
        return keys[a];
    }
    return keys[a] + (keys[i] - keys[a]) * ((t - times[a]) / (times[i] - times[a]));
}

float pulsed(float value, const EffectShader::Ramp& r, float t) {
    return std::clamp((std::sin(t * 2.0f * std::numbers::pi_v<float> * r.pulse_frequency) * r.pulse_amplitude + 1.0f) * value,
                      0.0f, 1.0f);
}

} // namespace

float EffectShader::total_time() const {
    return std::max(fill.in + fill.full + fill.out, edge.in + edge.full + edge.out);
}

float EffectShader::hold_time() const { return std::max(fill.in + fill.full, edge.in + edge.full); }

RefEffects::RefEffects(const forms::Database& db, const game::Settings& settings)
    : m_db(db), m_fill_min_rate(settings.number("fEffectShaderFillEdgeMinFadeRate", 0.05f)),
      m_art_wait(settings.number("fModelReferenceEffectMaxWaitTime", 3.0f)) {}

std::string_view RefEffects::magic_node(uint8_t source) {
    return source == 0 ? "npc l magicnode [lmag]" : source == 1 ? "npc r magicnode [rmag]" : "npc head magicnode [hmag]";
}

std::string RefEffects::art_model(uint32_t form) const {
    const forms::FormEntry* e = m_db.find(form);
    const Value* model = e ? Form(m_db, *e).get("MODL") : nullptr;
    return model ? game::model_path(model->text) : std::string();
}

uint32_t RefEffects::controller(uint32_t actor, uint32_t kind) {
    auto [it, fresh] = m_controllers.try_emplace(uint64_t(actor) << 2 | kind, m_next_controller);
    m_next_controller += fresh;
    return it->second;
}

const EffectShader* RefEffects::shader(uint32_t form) {
    auto [it, fresh] = m_records.try_emplace(form);
    if (fresh) {
        if (const forms::FormEntry* e = m_db.find(form)) {
            const Form f(m_db, *e);
            auto s = std::make_unique<EffectShader>();
            s->fill = ramp(f, "fill");
            s->edge = ramp(f, "edge");
            s->colours = {colour(f, "DATA.fill_color_key1"), colour(f, "DATA.fill_color_key2"),
                          colour(f, "DATA.fill_color_key3")};
            for (int i = 0; i < 3; ++i) {
                const std::string key = "DATA.fill_color_key" + std::to_string(i + 1);
                s->colour_scales[i] = real(f, key + "_scale", 1.0f);
                s->colour_times[i] = real(f, key + "_time");
            }
            s->flags = integer(f, "DATA.flags");
            s->birth = {real(f, "DATA.particle_birth_ramp_up_time"), real(f, "DATA.particle_full_birth_time"),
                        real(f, "DATA.particle_birth_ramp_down_time"), real(f, "DATA.particle_full_birth_ratio"),
                        real(f, "DATA.particle_persistent_count"), 0.0f, 0.0f};
            s->particles = !(s->flags & shader_no_particles) && std::max(s->birth.ratio, s->birth.persistent) > 0.0f;
            if (s->particles) {
                ParticleLook& p = s->particle_look;
                p.texture = texture(f, "ICO2");
                p.palette = s->flags & 0x30000u ? texture(f, "NAM9") : std::string();
                p.technique = 0x43 | (s->flags & 0x10000u ? 0x80000u : 0u) | (s->flags & 0x20000u ? 0x100000u : 0u);
                const uint32_t dst = blend_factor(integer(f, "DATA.particle_dest_blend_mode"));
                p.blend_src = blend_factor(integer(f, "DATA.particle_source_blend_mode"));
                p.blend_dst = dst;
                p.technique |= dst == 0 ? 0x400u : 0u;
                p.lifetime = real(f, "DATA.particle_lifetime");
                p.lifetime_range = real(f, "DATA.particle_lifetime_range");
                p.speed = real(f, "DATA.particle_initial_speed_along_normal");
                p.speed_range = real(f, "DATA.particle_initial_speed_along_normal_range");
                p.acceleration_along_normal = real(f, "DATA.particle_acceleration_along_normal");
                const auto vec = [&](const std::string& name) {
                    const forms::Value* v = f.get("DATA");
                    const forms::Value* m = v ? v->member(name) : nullptr;
                    const auto axis = [&](const char* a) {
                        const forms::Value* c = m ? m->member(a) : nullptr;
                        return c ? float(c->real) : 0.0f;
                    };
                    return glm::vec3(axis("x"), axis("y"), axis("z"));
                };
                p.velocity = vec("particle_initial_velocity");
                p.acceleration = vec("particle_acceleration");
                p.scale_key[0] = real(f, "DATA.particle_scale_key1");
                p.scale_key[1] = real(f, "DATA.particle_scale_key2");
                p.scale_time[0] = real(f, "DATA.particle_scale_key1_time");
                p.scale_time[1] = real(f, "DATA.particle_scale_key2_time");
                for (int i = 0; i < 3; ++i) {
                    const std::string key = "DATA.color_key" + std::to_string(i + 1);
                    p.colours[size_t(i)] = glm::vec4(colour(f, key), real(f, key + "_alpha"));
                    p.colour_times[size_t(i)] = real(f, key + "_time");
                }
                p.colour_scale = real(f, "DATA.color_scale", 1.0f);
                constexpr float k_radians = 0.017453292f; // the record's are degrees
                p.rotation = real(f, "DATA.particle_initial_rotation") * k_radians;
                p.rotation_range = real(f, "DATA.particle_initial_rotation_range") * k_radians;
                p.rotation_speed = real(f, "DATA.particle_rotation_speed") * k_radians;
                p.rotation_speed_range = real(f, "DATA.particle_rotation_speed_range") * k_radians;
                const forms::Value* data = f.get("DATA");
                const forms::Value* addon = data ? data->member("addon_models") : nullptr;
                const forms::FormEntry* debris = addon ? m_db.find(addon->form) : nullptr;
                if (debris && debris->type == forms::sig("DEBR")) {
                    const forms::Form d(m_db, *debris);
                    for (const forms::Value* v : d.all("DATA")) {
                        const forms::Value* percentage = v->member("percentage");
                        const forms::Value* model = v->member("model");
                        if (percentage && model && !model->text.empty()) {
                            s->addons.push_back({int(percentage->integer), game::model_path(model->text)});
                        }
                    }
                    s->addon_scale_start = real(f, "DATA.addon_models_scale_start");
                    s->addon_scale_end = real(f, "DATA.addon_models_scale_end");
                    s->addon_scale_in = real(f, "DATA.addon_models_scale_in_time");
                    s->addon_scale_out = real(f, "DATA.addon_models_scale_out_time");
                }
                p.birth_offset = real(f, "DATA.birth_position_offset");
                p.birth_offset_range = real(f, "DATA.birth_position_offset_range");
                p.cells_u = uint32_t(integer(f, "DATA.texture_count_u"));
                p.cells_v = uint32_t(integer(f, "DATA.texture_count_v"));
                p.animated = (s->flags & 0x8000u) != 0;
                p.start_frame = uint32_t(integer(f, "DATA.particle_start_frame"));
                p.start_variation = uint32_t(integer(f, "DATA.particle_start_frame_variation"));
                p.end_frame = uint32_t(integer(f, "DATA.particle_end_frame"));
                p.loop_start = uint32_t(integer(f, "DATA.particle_loop_start_frame"));
                p.loop_variation = uint32_t(integer(f, "DATA.particle_loop_start_variation"));
                p.frame_count = uint32_t(integer(f, "DATA.particle_frame_count"));
                p.frame_variation = uint32_t(integer(f, "DATA.particle_frame_count_variation"));
            }
            s->edge_colour = colour(f, "DATA.edge_color");
            if (s->flags & shader_edge_inverse) {
                s->edge_colour -= 1.0f;
            }
            s->edge_fall_off = real(f, "DATA.edge_fall_off");
            s->speed = {real(f, "DATA.fill_texture_animation_speed_u"), real(f, "DATA.fill_texture_animation_speed_v")};
            s->texture_scale = {real(f, "DATA.fill_texture_scale_u", 1.0f), real(f, "DATA.fill_texture_scale_v", 1.0f)};
            MembraneLook& l = s->look;
            l.fill = texture(f, "ICON");
            if (s->flags & (shader_palette_color | shader_palette_alpha)) {
                l.palette = texture(f, "NAM8");
            }
            render::game_blend(blend_factor(integer(f, "DATA.membrane_source_blend_mode")),
                               blend_factor(integer(f, "DATA.membrane_dest_blend_mode")), l.blend_src, l.blend_dst);
            l.technique = 0x42 | (l.blend_dst == 0 ? 0x400u : 0u);
            l.technique |= (s->flags & shader_palette_color ? 0x80000u : 0u) | (s->flags & shader_palette_alpha ? 0x100000u : 0u) |
                           (s->flags & shader_ignore_alpha ? 0x200000u : 0u) | (s->flags & shader_project_uvs ? 0x20000u : 0u) |
                           (s->flags & shader_lighting ? 0x10000u : 0u);
            l.ignore_base_alpha = s->flags & shader_ignore_base_alpha;
            l.no_weapons = s->flags & shader_no_weapons;
            l.skin_only = s->flags & shader_skin_only;
            it->second = std::move(s);
        }
    }
    return it->second.get();
}

void RefEffects::start(const Hit& hit) {
    discard(hit.id); // what a controller had playing goes when it starts again
    if (std::string model = hit.art ? art_model(hit.art) : std::string(); !model.empty() || hit.light) {
        Art a;
        a.id = hit.id;
        a.target = hit.target;
        a.model = std::move(model);
        a.node = hit.node;
        a.light = hit.light;
        a.scale = std::abs(hit.scale);
        a.lifetime = hit.lifetime;
        a.persist = hit.persist;
        a.finishing = !hit.persist;
        a.finished_at = hit.persist ? -1.0f : 0.0f;
        m_arts.push_back(std::move(a));
    }
    if (const EffectShader* record = hit.shader ? shader(hit.shader) : nullptr) {
        Shader s;
        s.id = hit.id;
        s.key = m_next_key++;
        s.target = hit.target;
        s.record = record;
        s.lifetime = hit.lifetime;
        s.persist = hit.persist;
        s.finishing = !hit.persist; // not Persist: the one shot runs from the start
        s.fill = record->fill.start();
        s.edge = record->edge.start();
        s.weapon = hit.weapon;
        s.node = hit.node;
        m_shaders.push_back(s);
    }
}

void RefEffects::finish(uint32_t id) {
    for (Shader& s : m_shaders) {
        s.finishing = s.finishing || s.id == id;
    }
    for (Art& a : m_arts) {
        if (a.id == id && !a.finishing) {
            a.finishing = true;
            a.finished_at = a.age;
        }
    }
}

// off removes it, it doesn't play out.
void RefEffects::hand(uint32_t actor, uint8_t source, uint32_t art, uint32_t light) {
    const uint32_t id = controller(actor, source);
    discard(id);
    m_hand_ids.insert(id);
    if (art || light) {
        start({.id = id, .target = actor, .art = art, .light = light, .persist = true,
               .node = std::string(magic_node(source))});
    }
}

void RefEffects::hand_cast(uint32_t actor, uint8_t source, game::CastPhase phase) {
    const uint32_t id = controller(actor, source);
    m_hand_phase[id] = phase;
    if (phase == game::CastPhase::released) {
        m_hand_released.insert(id);
    }
}

// Casting art's graph, the table of (castingfxbehavior01): each state plays a sequence of the art.
void RefEffects::hand_state(Art& a, fx::EffectInstance& instance, HandState state) {
    const auto name = [&](HandState s) -> std::string_view {
        switch (s) {
        case HandState::intro: return "mIntro";
        case HandState::idle: return "mIdle";
        case HandState::charge: return "mCharge";
        case HandState::ready:
        case HandState::concentration: return "mReady"; // mCastConcentration plays it
        case HandState::cast: return "mCast";
        default: return "mCastEnd";
        }
    };
    for (const std::string& s : instance.sequence_names()) {
        instance.deactivate_sequence(s);
    }
    instance.activate_sequence(name(state));
    a.hand_state = state;
    a.hand_length = instance.sequence_duration(name(state));
    a.hand_age = 0.0f;
}

void RefEffects::update_hand(Art& a, fx::EffectInstance& instance, float dt) {
    using P = game::CastPhase;
    using S = HandState;
    const auto it = m_hand_phase.find(a.id);
    const P phase = it == m_hand_phase.end() ? P::none : it->second;
    a.hand_age += dt;
    const bool ended = a.hand_length > 0.0f && a.hand_age >= a.hand_length; // `end` [I: the sequence generator's]
    const S s = a.hand_state;
    if (phase == P::sustained) { // Spell...ConcentrationStart: no release event reaches the art
        m_hand_released.erase(a.id);
        a.hand_phase = phase;
        if (s != S::concentration) {
            hand_state(a, instance, S::concentration);
        }
        return;
    }
    if (m_hand_released.erase(a.id)) {
        a.hand_phase = phase;
        hand_state(a, instance, S::cast);
        return;
    }
    if (phase != a.hand_phase) { // the actor's events reach the art's graph by name
        const P was = a.hand_phase;
        a.hand_phase = phase;
        if (phase == P::charging && (s == S::idle || s == S::intro || s == S::cast)) {
            hand_state(a, instance, S::charge);
        } else if (phase == P::ready && s != S::ready) {
            hand_state(a, instance, S::ready);
        } else if (phase == P::released) {
            hand_state(a, instance, S::cast);
        } else if (phase == P::sustained && (s == S::idle || s == S::intro || s == S::charge || s == S::ready)) {
            hand_state(a, instance, S::concentration);
        } else if (phase == P::none && (s == S::charge || s == S::ready || s == S::concentration)) {
            hand_state(a, instance, S::idle); // a cancelled cast
        } else if (phase == P::none && was == P::sustained && instance.sequence_duration("mCastEnd") > 0.0f) {
            hand_state(a, instance, S::cast_end); // wardhandfxbehavior
        }
        return;
    }
    if (ended && s == S::intro) {
        hand_state(a, instance, S::idle);
    } else if (ended && s == S::charge) {
        hand_state(a, instance, S::ready);
    } else if (ended && s == S::cast) {
        hand_state(a, instance, S::intro); // the art replays its intro after a cast
    } else if (ended && s == S::cast_end) {
        hand_state(a, instance, S::idle);
    }
}

// The rigid weapon sits on the hand's weapon node; a bow, the left's.
void RefEffects::weapon(uint32_t actor, uint8_t source, uint32_t art, uint32_t shader) {
    const uint32_t id = controller(actor, source + 2u);
    discard(id);
    if (art || shader) {
        start({.id = id, .target = actor, .art = art, .shader = shader, .persist = true,
               .node = source == 0 ? "shield" : "weapon", .weapon = true});
    }
}

void RefEffects::play(uint32_t target, uint32_t art) {
    start({.id = m_next_controller++, .target = target, .art = art});
}

void RefEffects::end_art(const Art& a, Space* space, WorldFx* fx, Vfx* vfx) {
    if (a.handle && fx) {
        fx->stop(a.handle);
    }
    if (a.shape && space) {
        space->despawn(a.id | k_shape_bit);
    }
    if (a.light && vfx) {
        vfx->remove_light(a.id);
    }
}

void RefEffects::discard(uint32_t id) {
    for (Shader& s : m_shaders) {
        s.gone = s.gone || s.id == id;
    }
    for (Art& a : m_arts) {
        a.gone = a.gone || a.id == id;
    }
}

// a model on each emitter node, by the DEBR's percentages, each entry once.: the scale ramps up, and back
// down when finishing. The fade isn't followed: spawned models have no alpha.
void RefEffects::update_addons(Shader& s, float dt, Space& space) {
    const EffectShader& r = *s.record;
    if (!s.addons_placed && !m_bones.empty()) {
        s.addons_placed = true;
        std::vector<const EffectShader::AddonModel*> pool;
        int total = 0;
        for (const auto& a : r.addons) {
            if (pool.size() < 20) {
                pool.push_back(&a);
                total += a.percentage;
            }
        }
        uint32_t state = s.id * 2654435761u + 1u;
        for (uint32_t bone = 0; bone < m_bones.size() && total > 0; ++bone) {
            state = state * 1664525u + 1013904223u;
            int roll = int(state >> 8) % total;
            for (size_t i = 0; i < pool.size(); ++i) {
                const int weight = r.addons[i].percentage;
                if (roll < weight) {
                    if (pool[i]) {
                        const uint32_t id = m_next_addon++;
                        space.spawn(pool[i]->model, m_bones[bone].to * units::k_skyrim_unit_m, glm::quat_cast(m_bones[bone].rotation), 1.0f, id);
                        s.addons.push_back({id, bone});
                        pool[i] = nullptr;
                    }
                    break;
                }
                roll -= weight;
            }
        }
    }
    const float time = s.finishing ? r.addon_scale_out : r.addon_scale_in;
    const float step = time > 0.001f ? dt / time : 1.0f;
    s.addon_ramp = std::clamp(s.addon_ramp + (s.finishing ? -step : step), 0.0f, 1.0f);
    const float least = std::max(s.finishing ? r.addon_scale_end : r.addon_scale_start, 0.01f);
    const float scale = (1.0f - least) * s.addon_ramp + least;
    for (const Shader::Addon& a : s.addons) {
        if (a.bone < m_bones.size()) {
            const BoneSegment& b = m_bones[a.bone];
            space.move_spawned(a.id, b.to * units::k_skyrim_unit_m, b.rotation * scale);
        }
    }
}

// The load, and where its AttachT says it goes.
void RefEffects::first_run(Art& a, fx::EffectInstance& instance) {
    const ose::Effect& d = instance.data();
    for (const uint32_t t : d.attach_techniques) {
        const std::string_view technique = d.string(t);
        if (technique.starts_with("NamedNode&")) { // PArrayOnHand, HavokGeometry attach plainly
            a.node = path::to_lower(technique.substr(10));
        }
    }
    for (const ose::Sequence& s : d.sequences) {
        if (d.string(s.name) == k_hit_sequence) {
            for (const std::string& name : instance.sequence_names()) {
                instance.deactivate_sequence(name);
            }
            instance.activate_sequence(k_hit_sequence);
            a.hit_sequence = true;
            a.hit_length = s.cycle_type != 0 ? instance.sequence_duration(k_hit_sequence) : -1.0f;
        }
    }
    // A casting art: the graph starts it in mIntro, or at mIdle in the graphs that have none.
    if (m_hand_ids.contains(a.id)) {
        a.hand = instance.sequence_duration("mIdle") > 0.0f;
        if (a.hand) {
            hand_state(a, instance, instance.sequence_duration("mIntro") > 0.0f ? HandState::intro : HandState::idle);
        }
    }
}

// with what parenting does to the art in the game. False: it's over. An art model's
// graph doesn't run here: it shows its sequences, and a one shot takes the wait a graph takes after StopEffect.
bool RefEffects::update_art(Art& a, float dt, Space& space, WorldFx& fx, Vfx* vfx) {
    a.age += dt;
    ActorSet& actors = *space.actors();
    glm::mat3 rotation;
    glm::vec3 position;
    if (!actors.node_world(a.target, a.node, rotation, position) && !actors.node_world(a.target, {}, rotation, position)) {
        return a.persist && !a.finishing && !a.handle; // no 3D yet
    }
    if (a.light && vfx) {
        const glm::vec3 units = position / units::k_skyrim_unit_m;
        vfx->set_light(a.id, a.light, {units.x, units.y, units.z}, 1.0f);
    }
    if (a.shape) {
        space.move_spawned(a.id | k_shape_bit, position, rotation * a.scale);
    } else if (!a.handle && !a.model.empty()) {
        if (fx.has_effect(a.model)) {
            a.handle = fx.track(a.model, rotation, position, a.scale);
        } else {
            a.shape = true; // a model with no particle systems: only its shapes
            space.spawn(a.model, position, 0.0f, a.id | k_shape_bit);
        }
    } else if (a.handle) {
        fx.move(a.handle, rotation, position);
        if (fx::EffectInstance* instance = a.running ? nullptr : fx.tracked(a.handle)) {
            a.running = true;
            first_run(a, *instance);
        }
        if (fx::EffectInstance* instance = a.hand ? fx.tracked(a.handle) : nullptr) {
            update_hand(a, *instance, dt);
        }
        if (fx::EffectInstance* instance = vfx ? fx.tracked(a.handle) : nullptr) {
            if (const uint32_t node = instance->find_node("AttachLight"); node != ose::k_none) {
                vfx->attach_light(a.target, instance->node_world_translation_units(node));
            }
        }
    }
    if (a.lifetime >= 0.0f && a.age >= a.lifetime && !a.finishing) {
        a.finishing = true;
        a.finished_at = a.age;
    }
    if (!a.finishing) {
        return true;
    }
    if (a.hit_sequence) {
        return a.age < a.hit_length;
    }
    return a.age - a.finished_at < m_art_wait;
}

void RefEffects::update_shader(Shader& s, float dt) {
    const EffectShader& r = *s.record;
    s.age += dt;
    if (s.lifetime >= 0.0f && s.age >= s.lifetime) {
        s.finishing = true;
    }
    const bool finishing = s.finishing && s.age >= r.hold_time();
    s.fill = step_ramp(r.fill, s.fill, s.age, dt, finishing, m_fill_min_rate);
    s.edge = step_ramp(r.edge, s.edge, s.age, dt, finishing, m_fill_min_rate);
    s.birth = r.particles ? step_ramp(r.birth, s.birth, s.age, dt, finishing, m_fill_min_rate) : 0.0f;
    s.uv += dt * r.speed;
}

render::GpuMaterialAnim RefEffects::membrane_block(const Shader& s) const {
    const EffectShader& r = *s.record;
    render::GpuMaterialAnim b{};
    const glm::vec3 fill = keyed(r.colour_times, r.colours, s.age);
    const float scale = keyed(r.colour_times, r.colour_scales, s.age);
    const float alpha = pulsed(s.fill, r.fill, s.age);
    const bool palette = (r.flags & shader_palette_color) != 0;
    const float property = palette ? 1.0f : scale; // raw for the palette, which takes the scale itself
    b.emit[0] = fill.r * property;
    b.emit[1] = fill.g * property;
    b.emit[2] = fill.b * property;
    b.emit[3] = alpha * property;
    b.base_color[0] = r.edge_colour.r;
    b.base_color[1] = r.edge_colour.g;
    b.base_color[2] = r.edge_colour.b;
    b.base_color[3] = pulsed(s.edge, r.edge, s.age);
    b.falloff[0] = r.edge_fall_off;
    b.falloff[2] = scale;
    b.uv[0] = s.uv.x;
    b.uv[1] = s.uv.y;
    b.uv[2] = r.texture_scale.x;
    b.uv[3] = r.texture_scale.y;
    b.effect[0] = r.flags & shader_lighting ? 1.0f : 0.0f; // LightingInfluence: not written for a membrane [I]
    b.extra[1] = 1.0f;                                     // posed by skin.comp
    return b;
}

void RefEffects::update(float dt, Space* space, WorldFx* fx, Vfx* vfx) {
    ActorSet* actors = space ? space->actors() : nullptr;
    std::erase_if(m_arts, [&](Art& a) {
        const bool on = !a.gone && actors && fx && update_art(a, dt, *space, *fx, vfx);
        if (!on) {
            end_art(a, space, fx, vfx);
        }
        return !on;
    });
    for (Shader& s : m_shaders) {
        update_shader(s, dt);
    }
    // Alive only with the target's 3D. A persisting effect waits for it: a ghost's ability starts before its
    // body loads [I].
    std::erase_if(m_shaders, [&](Shader& s) {
        const bool loaded = actors && actors->has_3d(s.target);
        const EffectShader& r = *s.record;
        const bool over = s.finishing && s.fill <= 0.0f && s.edge <= 0.0f && s.age >= r.total_time();
        const bool gone = s.gone || over || (!loaded && (!s.persist || s.finishing));
        if (gone && s.attached && actors) {
            actors->clear_membrane(s.target, s.key);
        }
        if (gone && space) {
            for (const Shader::Addon& a : s.addons) {
                space->despawn(a.id);
            }
        }
        return gone;
    });
    if (!actors) {
        return;
    }
    // Systems of 78 particles, as many as the ramp's largest count takes.
    for (Shader& s : m_shaders) {
        const EffectShader& r = *s.record;
        if (!r.particles || !fx) {
            continue;
        }
        if (!s.particles) {
            const float most = std::max(r.birth.ratio, r.birth.persistent);
            s.particles = std::make_shared<ShaderParticles>(r.particle_look, s.id * 2654435761u, uint32_t(std::ceil(most / 78.0f)) * 78u);
        }
        m_bones.clear();
        if (s.weapon) { // the Ray emitter: one line, along the blade
            if (BoneSegment blade; actors->weapon_ray(s.target, s.node, blade)) {
                m_bones.push_back(blade);
            }
        } else {
            actors->bone_segments(s.target, m_bones);
        }
        s.particles->step(dt, uint32_t(s.birth), m_bones);
        if (space && !r.addons.empty()) {
            update_addons(s, dt, *space);
        }
        if (!s.particles->empty()) {
            WorldFx::Batch b;
            b.texture = r.particle_look.texture;
            b.palette = r.particle_look.palette;
            b.technique = r.particle_look.technique;
            render::game_blend(r.particle_look.blend_src, r.particle_look.blend_dst, b.blend_src, b.blend_dst);
            b.colour_scale = r.particle_look.colour_scale;
            b.centre = s.particles->centre() * units::k_skyrim_unit_m;
            s.particles->collect(b.particles, 0);
            fx->batches().push_back(std::move(b));
        }
    }
    // A property holds one effect's data: the last to start shows.
    for (auto s = m_shaders.rbegin(); s != m_shaders.rend(); ++s) {
        if (s->record->flags & shader_no_membrane) {
            continue;
        }
        bool newest = true;
        for (auto later = m_shaders.rbegin(); later != s; ++later) {
            newest = newest && (later->target != s->target || later->weapon != s->weapon ||
                                (later->record->flags & shader_no_membrane));
        }
        if (newest) {
            MembraneLook look = s->record->look;
            look.key = s->key;
            look.weapon = s->weapon;
            s->attached = actors->set_membrane(s->target, look, membrane_block(*s));
        }
    }
}

} // namespace spud::engine
