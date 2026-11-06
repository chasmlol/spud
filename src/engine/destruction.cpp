#include "engine/destruction.hpp"

#include "formats/ost.hpp"

#include "core/path.hpp"
#include "formats/osm.hpp"

#include <algorithm>
#include <cmath>

namespace spud::engine {

namespace {

constexpr uint8_t k_stage_cap_damage = 1;
constexpr uint8_t k_stage_disable = 2;
constexpr uint8_t k_stage_destroy = 4;
constexpr uint8_t k_stage_ignore_external = 8;

} // namespace

const Destruction::Data& Destruction::data_of(uint32_t base) {
    const auto [it, added] = m_data.try_emplace(base);
    if (!added) {
        return it->second;
    }
    Data& data = it->second;
    const forms::FormEntry* entry = m_db.find(base);
    if (!entry) {
        return data;
    }
    const forms::Form form(m_db, *entry);
    for (const auto& field : form.fields()) {
        if (field.sig == forms::sig("DEST")) {
            data.health = static_cast<int32_t>(field.value.member("health")->integer);
        } else if (field.sig == forms::sig("DSTD")) {
            Stage& s = data.stages.emplace_back();
            s.percent = static_cast<uint8_t>(field.value.member("health_percent")->integer);
            s.model_stage = static_cast<uint8_t>(field.value.member("model_damage_stage")->integer);
            s.flags = static_cast<uint8_t>(field.value.member("flags")->integer);
            s.self_damage = static_cast<int32_t>(field.value.member("self_damage_per_second")->integer);
            if (const forms::Value* explosion = field.value.member("explosion")) {
                s.explosion = explosion->form;
            }
            if (const forms::Value* debris = field.value.member("debris")) {
                s.debris = debris->form;
            }
            if (const forms::Value* count = field.value.member("debris_count")) {
                s.debris_count = static_cast<uint32_t>(count->integer);
            }
        } else if (field.sig == forms::sig("DMDL") && !data.stages.empty()) {
            data.stages.back().model = field.value.text;
        }
    }
    return data;
}

// in the game.
std::optional<Destruction::Change> Destruction::damage(uint32_t ref, uint32_t base, const std::string& mesh, float amount,
                                                       bool self_damage) {
    const Data& data = data_of(base);
    if (data.health <= 0 || amount <= 0.0f) {
        return std::nullopt;
    }
    State& state = m_state[ref];
    const float full = static_cast<float>(data.health);
    const float current = state.health < 0.0f ? full : state.health;
    if (current == 0.0f) {
        return std::nullopt;
    }
    const auto percent = [full](float health) { return static_cast<int32_t>(std::ceil(health * 100.0f / full)); };
    const int32_t old_percent = percent(current);
    float remaining = current - amount;
    const int32_t new_percent = remaining > 0.0f ? std::max(percent(remaining), 1) : 0;

    Change change;
    change.old_index = state.index;
    uint8_t model_stage = 0;
    std::string replacement;
    int32_t rate = -1;
    for (size_t i = size_t(state.index); i < data.stages.size(); ++i) {
        const Stage& s = data.stages[i];
        if (s.percent >= old_percent || s.percent < new_percent) {
            if (s.percent < new_percent) {
                break;
            }
            continue;
        }
        state.index = static_cast<int32_t>(i) + 1;
        model_stage = s.model_stage ? s.model_stage : model_stage;
        replacement = s.model.empty() ? replacement : s.model;
        rate = s.self_damage > 0 ? s.self_damage : rate;
        if (s.explosion) {
            change.explosions.push_back(s.explosion);
        }
        if (s.debris && s.debris_count) {
            change.debris.push_back({s.debris, s.debris_count});
        }
        change.disable |= (s.flags & k_stage_disable) != 0;
        change.destroyed |= (s.flags & k_stage_destroy) != 0;
        const bool next_ignores =
            !self_damage && i + 1 < data.stages.size() && (data.stages[i + 1].flags & k_stage_ignore_external);
        if ((s.flags & k_stage_cap_damage) || next_ignores) {
            remaining = std::max(remaining, full * s.percent / 100.0f);
            break;
        }
    }
    state.health = std::max(remaining, 0.0f);
    if (state.health == 0.0f) {
        change.destroyed = true;
    }
    change.new_index = state.index;
    if (change.new_index == change.old_index) {
        return std::nullopt;
    }
    if (rate >= 0 && state.health > 0.0f) {
        m_self_damage[ref] = static_cast<float>(rate);
    } else if (state.health == 0.0f) {
        m_self_damage.erase(ref);
    }
    if (!replacement.empty()) {
        change.mesh = path::replace_extension(path::with_root(path::normalize(replacement), "meshes"), ".osm");
    } else if (model_stage) {
        change.mesh = osm::damage_stage_path(mesh, model_stage);
    }
    state.mesh = change.mesh.empty() ? state.mesh : change.mesh;
    return change;
}

void Destruction::save(BinaryWriter& w) const {
    w.write<uint32_t>(uint32_t(m_state.size()));
    for (const auto& [ref, s] : m_state) {
        w.write<uint32_t>(ref);
        w.write<float>(s.health);
        w.write<int32_t>(s.index);
        ost::put(w, s.mesh);
    }
    w.write<uint32_t>(uint32_t(m_self_damage.size()));
    for (const auto& [ref, rate] : m_self_damage) {
        w.write<uint32_t>(ref);
        w.write<float>(rate);
    }
}

void Destruction::load(BinaryReader& r) {
    m_state.clear();
    m_self_damage.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto ref = r.read<uint32_t>();
        State& s = m_state[ref];
        s.health = r.read<float>();
        s.index = r.read<int32_t>();
        s.mesh = ost::get_string(r);
    }
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto ref = r.read<uint32_t>();
        m_self_damage[ref] = r.read<float>();
    }
}

std::vector<std::pair<uint32_t, std::string>> Destruction::swapped() const {
    std::vector<std::pair<uint32_t, std::string>> out;
    for (const auto& [ref, s] : m_state) {
        if (!s.mesh.empty()) {
            out.emplace_back(ref, s.mesh);
        }
    }
    return out;
}

int32_t Destruction::stage(uint32_t ref) const {
    const auto it = m_state.find(ref);
    return it == m_state.end() ? 0 : it->second.index;
}

void Destruction::clear(uint32_t ref) {
    m_state.erase(ref);
    m_self_damage.erase(ref);
}

} // namespace spud::engine
