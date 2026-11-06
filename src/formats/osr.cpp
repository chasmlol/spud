#include "formats/osr.hpp"

#include <algorithm>
#include <cctype>

namespace spud::osr {

namespace {

constexpr auto k_sets = container::fourcc("SETS");
constexpr auto k_movt = container::fourcc("MOVT");
constexpr auto k_race = container::fourcc("RACE");
constexpr auto k_plyr = container::fourcc("PLYR");
constexpr auto k_keys = container::fourcc("KEYS");
constexpr auto k_strs = container::fourcc("STRS");
constexpr auto k_coll = container::fourcc("COLL");
constexpr auto k_stai = container::fourcc("STAI");
constexpr auto k_shak = container::fourcc("SHAK");
constexpr auto k_furn = container::fourcc("FURM");
constexpr auto k_rbnd = container::fourcc("RBND");

struct SettingRecord {
    uint32_t name;
    uint32_t reserved;
    double value;
};
static_assert(sizeof(SettingRecord) == 16);

struct MovementRecord {
    uint32_t form_id;
    uint32_t edid;
    uint32_t name;
    float speeds[11]; // left, right, forward, back (walk, run each), rotate in place (walk, run), rotate moving
    float thresholds[3];
};
static_assert(sizeof(MovementRecord) == 68);

struct RaceRecord {
    uint32_t form_id;
    uint32_t edid;
    float height[2];
    float weight[2];
    float base_mass;
    float acceleration_rate;
    float deceleration_rate;
    float angular_acceleration_rate;
    float angular_tolerance;
    uint32_t size;
    uint32_t flags;
    uint32_t movement_types[6];
    uint32_t skeleton[2];
};
static_assert(sizeof(RaceRecord) == 84);

struct RaceBoundRecord {
    float centre[2][3];
    float half[2][3];
};
static_assert(sizeof(RaceBoundRecord) == 48);

struct PlayerRecordV2 {
    uint32_t npc;
    uint32_t race;
    float npc_height;
    uint32_t female;
    float eye_height;
    float bound_centre[3];
    float bound_half[3];
};
static_assert(sizeof(PlayerRecordV2) == 44);

struct PlayerRecord : PlayerRecordV2 {
    float camera3_height;
};
static_assert(sizeof(PlayerRecord) == 48);

struct KeyRecord {
    uint32_t event;
    uint32_t context;
    uint16_t keys[2];
    uint16_t mouse;
    uint16_t reserved;
    uint32_t gamepad;
};
static_assert(sizeof(KeyRecord) == 20);

struct CollisionRecord {
    uint32_t index;
    uint32_t edid;
    uint32_t flags;
    uint32_t reserved;
    uint64_t collides;
};
static_assert(sizeof(CollisionRecord) == 24);

bool iequal(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

bool iless(std::string_view a, std::string_view b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
    });
}

} // namespace

std::optional<double> Rules::setting(std::string_view name) const {
    auto it = std::lower_bound(settings.begin(), settings.end(), name,
                               [](const Setting& s, std::string_view n) { return iless(s.name, n); });
    if (it != settings.end() && iequal(it->name, name)) {
        return it->value;
    }
    return std::nullopt;
}

const MovementType* Rules::movement_type(std::string_view edid) const {
    for (const auto& m : movement_types) {
        if (iequal(m.edid, edid)) {
            return &m;
        }
    }
    return nullptr;
}

const MovementType* Rules::movement_type(uint32_t form_id) const {
    for (const auto& m : movement_types) {
        if (m.form_id == form_id) {
            return &m;
        }
    }
    return nullptr;
}

const Race* Rules::race(uint32_t form_id) const {
    for (const auto& r : races) {
        if (r.form_id == form_id) {
            return &r;
        }
    }
    return nullptr;
}

const KeyBinding* Rules::key(std::string_view event, uint32_t context) const {
    for (const auto& k : keys) {
        if (k.context == context && iequal(k.event, event)) {
            return &k;
        }
    }
    return nullptr;
}

const CollisionLayer* Rules::collision_layer(uint32_t index) const {
    for (const auto& c : collision_layers) {
        if (c.index == index) {
            return &c;
        }
    }
    return nullptr;
}

