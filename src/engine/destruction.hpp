#pragma once

// Destructible references: how much damage each has taken and the stages it has crossed.

#include "core/binary.hpp"
#include "formats/forms_typed.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace spud::engine {

class Destruction {
public:
    explicit Destruction(const forms::Database& db) : m_db(db) {}

    struct Change {
        int32_t old_index = 0;  // stages crossed so far
        int32_t new_index = 0;
        std::string mesh;       // the cache path that draws it from now on; "" keeps what it has
        bool disable = false;
        bool destroyed = false;
        std::vector<uint32_t> explosions; // of the stages crossed, in order
        struct Debris {
            uint32_t form = 0; // DEBR
            uint32_t count = 0;
        };
        std::vector<Debris> debris;
    };
    // `amount` of damage to a reference of `base` drawn by `mesh`. Nothing when the base isn't destructible, or no stage
    // was crossed.
    std::optional<Change> damage(uint32_t ref, uint32_t base, const std::string& mesh, float amount, bool self_damage);
    int32_t stage(uint32_t ref) const;
    void clear(uint32_t ref);
    // What a save keeps: each damaged reference's health, stage and the model that draws it.
    void save(BinaryWriter& w) const;
    void load(BinaryReader& r);
    // The references drawn by another model than their own, and that model.
    std::vector<std::pair<uint32_t, std::string>> swapped() const;
    // Damage per second each burning reference does to itself.
    const std::unordered_map<uint32_t, float>& self_damaging() const { return m_self_damage; }

private:
    struct Stage {
        uint8_t percent = 0;
        uint8_t model_stage = 0;
        uint8_t flags = 0;
        int32_t self_damage = 0;
        uint32_t explosion = 0, debris = 0, debris_count = 0;
        std::string model;
    };
    struct Data {
        int32_t health = 0;
        std::vector<Stage> stages;
    };
    struct State {
        float health = -1.0f; // -1: whole
        int32_t index = 0;    // the next stage
        std::string mesh;     // "": its own
    };

    const Data& data_of(uint32_t base);

    const forms::Database& m_db;
    std::unordered_map<uint32_t, Data> m_data;
    std::unordered_map<uint32_t, State> m_state;
    std::unordered_map<uint32_t, float> m_self_damage;
};

} // namespace spud::engine
