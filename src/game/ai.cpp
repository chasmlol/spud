#include "game/ai.hpp"
#include "core/path.hpp"

#include "core/parallel.hpp"
#include "core/trace.hpp"

#include "core/game_random.hpp"
#include "core/log.hpp"
#include "game/game.hpp"

#include <algorithm>
#include <cstdio>
#include <format>
#include <bit>
#include <cmath>
#include <numbers>

namespace spud::game {

namespace {

using forms::Form;
using forms::Value;

constexpr uint32_t k_default_list = 0x00021E81; // DefaultMasterPackageList, default object "Default Pack List"
constexpr uint32_t k_flag_must_complete = 0x4, k_flag_preferred_speed = 0x2000;
constexpr uint32_t k_flag_unlock_at_start = 0x40, k_flag_unlock_at_end = 0x80;
constexpr uint16_t k_template_ai_packages = 0x0020; // ACBS template flags, bit 5 Use AI Packages (xEdit; bit 4 is AI Data)
constexpr uint16_t k_template_ai_data = 0x0010;
constexpr uint8_t k_allow_eating = 0x1, k_allow_sleeping = 0x2, k_allow_idle_markers = 0x8, k_allow_sitting = 0x10, k_allow_wandering = 0x20;
constexpr float k_arrive_units = 30.0f;  // near enough to a corner to take the next
constexpr size_t k_think_per_frame = 48; // actors off the attached cells looked at each frame
constexpr size_t k_searches = 4;         // path searches running at once

uint32_t form_of(const Value* v) { return v && v->kind == Value::Kind::form ? v->form : 0; }
int64_t integer(const Value* v, int64_t fallback = 0) { return v && v->kind == Value::Kind::integer ? v->integer : fallback; }

float flat(const NavPoint& a, const NavPoint& b) {
    return std::hypot(a[0] - b[0], a[1] - b[1]);
}

// The procedure a template's tree comes down to, for what this file does with an actor.
AiMode mode_of(std::string_view template_id) {
    if (template_id.starts_with("Sandbox") || template_id == "Eat") return AiMode::sandbox;
    if (template_id == "Sleep") return AiMode::sleep;
    if (template_id.starts_with("Patrol")) return AiMode::patrol;
    if (template_id == "Follow" || template_id == "Escort" || template_id == "FollowerPackageTemplate") return AiMode::follow;
    if (template_id.starts_with("ForceGreet")) return AiMode::force_greet;
    if (template_id == "Travel") return AiMode::travel;
    if (template_id == "Sit" || template_id == "SitTarget" || template_id == "UseIdleMarker") return AiMode::sit;
    if (template_id.starts_with("Orbit") || template_id == "DLC2OrbitAndHover") return AiMode::orbit;
    if (template_id.starts_with("Hover")) return AiMode::hover;
    return AiMode::stay;
}

} // namespace

Ai::Ai(Game& game) : m_game(game) {
    const forms::Database& db = game.db();
    for (const forms::FormEntry* e : db.of_type(forms::parse_sig("MOVT").value())) {
        if (db.editor_id(*e) == "Dragon_Flying_MT") {
            const Form f(db, *e);
            if (const Value* s = f.get("SPED")) {
                const Value* walk = s->member("forward_walk");
                const Value* run = s->member("forward_run");
                m_fly_walk_speed = walk ? float(walk->real) : m_fly_walk_speed;
                m_fly_run_speed = run ? float(run->real) : m_fly_run_speed;
            }
        }
        if (db.editor_id(*e) == "NPC_Default_MT") {
            const Form f(db, *e);
            if (const Value* s = f.get("SPED")) {
                const auto real = [s](const char* n) {
                    const Value* v = s->member(n);
                    return v ? float(v->real) : 0.0f;
                };
                m_walk_speed = real("forward_walk");
                m_run_speed = real("forward_run");
                // SPED's rotation is in radians a second; pathing scales it by fAITurnSpeedScale.
                m_turn_speed = real("rotate_in_place_walk") * game.settings().number("fAITurnSpeedScale", 1.5f);
            }
        }
    }
    m_door_index = std::async(std::launch::async, [this] { build_doors(); });
}

Ai::~Ai() {
    if (m_door_index.valid()) {
        m_door_index.wait();
    }
    for (Search& s : m_searches) {
        s.result.wait();
    }
}

void Ai::add(uint32_t ref, uint32_t npc, uint32_t space, const NavPoint& position, float heading, float scale) {
    Agent a;
    a.ref = ref;
    a.npc = npc;
    a.space = a.home_space = space;
    a.position = a.home = position;
    a.heading = heading;
    a.scale = scale;
    m_agents.emplace(ref, std::move(a));
    m_order.push_back(ref);
    ++m_roster;
}

const AiPackage* Ai::package(uint32_t form) {
    if (const auto it = m_packages.find(form); it != m_packages.end()) {
        return it->second.get();
    }
    std::unique_ptr<AiPackage> out;
    const forms::Database& db = m_game.db();
    if (const forms::FormEntry* e = db.find(form); e && e->type == forms::parse_sig("PACK").value()) {
        const Form f(db, *e);
        out = std::make_unique<AiPackage>();
        AiPackage& p = *out;
        p.form = form;
        if (const Value* d = f.get("PKDT")) {
            p.flags = uint32_t(integer(d->member("general_flags")));
            p.interrupts = uint16_t(integer(d->member("interrupt_flags")));
            p.preferred_speed = uint8_t(integer(d->member("preferred_speed")));
        }
        if (const Value* s = f.get("PSDT")) {
            p.month = int8_t(integer(s->member("month"), -1));
            p.day_of_week = int8_t(integer(s->member("day_of_week"), -1));
            p.date = int8_t(integer(s->member("date")));
            p.hour = int8_t(integer(s->member("hour"), -1));
            p.minute = int8_t(integer(s->member("minute"), -1));
            p.duration_minutes = uint32_t(integer(s->member("duration_minutes")));
        }
        const auto conditions = [&p](const Form& source) {
            for (const Value* c : source.all("condition")) { // not the tree's branch conditions
                p.conditions.push_back(condition_item(*c));
            }
        };
        conditions(f);
        // Data inputs by index: the template's values, then the instance's. Only these types carry a value in the list.
        std::unordered_map<int64_t, double> inputs;
        const auto read_inputs = [&inputs](const Form& source) {
            const auto types = source.all("value_type"), indices = source.all("value_index"), values = source.all("value");
            size_t at = 0;
            for (size_t i = 0; i < types.size() && i < indices.size(); ++i) {
                const std::string& t = types[i]->text;
                if (t == "Bool" || t == "Float" || t == "Int" || t == "ObjectList") {
                    if (at < values.size()) {
                        const Value* v = values[at++];
                        inputs[indices[i]->integer] = v->kind == Value::Kind::real ? v->real : double(v->integer);
                    }
                }
            }
        };
        const auto sandbox_inputs = [&p, &inputs](const Form& tree) {
            if (p.mode != AiMode::sandbox) {
                return;
            }
            const auto names = tree.all("data_input_name"), at = tree.all("data_input_index");
            for (size_t i = 0; i < names.size() && i < at.size(); ++i) {
                const auto value = inputs.find(at[i]->integer);
                if (value == inputs.end()) {
                    continue;
                }
                const std::string& name = names[i]->text;
                const uint8_t bit = name == "Allow Eating"         ? k_allow_eating
                                    : name == "Allow Sleeping"     ? k_allow_sleeping
                                    : name == "Allow Idle Markers" ? k_allow_idle_markers
                                    : name == "Allow Sitting"      ? k_allow_sitting
                                    : name == "Allow Wandering"    ? k_allow_wandering
                                                                   : uint8_t(0);
                if (bit) {
                    p.allow = value->second != 0.0 ? p.allow | bit : p.allow & uint8_t(~bit);
                } else if (name == "Energy") {
                    p.energy = float(value->second);
                }
            }
        };
        const Value* counter = f.get("PKCU");
        if (const forms::FormEntry* t = counter ? db.find(form_of(counter->member("package_template"))) : nullptr) {
            const Form tf(db, *t);
            conditions(tf);
            p.mode = mode_of(tf.editor_id());
            p.follower = tf.editor_id() == "FollowerPackageTemplate";
            p.idle_marker = tf.editor_id() == "UseIdleMarker";
            read_inputs(tf);
            read_inputs(f);
            sandbox_inputs(tf);
            const auto names = tf.all("data_input_name"), at = tf.all("data_input_index");
            for (size_t i = 0; i < names.size() && i < at.size(); ++i) {
                const auto value = inputs.find(at[i]->integer);
                if (value == inputs.end()) {
                    continue;
                }
                if (names[i]->text == "Repeatable?") {
                    p.patrol_repeats = value->second != 0.0;
                } else if (path::to_lower(names[i]->text) == "ride horse if possible?") {
                    p.ride_horse = value->second != 0.0;
                }
            }
            if (tf.editor_id().starts_with("DefaultMaster")) {
                // Its nine branches come down, for anyone without a linked patrol, to the last ones: a sandbox at
                // the editor location, radius 512 (section 2.3).
                p.mode = AiMode::sandbox;
                p.location_type = 3;
                p.location_radius = 512;
                p.default_master = true;
            }
        } else if (const auto procedures = f.all("procedure_type"); !procedures.empty()) {
            // Its own tree, as the default creature sandbox has: what it ends in.
            p.mode = mode_of(procedures.back()->text);
            read_inputs(f);
            sandbox_inputs(f);
        }
        for (const Value* v : f.all("value")) {
            if (v->kind == Value::Kind::real) {
                p.floats.push_back(float(v->real));
            }
        }
        // The first location and target among the data inputs (section 2).
        for (const Value* l : f.all("PLDT")) {
            if (p.default_master) {
                break;
            }
            p.location_type = int32_t(integer(l->member("type")));
            const Value* where = l->member("location");
            if (where && where->kind == Value::Kind::form) {
                p.location_form = where->form;
            } else if (where && where->kind == Value::Kind::object) {
                p.location_form = form_of(where->member("reference"));
            }
            p.location_radius = float(integer(l->member("radius")));
            break;
        }
        for (const Value* t : f.all("PDTO")) {
            if (integer(t->member("type")) == 0) {
                p.topic = form_of(t->member("data"));
                break;
            }
        }
        for (const Value* t : f.all("PTDA")) {
            if (integer(t->member("type")) == 0) {
                const Value* who = t->member("target");
                p.target = who && who->kind == Value::Kind::object ? form_of(who->member("reference")) : form_of(who);
            }
            p.target_linked = integer(t->member("type")) == 3;
            break;
        }
    }
    return m_packages.emplace(form, std::move(out)).first->second.get();
}

// The NPC_ giving its AI packages: through a template with the flag.
const std::vector<uint32_t>& Ai::packages_of(uint32_t npc) {
    if (const auto it = m_lists.find(npc); it != m_lists.end()) {
        return it->second;
    }
    const forms::Database& db = m_game.db();
    uint32_t source = npc;
    std::vector<uint32_t> list;
    for (int depth = 0; depth < 16; ++depth) {
        const forms::FormEntry* e = db.find(source);
        if (!e || e->type != forms::parse_sig("NPC_").value()) {
            break;
        }
        const Form f(db, *e);
        const Value* acbs = f.get("ACBS");
        const uint32_t next = form_of(f.get("TPLT"));
        if (next && acbs && (integer(acbs->member("template_flags")) & k_template_ai_packages)) {
            source = next;
            continue;
        }
        for (const Value* v : f.all("PKID")) {
            list.push_back(v->form);
        }
        break;
    }
    return m_lists.emplace(npc, std::move(list)).first->second;
}

const std::vector<uint32_t>& Ai::defaults_of(uint32_t npc) {
    if (const auto it = m_default_lists.find(npc); it != m_default_lists.end()) {
        return it->second;
    }
    const forms::Database& db = m_game.db();
    uint32_t list_form = k_default_list;
    if (const forms::FormEntry* e = db.find(npc)) {
        if (const uint32_t own = form_of(Form(db, *e).get("DPLT"))) {
            list_form = own;
        }
    }
    std::vector<uint32_t> list;
    if (const forms::FormEntry* l = db.find(list_form)) {
        list = forms::form_list(Form(db, *l));
    }
    return m_default_lists.emplace(npc, std::move(list)).first->second;
}

// The game.
bool Ai::qualifies(const Agent& a, const AiPackage& p, const AiClock& c) {
    if (p.month >= 0 && p.month != c.month) {
        return false;
    }
    if (p.date > 0 && p.date != c.date) {
        return false;
    }
    if (p.day_of_week >= 0) {
        const int d = c.day_of_week;
        const bool day = p.day_of_week <= 6   ? d == p.day_of_week
                         : p.day_of_week == 7 ? d >= 1 && d <= 5
                         : p.day_of_week == 8 ? d == 0 || d == 6
                         : p.day_of_week == 9 ? d == 1 || d == 3 || d == 5
                                              : d == 2 || d == 4;
        if (!day) {
            return false;
        }
    }
    if (p.hour >= 0) {
        const float start = float(p.hour) + float(std::max<int>(p.minute, 0)) / 60.0f;
        const float end = start + float(p.duration_minutes) / 60.0f;
        const bool in = end > 24.0f ? c.hour > start || c.hour < end - 24.0f : c.hour == start || (start < c.hour && c.hour < end);
        if (!in) {
            return false;
        }
    }
    if (!p.conditions.empty()) {
        return m_game.conditions().evaluate(p.conditions, a.ref, p.target);
    }
    return true;
}

// The game: quest aliases', the NPC's list, then the default list; nothing keeps what they have.
const AiPackage* Ai::select(const Agent& a, const AiClock& clock) {
    if (const auto forced = m_forced.find(a.ref); forced != m_forced.end()) {
        return package(forced->second);
    }
    if (alias_packages) {
        for (const uint32_t form : alias_packages(a.ref)) {
            const AiPackage* p = package(form);
            if (p && qualifies(a, *p, clock)) {
                return p;
            }
        }
    }
    for (const std::vector<uint32_t>* list : {&packages_of(a.npc), &defaults_of(a.npc)}) {
        for (const uint32_t form : *list) {
            const AiPackage* p = package(form);
            if (p && qualifies(a, *p, clock)) {
                return p;
            }
        }
    }
    return a.package;
}

bool Ai::reference_location(uint32_t ref, AiLocation& out) {
    const forms::Database& db = m_game.db();
    const forms::FormEntry* e = ref ? db.find(ref) : nullptr;
    if (!e) {
        return false;
    }
    const Form f(db, *e);
    const auto placed = forms::placement(f);
    if (!placed) {
        return false;
    }
    out.point = {placed->position[0], placed->position[1], placed->position[2]};
    out.space = m_game.space_of_cell(e->parent);
    return true;
}

// A reference's arrival radius: at least 50 for a marker, 150 for furniture, 100 for anything else.
float Ai::arrival_radius(uint32_t ref, float radius) {
    const forms::Database& db = m_game.db();
    const forms::FormEntry* e = db.find(ref);
    const forms::FormEntry* base = e ? db.find(m_game.base_of(ref)) : nullptr;
    const Settings& s = m_game.settings();
    float floor = s.number("fAIDistanceRadiusMinLocation", 100.0f);
    if (base && base->type == forms::parse_sig("FURN").value()) {
        floor = s.number("fAIFurnitureDestinationRadius", 150.0f);
    } else if (base && (base->form_id == 0x3B || base->form_id == 0x34)) { // XMarker, XMarkerHeading
        floor = s.number("fAIMarkerDestinationRadius", 50.0f);
    }
    return std::max(radius, floor);
}

uint32_t Ai::linked_ref(uint32_t ref) {
    const forms::Database& db = m_game.db();
    const forms::FormEntry* e = db.find(ref);
    if (!e) {
        return 0;
    }
    const Form f(db, *e);
    for (const Value* v : f.all("XLKR")) {
        if (!form_of(v->member("keyword"))) {
            return form_of(v->member("reference"));
        }
    }
    return 0;
}

AiLocation Ai::resolve(const Agent& a, const AiPackage& p) {
    AiLocation l;
    l.radius = p.location_radius;
    switch (p.location_type) {
    case 0: // near a reference
        if (reference_location(p.location_form, l)) {
            l.radius = arrival_radius(p.location_form, l.radius);
        }
        break;
    case 6: // near the actor's linked reference
        if (const uint32_t linked = linked_ref(a.ref); reference_location(linked, l)) {
            l.radius = arrival_radius(linked, l.radius);
        }
        break;
    case 1: {                                     // in a cell: its middle, as its navmesh has it
        l.space = m_game.space_of_cell(p.location_form);
        NavPoint middle{};
        if (m_game.navmeshes().middle(p.location_form, middle)) {
            l.point = middle;
            l.radius = std::max(l.radius, 300.0f);
        } else {
            l.space = 0;
        }
        break;
    }
    case 3: // near the editor location
        l.space = a.home_space;
        l.point = a.home;
        break;
    case 2: // near where the package started
        l.space = a.space;
        l.point = a.position;
        break;
    default: // near self, and an empty alias: already there
        l.space = a.space;
        l.point = a.position;
        break;
    }
    if (!l.valid()) {
        l.space = a.home_space;
        l.point = a.home;
    }
    return l;
}

// The game for those off the attached cells: they are simply there.
void Ai::place(Agent& a) {
    if (!a.location.valid()) {
        return;
    }
    change_space(a, a.location.space, a.location.point, a.heading);
    a.path.clear();
    a.next = 0;
    a.spot = {};
}

std::vector<uint32_t> Ai::followers(uint32_t leader) const {
    std::vector<uint32_t> out;
    for (const auto& [ref, a] : m_agents) {
        if (a.package && a.package->mode == AiMode::follow && a.follows == leader) {
            out.push_back(ref);
        }
    }
    return out;
}

void Ai::set_disabled(uint32_t ref, bool disabled) {
    if (Agent* a = mutable_agent(ref); a && a->disabled != disabled) {
        a->disabled = disabled;
        ++m_roster;
    }
}

void Ai::dump(const std::string& file) const {
    std::vector<std::string> lines;
    for (const auto& [ref, a] : m_agents) {
        if (a.high || a.fighting) {
            lines.push_back(std::format("{:08X} {:08X} {:.3f} {:.3f} {:.3f} {:.4f} p{:08X} act{} high{} fight{}\n", ref, a.space, a.position[0],
                                        a.position[1], a.position[2], a.heading, a.package ? a.package->form : 0, int(a.activity),
                                        int(a.high), int(a.fighting)));
        }
    }
    std::ranges::sort(lines);
    if (FILE* f = std::fopen(file.c_str(), "wb")) {
        for (const std::string& l : lines) {
            std::fwrite(l.data(), 1, l.size(), f);
        }
        std::fclose(f);
    }
}

uint64_t Ai::digest() const {
    // Summed, so the map's order doesn't count.
    uint64_t sum = 0;
    for (const auto& [ref, a] : m_agents) {
        uint64_t h = 0xCBF29CE484222325ull ^ ref;
        const auto mix = [&h](uint32_t v) { h = (h ^ v) * 0x100000001B3ull; };
        for (const float v : {a.position[0], a.position[1], a.position[2], a.heading}) {
            mix(std::bit_cast<uint32_t>(v));
        }
        mix(a.space);
        mix(a.package ? a.package->form : 0);
        mix(uint32_t(a.activity));
        mix(uint32_t(a.high) | uint32_t(a.fighting) << 1);
        sum += h;
    }
    return sum;
}

void Ai::place_at(uint32_t ref, uint32_t space, const NavPoint& position, float heading) {
    if (Agent* a = mutable_agent(ref)) {
        change_space(*a, space, position, heading);
        a->path.clear();
        a->next = 0;
        a->script_placed = !a->high;
    }
}

void Ai::change_space(Agent& a, uint32_t space, const NavPoint& position, float heading) {
    const bool moved = a.space != space;
    a.space = space;
    a.position = position;
    a.heading = heading;
    if (moved) {
        ++m_roster;
    }
}

// Package flags 0x40 and 0x80 (1.4), and Sleep's LockDoors: the doors within 5000 units the actor may use, and each door's
// other side. Shops open and close by these.
void Ai::set_doors_locked(const Agent& a, bool locked) {
    if (!m_doors_ready) {
        return;
    }
    for (const auto& [ref, d] : m_doors) {
        if (d.space != a.space || flat(a.position, d.position) > 5000.0f || !m_game.owner_of(ref) || !may_use(a, ref)) {
            continue;
        }
        for (const uint32_t door : {ref, d.destination}) {
            if (!door) {
                continue;
            }
            if (locked) {
                m_game.relock(door);
            } else {
                m_game.unlock(door);
            }
        }
    }
}

void Ai::start(Agent& a, const AiPackage* p) {
    if (p == a.package) {
        return;
    }
    if ((a.package && (a.package->flags & k_flag_unlock_at_end)) || (p && (p->flags & k_flag_unlock_at_start))) {
        set_doors_locked(a, false);
    }
    if (p && p->mode == AiMode::sleep) {
        set_doors_locked(a, true); // the template's Lock Doors input isn't read: most of the game's sleep packages set it
    }
    if (a.high && p != a.package) { // OnPackageChange and OnPackageEnd on the old one, OnPackageStart on the new
        if (a.package) {
            m_game.script_event(a.ref, "OnPackageChange", {{a.package->form, "Package"}});
            m_game.script_event(a.ref, "OnPackageEnd", {{a.package->form, "Package"}});
        }
        if (p) {
            m_game.script_event(a.ref, "OnPackageStart", {{p->form, "Package"}});
        }
    }
    a.package = p;
    a.location = p ? resolve(a, *p) : AiLocation{};
    if (p && p->mode == AiMode::sit && p->location_type < 0) { // SitTarget, UseIdleMarker: the target is the place
        AiLocation at;
        if (reference_location(p->target_linked ? linked_ref(a.ref) : p->target, at)) {
            a.location = at;
        }
    }
    leave_use(a);
    a.path.clear();
    a.next = 0;
    a.leg_door = 0;
    a.rest = 0;
    a.follows = p && p->mode == AiMode::follow ? (p->target ? p->target : k_player_ref)
                : p && p->mode == AiMode::force_greet ? k_player_ref
                                                      : 0;
    a.patrol = p && p->mode == AiMode::patrol ? (p->target ? p->target : linked_ref(a.ref)) : 0; // the input, or the ref's own link
    a.rides = 0;
    if (const forms::FormEntry* e = p && p->ride_horse ? m_game.db().find(a.ref) : nullptr) {
        a.rides = form_of(Form(m_game.db(), *e).get("XHOR"));
    }
    // Without the package's Preferred Speed flag an actor walks: levels 1 walk, 1.15 fast walk, 1.65 jog, 2
    // run, between the movement type's walk and run.
    float level = 1.0f;
    if (p && (p->flags & k_flag_preferred_speed)) {
        const Settings& s = m_game.settings();
        level = p->preferred_speed == 1   ? 1.0f + s.number("fJogInterpolationBetweenWalkAndRun", 0.65f)
                : p->preferred_speed == 2 ? 2.0f
                : p->preferred_speed == 3 ? 1.0f + s.number("fFastWalkInterpolationBetweenWalkAndRun", 0.15f)
                                          : 1.0f;
    }
    a.speed = m_walk_speed + (m_run_speed - m_walk_speed) * (level - 1.0f);
    if (!a.high && !a.follows && !a.script_placed) {
        place(a);
    }
}

// Follow: a path to the leader when outside the radius, running when well behind.
void Ai::follow(Agent& a) {
    NavPoint leader = m_player;
    uint32_t space = m_player_space;
    if (a.follows != k_player_ref) {
        const Agent* other = agent(a.follows);
        if (!other) {
            return;
        }
        leader = other->position;
        space = other->space;
    }
    if (a.follows == k_player_ref && std::ranges::find(m_player_ground, a.space) != m_player_ground.end()) {
        space = a.space; // a merged city's actors walk the same ground as the player
    }
    const Settings& s = m_game.settings();
    float radius = a.stop_distance;
    if (a.package && a.package->mode == AiMode::follow && !a.fighting) {
        // each stands further back by those ahead of it, so they queue.
        radius = a.package->floats.empty() ? s.number("fFollowSlowdownZoneWidth", 150.0f) : a.package->floats[0];
        const float own = flat(a.position, leader);
        float behind = 0.0f;
        for (const uint32_t ref : followers(a.follows)) {
            const Agent* other = agent(ref);
            if (other && ref != a.ref && flat(other->position, leader) < own) {
                behind += radius + s.number("fFollowSpaceBetweenFollowers", 192.0f);
            }
        }
        radius += behind;
        a.stop_distance = radius;
    }
    a.location = {space, leader, radius};
    const float away = flat(a.position, leader);
    // and past the radius the game's top: vanilla's min and max are equal.
    if (a.fighting) {
        a.speed = m_run_speed;
    } else if (away < radius) {
        a.speed = m_walk_speed;
    } else if (away > radius + s.number("fFollowMatchSpeedZoneWidth", 128.0f) || (a.follows == k_player_ref && m_player_running)) {
        a.speed = m_run_speed * s.number("fFollowRunMaxSpeedupMultiplier", 1.25f);
    }
    // The leader moved on from where the path aimed.
    if (!a.path.empty() && flat(a.path.back(), leader) > s.number("fAIMoveDistanceToRecalcFollowPath", 300.0f)) {
        a.path.clear();
        a.next = 0;
    }
}

void Ai::think(Agent& a, const AiClock& clock) {
    if (a.fighting || a.talk_partner) {
        return; // the combat package holds it, or the talk's
    }
    if (a.commander) {
        a.package = nullptr;
        a.follows = a.commander;
        a.evaluate_in = m_game.settings().number("fEvaluatePackageTimer", 15.0f);
        return;
    }
    // Must Complete holds until the trip is done.
    const bool busy = a.package && (a.package->flags & k_flag_must_complete) && (!a.path.empty() || a.searching);
    if (!busy) {
        start(a, select(a, clock));
    }
    a.last_hour = int(clock.hour);
    a.evaluate_in = m_game.settings().number("fEvaluatePackageTimer", 15.0f);
}

void Ai::evaluate(uint32_t ref) {
    if (const auto it = m_agents.find(ref); it != m_agents.end()) {
        it->second.evaluate_in = 0;
    }
}

void Ai::hold(uint32_t ref, bool held) {
    if (held) {
        m_held.insert(ref);
    } else {
        m_held.erase(ref);
    }
}

// The doors between spaces, from the references with teleport data.
void Ai::build_doors() {
    const forms::Database& db = m_game.db();
    const forms::Sig xtel = forms::parse_sig("XTEL").value();
    std::vector<Door> doors;
    for (const forms::FormEntry* e : db.of_type(forms::parse_sig("REFR").value())) {
        if (!db.field(*e, xtel)) {
            continue;
        }
        const Form f(db, *e);
        const Value* t = f.get("XTEL");
        const auto placed = forms::placement(f);
        if (!t || !placed) {
            continue;
        }
        Door d;
        d.ref = e->form_id;
        d.destination = form_of(t->member("door"));
        d.space = m_game.space_of_cell(e->parent);
        d.position = {placed->position[0], placed->position[1], placed->position[2]};
        if (const Value* p = t->member("position")) {
            const auto real = [p](const char* n) {
                const Value* v = p->member(n);
                return v ? float(v->real) : 0.0f;
            };
            d.arrival = {real("x"), real("y"), real("z")};
        }
        if (const Value* r = t->member("rotation")) {
            const Value* z = r->member("z");
            d.arrival_heading = z ? float(z->real) : 0.0f;
        }
        const forms::FormEntry* other = db.find(d.destination);
        d.destination_space = other ? m_game.space_of_cell(other->parent) : 0;
        if (d.space && d.destination_space) {
            doors.push_back(d);
        }
    }
    for (const Door& d : doors) {
        m_doors[d.ref] = d;
        m_doors_of_space[d.space].push_back(d.ref);
    }
    log::info("ai: {} load doors", m_doors.size());
    m_doors_ready = true;
}

// The first door on the way from one space to another: a breadth-first search over the doors.
uint32_t Ai::door_towards(uint32_t from, uint32_t to) const {
    if (!m_doors_ready) {
        return 0;
    }
    std::unordered_map<uint32_t, uint32_t> first; // space -> the door out of `from` that reached it
    std::deque<uint32_t> open{from};
    first[from] = 0;
    while (!open.empty()) {
        const uint32_t space = open.front();
        open.pop_front();
        const auto it = m_doors_of_space.find(space);
        if (it == m_doors_of_space.end()) {
            continue;
        }
        for (const uint32_t ref : it->second) {
            const Door& d = m_doors.at(ref);
            if (first.contains(d.destination_space)) {
                continue;
            }
            first[d.destination_space] = first[space] ? first[space] : ref;
            if (d.destination_space == to) {
                return first[to];
            }
            open.push_back(d.destination_space);
        }
    }
    return 0;
}

void Ai::request_path(Agent& a, const NavPoint& to) {
    // The far side of the world never takes the last of them from those in sight.
    if (a.searching || m_searches.size() >= (a.high ? k_searches : k_searches / 2) || m_search_clock < a.no_path_until) {
        return;
    }
    a.searching = true;
    NavMeshes* nav = &m_game.navmeshes();
    const uint32_t space = a.space;
    const NavPoint from = a.position;
    m_searches.push_back({a.ref, std::async(std::launch::async, [nav, space, from, to] {
                              const auto start = nav->locate(space, from, 512.0f), end = nav->locate(space, to, 512.0f);
                              return start && end ? nav->path(*start, *end) : std::vector<NavPoint>{};
                          })});
}

void Ai::walk(Agent& a, float dt) {
    if (a.crash_left >= 0.0f && (a.crash_left -= dt) < 0.0f) {
        a.crashing = true;
        a.fly = FlyState::none;
        a.allow_flying = false;
        a.fall_speed = 0.0f;
    }
    if (a.fall_speed >= 0.0f) {
        constexpr float k_gravity = 9.81f / 0.0142875f; // units a second squared
        a.fall_speed += k_gravity * dt;
        const float ground = ground_height ? ground_height(a.position[0], a.position[1]) : a.position[2];
        a.position[2] -= a.fall_speed * dt;
        if (a.position[2] <= ground) {
            a.position[2] = ground;
            a.fall_speed = -1.0f;
            if (a.crashing) {
                a.crashing = false;
                m_game.combat().end_deferred_kill(a.ref);
            }
        }
        return;
    }
    if (a.flier && (a.fly != FlyState::none || wants_air(a))) {
        fly(a, dt);
        return;
    }
    if (a.faces_player) {
        const float turn = std::remainder(std::atan2(m_player[0] - a.position[0], m_player[1] - a.position[1]) - a.heading,
                                          2.0f * std::numbers::pi_v<float>);
        const float most = m_turn_speed * dt;
        a.heading += std::clamp(turn, -most, most);
        a.faces_player = a.saying || std::abs(turn) > most;
    }
    if (m_held.contains(a.ref) || a.searching || a.saying) {
        return;
    }
    if (a.use == UseState::leaving) {
        a.leaving -= dt;
        if (a.leaving <= 0.0f) {
            a.use = UseState::none;
        }
        return;
    }
    if (a.next >= a.path.size()) {
        a.path.clear();
        a.next = 0;
        // Through the door the last leg led to.
        if (a.leg_door) {
            const Door& d = m_doors.at(a.leg_door);
            const bool to_high = false;
            (void)to_high;
            change_space(a, d.destination_space, d.arrival, d.arrival_heading);
            a.leg_door = 0;
            a.spot = {};
            return;
        }
        if (!a.location.valid()) {
            return;
        }
        if (a.location.space != a.space) {
            const uint32_t door = door_towards(a.space, a.location.space);
            if (!door) {
                if (m_doors_ready) {
                    place(a); // no way there on foot: the game moves them too (task 83)
                }
                return;
            }
            a.leg_door = door;
            request_path(a, m_doors.at(door).position);
            return;
        }
        const float away = flat(a.position, a.location.point);
        const bool roams = a.package && (a.package->mode == AiMode::sandbox || a.package->mode == AiMode::sleep);
        if (a.patrol && !a.fighting &&
            away <= std::max(a.location.radius, k_arrive_units) + m_game.settings().number("fAIPatrolHysteresisWidth", 50.0f)) {
            // On to the next of the markers linked one after another.
            const uint32_t next = linked_ref(a.patrol);
            a.patrol = next ? next : a.package && a.package->patrol_repeats ? linked_ref(a.ref) : 0;
            AiLocation l;
            if (a.patrol && reference_location(a.patrol, l)) {
                l.radius = arrival_radius(a.patrol, 0);
                a.location = l;
            }
        }
        if (a.package && a.package->mode == AiMode::sit) {
            update_sit(a, dt, away);
            return;
        }
        // A sandbox lets them out of its radius by the hysteresis before it brings them back.
        const float reach = roams ? std::max(a.location.radius, 100.0f) + m_game.settings().number("fSandBoxRadiusHysteresis", 50.0f)
                                  : std::max(a.location.radius, k_arrive_units);
        if (away > reach) {
            request_path(a, a.location.point);
        } else if (roams) {
            // Section 4: an activity for its time, then the next.
            if (a.use == UseState::going) {
                arrive_at_use(a, false);
                return;
            }
            if (a.eat_in >= 0.0f && (a.eat_in -= dt) < 0.0f) {
                a.event = "ChairEatingStart"; // the idle's event, once the enter has played
            }
            a.rest -= dt;
            if (a.activity == Activity::wander && a.rest > 0.0f && (a.wander_wait -= dt) <= 0.0f) {
                wander_leg(a);
            }
            if (a.rest <= 0.0f) {
                leave_use(a);
                if (a.use == UseState::none) {
                    pick_activity(a, a.arrived_unseen);
                    a.arrived_unseen = false;
                }
            }
        }
        return;
    }
    const NavPoint goal = a.path[a.next];
    const float dx = goal[0] - a.position[0], dy = goal[1] - a.position[1];
    const float distance = std::hypot(dx, dy);
    const float step = a.speed * dt;
    if (distance <= std::max(step, 1.0f)) {
        a.position = goal;
        ++a.next;
        return;
    }
    const NavPoint move = steer(a, dx / distance, dy / distance, distance, step);
    check_stuck(a, dt, step);
    if (move[0] == 0.0f && move[1] == 0.0f) {
        return;
    }
    // Turning at the movement type's rate, not at once.
    const float want = std::atan2(move[0], move[1]);
    float turn = std::remainder(want - a.heading, 2.0f * std::numbers::pi_v<float>);
    const float most = m_turn_speed * dt;
    a.heading += std::clamp(turn, -most, most);
    const NavPoint before = a.position;
    a.position[0] += move[0];
    a.position[1] += move[1];
    if (!a.spot.valid()) {
        if (const auto s = m_game.navmeshes().locate(a.space, a.position, 256.0f)) {
            a.spot = *s;
        }
    }
    if (a.spot.valid() && m_game.navmeshes().walk(a.spot, a.position[0], a.position[1])) {
        a.position[2] = a.spot.position[2];
    } else {
        a.position = before; // steered off the navmesh: stays, and the stuck check rebuilds
        a.spot = {};
    }
}

// Local steering: the way ahead, or 45 degree steps from it, then half speed, the first that doesn't run into
// anyone within the prediction time. Everyone near counts: the player, and actors, moving or not.
NavPoint Ai::steer(const Agent& a, float dx, float dy, float distance, float step) {
    const Settings& s = m_game.settings();
    const float time = s.number("fMaxCollisionTime", 1.0f);
    const float reach = std::clamp(s.number("fAvoidanceTimeDelta", 3.0f) * a.speed, s.number("fMinAvoidanceRadius", 256.0f),
                                   s.number("fMaxAvoidanceRadius", 512.0f));
    constexpr float k_body = 40.0f; // two actors' bound radii, in units: 2 x 22 as NPC_ OBND, less a little contact
    struct Obstacle {
        float x, y, vx, vy;
    };
    std::vector<Obstacle> near;
    const auto consider = [&](const NavPoint& p, float vx, float vy) {
        if (flat(p, a.position) < reach && std::abs(p[2] - a.position[2]) < 128.0f && near.size() < 10) {
            near.push_back({p[0], p[1], vx, vy});
        }
    };
    if (a.space == m_player_space) {
        consider(m_player, 0, 0);
    }
    for (const uint32_t ref : m_high) {
        if (ref == a.ref) {
            continue;
        }
        const auto it = m_agents.find(ref);
        if (it == m_agents.end() || it->second.space != a.space) {
            continue;
        }
        const Agent& o = it->second;
        const bool moving = o.next < o.path.size();
        const float h = o.heading;
        consider(o.position, moving ? std::sin(h) * o.speed : 0.0f, moving ? std::cos(h) * o.speed : 0.0f);
    }
    if (near.empty()) {
        return {dx * step, dy * step, 0};
    }
    const auto clear = [&](float ux, float uy, float speed) {
        const float vx = ux * speed, vy = uy * speed;
        for (const Obstacle& o : near) {
            const float rx = o.x - a.position[0], ry = o.y - a.position[1];
            const float wx = vx - o.vx, wy = vy - o.vy; // closing velocity
            const float len = wx * wx + wy * wy;
            const float t = len > 0.0f ? std::clamp((rx * wx + ry * wy) / len, 0.0f, time) : 0.0f;
            const float mx = rx - wx * t, my = ry - wy * t;
            if (mx * mx + my * my < k_body * k_body && rx * wx + ry * wy > 0.0f) {
                return false;
            }
        }
        return true;
    };
    (void)distance;
    const float slow = s.number("fSlowDownMultiplier", 0.5f);
    for (const float speed : {a.speed, a.speed * slow}) {
        for (const float angle : {0.0f, 45.0f, -45.0f, 90.0f, -90.0f}) {
            const float r = angle * std::numbers::pi_v<float> / 180.0f;
            const float c = std::cos(r), n = std::sin(r);
            const float ux = dx * c - dy * n, uy = dx * n + dy * c;
            if (clear(ux, uy, speed)) {
                const float length = step * speed / std::max(a.speed, 1.0f);
                return {ux * length, uy * length, 0};
            }
        }
    }
    return {0, 0, 0}; // blocked: the stuck check takes over
}

// CheckStuck: over a second, hardly any of the wanted movement happened: a new path, and after three
// (iWarpMaxPathFailureCount) on to the next corner, as the game's warp does out of sight.
void Ai::check_stuck(Agent& a, float dt, float step) {
    const Settings& s = m_game.settings();
    if (a.window_time <= 0.0f) {
        a.window_from = a.position;
        a.wanted = 0;
    }
    a.window_time += dt;
    a.wanted += step;
    if (a.window_time < s.number("fTotalTimeThreshold", 1.0f)) {
        return;
    }
    const float moved = flat(a.position, a.window_from);
    a.window_time = 0;
    if (a.wanted <= 0.0f || moved >= a.wanted * (1.0f - s.number("fAcceptableErrorRatio", 0.9f))) {
        a.failures = 0;
        return;
    }
    if (++a.failures >= int(s.number("iWarpMaxPathFailureCount", 3))) {
        a.failures = 0;
        if (a.next < a.path.size()) {
            a.position = a.path[a.next++];
            a.spot = {};
        }
        return;
    }
    if (!a.path.empty()) {
        const NavPoint to = a.path.back();
        a.path.clear();
        a.next = 0;
        request_path(a, to);
    }
}

void Ai::set_furniture(uint32_t space, std::vector<AiFurniture> items) { m_furniture[space] = std::move(items); }

// The game for furniture: nobody's, theirs, or a faction's they are in.
bool Ai::may_use(const Agent& a, uint32_t ref) {
    const uint32_t owner = m_game.owner_of(ref);
    return !owner || owner == a.npc || m_game.faction_rank(a.ref, owner) >= 0;
}

float Ai::energy_of(const Agent& a) {
    if (a.package && a.package->energy >= 0.0f) {
        return a.package->energy;
    }
    if (const auto it = m_energy.find(a.npc); it != m_energy.end()) {
        return it->second;
    }
    const forms::Database& db = m_game.db();
    float energy = 50.0f;
    uint32_t source = a.npc;
    for (int depth = 0; depth < 16; ++depth) {
        const forms::FormEntry* e = db.find(source);
        if (!e || e->type != forms::parse_sig("NPC_").value()) {
            break;
        }
        const Form f(db, *e);
        const Value* acbs = f.get("ACBS");
        const uint32_t next = form_of(f.get("TPLT"));
        if (next && acbs && (integer(acbs->member("template_flags")) & k_template_ai_data)) {
            source = next;
            continue;
        }
        if (const Value* ai = f.get("AIDT")) {
            energy = float(integer(ai->member("energy_level"), 50));
        }
        break;
    }
    m_energy.emplace(a.npc, energy);
    return energy;
}

float Ai::activity_seconds(const Agent& a, Activity activity) {
    // Duration in game hours into real seconds at the time scale. Most of these settings live in the game, not
    // the plugins: the fallbacks are its values.
    struct Mults {
        const char* duration;
        float duration_value;
        const char* energy;
        float energy_value;
    };
    static constexpr Mults k_mults[6] = {
        {"fSandboxDurationMultSitting", 2.0f, "fSandboxEnergyMultSitting", -0.1f},
        {"fSandboxDurationMultSleeping", 2.0f, "fSandboxEnergyMultSleeping", -0.1f},
        {"fSandboxDurationMultEatStanding", 1.5f, "fSandboxEnergyMultEatStanding", -0.05f},
        {"fSandboxDurationMultEatSitting", 1.5f, "fSandboxEnergyMultEatSitting", -0.05f},
        {"fSandboxDurationMultWandering", 0.5f, "fSandboxEnergyMultWandering", 0.2f},
        {"fSandboxDurationMultIdleMarker", 1.5f, "fSandboxEnergyMultIdleMarker", -0.05f},
    };
    const Settings& s = m_game.settings();
    const Mults& m = k_mults[size_t(std::max(int(activity), 0))];
    const float base = (s.number("fSandboxEnergyMult", 0.05f) * s.number(m.energy, m.energy_value) * energy_of(a) + 1.0f) *
                       (s.number("fSandboxDurationBase", 0.2f) * s.number(m.duration, m.duration_value));
    const float spread = s.number("fSandboxDurationRangeMult", 0.25f);
    const float r = 1.0f + spread * (2.0f * float(GameRandom::shared().next_unit()) - 1.0f);
    return base * r * 3600.0f / m_time_scale;
}

// The tree of Sit, SitTarget and UseIdleMarker comes down to this: Find what to use, Travel to it, stay on it for the package.
const AiFurniture* Ai::sit_place(const Agent& a) {
    const auto list = m_furniture.find(a.space);
    if (list == m_furniture.end() || !a.package) {
        return nullptr;
    }
    const AiPackage& p = *a.package;
    const uint32_t want = p.target_linked ? linked_ref(a.ref) : p.target;
    const float reach = a.location.radius > 0 ? a.location.radius : m_game.settings().number("fSandBoxSearchRadius", 6000.0f);
    const AiFurniture* best = nullptr;
    float nearest = std::numeric_limits<float>::max();
    for (const AiFurniture& f : list->second) {
        if (m_reserved.contains(f.key())) {
            continue;
        }
        if (want) {
            if (f.ref == want) {
                return &f;
            }
            continue;
        }
        const bool kind = p.idle_marker ? f.kind == FurnitureKind::idle_marker : f.kind == FurnitureKind::chair;
        const float d = flat(f.position, a.location.point);
        if (kind && d <= reach && d < nearest && may_use(a, f.ref)) {
            best = &f;
            nearest = d;
        }
    }
    return best;
}

void Ai::update_sit(Agent& a, float dt, float away) {
    if (a.use == UseState::using_it) {
        return;
    }
    if (a.use == UseState::going) {
        arrive_at_use(a, false);
        return;
    }
    if ((a.rest -= dt) > 0.0f) {
        return;
    }
    if (const AiFurniture* f = sit_place(a)) {
        a.activity = f->kind == FurnitureKind::idle_marker ? Activity::idle_marker : Activity::sit;
        a.use_ref = f->ref;
        a.use_marker = f->marker;
        m_reserved[f->key()] = a.ref;
        a.use = UseState::going;
        if (a.arrived_unseen) {
            a.arrived_unseen = false;
            arrive_at_use(a, true);
        } else {
            request_path(a, f->position);
        }
        return;
    }
    // Nothing free: to the location, and look again as a sandbox's rescan does.
    if (away > std::max(a.location.radius, k_arrive_units)) {
        request_path(a, a.location.point);
    }
    const Settings& s = m_game.settings();
    const float low = s.number("fMinSandboxRescanSeconds", 3.0f), high = s.number("fMaxSandboxRescanSeconds", 6.0f);
    a.rest = low + (high - low) * float(GameRandom::shared().next_unit());
}

// One leg of Wander: somewhere in the location at least the minimum distance away, then a pause, then the next,
// until the activity ends.
void Ai::wander_leg(Agent& a) {
    const Settings& s = m_game.settings();
    GameRandom& random = GameRandom::shared();
    const float radius = std::max(a.location.radius, 100.0f), least = s.number("fAIWanderDefaultMinDist", 200.0f);
    NavPoint to = a.location.point;
    for (int tries = 0; tries < 8; ++tries) {
        const float angle = float(random.next_unit()) * 2.0f * std::numbers::pi_v<float>;
        const float out = radius * float(random.next_unit());
        to = {a.location.point[0] + std::sin(angle) * out, a.location.point[1] + std::cos(angle) * out, a.location.point[2]};
        if (flat(to, a.position) >= least) {
            break;
        }
    }
    request_path(a, to);
    const float most = (s.number("fAIMaxWanderTime", 100.0f) -
                        (energy_of(a) * s.number("fAIEngergyLevelMult", 1.0f) + s.number("fAIEnergyLevelBase", 0.0f))) * 0.5f;
    a.wander_wait = std::max(most, 0.0f) * float(random.next_unit());
}

std::string Ai::idle_event(uint32_t base) {
    const forms::Database& db = m_game.db();
    const forms::FormEntry* e = db.find(base);
    if (!e) {
        return {};
    }
    const Form f(db, *e);
    const Value* list = f.get("IDLA");
    if (!list || list->items.empty()) {
        return {};
    }
    const Value& pick = list->items[GameRandom::shared().next_below(uint32_t(list->items.size()))];
    const forms::FormEntry* idle = db.find(pick.form);
    if (!idle) {
        return {};
    }
    const Form i(db, *idle);
    const Value* event = i.get("ENAM");
    return event ? event->text : std::string();
}

// The daily slots (): once one has passed, the nearest upcoming is rolled.
void Ai::roll_slot(Agent& a) {
    const Settings& s = m_game.settings();
    const float into = std::fmod(m_clock_hour - a.slot_start + 48.0f, 24.0f);
    if (a.slot >= 0 && (into < a.slot_hours || !a.slot_entered)) {
        a.slot_entered = a.slot_entered || into < a.slot_hours;
        return;
    }
    struct Slot {
        const char *min, *max;
        float low, high;
    };
    static constexpr Slot k_slots[4] = {{"fSandboxSleepStartMin", "fSandboxSleepStartMax", 19.0f, 1.0f},
                                        {"fSandboxBreakfastMin", "fSandboxBreakfastMax", 6.0f, 9.0f},
                                        {"fSandboxLunchMin", "fSandboxLunchMax", 11.5f, 12.5f},
                                        {"fSandboxDinnerMin", "fSandboxDinnerMax", 18.0f, 19.0f}};
    GameRandom& random = GameRandom::shared();
    int best = 0;
    float best_wait = 1e9f;
    for (int i = 0; i < 4; ++i) {
        const float start = s.number(k_slots[i].min, k_slots[i].low);
        const float wait = start >= m_clock_hour ? start - m_clock_hour : start + 24.0f - m_clock_hour;
        if (wait < best_wait) {
            best_wait = wait;
            best = i;
        }
    }
    const Slot& slot = k_slots[best];
    const float low = s.number(slot.min, slot.low);
    float high = s.number(slot.max, slot.high);
    high += high < low ? 24.0f : 0.0f; // sleep starts either side of midnight
    a.slot = best;
    a.slot_entered = false;
    a.slot_start = std::fmod(low + (high - low) * float(random.next_unit()), 24.0f);
    const float shortest = best == 0 ? s.number("fSandboxSleepDurationMin", 6.0f) : s.number("fSandboxMealDurationMin", 0.1f);
    const float longest = best == 0 ? s.number("fSandboxSleepDurationMax", 10.0f) : s.number("fSandboxMealDurationMax", 0.25f);
    a.slot_hours = shortest + (longest - shortest) * float(random.next_unit());
}

// The weighted pick: each activity by the changes since it was last chosen; the sleep package wants a bed.
void Ai::pick_activity(Agent& a, bool instant) {
    const float reach = a.location.radius > 0 ? a.location.radius : m_game.settings().number("fSandBoxSearchRadius", 6000.0f);
    const bool sleep = a.package && a.package->mode == AiMode::sleep;
    const uint8_t allow = a.package ? a.package->allow : uint8_t(0x3B);
    std::vector<const AiFurniture*> found[6];
    if (const auto it = m_furniture.find(a.space); it != m_furniture.end()) {
        for (const AiFurniture& f : it->second) {
            if (flat(f.position, a.location.point) > reach || m_reserved.contains(f.key()) || f.ref == a.use_ref) {
                continue;
            }
            const float dz = f.position[2] - a.location.point[2];
            // The cylinder is the sandbox's; the Sleep package's Find looks through the whole location.
            const bool bed = f.kind == FurnitureKind::bed || f.kind == FurnitureKind::bedroll;
            if (!(sleep && bed) && (dz < -100.0f || dz > 150.0f)) {
                continue;
            }
            if (!may_use(a, f.ref)) {
                continue;
            }
            if (f.kind == FurnitureKind::chair) {
                if (allow & (k_allow_sitting | k_allow_eating)) {
                    found[size_t(Activity::sit)].push_back(&f);
                }
            } else if (f.kind == FurnitureKind::idle_marker) {
                if (allow & k_allow_idle_markers) {
                    found[size_t(Activity::idle_marker)].push_back(&f);
                }
            } else if (sleep || (allow & k_allow_sleeping)) {
                found[size_t(Activity::sleep)].push_back(&f);
            }
        }
    }
    uint32_t weights[6] = {};
    uint32_t total = 0;
    for (size_t i = 0; i < 6; ++i) {
        a.since[i] = uint8_t(std::min(int(a.since[i]) + 1, 255));
        const bool has = i == size_t(Activity::wander)         ? allow & k_allow_wandering
                         : i == size_t(Activity::eat_standing) ? allow & k_allow_eating
                         : i == size_t(Activity::sit)          ? (allow & k_allow_sitting) && !found[i].empty()
                                                               : !found[i].empty();
        weights[i] = has ? a.since[i] : 0;
    }
    const bool has_bed = !found[size_t(Activity::sleep)].empty(), has_chair = !found[size_t(Activity::sit)].empty();
    found[size_t(Activity::eat_sitting)] = found[size_t(Activity::sit)];
    roll_slot(a);
    const bool in_slot = a.slot >= 0 && std::fmod(m_clock_hour - a.slot_start + 48.0f, 24.0f) < a.slot_hours;
    const bool slot_sleep = in_slot && a.slot == 0 && has_bed, slot_meal = in_slot && a.slot > 0 && has_chair && (allow & k_allow_eating);
    weights[size_t(Activity::sleep)] = (sleep || slot_sleep) && has_bed ? 100000 : 0;
    weights[size_t(Activity::eat_sitting)] = has_chair && (allow & k_allow_eating) ? a.since[size_t(Activity::eat_sitting)] : 0;
    if (slot_sleep || slot_meal) { // inside a slot: every weight 1 but its own
        for (size_t i = 0; i < 6; ++i) {
            weights[i] = weights[i] ? 1 : 0;
        }
        if (slot_sleep) {
            weights[size_t(Activity::sleep)] = 100000;
        } else {
            weights[size_t(Activity::eat_sitting)] = 100000;
            weights[size_t(Activity::eat_standing)] = 100;
        }
    }
    for (const uint32_t w : weights) {
        total += w;
    }
    GameRandom& random = GameRandom::shared();
    if (!total) { // nothing to do here: they stand, and look again at the next rescan
        a.activity = Activity::none;
        const Settings& s = m_game.settings();
        const float low = s.number("fMinSandboxRescanSeconds", 3.0f), high = s.number("fMaxSandboxRescanSeconds", 6.0f);
        a.rest = low + (high - low) * float(random.next_unit());
        return;
    }
    uint32_t roll = random.next_below(total);
    size_t chosen = size_t(Activity::wander);
    for (size_t i = 0; i < 6; ++i) {
        if (roll < weights[i]) {
            chosen = i;
            break;
        }
        roll -= weights[i];
    }
    a.since[chosen] = 1;
    a.activity = Activity(chosen);
    a.rest = activity_seconds(a, a.activity);
    if (a.activity == Activity::wander) {
        wander_leg(a);
        return;
    }
    if (a.activity == Activity::eat_standing) {
        a.event = "idleEatingStandingStart";
        return;
    }
    const auto& list = found[chosen];
    const AiFurniture* f = list[random.next_below(uint32_t(list.size()))];
    if (a.activity == Activity::sleep) { // their own bed first (inferred: Find isn't read, spec 4)
        for (const AiFurniture* own : list) {
            if (m_game.owner_of(own->ref) == a.npc) {
                f = own;
            }
        }
    }
    a.use_ref = f->ref;
    a.use_marker = f->marker;
    m_reserved[f->key()] = a.ref;
    a.use = UseState::going;
    if (instant) {
        arrive_at_use(a, true);
    } else {
        request_path(a, f->position);
    }
}

// On the furniture: at its place, facing its way, into its enter animation.
void Ai::arrive_at_use(Agent& a, bool instant) {
    const auto list = m_furniture.find(a.space);
    if (list == m_furniture.end()) {
        return;
    }
    for (const AiFurniture& f : list->second) {
        if (f.ref != a.use_ref || f.marker != a.use_marker) {
            continue;
        }
        // Into it from the side the actor came from, of those the marker allows.
        const float to_actor = std::atan2(a.position[0] - f.position[0], a.position[1] - f.position[1]);
        const float turn = std::remainder(to_actor - f.heading, 2.0f * std::numbers::pi_v<float>);
        const char* side = "Front";
        const auto allowed = [&](uint16_t bit) { return !f.entry || (f.entry & bit); };
        const bool bed_side = f.kind == FurnitureKind::bed;
        if (bed_side) {
            side = (turn < 0.0f && allowed(8)) || !allowed(4) ? "Left" : "Right";
        } else if (std::abs(turn) > 2.356f && allowed(2)) {
            side = "Behind";
        } else if (std::abs(turn) > 0.785f && allowed(turn < 0.0f ? 8 : 4)) {
            side = turn < 0.0f ? "Left" : "Right";
        }
        a.position = f.position;
        a.heading = f.heading;
        a.spot = {};
        a.use = UseState::using_it;
        m_game.script_event(a.ref, "OnSit", {{a.use_ref}});
        a.rest = activity_seconds(a, a.activity);
        switch (f.kind) {
        case FurnitureKind::chair:
            if (const FurnitureEvents& own = m_game.furniture_events(f.base); !own.enter.empty()) {
                a.event = instant && !own.instant.empty() ? own.instant : own.enter;
            } else {
                a.event = instant ? "IdleChairEnterInstant" : std::format("IdleChair{}Enter", side);
            }
            break;
        case FurnitureKind::bed: a.event = instant ? "IdleBedLeftEnterInstant" : std::format("IdleBed{}EnterStart", side); break;
        case FurnitureKind::bedroll: a.event = instant ? "IdleBedRollFrontEnterInstant" : "IdleBedRollFrontEnterStart"; break;
        case FurnitureKind::idle_marker: a.event = idle_event(f.base); break;
        default: break;
        }
        a.eat_in = a.activity == Activity::eat_sitting ? (instant ? 0.0f : 3.0f) : -1.0f;
        return;
    }
    a.use = UseState::none;
}

void Ai::leave_use(Agent& a) {
    if (a.use_ref) {
        m_reserved.erase(uint64_t(a.use_ref) << 8 | a.use_marker);
    }
    if (a.use == UseState::using_it) {
        m_game.script_event(a.ref, "OnGetUp", {{a.use_ref}});
        a.event = a.activity == Activity::sleep ? "IdleBedExitStart" : "IdleChairExitStart";
        a.use = UseState::leaving;
        a.leaving = 2.0f;
    } else if (a.use == UseState::going) {
        a.use = UseState::none;
    }
    a.use_ref = 0;
}

void Ai::update(float dt, const AiClock& clock, const std::vector<uint32_t>& high) {
    m_clock_hour = clock.hour;
    m_time_scale = std::max(clock.time_scale, 1.0f);
    m_search_clock += dt;
    if (m_door_index.valid() && (m_replay || m_door_index.wait_for(std::chrono::seconds(0)) == std::future_status::ready)) {
        m_door_index.get();
    }
    for (size_t i = m_searches.size(); i-- > 0;) {
        Search& s = m_searches[i];
        constexpr uint32_t k_replay_search_ticks = 6;
        if (m_replay ? ++s.ticks < k_replay_search_ticks : s.result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            continue;
        }
        if (const auto it = m_agents.find(s.ref); it != m_agents.end()) {
            Agent& a = it->second;
            a.searching = false;
            a.path = s.result.get();
            a.next = a.path.size() > 1 ? 1 : a.path.size();
            if (a.path.empty()) { // those with nowhere to go would hold every search there is, and everyone else would wait
                a.no_path_until = m_search_clock + 2.0;
            }
            if (a.path.empty() && a.leg_door) {
                const Door& d = m_doors.at(a.leg_door); // no way on foot to the door: through it all the same
                change_space(a, d.destination_space, d.arrival, d.arrival_heading);
                a.leg_door = 0;
            }
            a.spot = {};
        }
        m_searches.erase(m_searches.begin() + std::ptrdiff_t(i));
    }
    update_conversations(dt, high);
    update_left_behind(dt);
    const int hour = int(clock.hour);
    // Those coming into the attached cells are already at what they are doing (section 5).
    std::vector<Agent*> arrived;
    for (const uint32_t ref : high) {
        if (!m_high.contains(ref)) {
            if (const auto it = m_agents.find(ref); it != m_agents.end()) {
                Agent& a = it->second;
                a.arrived_unseen = true;
                a.rest = 0;
                arrived.push_back(&a);
            }
        }
    }
    // A placed actor can stand well off the floor too: the game's character controller drops it as it loads.
    // A location's point is a reference's, often off the floor. A journey brings a town in one tick: each only reads the
    // navmeshes, so they look at once.
    std::vector<uint8_t> grounded(arrived.size(), 0);
    SPUD_TRACE("arrivals");
    parallel::for_each(arrived.size(), [&](size_t i) {
        Agent& a = *arrived[i];
        if (const auto spot = m_game.navmeshes().locate(a.space, a.position, 256.0f)) {
            a.position = spot->position;
            grounded[i] = 1;
        }
    });
    // With no navmesh under them they still fall, to the land.
    for (size_t i = 0; i < arrived.size(); ++i) {
        Agent& a = *arrived[i];
        const float ground = !grounded[i] && ground_height ? ground_height(a.position[0], a.position[1]) : std::numeric_limits<float>::quiet_NaN();
        if (std::isfinite(ground) && a.position[2] > ground) {
            a.position[2] = ground;
        }
    }
    // Off the attached cells: packages only as the hour turns, a few a frame.
    for (size_t n = 0; n < k_think_per_frame && !m_order.empty(); ++n) {
        m_cursor = (m_cursor + 1) % m_order.size();
        Agent& a = m_agents.at(m_order[m_cursor]);
        if (!a.high && a.last_hour != hour) {
            think(a, clock);
        }
    }
    std::unordered_set<uint32_t> now(high.begin(), high.end());
    for (const uint32_t ref : m_high) {
        if (!now.contains(ref)) {
            if (const auto it = m_agents.find(ref); it != m_agents.end()) {
                it->second.high = false;
            }
        }
    }
    m_high = std::move(now);
    constexpr uint32_t k_thinks_per_tick = 12;
    uint32_t thinks_left = k_thinks_per_tick;
    for (const uint32_t ref : m_high) {
        const auto it = m_agents.find(ref);
        if (it == m_agents.end()) {
            continue;
        }
        Agent& a = it->second;
        a.high = true;
        a.script_placed = false;
        constexpr int k_av_waiting_for_player = 95;
        if (a.follows && a.package && a.package->follower && m_game.actor_values(a.ref).current(k_av_waiting_for_player) == 1.0f) {
            a.path.clear(); // the template's Wait: told to stay
            a.next = 0;
        } else if (a.follows) {
            follow(a);
        }
        a.evaluate_in -= dt;
        // The hour turning or a load has everyone due at once: a few a tick, the rest on the ticks after.
        if ((!a.package || a.evaluate_in <= 0.0f || a.last_hour != hour) && thinks_left > 0) {
            --thinks_left;
            think(a, clock);
        }
        walk(a, dt);
    }
}

std::vector<const Agent*> Ai::in_space(uint32_t space) const {
    if (m_by_space_roster != m_roster) {
        m_by_space_roster = m_roster;
        m_by_space.clear();
        for (const auto& [ref, a] : m_agents) {
            if (!a.disabled) {
                m_by_space[a.space].push_back(&a);
            }
        }
    }
    const auto it = m_by_space.find(space);
    return it != m_by_space.end() ? it->second : std::vector<const Agent*>{};
}

void Ai::pursue(Agent& a, bool on) {
    a.arresting = on;
    a.follows = on ? k_player_ref : 0;
    if (!on) {
        a.evaluate_in = 0;
        a.package = nullptr; // the schedule again
    }
}

void Ai::close_on(uint32_t ref, uint32_t target, float stop) {
    Agent* a = mutable_agent(ref);
    if (!a) {
        return;
    }
    a->fighting = true;
    a->follows = target;
    a->stop_distance = stop;
    follow(*a);
    NavPoint at = m_player;
    if (const Agent* other = target == k_player_ref ? nullptr : agent(target)) {
        at = other->position;
    }
    if (flat(a->position, at) <= stop) {
        a->path.clear();
        a->next = 0;
        a->heading = std::atan2(at[0] - a->position[0], at[1] - a->position[1]);
    } else if (a->path.empty() && !a->searching) {
        request_path(*a, at);
    }
}

// Followers the player left behind a door: out of the attached cells they walk the path unseen, at the path's speed,
// and come out at the door's far side. One door a time, so a longer way goes space by space.
void Ai::update_left_behind(float dt) {
    constexpr float k_check_seconds = 0.25f;
    m_left_behind_check -= dt;
    if (m_left_behind_check > 0.0f || !m_doors_ready) {
        return;
    }
    m_left_behind_check = k_check_seconds;
    for (const uint32_t ref : followers(k_player_ref)) {
        Agent& a = m_agents.at(ref);
        constexpr int k_av_waiting_for_player = 95;
        const bool waiting = a.package->follower && m_game.actor_values(ref).current(k_av_waiting_for_player) == 1.0f;
        if (a.high || waiting || a.space == m_player_space || std::ranges::find(m_player_ground, a.space) != m_player_ground.end()) {
            continue;
        }
        const uint32_t door = door_towards(a.space, m_player_space);
        const auto it = door ? m_doors.find(door) : m_doors.end();
        if (it == m_doors.end()) {
            continue;
        }
        if (a.leg_door != door) {
            a.leg_door = door;
            a.door_eta = flat(a.position, it->second.position) / std::max(m_run_speed, 1.0f);
        }
        a.door_eta -= k_check_seconds;
        if (a.door_eta <= 0.0f) {
            a.leg_door = 0;
            a.path.clear();
            a.next = 0;
            change_space(a, it->second.destination_space, it->second.arrival, it->second.arrival_heading);
        }
    }
}

float Ai::roll_talk_timer() const {
    const Settings& s = m_game.settings();
    const float low = s.number("fAISocialTimerForConversationsMin", 10.0f), high = s.number("fAISocialTimerForConversationsMax", 30.0f);
    return low + (high - low) * float(GameRandom::shared().next_unit());
}

// Actor & 0x2000 (inferred CanSpeak), taken here from the race's Allow PC Dialogue (inferred).
bool Ai::can_speak(uint32_t npc) {
    const auto it = m_can_speak.find(npc);
    if (it != m_can_speak.end()) {
        return it->second;
    }
    bool can = false;
    if (const forms::FormEntry* e = m_game.db().find(m_game.conditions().race_of(npc))) {
        const Form race(m_game.db(), *e);
        const Value* data = race.get("DATA");
        can = data && (integer(data->member("flags")) & 0x200000);
    }
    return m_can_speak.emplace(npc, can).first->second;
}

//(O, S): O can be spoken to now.
bool Ai::can_be_talked_to(const Agent& o, const Agent& s) {
    const bool interior = m_game.db().find(o.space) && m_game.db().find(o.space)->type == forms::parse_sig("CELL").value();
    if (!can_speak(o.npc) || o.saying || !o.package || !(o.package->interrupts & 2) || (interior && std::abs(o.position[2] - s.position[2]) >= 90.0f)) {
        return false;
    }
    return o.use == UseState::none || o.use == UseState::using_it; // sit states 0 and 3: not on the way in or out
}

//(S, O).
bool Ai::may_talk(const Agent& s, const Agent& o) {
    const float reach = m_game.settings().number("fDistancetoPlayerforConversations", 3000.0f);
    const bool asleep = s.use == UseState::using_it && s.activity == Activity::sleep;
    return s.high && o.high && o.ref != s.ref && can_speak(s.npc) && s.package && (s.package->interrupts & 2) && !asleep &&
           can_be_talked_to(o, s) && flat(o.position, m_player) <= reach;
}

// The game and.
void Ai::update_conversations(float dt, const std::vector<uint32_t>& high) {
    if (!ask_conversation) {
        return;
    }
    const Settings& s = m_game.settings();
    const float reach = s.number("fDistancetoPlayerforConversations", 3000.0f);
    const float radius = s.number("fAISocialRadiusToTriggerConversation", 500.0f);
    const float radius_inside = s.number("fAISocialRadiusToTriggerConversationInterior", 500.0f);
    const auto scene = [this](const Agent& a) { return in_scene && in_scene(a.ref); };
    const uint32_t cell = forms::parse_sig("CELL").value();
    for (const uint32_t ref : high) {
        const auto it = m_agents.find(ref);
        if (it == m_agents.end()) {
            continue;
        }
        Agent& a = it->second;
        if (a.talk_partner && a.talk_stop >= 0.0f) {
            const auto other = m_agents.find(a.talk_partner);
            if (other != m_agents.end()) {
                close_talk(a, other->second);
            }
        }
        if (a.talk_timer < 0.0f) {
            a.talk_timer = roll_talk_timer();
        }
        if (scene(a)) {
            continue;
        }
        a.talk_timer -= dt;
        if (a.talk_timer > 0.0f || a.talk_event || a.in_random_scene || a.fighting || a.disabled || !a.package ||
            !(a.package->interrupts & 2) || flat(a.position, m_player) > reach) {
            continue;
        }
        const forms::FormEntry* space = m_game.db().find(a.space);
        const float trigger = space && space->type == cell ? radius_inside : radius;
        for (const uint32_t candidate : high) {
            const auto found = m_agents.find(candidate);
            if (found == m_agents.end() || candidate == ref) {
                continue;
            }
            Agent& b = found->second;
            if (b.in_random_scene || b.talk_event || b.disabled || b.fighting || b.space != a.space || scene(b) ||
                !may_talk(b, a) || !may_talk(a, b) || flat(a.position, b.position) > trigger) {
                continue;
            }
            if (a.talked_timer > 0.0f) {
                a.talked_timer -= dt;
            } else {
                a.talked_to.clear();
                a.talked_timer = s.number("fAItalktosameNPCtimer", 120.0f);
            }
            if (std::ranges::find(a.talked_to, candidate) != a.talked_to.end()) {
                continue;
            }
            const uint32_t event = ask_conversation(a.ref, b.ref);
            if (!event) {
                break;
            }
            a.talk_event = b.talk_event = event;
            a.talked_to.push_back(b.ref);
            b.talked_to.push_back(a.ref);
            a.in_random_scene = b.in_random_scene = true;
            a.talk_initiator = true;
            break;
        }
    }
}

void Ai::event_processed(uint32_t event, bool started) {
    Agent* first = nullptr;
    Agent* second = nullptr;
    for (auto& [ref, a] : m_agents) {
        if (a.talk_event == event) {
            (a.talk_initiator ? first : second) = &a;
        }
    }
    for (Agent* a : {first, second}) {
        if (!a) {
            continue;
        }
        a->talk_event = 0;
        if (!started) {
            a->in_random_scene = a->talk_initiator = false;
        }
    }
    if (first) {
        first->talk_timer = roll_talk_timer();
    }
    if (second && started) {
        second->talk_timer = roll_talk_timer();
    }
}

bool Ai::can_talk(uint32_t a, uint32_t b) {
    const Agent* x = agent(a);
    const Agent* y = agent(b);
    return x && y && may_talk(*x, *y) && may_talk(*y, *x);
}

uint32_t Ai::initiator(uint32_t a, uint32_t b) {
    const Agent* x = agent(a);
    return x && x->talk_initiator ? a : b;
}

// the speaker goes to the other, who waits. In sight, and not already walking, they talk from where they are.
void Ai::start_talk(uint32_t speaker, uint32_t target) {
    Agent* a = mutable_agent(speaker);
    Agent* b = mutable_agent(target);
    if (!a || !b) {
        return;
    }
    const Settings& s = m_game.settings();
    const bool moving = !a->path.empty() || a->searching;
    const bool seated = a->use == UseState::using_it && b->use == UseState::using_it;
    const bool sees = line_of_sight && line_of_sight(speaker, target);
    float stop = s.number("iAISocialDistanceToTriggerEvent", 200.0f);
    if (!moving && (sees || seated)) {
        stop = s.number("iAIMaxSocialDistanceToTriggerEvent", 500.0f);
    }
    a->talk_stop = stop + (a->use == UseState::using_it ? 20.0f : 0.0f);
    a->talk_partner = target;
    b->talk_partner = speaker;
    b->talk_stop = -1.0f;
    a->talk_initiator = b->talk_initiator = false;
    b->path.clear();
    b->next = 0;
}

// The speaker's walk to the other, and both turned to each other once there.
void Ai::close_talk(Agent& a, Agent& b) {
    if (a.use != UseState::none) {
        return; // seated: from where they are
    }
    a.follows = b.ref;
    a.stop_distance = a.talk_stop;
    follow(a);
    if (flat(a.position, b.position) <= a.talk_stop) {
        a.path.clear();
        a.next = 0;
        a.heading = std::atan2(b.position[0] - a.position[0], b.position[1] - a.position[1]);
        if (b.use == UseState::none) {
            b.heading = std::atan2(a.position[0] - b.position[0], a.position[1] - b.position[1]);
        }
    } else if (a.path.empty() && !a.searching) {
        request_path(a, b.position);
    }
}

bool Ai::talk_ready(uint32_t speaker) {
    const Agent* a = agent(speaker);
    if (!a) {
        return true;
    }
    const Agent* b = agent(a->talk_partner);
    return a->path.empty() && !a->searching && (a->use != UseState::none || (b && flat(a->position, b->position) <= a->talk_stop));
}

bool Ai::talk_broken(uint32_t a, uint32_t b) {
    const Agent* x = agent(a);
    const Agent* y = agent(b);
    return !x || !y || x->disabled || y->disabled || x->fighting || y->fighting || x->space != y->space;
}

void Ai::end_talk(uint32_t first, uint32_t second) {
    for (const uint32_t ref : {first, second}) {
        Agent* a = mutable_agent(ref);
        if (!a) {
            continue;
        }
        a->in_random_scene = a->talk_initiator = false;
        a->talk_partner = 0;
        if (a->talk_stop >= 0.0f) {
            a->follows = 0;
            a->stop_distance = 128;
        }
        a->talk_stop = -1.0f;
        a->evaluate_in = 0;
        if (a->use == UseState::none) {
            a->package = nullptr; // back to what they were doing
        }
    }
}

float Ai::setting(const char* name, float fallback) {
    return m_game.settings().number(name, fallback);
}

void Ai::flee(uint32_t ref, uint32_t from, int attempt) {
    Agent* a = mutable_agent(ref);
    if (!a) {
        return;
    }
    NavPoint at = m_player;
    if (const Agent* other = from == k_player_ref ? nullptr : agent(from)) {
        at = other->position;
    }
    a->fighting = true;
    a->follows = 0;
    // Straight away first, then fanning out to either side, nearer each round: *inferred*, the game's Unrestricted Flee isn't read.
    const float side = float((attempt + 1) / 2) * (attempt % 2 ? 0.7854f : -0.7854f);
    const float reach = 2048.0f / float(1 + attempt / 4);
    const float heading = std::atan2(a->position[1] - at[1], a->position[0] - at[0]) + side;
    const NavPoint away = {a->position[0] + std::cos(heading) * reach, a->position[1] + std::sin(heading) * reach, a->position[2]};
    a->speed = m_run_speed;
    a->path.clear();
    a->next = 0;
    request_path(*a, away);
}

bool Ai::crash_land(uint32_t ref) {
    Agent* a = mutable_agent(ref);
    if (!a || !a->high || a->fly == FlyState::none || a->fly == FlyState::perching) {
        return false;
    }
    a->crash_left = 6.0f; // measured: dead 5 to 8 s after health ran out, flying on until then. How it comes down isn't read.
    return true;
}

void Ai::drop(uint32_t ref) {
    if (Agent* a = mutable_agent(ref); a && a->flier) {
        a->fly = FlyState::none;
        a->fall_speed = 0.0f;
    }
}

void Ai::command(uint32_t ref, uint32_t commander) {
    if (Agent* a = mutable_agent(ref)) {
        a->commander = commander;
        a->evaluate_in = 0;
    }
}

void Ai::stand_up(uint32_t ref) {
    Agent* a = mutable_agent(ref);
    if (!a) {
        return;
    }
    const bool seated = a->use == UseState::using_it &&
                        (a->activity == Activity::sit || a->activity == Activity::sleep || a->activity == Activity::eat_sitting);
    leave_use(*a);
    if (!seated) {
        a->use = UseState::none; // an idle marker or a meal on the feet: nothing to get up from
        a->event = "IdleForceDefaultState";
    }
    a->activity = Activity::none;
    a->eat_in = -1.0f;
}

void Ai::stand_down(uint32_t ref) {
    if (Agent* a = mutable_agent(ref)) {
        a->fighting = false;
        a->follows = 0;
        a->stop_distance = 128;
        a->evaluate_in = 0;
        a->package = nullptr;
    }
}

Agent* Ai::mutable_agent(uint32_t ref) {
    const auto it = m_agents.find(ref);
    return it == m_agents.end() ? nullptr : &it->second;
}

const AiFurniture* Ai::free_seat(uint32_t space, uint32_t ref, const NavPoint& near) const {
    const auto list = m_furniture.find(space);
    const AiFurniture* best = nullptr;
    float distance = std::numeric_limits<float>::max();
    for (const AiFurniture& f : list == m_furniture.end() ? std::vector<AiFurniture>{} : list->second) {
        if (f.ref == ref && !m_reserved.contains(f.key()) && flat(f.position, near) < distance) {
            distance = flat(f.position, near);
            best = &*std::ranges::find_if(list->second, [&f](const AiFurniture& o) { return o.key() == f.key(); });
        }
    }
    return best;
}

bool Ai::furniture_in_use(uint32_t ref) const {
    return std::ranges::any_of(m_reserved, [ref](const auto& entry) { return uint32_t(entry.first >> 8) == ref; });
}

const Agent* Ai::agent(uint32_t ref) const {
    const auto it = m_agents.find(ref);
    return it == m_agents.end() ? nullptr : &it->second;
}

} // namespace spud::game