std::vector<std::byte> write(const Rules& rules) {
    container::StringTableBuilder strings;
    auto str = [&](const std::string& s) { return s.empty() ? container::k_no_string : strings.add(s); };

    std::vector<Setting> sorted = rules.settings;
    std::stable_sort(sorted.begin(), sorted.end(), [](const Setting& a, const Setting& b) { return iless(a.name, b.name); });
    std::vector<SettingRecord> sets;
    for (const auto& s : sorted) {
        sets.push_back({str(s.name), 0, s.value});
    }
    std::vector<MovementRecord> movt;
    for (const auto& m : rules.movement_types) {
        MovementRecord r{};
        r.form_id = m.form_id;
        r.edid = str(m.edid);
        r.name = str(m.name);
        const float speeds[11] = {m.left[0],    m.left[1],    m.right[0],          m.right[1],
                                  m.forward[0], m.forward[1], m.back[0],           m.back[1],
                                  m.rotate_in_place[0], m.rotate_in_place[1], m.rotate_while_moving};
        std::copy(std::begin(speeds), std::end(speeds), r.speeds);
        std::copy(std::begin(m.thresholds), std::end(m.thresholds), r.thresholds);
        movt.push_back(r);
    }
    std::vector<RaceRecord> races;
    for (const auto& x : rules.races) {
        RaceRecord r{};
        r.form_id = x.form_id;
        r.edid = str(x.edid);
        std::copy(std::begin(x.height), std::end(x.height), r.height);
        std::copy(std::begin(x.weight), std::end(x.weight), r.weight);
        r.base_mass = x.base_mass;
        r.acceleration_rate = x.acceleration_rate;
        r.deceleration_rate = x.deceleration_rate;
        r.angular_acceleration_rate = x.angular_acceleration_rate;
        r.angular_tolerance = x.angular_tolerance;
        r.size = x.size;
        r.flags = x.flags;
        std::copy(x.movement_types.begin(), x.movement_types.end(), r.movement_types);
        r.skeleton[0] = str(x.skeleton[0]);
        r.skeleton[1] = str(x.skeleton[1]);
        races.push_back(r);
    }
    PlayerRecord player{};
    const auto& p = rules.player;
    player.npc = p.npc;
    player.race = p.race;
    player.npc_height = p.npc_height;
    player.female = p.female ? 1 : 0;
    player.eye_height = p.eye_height;
    player.camera3_height = p.camera3_height;
    std::copy(std::begin(p.bound_centre), std::end(p.bound_centre), player.bound_centre);
    std::copy(std::begin(p.bound_half), std::end(p.bound_half), player.bound_half);
    std::vector<KeyRecord> keys;
    for (const auto& k : rules.keys) {
        keys.push_back({str(k.event), k.context, {k.keys[0], k.keys[1]}, k.mouse, 0, k.gamepad});
    }

    std::vector<CollisionRecord> coll;
    for (const auto& c : rules.collision_layers) {
        coll.push_back({c.index, str(c.edid), c.flags, 0, c.collides});
    }

    container::Writer w(k_kind, k_version);
    w.add_array(k_sets, sets);
    w.add_array(k_movt, movt);
    w.add_array(k_race, races);
    std::vector<RaceBoundRecord> bounds; // parallel to RACE
    for (const auto& x : rules.races) {
        RaceBoundRecord b{};
        std::memcpy(b.centre, x.bound_centre, sizeof b.centre);
        std::memcpy(b.half, x.bound_half, sizeof b.half);
        bounds.push_back(b);
    }
    w.add_array(k_rbnd, bounds);
    w.add_struct(k_plyr, player);
    w.add_array(k_keys, keys);
    w.add_array(k_coll, coll);
    w.add_array(k_stai, rules.stairs_materials);
    w.add_array(k_shak, rules.camera_shake);
    w.add_array(k_furn, rules.furniture_markers);
    w.add(k_strs, strings.bytes());
    return w.finish();
}

