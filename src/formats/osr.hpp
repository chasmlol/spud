#pragma once

// .osr — spud rules: the game settings and records that drive gameplay systems, imported from the load
// order (rules/game.osr in the cache). Spec: docs/formats/osr.md
//
// For now this is what movement needs: every numeric game setting, movement types, race movement data, the
// player's body and camera dimensions, and the default key bindings. Values are converted to spud units
// (metres, seconds, radians) where the field says so; game settings are kept verbatim, in Skyrim units.

#include "formats/container.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace spud::osr {

inline constexpr container::FourCC k_kind = container::fourcc("RULE");
inline constexpr uint32_t k_version = 6; // 2: collision layers (COLL); 3: third-person camera height; 4: stairs
                                         // materials (STAI); 5: the camera shake (SHAK); 6: races' bounds (RBND). Older files read

// A numeric game setting (GMST). The name's first letter gives the type, as in the game: f float, i int,
// b bool, u unsigned. String settings are not kept.
struct Setting {
    std::string name;
    double value = 0;
};

// MOVT: speeds by direction for walking and running, metres per second; rotation in radians per second.
struct MovementType {
    uint32_t form_id = 0;
    std::string edid;
    std::string name; // MNAM: the behaviour-graph movement state it applies to ("NPCDefault", ...)
    float left[2] = {0, 0};    // walk, run
    float right[2] = {0, 0};
    float forward[2] = {0, 0};
    float back[2] = {0, 0};
    float rotate_in_place[2] = {0, 0};
    float rotate_while_moving = 0; // run
    // INAM: animation change thresholds (directional radians, speed m/s, rotation rad/s); <0 = none.
    float thresholds[3] = {-1, -1, -1};
};

// RACE movement and body data.
struct Race {
    uint32_t form_id = 0;
    std::string edid;
    float height[2] = {1, 1}; // male, female scale
    float weight[2] = {1, 1};
    float base_mass = 1;
    float acceleration_rate = 1;   // as authored; unit undocumented
    float deceleration_rate = 1;
    float angular_acceleration_rate = 0;
    float angular_tolerance = 0;
    uint32_t size = 1;             // 0 small, 1 medium, 2 large, 3 extra large
    uint32_t flags = 0;            // RACE DATA flags (bit 0 playable)
    // Default movement types (WKMV RNMV SWMV FLMV SNMV SPMV): walk, run, swim, fly, sneak, sprint; 0 = none.
    std::array<uint32_t, 6> movement_types{};
    std::string skeleton[2];      // male, female (cache-style path "meshes/...nif")
    // BSBound "BBX" of each skeleton, metres, unscaled; all zero: none, the game's fallback holds.
    float bound_centre[2][3] = {};
    float bound_half[2][3] = {};
};

// The player character as the game starts it, with the body dimensions the controller and camera need.
struct Player {
    uint32_t npc = 0;  // NPC_ FormID
    uint32_t race = 0; // RACE FormID
    float npc_height = 1;
    bool female = false;
    // Unscaled skeleton dimensions, metres (multiply by race height x npc height):
    float eye_height = 0;      // first-person camera node above the feet (Camera1st [Cam1])
    float camera3_height = 0;  // third-person camera pivot: the Camera3rd [Cam3] node's local z (0 before version 3)
    float bound_centre[3] = {0, 0, 0};   // BSBound "BBX" of the third-person skeleton
    float bound_half[3] = {0, 0, 0};
};

// A collision layer (COLL): which layers it collides with at runtime. The game rebuilds its collision matrix
// from these records after loading the plugins.
struct CollisionLayer {
    uint32_t index = 0;    // BNAM: the layer number shapes carry
    std::string edid;      // "L_CHARCONTROLLER", ...
    uint32_t flags = 0;    // GNAM: bit 0 trigger volume, bit 1 sensor
    uint64_t collides = 0; // bit n: collides with layer n
};

// One user event of controlmap.txt. Keys are DirectInput scan codes, 0xFF = unmapped.
struct KeyBinding {
    std::string event;     // "Forward", "Jump", ...
    uint32_t context = 0;  // input context: 0 main gameplay, then in file order
    std::array<uint16_t, 2> keys{0xFF, 0xFF}; // alternatives
    uint16_t mouse = 0xFF; // mouse button / axis id
    uint32_t gamepad = 0xFF;
};

inline constexpr float k_camera_shake_step = 1.0f / 120.0f;

// A place to sit or lie on a FURN, from its model's BSFurnitureMarkerNode: where the user's root ends up, in the
// furniture's own space, with their heading from its own.
struct FurnitureMarker {
    uint32_t form = 0;    // FURN
    uint32_t index = 0;   // the marker's number, FNPR order
    float offset[3] = {}; // units
    float heading = 0;    // radians
    uint16_t animation = 0;
    uint16_t entry = 0;   // 1 front, 2 behind, 4 right, 8 left, 16 up
};

struct Rules {
    std::vector<Setting> settings; // sorted by name (case-insensitive)
    std::vector<MovementType> movement_types;
    std::vector<Race> races;
    Player player;
    std::vector<KeyBinding> keys;
    std::vector<CollisionLayer> collision_layers; // by index; empty in version 1 files
    // Havok material IDs whose surfaces always count as steps: the MATT records flagged "stair material", as
    // The game of the lower-cased MNAM (the ID shapes carry). Sorted; empty before version 4.
    std::vector<uint32_t> stairs_materials;
    // The camera shake's loop: euler angles (radians, about the camera's right, forward and up) every
    // k_camera_shake_step seconds. Empty before version 5.
    std::vector<std::array<float, 3>> camera_shake;
    std::vector<FurnitureMarker> furniture_markers; // by form, then index

    std::optional<double> setting(std::string_view name) const; // case-insensitive
    const MovementType* movement_type(std::string_view edid) const;
    const MovementType* movement_type(uint32_t form_id) const;
    const Race* race(uint32_t form_id) const;
    const KeyBinding* key(std::string_view event, uint32_t context = 0) const;
    const CollisionLayer* collision_layer(uint32_t index) const;
};

std::vector<std::byte> write(const Rules& rules);
Rules read(std::span<const std::byte> file);

} // namespace spud::osr
