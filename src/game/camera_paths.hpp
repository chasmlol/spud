#pragma once

// The kill camera's tree: CPTH paths and their CAMS shots.

#include "formats/forms.hpp"
#include "game/conditions.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace spud::game {

struct CameraShot {
    enum Action { shoot = 0, fly = 1, hit = 2, zoom = 3 };
    enum Place { attacker = 0, projectile = 1, target = 2, lead_actor = 3 };
    enum Flags {
        position_follows = 1, rotation_follows = 2, no_bone = 4, first_person = 8, no_tracer = 0x10, from_zero = 0x20
    };
    std::string model; // lower case, under meshes: "cameras/base.nif"; "" for none
    int action = 0, location = 0, look = 0, flags = 0;
    float player_rate = 1, target_rate = 1, global_rate = 1;
    float max_time = 0, min_time = 0, percent = 0, near_distance = 0;
    uint32_t image_space = 0;
};

struct CameraPath {
    uint32_t form = 0;
    std::vector<CameraShot> shots;
};

class CameraPaths {
public:
    CameraPaths(const forms::Database& db, Conditions& conditions) : m_db(&db), m_conditions(&conditions) {}

    // the attacker as subject and the victim as target. Null when no path answers,
    // or the one that does has a shot with no time, as every Exit path has.
    const CameraPath* pick(uint32_t attacker, uint32_t victim);

private:
    struct Node {
        uint32_t form = 0, parent = 0, previous = 0;
        std::vector<ConditionItem> conditions;
        CameraPath path;
        std::vector<Node*> children;
    };
    void build();
    const Node* resolve(const Node& node, uint32_t attacker, uint32_t victim);
    const forms::Database* m_db;
    Conditions* m_conditions;
    bool m_built = false;
    std::unordered_map<uint32_t, std::unique_ptr<Node>> m_nodes;
    std::vector<Node*> m_roots;
};

} // namespace spud::game