Rules read(std::span<const std::byte> file) {
    container::Reader r(file, k_kind);
    if (r.version() < 1 || r.version() > k_version) {
        throw ParseError("unsupported .osr version " + std::to_string(r.version()));
    }
    container::StringTable strings(r.require(k_strs));
    auto str = [&](uint32_t offset) { return std::string(strings.get(offset)); };
    Rules rules;
    for (const auto& s : r.read_array<SettingRecord>(k_sets)) {
        rules.settings.push_back({str(s.name), s.value});
    }
    for (const auto& m : r.read_array<MovementRecord>(k_movt)) {
        MovementType t;
        t.form_id = m.form_id;
        t.edid = str(m.edid);
        t.name = str(m.name);
        const float* s = m.speeds;
        t.left[0] = s[0];
        t.left[1] = s[1];
        t.right[0] = s[2];
        t.right[1] = s[3];
        t.forward[0] = s[4];
        t.forward[1] = s[5];
        t.back[0] = s[6];
        t.back[1] = s[7];
        t.rotate_in_place[0] = s[8];
        t.rotate_in_place[1] = s[9];
        t.rotate_while_moving = s[10];
        std::copy(std::begin(m.thresholds), std::end(m.thresholds), t.thresholds);
        rules.movement_types.push_back(std::move(t));
    }
    for (const auto& x : r.read_array<RaceRecord>(k_race)) {
        Race race;
        race.form_id = x.form_id;
        race.edid = str(x.edid);
        std::copy(std::begin(x.height), std::end(x.height), race.height);
        std::copy(std::begin(x.weight), std::end(x.weight), race.weight);
        race.base_mass = x.base_mass;
        race.acceleration_rate = x.acceleration_rate;
        race.deceleration_rate = x.deceleration_rate;
        race.angular_acceleration_rate = x.angular_acceleration_rate;
        race.angular_tolerance = x.angular_tolerance;
        race.size = x.size;
        race.flags = x.flags;
        std::copy(std::begin(x.movement_types), std::end(x.movement_types), race.movement_types.begin());
        race.skeleton[0] = str(x.skeleton[0]);
        race.skeleton[1] = str(x.skeleton[1]);
        rules.races.push_back(std::move(race));
    }
    if (r.version() >= 6) {
        const auto bounds = r.read_array<RaceBoundRecord>(k_rbnd);
        for (size_t i = 0; i < std::min(bounds.size(), rules.races.size()); ++i) {
            std::memcpy(rules.races[i].bound_centre, bounds[i].centre, sizeof bounds[i].centre);
            std::memcpy(rules.races[i].bound_half, bounds[i].half, sizeof bounds[i].half);
        }
    }
    PlayerRecord p{};
    if (r.version() >= 3) {
        p = r.read_struct<PlayerRecord>(k_plyr);
    } else {
        static_cast<PlayerRecordV2&>(p) = r.read_struct<PlayerRecordV2>(k_plyr);
    }
    rules.player.camera3_height = p.camera3_height;
    rules.player.npc = p.npc;
    rules.player.race = p.race;
    rules.player.npc_height = p.npc_height;
    rules.player.female = p.female != 0;
    rules.player.eye_height = p.eye_height;
    std::copy(std::begin(p.bound_centre), std::end(p.bound_centre), rules.player.bound_centre);
    std::copy(std::begin(p.bound_half), std::end(p.bound_half), rules.player.bound_half);
    for (const auto& k : r.read_array<KeyRecord>(k_keys)) {
        KeyBinding b;
        b.event = str(k.event);
        b.context = k.context;
        b.keys = {k.keys[0], k.keys[1]};
        b.mouse = k.mouse;
        b.gamepad = k.gamepad;
        rules.keys.push_back(std::move(b));
    }
    if (r.find(k_coll)) {
        for (const auto& x : r.read_array<CollisionRecord>(k_coll)) {
            rules.collision_layers.push_back({x.index, str(x.edid), x.flags, x.collides});
        }
    }
    if (r.find(k_stai)) {
        rules.stairs_materials = r.read_array<uint32_t>(k_stai);
    }
    if (r.find(k_shak)) {
        rules.camera_shake = r.read_array<std::array<float, 3>>(k_shak);
    }
    if (r.find(k_furn)) {
        rules.furniture_markers = r.read_array<FurnitureMarker>(k_furn);
    }
    return rules;
}

} // namespace spud::osr
