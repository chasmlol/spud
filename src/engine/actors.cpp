#include "core/parallel.hpp"
#include "core/trace.hpp"
#include "engine/actors.hpp"

#include "core/error.hpp"
#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "core/units.hpp"
#include "engine/face_tint.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <format>

namespace spud::engine {

namespace {

constexpr uint32_t k_player_ref = 0x14;
constexpr uint32_t k_player_first_person_ref = 0xFFFFFF14; // no form: the player's second body
constexpr float k_first_person_pitch_factor = 0.75f; // f1stPitchOffsetTarget:Camera, the engine's default
constexpr uint32_t k_player_npc = 0x7;

// ACBS template flags: which traits a template supplies.
constexpr uint16_t k_template_traits = 0x01;
constexpr uint16_t k_template_inventory = 0x100;
constexpr uint32_t k_npc_female = 0x1;

constexpr uint32_t k_ref_starts_dead = 0x200;
// Settings the face runs on, from Skyrim.esm where it has them.
constexpr float k_blink_down = 0.04f, k_blink_up = 0.14f;              // fBlinkDownTime, fBlinkUpTime
constexpr float k_blink_delay_min = 0.5f, k_blink_delay_max = 2.5f;    // fBlinkDelayMin, fBlinkDelayMax
constexpr float k_look_down_blink = 0.25f;                             // fLookDownDisableBlinkingAmt
constexpr float k_expression_rate = 2.0f, k_expression_strength_add = 1.0f; // fExpressionChangePerSec, fExpressionStrengthAdd
constexpr float k_mood_strength = 0.5f;                                // iMoodFaceValue
constexpr float k_speech_delay = 0.1f;                                 // fSpeechDelay
constexpr float k_face_lod_distance = 500.0f, k_face_talking_distance = 2000.0f; // fLodDistance:LOD, fTalkingDistance:LOD
// The game's channels by the names the.tri files use: the last three modifiers turn
// the head, and have no morphs.
constexpr const char* k_face_channel_names[k_face_channels] = {
    "Aah", "BigAah", "BMP", "ChJSh", "DST", "Eee", "Eh", "FV", "I", "K", "N", "Oh", "OohQ", "R", "Th", "W",
    "BlinkLeft", "BlinkRight", "BrowDownLeft", "BrowDownRight", "BrowInLeft", "BrowInRight", "BrowUpLeft", "BrowUpRight",
    "LookDown", "LookLeft", "LookRight", "LookUp", "SquintLeft", "SquintRight", "", "", "",
    "DialogueAnger", "DialogueFear", "DialogueHappy", "DialogueSad", "DialogueSurprise", "DialoguePuzzled",
    "DialogueDisgusted", "MoodNeutral", "MoodAnger", "MoodFear", "MoodHappy", "MoodSad", "MoodSurprise", "MoodPuzzled",
    "MoodDisgusted", "CombatAnger", "CombatShout"};
// Where an object's rigged model and skeleton are, from the model the world names.
std::string object_path(std::string_view mesh, std::string_view extension) {
    return "skinned/" + path::replace_extension(mesh, extension);
}

constexpr float k_ragdoll_margin = 0.7f; // metres from a limb's body to its hand or foot, for culling
constexpr uint32_t k_ref_initially_disabled = 0x800;

constexpr uint32_t k_start_per_frame = 8;  // each reads its records on a worker
constexpr uint32_t k_finish_per_frame = 4; // actors finished in one frame: a variant and a job per shape

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

uint32_t mix_seed(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u);
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    return h;
}

glm::mat4 local_matrix(const osk::Bone& b) {
    glm::mat4 m(1.0f);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            m[c][r] = b.rotation[r][c] * b.scale;
        }
        m[3][r] = b.translation[r];
    }
    return m;
}

uint64_t member_int(const forms::Form& f, std::string_view path, uint64_t otherwise = 0) {
    const auto* v = f.get(path);
    return v ? uint64_t(v->integer) : otherwise;
}

uint32_t member_form(const forms::Form& f, std::string_view path) {
    const auto* v = f.get(path);
    return v ? v->form : 0;
}

float member_real(const forms::Form& f, std::string_view path, float otherwise) {
    const auto* v = f.get(path);
    return v ? float(v->real) : otherwise;
}

// BOD2, or BODT in older forms: both start with the biped slots.
uint64_t biped_slots(const forms::Form& f) {
    return f.get("BOD2") ? member_int(f, "BOD2.biped_flags") : member_int(f, "BODT.biped_flags");
}

std::string texture_path(const forms::Value* v) {
    if (!v || v->text.empty()) {
        return {};
    }
    return path::with_root(path::normalize(v->text), "textures");
}

glm::vec3 clfm_color(const forms::Form& f) {
    const auto* c = f.get("CNAM");
    if (!c) {
        return glm::vec3(1.0f);
    }
    auto byte = [&](const char* name) {
        const auto* v = c->member(name);
        return v ? float(v->integer) / 255.0f : 1.0f;
    };
    return {byte("r"), byte("g"), byte("b")};
}

} // namespace

// One armour addon on an actor.
struct WornAddon {
    uint32_t slots = 0; // BOD2
    uint8_t priority = 0;
    bool swap_list = false; // NAM2
    uint32_t footstep_set = 0; // SNDD
    float weapon_adjust = 0.0f; // DNAM
    ActorPart part;
};

// The biped's 32 armour slots as the game fills them.
struct Biped {
    struct Slot {
        uint32_t item = 0;    // ARMO holding the slot, or the ARMA itself for a lone addon, 0 for none
        bool lone = false;
        int32_t addon = -1;   // into addons, -1 none
    };
    std::array<Slot, 32> slots;
    std::vector<WornAddon> addons;

    void remove_model(int32_t addon) {
        for (auto& s : slots) {
            if (s.addon >= 0 && addons[size_t(s.addon)].part.mesh == addons[size_t(addon)].part.mesh) {
                s = Slot{};
            }
        }
    }
    void place(uint32_t armor, uint32_t armor_slots, uint32_t addon_form, WornAddon addon) {
        const auto index = static_cast<int32_t>(addons.size());
        addons.push_back(std::move(addon));
        const WornAddon& a = addons.back();
        for (uint32_t i = 0; i < 32; ++i) {
            Slot& s = slots[i];
            if (!((armor_slots >> i) & 1) || s.item == 0 || s.item == armor || s.item == addon_form) {
                continue;
            }
            if (!s.lone) {
                if (s.addon >= 0) {
                    remove_model(s.addon); // a worn item goes whatever its priority: a cuirass displaces the body
                }
                s = Slot{};
            } else if (s.addon < 0 || a.priority > addons[size_t(s.addon)].priority) {
                s = Slot{};
            }
        }
        for (uint32_t i = 0; i < 32; ++i) {
            Slot& s = slots[i];
            const bool in_armor = (armor_slots >> i) & 1;
            if ((a.slots >> i) & 1) {
                const WornAddon* old = s.addon >= 0 ? &addons[size_t(s.addon)] : nullptr;
                if (old && s.lone && old->priority > a.priority) {
                    continue;
                }
                if (!old || s.addon == index || old->priority != a.priority || old->swap_list) {
                    s.addon = index;
                    s.item = in_armor ? armor : addon_form;
                    s.lone = !in_armor;
                }
            } else if (in_armor) {
                s.item = armor; // claimed; with no addon there it draws nothing
                s.lone = false;
            }
        }
    }
};

ActorData::ActorData(const std::filesystem::path& cache_root) : m_root(cache_root) {
    m_db = std::make_unique<forms::Database>(cache_root / "forms.osf");
    m_indexed = std::async(std::launch::async, [this] { index(); }).share();
}

ActorData::~ActorData() {
    if (m_indexed.valid()) {
        m_indexed.wait();
    }
}

void ActorData::index() {
    for (const auto* e : m_db->of_type(forms::sig("GMST"))) {
        const auto name = m_db->editor_id(*e);
        if (name.size() < 10 || (!name.starts_with("fLevel") && !name.starts_with("iCalcLevelAdjust"))) {
            continue;
        }
        const forms::Form f(*m_db, *e);
        if (name == "fLevelScalingMult") m_level_scaling = member_real(f, "DATA", 1.0f);
        if (name == "fLeveledActorMultEasy") m_level_mults[0] = member_real(f, "DATA", 1.0f);
        if (name == "fLeveledActorMultMedium") m_level_mults[1] = member_real(f, "DATA", 1.0f);
        if (name == "fLeveledActorMultHard") m_level_mults[2] = member_real(f, "DATA", 1.0f);
        if (name == "fLeveledActorMultVeryHard") m_level_mults[3] = member_real(f, "DATA", 1.0f);
        if (name == "iCalcLevelAdjustUp") m_level_adjust_up = static_cast<int32_t>(member_int(f, "DATA"));
        if (name == "iCalcLevelAdjustDown") m_level_adjust_down = static_cast<int32_t>(member_int(f, "DATA"));
    }
    for (const auto* e : m_db->of_type(forms::sig("IDLE"))) {
        m_idles.emplace(lower(m_db->editor_id(*e)), e->form_id);
    }
    for (const auto* e : m_db->of_type(forms::sig("ANIO"))) {
        const forms::Form f(*m_db, *e);
        const auto* model = f.get("MODL");
        const auto* unload = f.get("BNAM");
        if (model && !model->text.empty()) {
            m_anim_objects.emplace(lower(m_db->editor_id(*e)), AnimObject{skinned_path(model->text), unload ? lower(unload->text) : ""});
        }
    }
    constexpr forms::Sig k_achr = forms::sig("ACHR"), k_refr = forms::sig("REFR"), k_idlm = forms::sig("IDLM"), k_furn = forms::sig("FURN");
    for (const auto& e : m_db->forms()) {
        if ((e.type != k_achr && e.type != k_refr) || (e.flags & forms::form_deleted)) {
            continue;
        }
        const forms::FormEntry* cell = m_db->find(e.parent);
        if (!cell) {
            continue;
        }
        if (e.type == k_refr) {
            // a NorthMarker sets its interior's north to minus its z angle.
            constexpr uint32_t k_north_marker = 0x00000003;
            if (!cell->parent && forms::placed_base(*m_db, e) == k_north_marker) {
                if (const auto p = forms::placement(*m_db, e)) {
                    m_north[e.parent] = -p->rotation[2];
                }
            }
            const forms::FormEntry* base = m_db->find(forms::placed_base(*m_db, e));
            if (base && (base->type == k_idlm || base->type == k_furn) && !(e.flags & k_ref_initially_disabled)) {
                if (const auto p = forms::placement(*m_db, e)) {
                    m_markers[cell->parent ? cell->parent : e.parent].push_back(
                        {e.form_id, p->base, {p->position[0], p->position[1], p->position[2]}, p->rotation[2], p->scale});
                }
            }
            continue;
        }
        if (cell->parent != 0) {
            m_by_world[cell->parent].push_back(e.form_id);
        } else {
            m_by_cell[e.parent].push_back(e.form_id);
        }
    }
}

const ActorData::AnimObject* ActorData::anim_object(const std::string& editor_id) {
    m_indexed.wait();
    const auto it = m_anim_objects.find(editor_id);
    return it != m_anim_objects.end() ? &it->second : nullptr;
}

const std::vector<PlacedMarker>& ActorData::usable_markers(uint32_t space) {
    static const std::vector<PlacedMarker> none;
    m_indexed.wait();
    const auto it = m_markers.find(space);
    return it != m_markers.end() ? it->second : none;
}

float ActorData::north_rotation(uint32_t cell) const {
    if (m_indexed.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return 0.0f; // asked every frame: never waited for
    }
    const auto it = m_north.find(cell);
    return it != m_north.end() ? it->second : 0.0f;
}

bool ActorData::enabled_at_start(const forms::Form& ref, int depth) const {
    bool disabled = (ref.entry().flags & k_ref_initially_disabled) != 0;
    if (const auto* parent = ref.get("XESP.reference"); parent && parent->form && depth < 8) {
        if (const auto* e = m_db->find(parent->form)) {
            const bool on = enabled_at_start(forms::Form(*m_db, *e), depth + 1);
            const auto* flags = ref.get("XESP.flags");
            const bool opposite = flags && (flags->integer & 1);
            return opposite ? !on : on;
        }
    }
    return !disabled;
}

std::vector<PlacedActor> ActorData::placed_in(const osw::World& world) {
    m_indexed.wait();
    std::vector<uint32_t> refs;
    auto take = [&](const std::unordered_map<uint32_t, std::vector<uint32_t>>& index, uint32_t key) {
        if (auto it = index.find(key); it != index.end()) {
            refs.insert(refs.end(), it->second.begin(), it->second.end());
        }
    };
    if (world.flags & osw::world_interior) {
        for (const auto& c : world.cells) {
            take(m_by_cell, c.form_id);
        }
    } else {
        take(m_by_world, world.form_id);
        for (const auto& region : world.regions) {
            take(m_by_world, region.form_id);
        }
    }
    std::sort(refs.begin(), refs.end());
    refs.erase(std::unique(refs.begin(), refs.end()), refs.end()); // an interior's cells all name its CELL
    std::vector<PlacedActor> out;
    out.reserve(refs.size());
    for (uint32_t id : refs) {
        const forms::Form ref(*m_db, *m_db->find(id));
        if (!enabled_at_start(ref)) {
            continue;
        }
        const auto p = forms::placement(ref);
        if (!p) {
            continue;
        }
        PlacedActor a;
        a.ref = id;
        a.dead = (ref.entry().flags & k_ref_starts_dead) != 0;
        a.base = p->base;
        a.position = glm::vec3(p->position[0], p->position[1], p->position[2]) * units::k_skyrim_unit_m;
        a.heading = p->rotation[2];
        a.scale = std::round(p->scale * 100.0f) / 100.0f; // kept as a percentage
        out.push_back(a);
    }
    // Animated objects: references of activators, statics that move, containers and furniture whose models have graphs.
    std::vector<int8_t> animated(world.meshes.size(), -1);
    for (const osw::Ref& ref : world.refs) {
        if (ref.mesh >= animated.size()) {
            continue;
        }
        int8_t& known = animated[ref.mesh];
        if (known < 0) {
            std::error_code missing;
            known = std::filesystem::exists(m_root / object_path(world.meshes[ref.mesh], ".osk"), missing) ? 1 : 0;
        }
        const auto* base = known ? m_db->find(ref.base_form_id) : nullptr;
        if (!base || (base->type != forms::sig("ACTI") && base->type != forms::sig("MSTT") && base->type != forms::sig("CONT") &&
                      base->type != forms::sig("STAT") && base->type != forms::sig("FURN"))) {
            continue;
        }
        PlacedActor a;
        a.ref = ref.form_id;
        a.base = ref.base_form_id;
        a.object = true;
        a.mesh = world.meshes[ref.mesh];
        a.position = glm::vec3(ref.position[0], ref.position[1], ref.position[2]);
        a.rotation = glm::quat(ref.rotation[3], ref.rotation[0], ref.rotation[1], ref.rotation[2]);
        a.scale = ref.scale;
        a.starts_disabled = (ref.flags & osw::ref_initially_disabled) != 0;
        out.push_back(std::move(a));
    }
    return out;
}

ActivateChildren ActorData::activate_children(const osw::World& world) const {
    m_indexed.wait();
    ActivateChildren out;
    for (const osw::Ref& ref : world.refs) {
        const forms::FormEntry* e = m_db->find(ref.form_id);
        if (!e || !m_db->field(*e, forms::sig("XAPR"))) {
            continue;
        }
        const forms::Form form(*m_db, *e);
        for (const forms::Value* parent : form.all("XAPR")) {
            const forms::Value* reference = parent->member("reference");
            const forms::Value* delay = parent->member("delay");
            if (reference) {
                out[reference->form].push_back({ref.form_id, delay ? static_cast<float>(delay->real) : 0.0f});
            }
        }
    }
    return out;
}

bool ActorData::object_model(const std::string& mesh) const {
    std::error_code missing;
    return std::filesystem::exists(m_root / object_path(mesh, ".osk"), missing);
}

ActorLook ActorData::object_look(const std::string& mesh, uint32_t base) {
    ActorLook out;
    out.height = 1.0f;
    out.skeleton = skeleton(object_path(mesh, ".osk"));
    if (!out.skeleton) {
        throw Error("an animated object without a skeleton: " + mesh);
    }
    if (out.skeleton->nodes.project.empty()) {
        out.loop = fx::load_effect(read_file(m_root / path::replace_extension(mesh, ".ose")));
    } else {
        out.project = project(path::replace_extension(out.skeleton->nodes.project, ""));
    }
    ActorPart part;
    part.mesh = object_path(mesh, ".osm");
    part.whole = true;
    if (m_texture_swaps) {
        if (const auto it = m_texture_swaps->find(base); it != m_texture_swaps->end()) {
            part.swap = &it->second;
        }
    }
    out.parts.push_back(std::move(part));
    return out;
}

RagdollStart ActorData::ragdoll_start(uint32_t ref) const {
    RagdollStart out;
    const forms::FormEntry* entry = m_db->find(ref);
    if (!entry) {
        return out;
    }
    const forms::Form form(*m_db, *entry);
    const auto vec = [](const forms::Value* v) {
        glm::vec3 out(0.0f);
        const char* axes[] = {"x", "y", "z"};
        for (int i = 0; v && i < 3; ++i) {
            if (const forms::Value* axis = v->member(axes[i])) {
                out[i] = static_cast<float>(axis->real);
            }
        }
        return out;
    };
    if (const forms::Value* bones = form.get("XRGD")) {
        for (const forms::Value& b : bones->items) {
            RagdollStart::Bone& bone = out.bones.emplace_back();
            bone.part = static_cast<uint32_t>(b.member("bone_id")->integer);
            bone.position = vec(b.member("position"));
            bone.rotation = vec(b.member("rotation"));
        }
    }
    out.root_rotation = vec(form.get("XRGB"));
    return out;
}

std::vector<WorldActor> ActorData::placed_everywhere() {
    m_indexed.wait();
    std::vector<WorldActor> out;
    const auto take = [&](uint32_t space, uint32_t id) {
        const forms::Form ref(*m_db, *m_db->find(id));
        const auto p = forms::placement(ref);
        if (!p || (ref.entry().flags & k_ref_starts_dead)) {
            return; // those disabled are here too: a script may enable them
        }
        WorldActor a;
        a.placed.ref = id;
        a.placed.base = p->base;
        a.placed.position = glm::vec3(p->position[0], p->position[1], p->position[2]) * units::k_skyrim_unit_m;
        a.placed.heading = p->rotation[2];
        a.placed.scale = std::round(p->scale * 100.0f) / 100.0f;
        a.space = space;
        out.push_back(a);
    };
    for (const auto& [cell, refs] : m_by_cell) {
        for (const uint32_t id : refs) {
            take(cell, id);
        }
    }
    for (const auto& [world, refs] : m_by_world) {
        for (const uint32_t id : refs) {
            take(world, id);
        }
    }
    return out;
}

std::shared_ptr<const ActorSkeleton> ActorData::skeleton(const std::string& path) {
    {
        std::lock_guard lock(m_mutex);
        if (auto it = m_skeletons.find(path); it != m_skeletons.end()) {
            return it->second;
        }
    }
    auto s = std::make_shared<ActorSkeleton>();
    s->nodes = osk::read(read_file(m_root / path));
    s->rest.resize(s->nodes.bones.size());
    for (size_t i = 0; i < s->nodes.bones.size(); ++i) {
        const auto& b = s->nodes.bones[i];
        s->by_name.emplace(lower(b.name), static_cast<uint32_t>(i));
        s->rest_local.push_back(local_matrix(b));
        s->rest[i] = b.parent >= 0 ? s->rest[size_t(b.parent)] * s->rest_local[i] : s->rest_local[i];
    }
    std::lock_guard lock(m_mutex);
    return m_skeletons.try_emplace(path, std::move(s)).first->second;
}

// A behaviour project, loaded once by whichever worker needs it first.
std::shared_ptr<const anim::Project> ActorData::project(const std::string& path) {
    std::shared_future<std::shared_ptr<const anim::Project>> future;
    std::promise<std::shared_ptr<const anim::Project>> promise;
    bool mine = false;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_projects.find(path);
        if (it == m_projects.end()) {
            future = promise.get_future().share();
            m_projects.emplace(path, future);
            mine = true;
        } else {
            future = it->second;
        }
    }
    if (mine) {
        std::shared_ptr<const anim::Project> p;
        try {
            p = anim::Project::load(m_root, path);
        } catch (const std::exception& e) {
            log::warn("actors: {}: {}", path, e.what());
        }
        promise.set_value(std::move(p));
    }
    return future.get();
}

std::string ActorData::idle_event(std::string_view editor_id) const {
    auto it = m_idles.find(lower(editor_id));
    const auto* e = it != m_idles.end() ? m_db->find(it->second) : nullptr;
    if (!e) {
        return {};
    }
    const forms::Form f(*m_db, *e);
    const auto* v = f.get("ENAM");
    return v ? v->text : std::string();
}

uint32_t ActorData::land_material(uint32_t ltex) const {
    const auto* e = ltex ? m_db->find(ltex) : nullptr;
    return e ? member_form(forms::Form(*m_db, *e), "MNAM") : 0;
}

std::string ActorData::skinned_path(std::string_view model) const {
    auto p = path::normalize(model);
    if (p.empty()) {
        return p;
    }
    p = path::with_root(p, "meshes");
    // Both ends of a weight-morphed model are one file, named after its _1 end.
    if (p.size() > 6 && p[p.size() - 6] == '_' && p[p.size() - 5] == '0') {
        std::string heavy = p;
        heavy[heavy.size() - 5] = '1';
        const auto out = path::replace_extension("skinned/" + heavy, ".osm");
        if (std::filesystem::exists(m_root / out)) {
            return out;
        }
    }
    return path::replace_extension("skinned/" + p, ".osm");
}

// The level a leveled list is calculated at for a reference: the encounter zone's for a
// new game's level 1 player, then the difficulty its XLCM names. `modifier` is that XLCM, -1 without one. The game steps
// the level through iCalcLevelAdjustDown..Up from one call to the next (0 in Skyrim.esm); here the step is seeded by the
// reference. WorldSim::zone_level is the latched level once a game has moved on from level 1.
void ActorData::set_zone_level(uint32_t zone, int32_t level) {
    std::lock_guard lock(m_level_mutex);
    m_zone_levels[zone] = level;
}

void ActorData::set_outfit(uint32_t npc, uint32_t outfit) {
    std::lock_guard lock(m_level_mutex);
    m_outfits[npc] = outfit;
}

void ActorData::npc_equip(uint32_t ref, uint32_t form, bool on) {
    std::lock_guard lock(m_level_mutex);
    NpcGear& gear = m_gear[ref];
    std::erase(gear.on, form);
    std::erase(gear.off, form);
    (on ? gear.on : gear.off).push_back(form);
}

void ActorData::npc_bare(uint32_t ref) {
    std::lock_guard lock(m_level_mutex);
    const bool headless = m_gear[ref].headless;
    m_gear[ref] = {.bare = true, .headless = headless};
}

void ActorData::npc_headless(uint32_t ref, bool headless) {
    std::lock_guard lock(m_level_mutex);
    m_gear[ref].headless = headless;
}

bool ActorData::npc_is_headless(uint32_t ref) const {
    std::lock_guard lock(m_level_mutex);
    const auto it = m_gear.find(ref);
    return it != m_gear.end() && it->second.headless;
}

std::unordered_map<uint32_t, NpcGear> ActorData::npc_gear() const {
    std::lock_guard lock(m_level_mutex);
    return m_gear;
}

void ActorData::set_npc_gear(std::unordered_map<uint32_t, NpcGear> gear) {
    std::lock_guard lock(m_level_mutex);
    m_gear = std::move(gear);
}

void ActorData::reroll(uint32_t ref) {
    std::lock_guard lock(m_level_mutex);
    ++m_rerolls[ref];
}

std::unordered_map<uint32_t, uint32_t> ActorData::rerolls() const {
    std::lock_guard lock(m_level_mutex);
    return m_rerolls;
}

void ActorData::set_rerolls(std::unordered_map<uint32_t, uint32_t> rerolls) {
    std::lock_guard lock(m_level_mutex);
    m_rerolls = std::move(rerolls);
}

int32_t ActorData::spawn_level(uint32_t ref, int32_t& modifier) const {
    const int32_t player_level = m_player_level;
    modifier = -1;
    int32_t level = player_level;
    uint32_t zone = 0;
    const auto* re = ref ? m_db->find(ref) : nullptr;
    if (re) {
        const forms::Form placed(*m_db, *re);
        zone = member_form(placed, "XEZN");
        if (const auto* cell = !zone ? m_db->find(re->parent) : nullptr) {
            zone = member_form(forms::Form(*m_db, *cell), "XEZN");
            if (const auto* world = !zone && cell->parent ? m_db->find(cell->parent) : nullptr) {
                zone = member_form(forms::Form(*m_db, *world), "XEZN");
            }
        }
        if (const auto* v = placed.get("XLCM")) {
            modifier = static_cast<int32_t>(v->integer);
        }
    } else {
        std::lock_guard lock(m_level_mutex);
        if (const auto it = m_created.find(ref); it != m_created.end()) {
            zone = it->second.zone;
            modifier = it->second.modifier;
        }
    }
    {
        if (const auto* ze = zone ? m_db->find(zone) : nullptr) {
            const forms::Form z(*m_db, *ze);
            const auto min_level = static_cast<int32_t>(static_cast<int8_t>(member_int(z, "DATA.min_level")));
            const auto max_level = static_cast<int32_t>(static_cast<int8_t>(member_int(z, "DATA.max_level")));
            const auto flags = member_int(z, "DATA.flags");
            if (player_level >= min_level) {
                level = static_cast<int32_t>(float(player_level - min_level) * m_level_scaling) + min_level;
            } else if (!(flags & 2)) {
                level = min_level;
            }
            if (max_level != 0) {
                level = std::min(level, max_level);
            }
            // A zone's level latches once, as the player first enters it; an unlatched one is worked out live.
            std::lock_guard lock(m_level_mutex);
            if (const auto it = m_zone_levels.find(zone); it != m_zone_levels.end()) {
                level = it->second;
            }
        }
    }
    const auto steps = static_cast<uint32_t>(m_level_adjust_up + m_level_adjust_down + 1);
    level = std::max(level + static_cast<int32_t>(mix_seed(ref, 3) % steps) - m_level_adjust_down, 1);
    const float mult = modifier >= 0 && modifier < 5 ? m_level_mults[modifier] : 1.0f;
    return std::max(int32_t(float(level) * mult), 1);
}

// A list's result at a level. The game draws from its shared generator; here the choice is
// seeded by the reference, so it doesn't depend on load order.
std::vector<uint32_t> ActorData::leveled_pick(const forms::Form& list, int32_t level, int32_t modifier,
                                              uint32_t seed) const {
    std::vector<uint32_t> out;
    auto leveled = forms::leveled_list(list);
    if (!leveled || leveled->entries.empty()) {
        return out;
    }
    std::stable_sort(leveled->entries.begin(), leveled->entries.end(),
                     [](const auto& a, const auto& b) { return a.level < b.level; });
    bool all_levels = leveled->flags & 1;
    if (modifier == 0) {
        all_levels = true;
    } else if (modifier >= 1 && modifier <= 3) {
        all_levels = false;
    }
    if (all_levels) {
        level = std::min<int32_t>(level, leveled->entries.back().level);
    }
    if (mix_seed(seed, list.id()) % 100 < leveled->chance_none) {
        return out;
    }
    int32_t top = -1;
    for (const auto& e : leveled->entries) {
        if (e.level <= level) {
            top = e.level;
        }
    }
    if (modifier == 3 && top < level) { // steps up to the next level above
        for (const auto& e : leveled->entries) {
            if (e.level > level) {
                top = e.level;
                level = top;
                break;
            }
        }
    }
    std::vector<uint32_t> candidates;
    for (const auto& e : leveled->entries) {
        if (e.level <= level && (all_levels || (leveled->flags & 4) || e.level == top)) {
            candidates.push_back(e.form);
        }
    }
    if (candidates.empty()) {
        return out;
    }
    if (leveled->flags & 4) { // use all
        return candidates;
    }
    out.push_back(candidates[mix_seed(seed, list.id() ^ 0x5bd1e995u) % candidates.size()]);
    return out;
}

void ActorData::set_created(uint32_t ref, uint32_t zone, int32_t modifier) {
    std::lock_guard lock(m_level_mutex);
    m_created[ref] = {zone, modifier};
}

std::vector<std::array<uint32_t, 3>> ActorData::created() const {
    std::lock_guard lock(m_level_mutex);
    std::vector<std::array<uint32_t, 3>> out;
    for (const auto& [ref, c] : m_created) {
        out.push_back({ref, c.zone, uint32_t(c.modifier)});
    }
    std::ranges::sort(out);
    return out;
}

uint32_t ActorData::created_npc(uint32_t base, uint32_t ref) const {
    int32_t modifier = -1;
    const int32_t level = spawn_level(ref, modifier);
    return pick_npc(base, level, modifier, ref, 0);
}

// A template's NPC: itself, or the leveled list's pick; 0 when the list gives none (the reference then goes).
uint32_t ActorData::pick_npc(uint32_t id, int32_t level, int32_t modifier, uint32_t seed, int depth) const {
    const auto* e = m_db->find(id);
    if (!e || depth > 8) {
        return 0;
    }
    if (e->type == forms::sig("NPC_")) {
        return id;
    }
    const auto picked = leveled_pick(forms::Form(*m_db, *e), level, modifier, seed);
    return picked.empty() ? 0 : pick_npc(picked.front(), level, modifier, mix_seed(seed, picked.front()), depth + 1);
}

// Where a template-using NPC gets the traits `flag` covers: the end of its template chain, 0 for none.
uint32_t ActorData::template_root(uint32_t npc, uint16_t flag, int32_t level, int32_t modifier, uint32_t seed) const {
    for (int depth = 0; depth < 16; ++depth) {
        const auto* e = m_db->find(npc);
        if (!e) {
            return npc;
        }
        const forms::Form f(*m_db, *e);
        const uint32_t next = member_form(f, "TPLT");
        if (!next || !(member_int(f, "ACBS.template_flags") & flag)) {
            return npc;
        }
        npc = pick_npc(next, level, modifier, seed, 0);
        if (!npc) {
            return 0;
        }
    }
    return npc;
}

void ActorData::add_armor(const forms::Form& armor, ActorLook& look, const forms::Form& race, Biped& biped,
                          float weight) const {
    // Every addon for the race goes on. A race with an armour race (RACE RNAM) is judged by that race alone.
    uint32_t wearer = member_form(race, "RNAM");
    if (!wearer) {
        wearer = race.id();
    }
    const auto armor_slots = static_cast<uint32_t>(biped_slots(armor));
    for (const auto* v : armor.all("MODL")) {
        const auto* e = m_db->find(v->form);
        if (!e) {
            continue;
        }
        const forms::Form addon(*m_db, *e);
        bool fits = member_form(addon, "RNAM") == wearer;
        for (const auto* extra : addon.all("MODL")) {
            fits = fits || extra->form == wearer;
        }
        if (!fits) {
            continue;
        }
        // The model for the sex and view: a female falls back on the male's, first person on third.
        const forms::Value* model = nullptr;
        uint32_t model_variant = ots::variant_male;
        auto choose = [&](const char* field) {
            if (!model || model->text.empty()) {
                model = addon.get(field);
                model_variant = uint32_t(field[3] - '2') + ots::variant_male; // MOD2 to MOD5, the lists' order
            }
        };
        if (look.first_person) {
            if (look.female) {
                choose("MOD5");
            }
            choose("MOD4");
        }
        if (look.female) {
            choose("MOD3");
        }
        choose("MOD2");
        if (!model || model->text.empty()) {
            continue;
        }
        WornAddon a;
        a.slots = static_cast<uint32_t>(biped_slots(addon));
        if (look.first_person) {
            a.slots &= static_cast<uint32_t>(member_int(race, race.get("BOD2") ? "BOD2.first_person_flags" : "BODT.first_person_flags"));
        }
        a.priority = static_cast<uint8_t>(member_int(addon, look.female ? "DNAM.female_priority" : "DNAM.male_priority"));
        a.swap_list = addon.get("NAM2") != nullptr;
        a.weapon_adjust = member_real(addon, "DNAM.weapon_adjust", 0.0f);
        a.footstep_set = member_form(addon, "SNDD");
        a.part.mesh = skinned_path(model->text);
        if (m_addon_swaps) {
            if (const auto it = m_addon_swaps->find(uint64_t(addon.id()) << 3 | model_variant); it != m_addon_swaps->end()) {
                a.part.swap = &it->second;
            }
        }
        // The slider on blends the two ends; off, the record's own file draws alone.
        const bool slider = member_int(addon, look.female ? "DNAM.female_weight_slider" : "DNAM.male_weight_slider") > 1;
        const auto& name = model->text;
        a.part.morph_weight = slider ? weight : name.size() > 5 && name[name.size() - 5] == '0' ? 0.0f : 1.0f;
        if (const auto* t = addon.get(look.female ? "NAM1" : "NAM0"); t && t->form) {
            if (const auto* te = m_db->find(t->form)) {
                const forms::Form set(*m_db, *te);
                a.part.diffuse = texture_path(set.get("TX00"));
                a.part.normal = texture_path(set.get("TX01"));
                a.part.specular = texture_path(set.get("TX07"));
            }
        }
        biped.place(armor.id(), armor_slots, addon.id(), std::move(a));
    }
}

// An outfit's entry: an armour, or a leveled list of them.
void ActorData::wear(uint32_t item, int32_t level, uint32_t seed, ActorLook& look, const forms::Form& race, Biped& biped,
                     float weight, uint32_t& worn_mask, int depth, std::span<const uint32_t> off) const {
    const auto* e = m_db->find(item);
    if (!e || depth > 8) {
        return;
    }
    const forms::Form f(*m_db, *e);
    if (e->type == forms::sig("ARMO")) {
        if (std::ranges::contains(off, item)) {
            return; // what a leveled outfit rolled, since taken off
        }
        add_armor(f, look, race, biped, weight);
        look.armor.push_back(item);
        worn_mask |= static_cast<uint32_t>(biped_slots(f));
    } else if (e->type == forms::sig("LVLI")) {
        for (uint32_t pick : leveled_pick(f, level, -1, seed)) {
            wear(pick, level, mix_seed(seed, pick), look, race, biped, weight, worn_mask, depth + 1, off);
        }
    }
}

// The weapons an NPC gears up with, shown sheathed: the best of each type, the top Geared Up Weapons of those
// Staffs show nothing sheathed, and left-hand weapons aren't picked here.
void ActorData::add_weapons(const forms::Form& inventory, const forms::Form& traits, std::span<const uint32_t> outfit,
                            int32_t level, uint32_t seed, ActorLook& out) const {
    constexpr uint8_t k_staff = 8;
    constexpr float k_dps_melee_speed_mult = 1.0f; // fCombatDPSMeleeSpeedMult
    constexpr float k_dps_bow_speed_mult = 0.2f;   // fCombatDPSBowSpeedMult
    const auto count = static_cast<size_t>(member_int(inventory, "DNAM.geared_up_weapons"));
    if (count == 0) {
        return;
    }
    float melee_mult = 1.0f, ranged_mult = 1.0f;
    if (const auto* style = m_db->find(member_form(traits, "ZNAM"))) {
        const forms::Form s(*m_db, *style);
        melee_mult = member_real(s, "CSGD.melee_score_mult", 1.0f);
        ranged_mult = member_real(s, "CSGD.ranged_score_mult", 1.0f);
    }
    struct Best {
        std::string model;
        float score = -1.0f;
        int64_t value = 0;
        uint32_t form = 0;
    };
    std::array<Best, 10> best;
    Best staff; // fights draw it for its enchantment; sheathed it shows nothing
    auto consider = [&](uint32_t id, auto&& self, int depth) -> void {
        const auto* e = m_db->find(id);
        if (!e || depth > 8) {
            return;
        }
        const forms::Form f(*m_db, *e);
        if (e->type == forms::sig("LVLI")) {
            for (uint32_t pick : leveled_pick(f, level, -1, mix_seed(seed, id))) {
                self(pick, self, depth + 1);
            }
            return;
        }
        if (e->type != forms::sig("WEAP")) {
            return;
        }
        const auto type = static_cast<uint8_t>(member_int(f, "DNAM.animation_type"));
        const auto* model = f.get("MODL");
        if (type == k_staff && model && f.get("EITM")) {
            staff = {model->text, 0.0f, 0, id};
        }
        if (type == 0 || type >= best.size() || type == k_staff || !model || (member_int(f, "DNAM.flags") & 1)) {
            return;
        }
        const bool ranged = type == 7 || type == 9;
        float score = float(member_int(f, "DATA.damage")) * member_real(f, "DNAM.speed", 1.0f) *
                      (ranged ? k_dps_bow_speed_mult * ranged_mult : k_dps_melee_speed_mult * melee_mult);
        const auto value = static_cast<int64_t>(member_int(f, "DATA.value"));
        Best& b = best[type];
        if (score > b.score || (score == b.score && value > b.value)) {
            b = {model->text, score, value, id};
        }
    };
    for (const auto& item : forms::container_items(inventory)) {
        consider(item.item, consider, 0);
    }
    for (uint32_t item : outfit) {
        consider(item, consider, 0);
    }
    if (best[5].score >= 0.0f && best[6].score >= 0.0f) { // two-handed swords and axes share a place
        best[best[5].score < best[6].score ? 5 : 6] = Best{};
    }
    std::vector<const Best*> ranked;
    for (const auto& b : best) {
        if (b.score >= 0.0f) {
            ranked.push_back(&b);
        }
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const Best* a, const Best* b) { return a->score > b->score; });
    for (size_t i = 0; i < ranked.size(); ++i) {
        const auto type = static_cast<int32_t>(ranked[i] - best.data());
        if (i == 0) {
            out.right_hand_type = type;
            out.right_weapon = ranked[i]->form;
        }
        out.weapons.emplace_back(ranked[i]->form, type);
        ActorPart p;
        p.mesh = skinned_path(ranked[i]->model);
        p.weapon = ranked[i]->form;
        p.spare = i >= count;
        out.parts.push_back(std::move(p));
    }
    if (staff.form) {
        out.weapons.emplace_back(staff.form, k_staff);
        ActorPart p;
        p.mesh = skinned_path(staff.model);
        p.weapon = staff.form;
        p.spare = true;
        out.parts.push_back(std::move(p));
    }
}

// The race's decapitate armor (RACE DNAM, by sex) and blood, kept for the moment they're wanted.
void ActorData::add_stump(const forms::Form& race, float weight, ActorLook& out) const {
    out.decapitate_art = member_form(race, "NAM7");
    const auto* e = m_db->find(member_form(race, out.female ? "DNAM.female" : "DNAM.male"));
    if (!e || e->type != forms::sig("ARMO")) {
        return;
    }
    const forms::Form armor(*m_db, *e);
    Biped biped;
    add_armor(armor, out, race, biped, weight);
    if (!biped.addons.empty()) {
        out.stump = biped.addons.front().part;
        out.stump.slots = static_cast<uint32_t>(biped_slots(armor));
    }
}

// Weapons scripts took away go, and one they gave takes the hand.
void ActorData::add_gear_weapons(const NpcGear& gear, ActorLook& out) const {
    const auto gone = [&gear](uint32_t weapon) { return weapon && (gear.bare || std::ranges::contains(gear.off, weapon)); };
    std::erase_if(out.parts, [&](const ActorPart& p) { return gone(p.weapon); });
    std::erase_if(out.weapons, [&](const auto& w) { return gone(w.first); });
    if (gone(out.right_weapon)) {
        out.right_weapon = out.weapons.empty() ? 0 : out.weapons.front().first;
        out.right_hand_type = out.weapons.empty() ? 0 : out.weapons.front().second;
        for (ActorPart& p : out.parts) {
            p.spare = p.spare && p.weapon != out.right_weapon;
        }
    }
    for (const uint32_t id : gear.on) {
        const auto* e = m_db->find(id);
        if (!e || e->type != forms::sig("WEAP")) {
            continue;
        }
        const forms::Form w(*m_db, *e);
        const forms::Value* model = w.get("MODL");
        if (!model || model->text.empty()) {
            continue;
        }
        for (ActorPart& p : out.parts) {
            p.spare = p.spare || p.weapon;
        }
        std::erase_if(out.parts, [id](const ActorPart& p) { return p.weapon == id; });
        std::erase_if(out.weapons, [id](const auto& x) { return x.first == id; });
        out.right_weapon = id;
        out.right_hand_type = static_cast<int32_t>(member_int(w, "DNAM.animation_type"));
        out.weapons.insert(out.weapons.begin(), {id, out.right_hand_type});
        ActorPart p;
        p.mesh = skinned_path(model->text);
        p.weapon = id;
        out.parts.push_back(std::move(p));
    }
}

// The player's weapon and quiver: the right hand's on its Prn node, in first person its WNAM model; ammo
// in slot 41, on the third-person body only.
void ActorData::add_equipped(const std::vector<uint32_t>& items, uint32_t right, uint32_t left, ActorLook& out) const {
    const auto hold = [&](uint32_t form, bool left_hand) {
        const auto* e = form ? m_db->find(form) : nullptr;
        if (!e || e->type != forms::sig("WEAP")) {
            return;
        }
        const forms::Form w(*m_db, *e);
        const forms::Value* model = w.get("MODL");
        if (out.first_person) {
            const auto* stat = m_db->find(member_form(w, "WNAM"));
            model = stat ? forms::Form(*m_db, *stat).get("MODL") : nullptr;
        }
        const auto type = static_cast<int32_t>(member_int(w, "DNAM.animation_type"));
        if (left_hand) {
            out.left_hand_type = type;
        } else {
            out.right_hand_type = type;
            out.right_weapon = form;
        }
        if (model && !model->text.empty()) {
            ActorPart p;
            p.mesh = skinned_path(model->text);
            p.no_scabbard = out.first_person || left_hand;
            p.left_hand = left_hand;
            out.parts.push_back(std::move(p));
        }
    };
    hold(right, false);
    hold(left, true);
    if (out.first_person) {
        return;
    }
    for (const uint32_t item : items) {
        if (const auto* e = m_db->find(item); e && e->type == forms::sig("AMMO")) {
            const forms::Form ammo(*m_db, *e);
            if (const auto* model = ammo.get("MODL"); model && !model->text.empty()) {
                ActorPart p;
                p.mesh = skinned_path(model->text);
                out.parts.push_back(std::move(p));
            }
        }
    }
}

void ActorData::set_player_beast(uint32_t race) {
    std::lock_guard lock(m_face_mutex);
    m_player_beast = race;
}

void ActorData::set_player_worn(std::vector<uint32_t> items, uint32_t right, uint32_t left) {
    std::lock_guard lock(m_level_mutex);
    m_player_worn = std::move(items);
    m_player_right = right;
    m_player_left = left;
}

// A head with no FaceGen model: the NPC's head parts, with the race's default for each part type it lacks.
// The head parts the game lists for an NPC: its own, then the race's defaults for what it lacks; extra parts
// join as they are met.
std::vector<uint32_t> ActorData::head_parts(const forms::Form& traits, const forms::Form& race, bool is_female,
                                            const std::vector<uint32_t>* own) const {
    std::vector<uint32_t> parts;
    if (own) {
        parts = *own;
    } else {
        for (const auto* v : traits.all("PNAM")) {
            parts.push_back(v->form);
        }
    }
    // The race's head data, per sex: HEAD fields after its MNAM / FNAM marker, past NAM0.
    bool head_data = false, female = false;
    std::vector<uint32_t> defaults;
    for (const auto& field : race.fields()) {
        if (field.sig == forms::sig("NAM0")) {
            head_data = true;
        } else if (head_data && field.sig == forms::sig("MNAM")) {
            female = false;
        } else if (head_data && field.sig == forms::sig("FNAM")) {
            female = true;
        } else if (head_data && field.sig == forms::sig("HEAD") && female == is_female) {
            defaults.push_back(field.value.form);
        }
    }
    std::unordered_set<uint64_t> types;
    auto part_type = [&](uint32_t id) -> uint64_t {
        const auto* e = m_db->find(id);
        return e ? member_int(forms::Form(*m_db, *e), "PNAM", ~0ull) : ~0ull;
    };
    for (uint32_t p : parts) {
        types.insert(part_type(p));
    }
    for (uint32_t p : defaults) {
        if (types.insert(part_type(p)).second) {
            parts.push_back(p);
        }
    }
    std::unordered_set<uint32_t> seen;
    for (size_t i = 0; i < parts.size(); ++i) {
        const auto* e = m_db->find(parts[i]);
        if (e && seen.insert(parts[i]).second) {
            for (const auto* extra : forms::Form(*m_db, *e).all("HNAM")) {
                parts.push_back(extra->form);
            }
        }
    }
    std::erase_if(parts, [&, keep = std::unordered_set<uint32_t>()](uint32_t p) mutable { return !keep.insert(p).second; });
    return parts;
}

// Which morph files a head part has (HDPT NAM0, NAM1).
FacePart ActorData::face_part(const forms::Form& part, std::string shape) const {
    FacePart out;
    out.shape = std::move(shape);
    int64_t role = -1;
    for (const auto& field : part.fields()) {
        if (field.sig == forms::sig("NAM0")) {
            role = field.value.integer;
        } else if (field.sig == forms::sig("NAM1") && !field.value.text.empty()) {
            const std::string file =
                "skinned/" + path::replace_extension(path::with_root(path::normalize(field.value.text), "meshes"), ".ofm");
            (role == 1 ? out.tri : role == 0 ? out.race : out.chargen) = file;
        }
    }
    return out;
}

void ActorData::set_player_face(const game::FaceData& face) {
    std::lock_guard lock(m_face_mutex);
    m_player_face = face;
}

// What an assembled head bakes in: the race's morph, the presets, the sliders, and
// SkinnyMorph from the weight.
void ActorData::bake_morphs(const game::FaceData& face, const forms::Form& race, ActorLook& out) const {
    const forms::Form* named = &race;
    std::optional<forms::Form> morph_race;
    if (const uint32_t id = member_form(race, "NAM8")) {
        if (const auto* e = m_db->find(id)) {
            named = &morph_race.emplace(*m_db, *e);
        }
    }
    out.race_morph = std::string(named->editor_id());
    constexpr const char* k_presets[4] = {"NoseType", "BrowType", "EyesType", "LipType"};
    for (int i = 0; i < 4; ++i) {
        if (face.presets[size_t(i)] > 0) {
            out.chargen_morphs.emplace_back(std::format("{}{}", k_presets[i], face.presets[size_t(i)]), 1.0f);
        }
    }
    constexpr std::array<std::array<const char*, 2>, 18> k_sliders = {{
        {"NoseShort", "NoseLong"}, {"NoseDown", "NoseUp"}, {"JawUp", "JawDown"}, {"JawNarrow", "JawWide"},
        {"JawBack", "JawForward"}, {"CheeksDown", "CheeksUp"}, {"CheeksIn", "CheeksOut"}, {"EyesMoveDown", "EyesMoveUp"},
        {"EyesMoveIn", "EyesMoveOut"}, {"BrowDown", "BrowUp"}, {"BrowIn", "BrowOut"}, {"BrowBack", "BrowForward"},
        {"LipMoveDown", "LipMoveUp"}, {"LipMoveIn", "LipMoveOut"}, {"ChinThin", "ChinWide"}, {"ChinMoveUp", "ChinMoveDown"},
        {"Overbite", "Underbite"}, {"EyesBack", "EyesForward"}}};
    for (size_t i = 0; i < k_sliders.size(); ++i) {
        const float v = face.morphs[i];
        if (v != game::k_morph_unset && v != 0.0f) {
            out.chargen_morphs.emplace_back(k_sliders[i][v < 0.0f ? 0 : 1], std::abs(v));
        }
    }
    out.skinny = 1.0f - std::clamp(face.weight / 100.0f, 0.0f, 1.0f);
}

void ActorData::add_head_parts(const forms::Form& traits, const forms::Form& race, ActorLook& out) const {
    std::optional<game::FaceData> mine;
    if (out.player) {
        std::lock_guard lock(m_face_mutex);
        mine = m_player_face;
    }
    const game::FaceData face = mine ? *mine : game::Faces(*m_db).from_npc(out.npc);
    bake_morphs(face, race, out);
    if (mine) {
        out.skin_tone = glm::vec3(mine->skin_tone[0], mine->skin_tone[1], mine->skin_tone[2]) / 255.0f;
        out.has_skin_tone = true;
    }
    auto tint = mine && mine->tint_edited ? compose_face_tint(m_root, face.tints) : std::vector<std::byte>{};
    if (tint.empty() && out.has_skin_tone) {
        tint = plain_face_tint(out.skin_tone);
    }
    if (!tint.empty()) {
        out.face_tint = std::make_shared<const std::vector<std::byte>>(std::move(tint));
    }
    const std::vector<uint32_t> parts = head_parts(traits, race, out.female, mine ? &mine->head_parts : nullptr);
    // Face texture set: the NPC's, else the race's default, else the part's own.
    uint32_t face_set = mine ? mine->face_details : member_form(traits, "FTST");
    if (!face_set) {
        face_set = member_form(race, out.female ? "DFTF" : "DFTM");
    }
    // Hair colour: the part's own, else the NPC's, else the race's default; stored doubled.
    uint32_t hair = mine ? mine->hair_color : member_form(traits, "HCLF");
    if (!hair) {
        if (const auto* h = race.get(out.female ? "HCLF.female" : "HCLF.male")) {
            hair = h->form;
        }
    }
    for (const uint32_t id : parts) {
        const auto* e = m_db->find(id);
        if (!e) {
            continue;
        }
        const forms::Form part(*m_db, *e);
        const auto* model = part.get("MODL");
        if (!model || model->text.empty()) {
            continue;
        }
        ActorPart p;
        p.mesh = skinned_path(model->text);
        p.head = true;
        p.assembled = true;
        const uint32_t set_id = member_int(part, "PNAM", ~0ull) == 1 && face_set ? face_set : member_form(part, "TNAM");
        if (const auto* set = set_id ? m_db->find(set_id) : nullptr) {
            const forms::Form t(*m_db, *set);
            p.diffuse = texture_path(t.get("TX00"));
            p.normal = texture_path(t.get("TX01"));
            p.specular = texture_path(t.get("TX07"));
        }
        const uint32_t color = member_form(part, "CNAM") ? member_form(part, "CNAM") : hair;
        if (const auto* he = color ? m_db->find(color) : nullptr) {
            p.hair_color = clfm_color(forms::Form(*m_db, *he)) * 2.0f;
            p.has_hair_color = true;
        }
        p.face.push_back(face_part(part, std::string()));
        out.parts.push_back(std::move(p));
    }
}

ActorLook ActorData::look(uint32_t npc_id, uint32_t ref, bool first_person) {
    ActorLook out;
    out.first_person = first_person;
    uint32_t seed = ref ? ref : npc_id;
    {
        std::lock_guard lock(m_level_mutex);
        if (const auto it = m_rerolls.find(ref); it != m_rerolls.end()) {
            seed = mix_seed(seed, it->second);
        }
    }
    int32_t modifier = -1;
    const int32_t level = spawn_level(ref, modifier);
    const uint32_t traits_id = template_root(npc_id, k_template_traits, level, modifier, seed);
    const uint32_t inventory_id = template_root(npc_id, k_template_inventory, level, modifier, seed);
    const auto* te = m_db->find(traits_id);
    const auto* ie = m_db->find(inventory_id);
    if (!te || !ie) {
        return out;
    }
    const forms::Form traits(*m_db, *te);
    const forms::Form inventory(*m_db, *ie);
    out.npc = traits_id;
    out.inventory_npc = inventory_id;
    out.female = (member_int(traits, "ACBS.flags") & k_npc_female) != 0;
    uint32_t race_id = member_form(traits, "RNAM");
    bool beast = false; // a werewolf or Vampire Lord: the race's body alone
    out.player = ref == k_player_ref && !first_person;
    out.mood = static_cast<int>(member_int(traits, "AIDT.mood", 0));
    if (ref == k_player_ref) {
        std::lock_guard lock(m_face_mutex);
        if (m_player_face) {
            out.female = m_player_face->female;
            race_id = m_player_face->race;
        }
        beast = m_player_beast != 0;
        race_id = beast ? m_player_beast : race_id;
    }
    const auto* re = m_db->find(race_id);
    if (!re) {
        return out;
    }
    const forms::Form race(*m_db, *re);

    // Skeleton: the race's, the male one when it has no female one.
    const auto skeletons = race.all("ANAM");
    const forms::Value* skeleton_model = skeletons.empty() ? nullptr : skeletons[0];
    if (out.female && skeletons.size() > 1 && !skeletons[1]->text.empty()) {
        skeleton_model = skeletons[1];
    }
    // What an action's idle goes by; the first-person graph takes the same events.
    const auto graphs = race.all("behavior_graph");
    const forms::Value* graph = graphs.empty() ? nullptr : graphs[out.female && graphs.size() > 1 ? 1 : 0];
    if (graph && !graph->text.empty()) {
        out.behaviour_folder = graph->text.substr(0, graph->text.find_last_of('\\'));
    }
    if (first_person) {
        // One skeleton and one graph for every race: sFirstPersonSkeleton, strPlayerCharacterBehavior1stPGraph:Animation.
        out.skeleton = skeleton("skinned/meshes/actors/character/_1stperson/skeleton.osk");
        out.project = project("animations/meshes/actors/character/_1stperson/firstperson");
        // Actions are picked from the idle tree of the race's graph, whichever body shows: DNAM never names this one.
        const auto graphs = race.all("behavior_graph");
        if (const forms::Value* graph = graphs.empty() ? nullptr : graphs[out.female && graphs.size() > 1 ? 1 : 0]) {
            out.behaviour_folder = graph->text.substr(0, graph->text.find_last_of('\\'));
        }
    } else if (skeleton_model) {
        auto p = path::with_root(path::normalize(skeleton_model->text), "meshes");
        try {
            out.skeleton = skeleton(path::replace_extension("skinned/" + p, ".osk"));
            // The race's behaviour project for the sex: its NAM3 section's models, male then female.
            if (graph && !graph->text.empty()) {
                out.project = project(path::replace_extension("animations/" + path::with_root(path::normalize(graph->text), "meshes"), ""));
            }
        } catch (const std::exception& e) {
            log::debug("actors: {}: {}", p, e.what());
        }
    }

    const float race_height = member_real(race, out.female ? "DATA.female_height" : "DATA.male_height", 1.0f);
    out.race = race_id;
    const float npc_height = member_real(traits, "NAM6", 1.0f);
    out.height = race_height * (npc_height == 0.0f ? 1.0f : npc_height); // the game reads an exact 0 as 1
    // The player's is the face's: ChangeWeight sets NAM7 and redoes SkinnyMorph and both bipeds together.
    float weight_percent = member_real(traits, "NAM7", 50.0f);
    if (ref == k_player_ref) {
        std::lock_guard lock(m_face_mutex);
        if (m_player_face) {
            weight_percent = m_player_face->weight;
        }
    }
    const float weight = std::clamp(weight_percent / 100.0f, 0.0f, 1.0f);

    if (const auto* q = traits.get("QNAM")) {
        auto c = [&](const char* name) { // kept as bytes, truncated
            const auto* v = q->member(name);
            return v ? std::floor(float(v->real) * 255.0f) / 255.0f : 1.0f;
        };
        out.skin_tone = {c("r"), c("g"), c("b")};
        out.has_skin_tone = true;
    }

    // The biped: the skin first, then each worn item; what they claim decides the rest.
    Biped biped;
    uint32_t skin_id = beast ? 0 : member_form(traits, "WNAM");
    if (!skin_id) {
        skin_id = member_form(race, "WNAM");
    }
    if (const auto* se = skin_id ? m_db->find(skin_id) : nullptr) {
        add_armor(forms::Form(*m_db, *se), out, race, biped, weight);
    }
    uint32_t worn_mask = 0;
    std::vector<uint32_t> outfit_items;
    uint32_t outfit = member_form(inventory, "DOFT");
    std::optional<std::vector<uint32_t>> equipped;
    uint32_t right_hand = 0, left_hand = 0;
    NpcGear gear;
    {
        std::lock_guard lock(m_level_mutex);
        if (const auto it = m_outfits.find(npc_id); it != m_outfits.end()) {
            outfit = it->second;
        }
        if (const auto it = m_gear.find(ref); it != m_gear.end()) {
            gear = it->second;
        }
        if (npc_id == k_player_npc) {
            equipped = m_player_worn;
            right_hand = m_player_right;
            left_hand = m_player_left;
        }
    }
    if (equipped) {
        for (const uint32_t item : *equipped) {
            const auto* ie2 = m_db->find(item);
            if (ie2 && ie2->type == forms::sig("ARMO")) {
                wear(item, level, mix_seed(seed, item), out, race, biped, weight, worn_mask, 0);
            }
        }
    } else if (const auto* oe = outfit ? m_db->find(outfit) : nullptr) {
        const forms::Form o(*m_db, *oe);
        // What's put on takes its slots from what the outfit would have there.
        uint32_t taken = 0;
        for (const uint32_t item : gear.on) {
            if (const auto* ae = m_db->find(item); ae && ae->type == forms::sig("ARMO")) {
                taken |= static_cast<uint32_t>(biped_slots(forms::Form(*m_db, *ae)));
            }
        }
        if (const auto* items = o.get("INAM")) {
            for (const auto& item : items->items) {
                const auto* ae = m_db->find(item.form);
                const bool armor = ae && ae->type == forms::sig("ARMO");
                if (gear.bare || std::ranges::contains(gear.off, item.form) ||
                    (armor && (static_cast<uint32_t>(biped_slots(forms::Form(*m_db, *ae))) & taken))) {
                    continue;
                }
                outfit_items.push_back(item.form);
                wear(item.form, level, mix_seed(seed, item.form), out, race, biped, weight, worn_mask, 0, gear.off);
            }
        }
    }
    if (!equipped) {
        for (const uint32_t item : gear.on) {
            if (const auto* ae = m_db->find(item); ae && ae->type == forms::sig("ARMO")) {
                wear(item, level, mix_seed(seed, item), out, race, biped, weight, worn_mask, 0);
            }
        }
    }
    // weapAdj starts from the body's weight (fBodyMorphWeaponAdjustMult 0.5), and each
    // slot's addon adds its own, unless the addon is the item itself. The graph's poses follow it.
    float weap_adj = weight * 0.5f;
    // Each model once, with the slots that still hold it.
    for (uint32_t i = 0; i < 32; ++i) {
        const int32_t addon = biped.slots[i].addon;
        if (addon < 0) {
            continue;
        }
        const WornAddon& a = biped.addons[size_t(addon)];
        if (!biped.slots[i].lone) {
            weap_adj += a.weapon_adjust;
        }
        auto it = std::ranges::find_if(out.parts, [&](const ActorPart& p) { return p.mesh == a.part.mesh; });
        if (it == out.parts.end()) {
            out.parts.push_back(a.part);
            it = out.parts.end() - 1;
        }
        it->slots |= 1u << i;
        if (a.footstep_set && std::ranges::find(out.footstep_sets, a.footstep_set) == out.footstep_sets.end()) {
            out.footstep_sets.push_back(a.footstep_set);
        }
    }
    out.weap_adj = std::clamp(weap_adj, 0.0f, 1.0f);
    if (equipped) {
        add_equipped(*equipped, right_hand, left_hand, out);
    }
    if (first_person) {
        // The game reads the third-person biped, and here the boots aren't on these slots.
        out.footstep_sets = look(npc_id, ref, false).footstep_sets;
        return out; // the hands and forearms: the head stays with the third-person body
    }
    if (!equipped) {
        add_weapons(inventory, traits, outfit_items, level, seed, out);
        add_gear_weapons(gear, out);
    }
    add_stump(race, weight, out);
    out.headless = gear.headless;

    // The head, hidden whole when its slot is worn. Hair worn hides the hair slot's partitions; the addon in the hair
    // slot also hides those of the other slots it fills (long hair, ears).
    const int64_t head_slot = member_int(race, "DATA.head_biped_object", ~0ull);
    const int64_t hair_slot = member_int(race, "DATA.hair_biped_object", ~0ull);
    if (head_slot >= 0 && head_slot < 32 && ((worn_mask >> head_slot) & 1)) {
        return out;
    }
    if (hair_slot >= 0 && hair_slot < 32) {
        if ((worn_mask >> hair_slot) & 1) {
            out.head_hidden |= 1u << hair_slot;
            out.hair_worn = true;
        }
        if (const int32_t addon = biped.slots[size_t(hair_slot)].addon; addon >= 0) {
            for (uint32_t i = 0; i < 32; ++i) {
                if (biped.slots[i].addon == addon) {
                    out.head_hidden |= 1u << i;
                }
            }
        }
    }
    // The FaceGen model made for the NPC, under the plugin that first defined it.
    const auto sources = m_db->sources(*te);
    if (!sources.empty()) {
        const auto plugin = lower(m_db->plugin_name(sources[0]));
        const bool light = (m_db->plugins()[sources[0]].flags & forms::plugin_light) != 0;
        const auto id = std::format("{:08x}", traits_id & (light ? 0x00000FFFu : 0x00FFFFFFu));
        const auto head = "skinned/meshes/actors/character/facegendata/facegeom/" + plugin + "/" + id + ".osm";
        if (std::filesystem::exists(m_root / head)) {
            ActorPart part;
            part.mesh = head;
            part.head = true;
            const auto tint = "textures/actors/character/facegendata/facetint/" + plugin + "/" + id + ".dds";
            if (std::filesystem::exists(m_root / tint)) {
                part.face_tint = tint;
            }
            for (const uint32_t id : head_parts(traits, race, out.female)) {
                if (const auto* e = m_db->find(id)) {
                    const forms::Form p(*m_db, *e);
                    part.face.push_back(face_part(p, lower(p.editor_id())));
                }
            }
            out.parts.push_back(std::move(part));
            return out;
        }
    }
    if (!beast) {
        add_head_parts(traits, race, out);
    }
    return out;
}

ActorSet::ActorSet(render::Renderer& renderer, ActorData& data, std::shared_ptr<const osw::World> world)
    : m_renderer(renderer), m_data(data), m_world(std::move(world)) {
    m_placing = std::async(std::launch::async, [this] { return m_data.placed_in(*m_world); });
    m_children_loading = std::async(std::launch::async, [this] { return m_data.activate_children(*m_world); });
}

ActorSet::~ActorSet() {
    join();
    std::vector<uint32_t> refs;
    for (const auto& [ref, live] : m_live) {
        refs.push_back(ref);
    }
    for (uint32_t ref : refs) {
        end(ref);
    }
    if (m_placing.valid()) {
        m_placing.wait();
    }
}

void ActorSet::start(const PlacedActor& placed) {
    auto live = std::make_unique<Live>();
    live->placed = placed;
    live->visual.ref = placed.ref;
    live->visual.casts_shadow = placed.ref != k_player_first_person_ref;
    ActorData* data = &m_data;
    if (placed.object) {
        live->building = std::async(std::launch::async, [data, mesh = placed.mesh, base = placed.base] { return data->object_look(mesh, base); });
        m_live.emplace(placed.ref, std::move(live));
        return;
    }
    live->building = std::async(std::launch::async, [data, npc = placed.base, ref = placed.ref, first = placed.first_person] {
        return data->look(npc, first ? k_player_ref : ref, first);
    });
    m_live.emplace(placed.ref, std::move(live));
}

void ActorSet::end(uint32_t ref) {
    join();
    if (const auto it = m_live.find(ref); it != m_live.end()) {
        std::erase(m_animating, it->second.get());
    }
    auto it = m_live.find(ref);
    if (it == m_live.end()) {
        return;
    }
    Live& a = *it->second;
    if (a.severed && m_physics) {
        m_physics->remove_ragdoll(a.severed->ragdoll);
    }
    // Still being built: it finishes on its own, here where its result is dropped, not on this thread's time. Its future
    // would wait as it went.
    std::erase_if(m_abandoned, [](const std::future<ActorLook>& f) {
        return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    });
    if (a.building.valid()) {
        m_abandoned.push_back(std::move(a.building));
    }
    if (a.placed.object && a.finished) {
        m_object_changes.push_back({ref, false, {}, 1.0f});
    }
    if (a.animation && a.animation->corpse) {
        if (a.animation->ragdoll && m_physics) {
            m_physics->remove_ragdoll(a.animation->ragdoll);
        }
        m_corpses[ref] = std::move(a.animation->bodies);
        for (auto* list : {&m_placed, &m_placed_dead}) { // it starts again where it lies
            for (PlacedActor& p : *list) {
                if (p.ref == ref) {
                    p.position = a.placed.position;
                }
            }
        }
    }
    auto& meshes = m_renderer.meshes();
    for (Membrane& m : a.membranes) {
        drop_membrane(a, m);
    }
    drop_weapon_blood(a);
    for (const ActorSkinDecal& d : a.visual.skin_decals) {
        m_renderer.device().defer([data = &m_data, slot = d.slot] { data->free_skin_decal(slot); });
    }
    for (auto& p : a.parts) {
        if (p.variant != render::k_no_variant) {
            meshes.free_skinned_variant(p.variant);
        }
        if (p.mesh != render::k_invalid_mesh) {
            meshes.release(p.mesh);
        }
        if (!p.textures.empty()) {
            // Not `this`: the set can be gone when this runs.
            m_renderer.device().defer([renderer = &m_renderer, textures = std::move(p.textures)] {
                for (auto t : textures) {
                    renderer->textures().release(t);
                }
            });
        }
    }
    m_live.erase(it);
    m_visuals_dirty = true;
}

void ActorSet::finish_parts(Live& a, size_t first, size_t last, glm::vec3& lo, glm::vec3& hi) {
    auto& meshes = m_renderer.meshes();
    const ActorLook& look = a.look;
    for (size_t i = first; i < last; ++i) {
        Part& part = a.parts[i];
        if (!meshes.ready(part.mesh)) {
            log::warn("actors: {:08X}: {} not ready", a.placed.ref, part.source.mesh);
            continue;
        }
        const render::SkinnedModel* model = meshes.skinned_model(part.mesh);
        if (!model) {
            log::warn("actors: {:08X}: {} is not skinned", a.placed.ref, part.source.mesh);
            continue;
        }
        bind_face(a, part, *model);
        const auto& shapes = model->shapes;
        // Which dismember partitions of each shape show. Part ids count from 30: 130 and
        // 230 are the same slots on heads and bodies.
        auto ranges = [&](uint32_t k, render::MeshManager::IndexRuns& runs) {
            const render::SkinnedShape& shape = shapes[k];
            uint32_t total = 0;
            for (uint32_t i = 0; i < shape.part_count; ++i) {
                total += model->part_indices[shape.first_part + i];
            }
            if (shape.part_count == 0 && part.source.head && shape.type == 6 && look.hair_worn) {
                return;
            }
            if (total != shape.index_count) {
                runs.emplace_back(0u, shape.index_count); // not kept by partition: whole
                return;
            }
            uint32_t at = 0;
            for (uint32_t i = 0; i < shape.part_count; ++i) {
                const uint32_t n = model->part_indices[shape.first_part + i];
                int32_t p = model->parts[shape.first_part + i];
                p = p >= 230 && p <= 261 ? p - 200 : p >= 130 && p <= 161 ? p - 100 : p;
                const bool in_range = p >= 30 && p <= 61;
                const bool on = part.source.whole ? true : part.source.head ? !(in_range && ((look.head_hidden >> (p - 30)) & 1))
                                                 : in_range && ((part.source.slots >> (p - 30)) & 1);
                if (on && n > 0) {
                    if (!runs.empty() && runs.back().first + runs.back().second == at) {
                        runs.back().second += n;
                    } else {
                        runs.emplace_back(at, n);
                    }
                }
                at += n;
            }
        };
        auto swap = [&](const std::string& path, render::TextureKind kind, uint32_t& slot) {
            if (!path.empty()) {
                slot = m_renderer.textures().acquire(path, kind, 1.0f);
                part.textures.push_back(slot);
            }
        };
        const ActorPart& src = part.source;
        auto tone = [&](render::GpuMaterial& g) {
            if (look.has_skin_tone) {
                g.tint[0] = look.skin_tone.r;
                g.tint[1] = look.skin_tone.g;
                g.tint[2] = look.skin_tone.b;
            }
        };
        auto dress = [&](render::GpuMaterial& g) {
            swap(src.diffuse, render::TextureKind::color, g.tex[render::gtex_diffuse]);
            swap(src.normal, render::TextureKind::normal, g.tex[render::gtex_normal]);
        };
        // A FaceGen model keeps its colours; only its tint map is the file's.
        auto edit = [&](uint32_t k, render::GpuMaterial& g) {
            const uint32_t type = g.technique >> 24;
            if (src.swap) {
                const render::SkinnedModel* model = meshes.skinned_model(part.mesh);
                for (const ots::Shape& shape : src.swap->shapes) {
                    if (k < model->shape_names.size() && path::to_lower(model->shape_names[k]) == path::to_lower(shape.name)) {
                        swap(shape.textures[osm::texture_diffuse], render::TextureKind::color, g.tex[render::gtex_diffuse]);
                        swap(shape.textures[osm::texture_normal], render::TextureKind::normal, g.tex[render::gtex_normal]);
                    }
                }
            }
            if (!src.head) {
                if (type == 4) { // a Facegen shape on the body: FacegenTint, as the skin tints it
                    g.technique = (g.technique & 0x00FFFFFFu) | (5u << 24);
                }
                if (type == 4 || type == 5) {
                    tone(g);
                    dress(g);
                }
            } else if (src.assembled) {
                if (type == 4 || type == 5 || type == 16) { // 16: the eyes, whose colour is the part's TNAM
                    dress(g);
                }
                if (type == 4 && look.face_tint) {
                    const std::string name = std::format("made face tint {}", ++m_made_tints);
                    m_renderer.textures().provide(name, *look.face_tint);
                    swap(name, render::TextureKind::color, g.tex[render::gtex_height]);
                } else if (type == 4) { // the default map, as FillDefaultTextures leaves slot 6
                    g.tex[render::gtex_height] = m_renderer.textures().grey();
                }
                if (type == 5) {
                    tone(g);
                }
                if (type == 6 && src.has_hair_color) {
                    g.tint[0] = src.hair_color.r;
                    g.tint[1] = src.hair_color.g;
                    g.tint[2] = src.hair_color.b;
                }
            } else if (type == 4) {
                swap(src.face_tint, render::TextureKind::color, g.tex[render::gtex_height]);
            }
        };
        part.variant = meshes.skinned_variant(part.mesh, ranges, edit, part.targets);
        if (part.variant == render::k_no_variant || part.variant == render::MeshManager::k_no_material) {
            log::warn("actors: {:08X}: {}: no room for a variant", a.placed.ref, part.source.mesh);
            part.variant = render::k_no_variant;
            continue;
        }
        const glm::vec4 s = meshes.sphere(part.mesh);
        lo = glm::min(lo, glm::vec3(s) - s.w);
        hi = glm::max(hi, glm::vec3(s) + s.w);
        a.visual.draws.push_back({part.mesh, part.variant});
    }
}

void ActorSet::finish(Live& a) {
    const ActorLook& look = a.look;
    glm::vec3 lo(1e30f), hi(-1e30f);
    finish_parts(a, 0, a.parts.size(), lo, hi);
    a.finished = true;
    if (a.visual.draws.empty()) {
        return;
    }
    const float h = look.first_person ? 1.0f : look.height * a.placed.scale;
    Visual& v = a.visual;
    v.scale = h;
    v.position = a.placed.position;
    const float c = std::cos(a.placed.heading), s = std::sin(a.placed.heading);
    v.rotation = a.placed.object ? glm::mat3_cast(a.placed.rotation)
                                 : glm::mat3(glm::vec3(c, -s, 0.0f), glm::vec3(s, c, 0.0f), glm::vec3(0, 0, 1));
    const glm::vec3 centre = (lo + hi) * 0.5f;
    v.sphere = glm::vec4(v.rotation * (h * centre) + v.position, glm::length(hi - lo) * 0.5f * h);
    v.moved = true;
    start_animation(a);
    if (a.animation) {
        animate(a);
    } else if (a.loop) {
        run_loop(a);
    } else {
        pose(a);
    }
    m_visuals_dirty = true;
    if (a.placed.object) {
        m_object_changes.push_back({a.placed.ref, true, object_path(a.placed.mesh, ".osp"), v.scale});
    }
}

std::span<const ActivateChild> ActorSet::activate_children(uint32_t parent) {
    if (m_children_loading.valid() && m_children_loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        m_children = m_children_loading.get();
    }
    const auto it = m_children.find(parent);
    return it == m_children.end() ? std::span<const ActivateChild>{} : std::span<const ActivateChild>(it->second);
}

std::vector<ActorSet::ObjectChange> ActorSet::take_object_changes() {
    join();
    std::vector<ObjectChange> out;
    out.swap(m_object_changes);
    return out;
}

std::vector<ActorSet::ObjectMove> ActorSet::take_object_moves() {
    join();
    std::vector<ObjectMove> out;
    out.swap(m_object_moves);
    return out;
}

// The graph of a new actor, with the initial values the game gives one.
void ActorSet::start_animation(Live& a) {
    if (a.look.loop && a.look.skeleton) {
        auto loop = std::make_unique<ObjectLoop>();
        loop->effect = std::make_unique<fx::EffectInstance>(a.look.loop, a.placed.ref);
        const auto names = loop->effect->sequence_names();
        const auto& sequences = loop->effect->data().sequences;
        if (!names.empty() && !sequences.empty() && sequences.front().cycle_type == 0) {
            loop->effect->activate_sequence(names.front()); // the controller manager's auto-play
        }
        const ActorSkeleton& s = *a.look.skeleton;
        loop->nodes = s.rest;
        for (const auto& bone : s.nodes.bones) {
            loop->node_of.push_back(loop->effect->find_node(bone.name));
        }
        a.loop = std::move(loop);
        return;
    }
    if (!a.look.project || !a.look.skeleton) {
        return;
    }
    auto anim_state = std::make_unique<Animation>();
    Animation& m = *anim_state;
    m.character.random_state = mix_seed(a.placed.ref, 0x51ED270Bu);
    a.face.random = mix_seed(a.placed.ref, 0x1B73CAFEu);
    m.graph = std::make_unique<anim::Graph>(a.look.project, m.character);
    anim::Graph& g = *m.graph;
    const bool player = a.placed.ref == k_player_ref || a.look.first_person;
    m.character.cast_ray = [this](const glm::vec3& from, const glm::vec3& to, glm::vec3& point, glm::vec3& normal) {
        if (!m_physics) {
            return false;
        }
        const glm::vec3 a_m = from * units::k_skyrim_unit_m, b_m = to * units::k_skyrim_unit_m;
        const float length = glm::length(b_m - a_m);
        if (length <= 0.0f) {
            return false;
        }
        const auto hit = m_physics->raycast_shared(a_m, (b_m - a_m) / length, length);
        if (!hit) {
            return false;
        }
        point = from + (to - from) * (hit->distance / length);
        normal = hit->normal;
        return true;
    };
    g.set_float("fScale", a.visual.scale);
    g.set_bool("IsPlayer", player);
    g.set_bool("IsNPC", !player);
    g.set_int("iIsPlayer", player ? 1 : 0);
    g.set_bool("IsFirstPerson", a.look.first_person);
    g.set_int("i1stPerson", a.look.first_person ? 1 : 0);
    g.set_float("fIsFirstPerson", a.look.first_person ? 1.0f : 0.0f);
    g.set_bool("bHumanoidFootIKEnable", true);
    g.set_int("iRightHandType", a.look.right_hand_type);
    g.set_int("iLeftHandType", a.look.left_hand_type);
    g.set_int("FemaleOffset", a.look.female ? 1 : 0);
    g.set_float("weapAdj", a.look.weap_adj);
    g.set_float("weaponSpeedMult", 1.0f);
    g.set_float("leftWeaponSpeedMult", 1.0f);

    const ActorSkeleton& s = *a.look.skeleton;
    const anim::Skeleton& bones = a.look.project->skeleton();
    m.bones.resize(s.nodes.bones.size(), -1);
    for (size_t i = 0; i < s.nodes.bones.size(); ++i) {
        m.bones[i] = bones.bone(s.nodes.bones[i].name);
    }
    m.nodes = s.rest;
    m.output.resize(bones.size(), 0);
    Live* live = &a;
    m.character.ragdoll_touching = [this, live](std::span<const int32_t> bones) {
        const Animation& at = *live->animation;
        const anim::Ragdoll* ragdoll = live->look.project->ragdoll();
        if (!m_physics || !at.dying || at.keyframed.empty()) {
            return false;
        }
        const Visual& v = live->visual;
        const float scale = v.scale * units::k_skyrim_unit_m;
        const glm::quat turn = glm::quat_cast(v.rotation);
        for (const int32_t bone : bones) {
            const int32_t body = bone >= 0 && size_t(bone) < ragdoll->bone_to_body.size() ? ragdoll->bone_to_body[size_t(bone)] : -1;
            if (body < 0 || size_t(body) >= at.keyframed.size()) {
                continue;
            }
            const anim::RagdollBody& b = ragdoll->bodies[size_t(body)];
            const anim::Qs& q = at.keyframed[size_t(body)];
            const glm::vec3 end_a = v.position + turn * ((q.t + q.r * b.a) * scale);
            const glm::vec3 end_b = v.position + turn * ((q.t + q.r * b.b) * scale);
            if (m_physics->touching_shared(end_a, end_b, b.radius * scale)) {
                return true;
            }
        }
        return false;
    };
    m.character.on_event = [this, live](std::string_view tag, const std::string* payload) {
        ActorEvent e;
        e.ref = live->placed.ref;
        e.tag = std::string(tag);
        e.payload = payload ? *payload : std::string();
        e.position = live->placed.position;
        e.first_person = live->look.first_person;
        e.player = e.first_person || e.ref == k_player_ref;
        e.run_blend = live->animation ? live->animation->run_blend : 0.0f;
        e.footstep_sets = live->look.footstep_sets;
        if (live->animation) {
            // RightHandWeaponDraw/the game: the weapon moves between the sheath and the hand.
            if (e.tag == "Ragdoll") {
                live->animation->ragdoll_event = true;
            } else if (e.tag == "weaponDraw") {
                live->animation->weapon_drawn = true;
            } else if (e.tag == "weaponSheathe") {
                live->animation->weapon_drawn = false;
            }
        }
        std::lock_guard lock(m_events_mutex); // graphs run side by side
        if (e.tag.starts_with("AnimObj")) { // Load, Draw and the records' unload events
            m_prop_events.push_back({e.ref, e.tag, e.payload});
        }
        m_events.push_back(std::move(e));
    };
    a.animation = std::move(anim_state);
    if (auto corpse = m_corpses.find(a.placed.ref); corpse != m_corpses.end() && a.look.project->ragdoll()) {
        a.animation->corpse = true;
        a.animation->bodies = std::move(corpse->second);
        m_corpses.erase(corpse);
        a.face.eyes_closed = true;
    } else if (a.placed.dead && a.look.project->ragdoll()) {
        a.face.eyes_closed = true;
        // Action Ragdoll Instant once the graph is ready: into FullyRagdoll with motors at 0, from the plugin's pose if
        // it has one, else from whatever the graph gives.
        a.animation->laid_out = lay_out(a);
        a.animation->dying = true;
        a.animation->ragdoll_event = true;
    }
}

// Each head shape finds its morphs: by its head part's name in a FaceGeom model, or the model's own part. The game's morph
// names are FaceGen's; a morph that isn't in the file does nothing.
void ActorSet::bind_face(Live& a, Part& part, const render::SkinnedModel& model) {
    auto& meshes = m_renderer.meshes();
    part.face.clear();
    part.face.resize(model.shapes.size());
    for (size_t f = 0; f < part.source.face.size(); ++f) {
        const FacePart& source = part.source.face[f];
        const render::FaceMorphs* tri = meshes.face_morphs(part.face_morphs[f][0]);
        const render::FaceMorphs* race = meshes.face_morphs(part.face_morphs[f][1]);
        const render::FaceMorphs* chargen = meshes.face_morphs(part.face_morphs[f][2]);
        for (size_t k = 0; k < model.shapes.size(); ++k) {
            if (!source.shape.empty() && (k >= model.shape_names.size() || lower(model.shape_names[k]) != source.shape)) {
                continue;
            }
            // the shape needs at least the morph's vertices.
            const auto fits = [&](const render::FaceMorphs* m) { return m && m->vertex_count <= model.shapes[k].vertex_count; };
            auto shape = std::make_unique<FaceShape>();
            shape->channels.fill(-1);
            const auto fix = [&](const render::FaceMorphs* m, std::string_view name, float weight) {
                if (const int32_t i = fits(m) ? m->find(name) : -1; i >= 0 && std::abs(weight) <= 1.0f && weight != 0.0f) {
                    shape->fixed.push_back({m->deltas[size_t(i)], weight * m->scales[size_t(i)], 0});
                }
            };
            if (fits(tri)) {
                shape->tri = tri;
                for (int c = 0; c < k_face_channels; ++c) {
                    shape->channels[size_t(c)] = static_cast<int16_t>(tri->find(k_face_channel_names[c]));
                }
            }
            fix(tri, "SkinnyMorph", a.look.skinny);
            fix(race, a.look.race_morph, 1.0f);
            for (const auto& [name, weight] : a.look.chargen_morphs) {
                fix(chargen, name, weight);
            }
            if (shape->tri || !shape->fixed.empty()) {
                part.face[k] = std::move(shape);
            }
        }
    }
}

void ActorSet::set_expression_override(uint32_t ref, int index, float strength) {
    join();
    if (const auto it = m_live.find(ref); it != m_live.end()) {
        Face& face = it->second->face;
        face.target = index;
        face.target_strength = strength;
        face.override_lock = true;
    }
}

void ActorSet::clear_expression_override(uint32_t ref) {
    join();
    if (const auto it = m_live.find(ref); it != m_live.end()) {
        it->second->face.override_lock = false;
    }
}

void ActorSet::speak(uint32_t ref, const std::string& line, int emotion, int strength) {
    join();
    const auto it = m_live.find(ref);
    if (it == m_live.end()) {
        return;
    }
    Face& face = it->second->face;
    face.lip.reset();
    face.emotion = emotion;
    face.strength = strength;
    face.lip_loading = std::async(std::launch::async, [file = m_data.root() / (line + ".osv")] {
        std::shared_ptr<const osv::LipTrack> track;
        try {
            track = std::make_shared<osv::LipTrack>(osv::read(read_file(file)));
        } catch (const std::exception& e) {
            log::debug("actors: {}: {}", file.string(), e.what());
        }
        return track;
    });
}

void ActorSet::rebuild_player() {
    join();
    for (const uint32_t ref : {k_player_ref, k_player_first_person_ref}) {
        if (m_live.contains(ref)) {
            end(ref); // place_player starts it again
        }
    }
}

bool ActorSet::player_ready() const {
    join();
    const auto it = m_live.find(k_player_ref);
    return it == m_live.end() || it->second->finished;
}

void ActorSet::stop_speaking(uint32_t ref) {
    join();
    if (const auto it = m_live.find(ref); it != m_live.end()) {
        it->second->face.lip.reset();
        it->second->face.lip_loading = {};
    }
}

// Both eyes together, straight into the result. The frame a blink closes
// writes 0, and it parks after one blink unless LookDown is high: a head blinks once, measured in the game too. Blinks after
// that are the lip tracks' own.
void ActorSet::update_blink(Face& face) {
    const auto delay = [&] {
        face.random = face.random * 1664525u + 1013904223u;
        return k_blink_delay_min + (k_blink_delay_max - k_blink_delay_min) * float(face.random >> 8) / float(1u << 24);
    };
    if (face.blink_timer < 0.0f) {
        face.blink_timer = delay();
    }
    float* blink = face.weights.data() + k_face_phonemes;
    const float look_down = blink[8];
    const auto write = [&](float value) { blink[0] = blink[1] = value; };
    switch (face.blink_state) {
    case 0:
        if (face.blink_timer > m_dt) {
            face.blink_timer -= m_dt;
        } else {
            face.blink_state = 1;
            face.blink_timer = k_blink_down;
        }
        break;
    case 1:
        if (face.blink_timer > m_dt) {
            face.blink_timer -= m_dt;
            write(1.0f - face.blink_timer / k_blink_down);
        } else {
            face.blink_state = 2;
            face.blink_timer = k_blink_up;
            write(0.0f);
        }
        break;
    case 2:
        if (face.blink_timer > m_dt) {
            face.blink_timer -= m_dt;
            write(face.blink_timer / k_blink_up);
        } else {
            write(0.0f);
            face.blink_state = 3;
            [[fallthrough]];
        case 3:
            if (look_down >= k_look_down_blink) {
                face.blink_state = 0;
                face.blink_timer = delay();
            }
        }
        break;
    }
}

// what the face should show is
// decided afresh every frame, and the channels ease there. Fear and combat anger wait for Combat.
void ActorSet::update_expression(Live& a) {
    Face& face = a.face;
    float* expression = face.weights.data() + k_face_phonemes + k_face_modifiers;
    if (!face.override_lock) {
        const bool speaking = face.lip != nullptr;
        constexpr int k_dialogue[8] = {-1, 0, 6, 1, 3, 2, 4, 5}; // TRDT emotion to an expression
        constexpr int k_mood[8] = {0, 1, 7, 2, 4, 3, 5, 6};      // ... to a mood
        if (speaking) {
            face.target = k_dialogue[std::clamp(face.emotion, 0, 7)];
            face.target_strength = float(face.strength) * 0.01f;
        } else if (a.look.player || a.look.first_person) {
            face.target = -1; // measured: no mood on the player
        } else {
            face.target = 7 + a.look.mood; // Mood, at iMoodFaceValue
            face.target_strength = k_mood_strength;
        }
    }
    const float high = face.target >= 0 ? face.target_strength : 0.0f;
    const float step = (k_expression_rate + high * k_expression_strength_add) * m_dt;
    for (int i = 0; i < k_face_expressions; ++i) {
        const float goal = i == face.target ? face.target_strength : 0.0f;
        expression[i] += std::clamp(goal - expression[i], -step, step);
    }
}

// rows of the lip track at 30 a second, held, the last for 0.2 s,
// the blink underneath. A row that changes writes its whole self over the result.
// Eyes are morphs; the bone only gives the angle. the game reads
// it from NPCEyeBone in the head's frame, the game moves to it at fTrackSpeed, and the game
// turns it into the look morphs.
// where the eyes wander, by the strongest expression. Dialogue and mood
// channels of one emotion go together; none counts as angry. Other expressions keep what they had.
void ActorSet::wander_eyes(Face& face) {
    face.eye_wander -= m_dt;
    if (face.eye_wander > 0.0f) {
        return;
    }
    const float* expression = face.weights.data() + k_face_phonemes + k_face_modifiers;
    int dominant = -1;
    float strongest = 0.0f;
    for (int i = 0; i < std::min(k_face_expressions, 17); ++i) {
        if (expression[i] > strongest && expression[i] <= 1.0f) {
            strongest = expression[i];
            dominant = i;
        }
    }
    const auto roll = [&](float lo, float hi) {
        face.random = face.random * 1664525u + 1013904223u; // per face: workers run this, away from the game's generator
        return lo + (hi - lo) * float(face.random >> 8) / float(1u << 24);
    };
    const auto settle = [&](float lo, float hi) {
        face.eye_wander = roll(lo, hi);
        face.eye_off_heading = face.eye_off_pitch = 0.0f;
    };
    const auto look = [&](const std::array<float, 4>& r) {
        face.eye_off_pitch = roll(r[0], r[1]);
        face.eye_off_heading = roll(r[2], r[3]);
    };
    enum { angry, fear, happy, sad, neutral };
    switch (dominant) {
    case -1:
    case 0:
    case 8:
        if (roll(0.0f, 1.0f) < 0.3f) {
            settle(2.0f, 3.0f);
        } else {
            face.eye_wander = roll(0.5f, 1.5f);
            look(m_eye_wander[angry]);
        }
        break;
    case 1:
    case 9:
        face.eye_wander = roll(0.5f, 1.5f);
        face.eye_off_pitch = 0.0f;
        face.eye_off_heading = roll(0.0f, 1.0f) < 0.5f ? 0.0f : roll(m_eye_wander[fear][2], m_eye_wander[fear][3]);
        break;
    case 2:
    case 4:
    case 10:
        if (roll(0.0f, 1.0f) < 0.3f) {
            settle(3.0f, 4.0f);
        } else {
            face.eye_wander = roll(0.5f, 1.5f);
            look(m_eye_wander[happy]);
        }
        break;
    case 3:
    case 11:
        face.eye_wander = roll(2.0f, 3.0f);
        if (roll(0.0f, 1.0f) < 0.3f) {
            face.eye_off_heading = face.eye_off_pitch = 0.0f;
        } else {
            look(m_eye_wander[sad]);
        }
        break;
    case 7:
        if (roll(0.0f, 1.0f) < 0.3f) {
            settle(3.0f, 4.0f);
        } else {
            face.eye_wander = roll(0.5f, 1.5f);
            look(m_eye_wander[neutral]);
        }
        break;
    default:
        break;
    }
}

void ActorSet::track_eyes(Live& a) {
    constexpr float k_track_speed = 5.0f;                    // fTrackSpeed, radians a second
    constexpr float k_track_xy = glm::radians(28.0f);        // fTrackEyeXY
    constexpr float k_track_z = glm::radians(20.0f);         // fTrackEyeZ
    Face& face = a.face;
    float* look = face.weights.data() + k_face_phonemes + 8; // down, left, right, up
    const ActorSkeleton* skeleton = a.look.skeleton.get();
    if (!a.animation || face.eyes_closed || !skeleton) {
        if (face.eyes_tracking) {
            std::fill_n(look, 4, 0.0f);
            face.eye_heading = face.eye_pitch = 0.0f;
            face.eyes_tracking = false;
        }
        return;
    }
    const auto head = skeleton->by_name.find("npc head [head]"), eye = skeleton->by_name.find("npceyebone");
    const auto& nodes = a.animation->nodes;
    if (head == skeleton->by_name.end() || eye == skeleton->by_name.end() || std::max(head->second, eye->second) >= nodes.size()) {
        return;
    }
    float heading = 0.0f, pitch = 0.0f;
    if (a.animation->looking.load(std::memory_order_relaxed)) {
        const glm::vec3 forward =
            glm::transpose(glm::mat3(nodes[head->second])) * glm::normalize(glm::mat3(nodes[eye->second])[1]);
        heading = std::atan2(forward.x, forward.y);
        pitch = std::asin(std::clamp(forward.z, -1.0f, 1.0f));
    }
    wander_eyes(face);
    heading += face.eye_off_heading;
    pitch += face.eye_off_pitch;
    const float step = k_track_speed * m_dt;
    face.eye_heading += std::clamp(heading - face.eye_heading, -step, step);
    face.eye_pitch += std::clamp(pitch - face.eye_pitch, -step, step);
    const float across = std::clamp(face.eye_heading, -k_track_xy, k_track_xy) / k_track_xy;
    const float up = std::clamp(face.eye_pitch, -k_track_z, k_track_z) / k_track_z;
    look[0] = std::max(-up, 0.0f);
    look[1] = std::max(-across, 0.0f);
    look[2] = std::max(across, 0.0f);
    look[3] = std::max(up, 0.0f);
    face.eyes_tracking = true;
}

void ActorSet::animate_face(Live& a) {
    Face& face = a.face;
    if (m_dt <= 0.0f) {
        return;
    }
    if (face.eyes_closed) { // SetEyesClosed: at once, and the face goes slack
        if (!face.eyes_shut) {
            face.eyes_shut = true;
            face.lip.reset();
            face.lip_loading = {};
            face.weights.fill(0.0f);
            face.weights[k_face_phonemes] = face.weights[k_face_phonemes + 1] = 1.0f;
        }
        return;
    }
    if (face.lip_loading.valid() && face.lip_loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        face.lip = face.lip_loading.get();
        // The game holds the sound back by fSpeechDelay and the track's lead (4.3); here the sound starts at once, so the
        // track starts at the row the game would be on.
        const float lead = face.lip && face.lip->start_frame < 0 ? float(-face.lip->start_frame) / 30.0f : 0.0f;
        face.lip_time = k_speech_delay + lead;
        face.lip_row = -1;
    }
    update_blink(face);
    update_expression(a);
    if (!face.lip || face.lip->frame_count() == 0) {
        face.lip.reset();
        return;
    }
    const osv::LipTrack& lip = *face.lip;
    face.lip_time += m_dt;
    const float seconds = static_cast<float>(lip.frame_count()) / 30.0f;
    const float lead = lip.start_frame < 0 ? static_cast<float>(-lip.start_frame) / 30.0f : 0.0f;
    const auto clear = [&] {
        std::fill_n(face.weights.begin(), k_face_phonemes + k_face_modifiers, 0.0f);
    };
    if (face.lip_time <= seconds + 0.2f) {
        const uint32_t row = std::min(static_cast<uint32_t>(face.lip_time * 29.999998f), lip.frame_count() - 1);
        if (row != face.lip_row) {
            face.lip_row = row;
            const size_t width = size_t(lip.phoneme_count) + lip.modifier_count;
            const float* values = lip.frames.data() + row * width;
            std::copy_n(values, std::min<uint32_t>(lip.phoneme_count, k_face_phonemes), face.weights.begin());
            std::copy_n(values + lip.phoneme_count, std::min<uint32_t>(lip.modifier_count, k_face_modifiers),
                        face.weights.begin() + k_face_phonemes);
        }
    } else if (face.lip_row != -2) {
        face.lip_row = -2;
        clear();
    }
    if (face.lip_time > seconds + lead + 0.2f) {
        face.lip.reset();
    }
}

void ActorSet::set_physics(physics::World* world) {
    join();
    if (world == m_physics) {
        return;
    }
    if (m_physics) {
        for (auto& [ref, live] : m_live) {
            if (live->animation && live->animation->ragdoll) {
                m_physics->remove_ragdoll(live->animation->ragdoll);
            }
        }
    }
    for (auto& [ref, live] : m_live) {
        if (live->animation) {
            live->animation->ragdoll = 0;
            live->animation->shown_nodes.clear();
        }
        if (live->placed.object && live->finished) { // a new world needs its bodies
            m_object_changes.push_back({ref, true, object_path(live->placed.mesh, ".osp"), live->visual.scale});
        }
    }
    m_physics = world;
}

bool ActorSet::start_pair(uint32_t lead, uint32_t follower, std::string_view lead_event, std::string_view follower_event) {
    join();
    Live* a = live_actor(lead);
    Live* b = live_actor(follower);
    if (!a || !b || a == b || !a->animation || !b->animation || a->animation->pair_done || b->animation->pair_done) {
        return false;
    }
    auto done = std::make_shared<std::atomic<bool>>(false);
    const auto scene = anim::make_sync_scene(2, [done] { done->store(true); }, {});
    a->animation->character.sync_scene = b->animation->character.sync_scene = scene;
    if (!b->animation->graph->notify(follower_event) || !a->animation->graph->notify(lead_event)) {
        a->animation->character.sync_scene.reset();
        b->animation->character.sync_scene.reset();
        return false;
    }
    a->animation->pair_done = b->animation->pair_done = done;
    return true;
}

bool ActorSet::player_body_ready(bool third_person) const {
    join();
    const auto it = m_live.find(third_person ? k_player_ref : k_player_first_person_ref);
    return it != m_live.end() && it->second->animation && it->second->finished;
}

bool ActorSet::paired(uint32_t ref, glm::vec3& feet, float& heading) const {
    join();
    auto it = m_live.find(ref);
    if (it == m_live.end() && ref == k_player_body) {
        it = m_live.find(k_player_first_person_ref);
    }
    return it != m_live.end() && it->second->animation && it->second->animation->pair_done &&
           this->feet(ref, feet, heading);
}

bool ActorSet::node_world(uint32_t ref, std::string_view lower_name, glm::vec3& at) const {
    join();
    auto it = m_live.find(ref);
    if (it == m_live.end() || !it->second->animation || !it->second->look.skeleton) {
        return false;
    }
    const Live& a = *it->second;
    const auto node = a.look.skeleton->by_name.find(std::string(lower_name));
    if (node == a.look.skeleton->by_name.end() || node->second >= a.animation->nodes.size()) {
        return false;
    }
    at = a.visual.position + a.visual.rotation * (a.visual.scale * glm::vec3(a.animation->nodes[node->second][3]));
    return true;
}

bool ActorSet::feet(uint32_t ref, glm::vec3& feet, float& heading) const {
    join();
    auto it = m_live.find(ref);
    if (it == m_live.end() && ref == k_player_body) {
        it = m_live.find(k_player_first_person_ref);
    }
    if (it == m_live.end()) {
        return false;
    }
    const Visual& v = it->second->visual;
    feet = v.position;
    heading = std::atan2(v.rotation[1][0], v.rotation[0][0]);
    return true;
}

bool ActorSet::kill(uint32_t ref) {
    join();
    Live* a = live_actor(ref);
    if (a && a->animation && !a->look.project->ragdoll() && !a->animation->no_body) {
        // Nothing to fall as: its death clip ends it, where it stands.
        a->animation->no_body = true;
        a->animation->speed = 0.0f;
        a->face.eyes_closed = true;
        a->animation->graph->notify("DeathAnim");
        return true;
    }
    if (!a || !a->animation || !a->look.project->ragdoll() || a->animation->dying || a->animation->corpse) {
        return false;
    }
    Animation& m = *a->animation;
    m.speed = 0.0f;
    m.dying = true;
    a->face.eyes_closed = true;
    // Actor::Kill performs Action Death Wait, whose idle sends DeathAnim: AnimateToRagdoll in 0_master.
    m.ragdoll_event = !m.graph->notify("DeathAnim");
    return true;
}

bool ActorSet::hidden_now(const Live& a) const {
    return m_no_body.contains(a.placed.ref) || (m_disabled ? m_disabled(a.placed.ref) : a.placed.starts_disabled);
}

void ActorSet::drop_body(uint32_t ref) {
    join();
    m_no_body.insert(ref);
}

bool ActorSet::dead(uint32_t ref) const {
    join();
    if (const auto it = m_live.find(ref); it != m_live.end() && it->second->animation) {
        const Animation& m = *it->second->animation;
        return m.dying || m.corpse || m.no_body;
    }
    return m_corpses.contains(ref) || std::ranges::any_of(m_placed_dead, [ref](const PlacedActor& p) { return p.ref == ref; });
}

uint32_t ActorSet::ragdoll_of(uint32_t ref) const {
    join();
    const auto it = m_live.find(ref);
    return it != m_live.end() && it->second->animation ? it->second->animation->ragdoll : 0;
}

bool ActorSet::worn_items(uint32_t ref, uint32_t& inventory_npc, std::vector<uint32_t>& items) const {
    join();
    const auto it = m_live.find(ref);
    if (it == m_live.end()) {
        return false;
    }
    const ActorLook& look = it->second->look;
    inventory_npc = look.inventory_npc;
    items = look.armor;
    for (const auto& [weapon, type] : look.weapons) {
        items.push_back(weapon);
    }
    if (look.right_weapon) {
        items.push_back(look.right_weapon);
    }
    return true;
}

// which meshes take it. Art on the magic nodes isn't among the actor's parts here.
void ActorSet::build_membrane(Live& a, Membrane& membrane, const MembraneLook& look) {
    auto& meshes = m_renderer.meshes();
    auto& textures = m_renderer.textures();
    membrane.key = look.key;
    membrane.block = meshes.allocate_material_block();
    // No fill texture: g_DefaultTexEffectMap, 4x4 of (0, 0, 0, 255), so Healing's rim is all that shows.
    const render::TextureId fill = look.fill.empty() ? textures.black() : textures.acquire(look.fill, render::TextureKind::color, 1.0f);
    const render::TextureId palette = textures.acquire(look.palette, render::TextureKind::color, 1.0f);
    membrane.textures = {fill, palette};
    render::DrawState state;
    state.program = render::program_effect_blend;
    state.blend_src = static_cast<uint8_t>(look.blend_src);
    state.blend_dst = static_cast<uint8_t>(look.blend_dst);
    state.z_write = false;
    state.membrane = true;
    for (const Part& part : a.parts) {
        if (part.variant == render::k_no_variant) {
            continue;
        }
        const std::string attach = lower(meshes.skinned_model(part.mesh)->attach);
        const bool held = attach.starts_with("weapon") || attach == "shield" || attach == "quiver";
        if (look.weapon ? !held : held && look.no_weapons) {
            continue;
        }
        const auto groups = meshes.groups(part.mesh, part.variant);
        for (uint32_t k = 0; k < groups.size(); ++k) {
            const render::GpuMaterial& source = meshes.group_material(groups[k]);
            const uint32_t type = source.technique >> 24;
            if ((source.flags & (render::k_material_effect_bit | osm::material_decal)) || (look.skin_only && type != 4 && type != 5)) {
                continue;
            }
            const uint32_t material = meshes.copy_material(groups[k], [&](render::GpuMaterial& g) {
                const bool base_alpha = (g.flags & osm::material_alpha_test) && !look.ignore_base_alpha;
                g.technique = look.technique | 0x8000u | (g.technique & 1u) | (base_alpha ? 0x800000u : 0u);
                // SetupMembraneGeometry: one over the threshold, and one more at 4.
                const uint32_t threshold = uint32_t(std::lround(g.alpha_threshold * 255.0f));
                g.layer[0] = float(threshold + (threshold == 4 ? 2 : 1)) / 255.0f;
                g.flags = render::k_material_effect_bit | (g.flags & osm::material_model_space_normals);
                g.effect_flags = render::k_effect_membrane;
                g.tex[render::gtex_glow] = g.tex[render::gtex_diffuse];
                g.tex[render::gtex_diffuse] = fill;
                g.palette = palette;
                g.sampler = 3;
                g.uv_anim = membrane.block;
            });
            if (material != ~0u) {
                membrane.draws.push_back({part.mesh, part.variant, k, material, state.pack()});
            }
        }
    }
    a.visual.membranes.insert(a.visual.membranes.end(), membrane.draws.begin(), membrane.draws.end());
}

void ActorSet::drop_membrane(Live& a, Membrane& membrane) {
    if (!membrane.key) {
        return;
    }
    std::erase_if(a.visual.membranes, [&](const ActorMembrane& d) {
        return std::ranges::any_of(membrane.draws, [&](const ActorMembrane& m) { return m.material == d.material; });
    });
    // A frame in flight may still draw them; not `this`: the set can be gone when this runs.
    m_renderer.device().defer([renderer = &m_renderer, block = membrane.block, draws = std::move(membrane.draws),
                               textures = std::move(membrane.textures)] {
        for (const ActorMembrane& d : draws) {
            renderer->meshes().free_material(d.material);
        }
        renderer->meshes().free_material_block(block);
        for (render::TextureId t : textures) {
            renderer->textures().release(t);
        }
    });
    membrane = {};
}

void ActorSet::add_weapon_blood(uint32_t ref, const WeaponBloodLook& look) {
    join();
    for (const uint32_t r : {ref, ref == k_player_ref ? k_player_first_person_ref : ref}) {
        if (const auto it = m_live.find(r); it != m_live.end()) {
            WeaponBlood& b = it->second->blood;
            b.look = look;
            b.level = std::clamp(b.level + look.amount, 0.0f, 1.0f);
            b.lifetime = b.age + look.lifetime;
        }
        if (ref != k_player_ref) {
            break;
        }
    }
}

bool ActorSet::add_skin_decal(uint32_t ref, const SkinDecalLook& look) {
    join();
    const auto it = m_live.find(ref);
    if (it == m_live.end() || !it->second->finished || it->second->visual.skin_decals.size() >= look.limit || look.size <= 0.0f) {
        return false;
    }
    Live& a = *it->second;
    const uint32_t slot = m_data.allocate_skin_decal();
    if (slot == ~0u) {
        return false;
    }
    // The hit in the space the posed vertices are in; the shapes' world rotation is the actor's.
    const glm::mat3 to_model = glm::transpose(a.visual.rotation);
    render::SkinDecalBuild build;
    build.slot = slot;
    build.hit = glm::vec4(to_model * (look.position - a.visual.position) / a.visual.scale, a.visual.scale / look.size);
    build.axis_x = glm::vec4(to_model * look.axis_x, 0.0f);
    build.axis_y = glm::vec4(to_model * look.axis_y, 0.0f);
    build.direction = glm::vec4(to_model * look.direction, 0.0f);
    auto& meshes = m_renderer.meshes();
    auto& builds = m_data.skin_decal_builds();
    const size_t before = builds.size();
    for (const Part& part : a.parts) {
        // BSDynamicTriShape takes none: a head that morphs. A held model has no skin instance.
        const render::SkinnedModel* model = meshes.skinned_model(part.mesh);
        if (part.variant == render::k_no_variant || !model || !model->attach.empty() ||
            std::ranges::any_of(part.face, [](const auto& shape) { return shape != nullptr; })) {
            continue;
        }
        for (const uint32_t g : meshes.groups(part.mesh, part.variant)) {
            const render::DrawGroup& group = meshes.group(g);
            if (meshes.group_material(g).flags & (render::k_material_effect_bit | osm::material_decal)) {
                continue;
            }
            build.first_index = group.first_index;
            build.index_count = group.index_count;
            build.vertex_offset = group.vertex_offset;
            builds.push_back(build);
        }
    }
    if (builds.size() == before) {
        m_data.free_skin_decal(slot);
        return false;
    }
    a.visual.skin_decals.push_back({.slot = slot, .texture = look.texture, .lifetime = look.lifetime});
    return true;
}

// past its lifetime, a second's fade from the start of it.
void ActorSet::update_skin_decals(Live& a) {
    std::erase_if(a.visual.skin_decals, [&](ActorSkinDecal& d) {
        d.age += m_dt;
        const float past = d.age - d.lifetime;
        d.fade = std::max(1.0f - std::max(past, 0.0f), 0.0f);
        if (past < 1.0f) {
            return false;
        }
        m_renderer.device().defer([data = &m_data, slot = d.slot] { data->free_skin_decal(slot); });
        return true;
    });
}

void ActorSet::drop_weapon_blood(Live& a) {
    WeaponBlood& b = a.blood;
    std::erase_if(a.visual.membranes, [&](const ActorMembrane& d) {
        return std::ranges::any_of(b.draws, [&](const ActorMembrane& m) { return m.material == d.material; });
    });
    if (!b.draws.empty()) {
        m_renderer.device().defer([renderer = &m_renderer, draws = std::move(b.draws)] {
            for (const ActorMembrane& d : draws) {
                renderer->meshes().free_material(d.material);
            }
        });
    }
    b.draws.clear();
    b.threshold = -1;
}

// it holds for its lifetime, then falls under a ramp
// to nothing; the level is an alpha threshold.
void ActorSet::update_weapon_blood(Live& a) {
    WeaponBlood& b = a.blood;
    if (b.level <= 0.0f && b.draws.empty()) {
        return;
    }
    b.age += m_dt;
    if (const float past = b.age - b.lifetime; past > 0.0f) {
        if (past > b.look.fade) {
            drop_weapon_blood(a);
            b = WeaponBlood{};
            return;
        }
        b.level = std::min(b.level, std::clamp((b.look.fade - past) / b.look.fade, 0.0f, 1.0f));
    }
    const int threshold = int((1.0f - b.level) * 255.0f);
    if (threshold == b.threshold) {
        return;
    }
    drop_weapon_blood(a);
    b.threshold = threshold;
    auto& meshes = m_renderer.meshes();
    for (const Part& part : a.parts) {
        if (part.variant == render::k_no_variant || part.source.spare) {
            continue;
        }
        const auto groups = meshes.groups(part.mesh, part.variant);
        for (uint32_t k = 0; k < groups.size(); ++k) {
            const render::DrawGroup& group = meshes.group(groups[k]);
            if (!group.blood) {
                continue;
            }
            const uint32_t material = meshes.copy_material(groups[k], [&](render::GpuMaterial& g) {
                g.alpha_threshold = float(threshold) / 255.0f;
                g.layer[1] = b.look.falloff;
                g.layer[2] = b.look.rgb_scale;
            });
            if (material == ~0u) {
                continue;
            }
            render::DrawState state = render::DrawState::unpack(group.state);
            if (!(meshes.group_material(groups[k]).flags & render::k_material_effect_bit)) {
                // The pass clears a lighting shape's blend pair to ONE, ONE: SRC_ALPHA, ONE as it draws.
                state.blend_src = 6;
                state.blend_dst = 0;
            }
            b.draws.push_back({part.mesh, part.variant, k, material, state.pack()});
        }
    }
    a.visual.membranes.insert(a.visual.membranes.end(), b.draws.begin(), b.draws.end());
}

bool ActorSet::set_membrane(uint32_t ref, const MembraneLook& look, const render::GpuMaterialAnim& values) {
    join();
    bool attached = false;
    for (const uint32_t r : {ref, ref == k_player_ref ? k_player_first_person_ref : ref}) {
        const auto it = m_live.find(r);
        if (it == m_live.end() || !it->second->finished || (attached && r == ref)) {
            continue;
        }
        Membrane& m = it->second->membranes[look.weapon];
        if (m.key != look.key) {
            drop_membrane(*it->second, m);
            build_membrane(*it->second, m, look);
        }
        m_renderer.meshes().set_material_block(m.block, values);
        attached = true;
    }
    return attached;
}

void ActorSet::clear_membrane(uint32_t ref, uint32_t key) {
    join();
    for (const uint32_t r : {ref, ref == k_player_ref ? k_player_first_person_ref : ref}) {
        if (const auto it = m_live.find(r); it != m_live.end()) {
            for (Membrane& m : it->second->membranes) {
                if (m.key == key) {
                    drop_membrane(*it->second, m);
                }
            }
        }
    }
}

bool ActorSet::has_3d(uint32_t ref) const {
    join();
    const auto it = m_live.find(ref);
    return (it != m_live.end() && it->second->finished) ||
           (ref == k_player_ref && m_live.contains(k_player_first_person_ref));
}

bool ActorSet::node_world(uint32_t ref, std::string_view lower_name, glm::mat3& rotation, glm::vec3& position) const {
    join();
    auto it = m_live.find(ref);
    if (it == m_live.end() && (ref == k_player_ref || ref == k_player_body)) {
        it = m_live.find(k_player_first_person_ref);
    }
    if (it == m_live.end() || !it->second->animation || !it->second->look.skeleton) {
        return false;
    }
    const Live& a = *it->second;
    if (lower_name.empty()) {
        rotation = a.visual.rotation;
        position = a.visual.position;
        return true;
    }
    const auto node = a.look.skeleton->by_name.find(std::string(lower_name));
    if (node == a.look.skeleton->by_name.end()) {
        return false;
    }
    const glm::mat4& m = a.animation->nodes[node->second];
    rotation = a.visual.rotation * glm::mat3(m);
    position = a.visual.position + a.visual.rotation * (a.visual.scale * glm::vec3(m[3]));
    return true;
}

float ActorSet::camera_fov_offset() const {
    join();
    auto it = m_live.find(k_player_ref);
    const auto first = m_live.find(k_player_first_person_ref);
    if (first != m_live.end() && (it == m_live.end() || it->second->visual.hidden)) {
        it = first;
    }
    if (it == m_live.end() || !it->second->animation || !it->second->look.skeleton) {
        return 0.0f;
    }
    const Live& a = *it->second;
    const auto node = a.look.skeleton->by_name.find("camera control");
    if (node == a.look.skeleton->by_name.end()) {
        return 0.0f;
    }
    const Animation& m = *a.animation;
    const int32_t bone = m.bones[node->second];
    if (bone >= 0 && m.output.pose_fraction > 0.0f && size_t(bone) < m.output.pose.size()) {
        return m.output.pose[size_t(bone)].t.z; // the pose is in game units, read here as degrees
    }
    return a.look.skeleton->nodes.bones[node->second].translation[2] / units::k_skyrim_unit_m;
}

bool ActorSet::camera_node(std::string_view lower_name, glm::vec3& offset, glm::mat3& rotation) const {
    join();
    auto it = m_live.find(k_player_ref);
    const auto first = m_live.find(k_player_first_person_ref);
    if (first != m_live.end() && (it == m_live.end() || it->second->visual.hidden)) {
        it = first;
    }
    if (it == m_live.end() || !it->second->animation || !it->second->look.skeleton || !it->second->posed) {
        return false;
    }
    const Live& a = *it->second;
    const auto node = a.look.skeleton->by_name.find(std::string(lower_name));
    if (node == a.look.skeleton->by_name.end()) {
        return false;
    }
    const glm::mat4& m = a.animation->nodes[node->second];
    offset = a.visual.rotation * (a.visual.scale * glm::vec3(m[3]));
    rotation = glm::mat3(m);
    return true;
}

bool ActorSet::camera_delta(std::string_view node, glm::vec3& translation, glm::quat& rotation) const {
    join();
    const auto it = m_live.find(k_player_ref);
    if (it == m_live.end() || !it->second->animation || !it->second->look.project) {
        return false;
    }
    const Animation& m = *it->second->animation;
    const int32_t bone = it->second->look.project->skeleton().bone(node);
    if (bone < 0 || m.output.pose_fraction <= 0.0f || size_t(bone) >= m.output.pose.size()) {
        return false;
    }
    translation = m.output.pose[size_t(bone)].t;
    rotation = m.output.pose[size_t(bone)].r;
    return true;
}

bool ActorSet::weapon_ray(uint32_t ref, std::string_view node, BoneSegment& out) const {
    join();
    auto it = m_live.find(ref);
    if (ref == k_player_ref) { // the tree that shows
        const auto first = m_live.find(k_player_first_person_ref);
        if (first != m_live.end() && (it == m_live.end() || it->second->visual.hidden)) {
            it = first;
        }
    }
    if (it == m_live.end() || !it->second->animation || !it->second->look.skeleton) {
        return false;
    }
    const Live& a = *it->second;
    const auto at = a.look.skeleton->by_name.find(std::string(node));
    if (at == a.look.skeleton->by_name.end()) {
        return false;
    }
    const auto& meshes = m_renderer.meshes();
    float length = 0.0f;
    for (const Part& part : a.parts) {
        const render::SkinnedModel* model = part.variant == render::k_no_variant ? nullptr : meshes.skinned_model(part.mesh);
        if (model && !part.source.spare && lower(model->attach).starts_with("weapon")) {
            const glm::vec4 bound = meshes.sphere(part.mesh);
            length = std::max(length, glm::length(glm::vec3(bound)) + bound.w);
        }
    }
    if (length <= 0.0f) {
        return false;
    }
    const glm::mat4& m = a.animation->nodes[at->second];
    const glm::mat3 linear(m);
    const float scale = glm::length(linear[0]);
    out.rotation = a.visual.rotation * (scale > 0.0f ? linear / scale : glm::mat3(1.0f));
    const glm::vec3 from = a.visual.position + a.visual.rotation * (a.visual.scale * glm::vec3(m[3]));
    out.from = from / units::k_skyrim_unit_m;
    out.to = (from + out.rotation * glm::vec3(0.0f, length * a.visual.scale, 0.0f)) / units::k_skyrim_unit_m;
    return true;
}

void ActorSet::bone_segments(uint32_t ref, std::vector<BoneSegment>& out) const {
    join();
    auto it = m_live.find(ref);
    bool first_person = false;
    if (ref == k_player_ref) { // the tree that shows
        const auto first = m_live.find(k_player_first_person_ref);
        if (first != m_live.end() && (it == m_live.end() || it->second->visual.hidden)) {
            it = first;
            first_person = true;
        }
    }
    if (it == m_live.end() || !it->second->animation || !it->second->look.skeleton || !it->second->look.project) {
        return;
    }
    const Live& a = *it->second;
    const Animation& m = *a.animation;
    const anim::Ragdoll* ragdoll = a.look.project->ragdoll();
    const ActorSkeleton& s = *a.look.skeleton;
    const auto world = [&](const glm::mat4& node) {
        return (a.visual.position + a.visual.rotation * (glm::vec3(node[3]) * a.visual.scale)) / units::k_skyrim_unit_m;
    };
    // first person, and races marked
    // RaceUseGeometryEmitterForParticles, all three of which have no bodies: each skinned bone and its parent.
    if (first_person || !ragdoll) {
        for (const Part& part : a.parts) {
            if (part.variant == render::k_no_variant) {
                continue;
            }
            // The scan stops at these nodes.
            const std::string attach = lower(m_renderer.meshes().skinned_model(part.mesh)->attach);
            if (attach.starts_with("weapon") || attach == "shield" || attach == "quiver") {
                continue;
            }
            for (const BoneBind& bind : part.binds) {
                const int32_t node = bind.node;
                const int32_t parent = node >= 0 && size_t(node) < s.nodes.bones.size() ? s.nodes.bones[size_t(node)].parent : -1;
                if (parent >= 0 && size_t(node) < m.nodes.size() && size_t(parent) < m.nodes.size()) {
                    const glm::mat3 linear(m.nodes[size_t(node)]);
                    const float scale = glm::length(linear[0]);
                    out.push_back({world(m.nodes[size_t(parent)]), world(m.nodes[size_t(node)]),
                                   a.visual.rotation * (scale > 0.0f ? linear / scale : glm::mat3(1.0f))});
                }
            }
        }
        return;
    }
    for (size_t i = 0; i < s.nodes.bones.size() && i < m.nodes.size(); ++i) {
        const int32_t bone = m.bones[i], parent = s.nodes.bones[i].parent;
        const bool body = ragdoll && bone >= 0 && size_t(bone) < ragdoll->bone_to_body.size() && ragdoll->bone_to_body[size_t(bone)] >= 0;
        if (body && parent >= 0) {
            const glm::mat3 linear(m.nodes[i]);
            const float scale = glm::length(linear[0]);
            out.push_back({world(m.nodes[size_t(parent)]), world(m.nodes[i]), a.visual.rotation * (scale > 0.0f ? linear / scale : glm::mat3(1.0f))});
        }
    }
}

bool ActorSet::weapon_drawn(uint32_t ref) const {
    join();
    const auto it = m_live.find(ref);
    return it != m_live.end() && it->second->animation && it->second->animation->weapon_drawn;
}

bool ActorSet::resurrect(uint32_t ref) {
    join();
    if (std::ranges::any_of(m_placed_dead, [ref](const PlacedActor& p) { return p.ref == ref; })) {
        return false;
    }
    m_data.npc_headless(ref, false); // Actor::Resurrect puts back what was lost
    end(ref);
    m_corpses.erase(ref); // after: ending a corpse keeps where it lay
    return true;
}

// the task it queues.
bool ActorSet::decapitate(uint32_t ref, bool thrown) {
    join();
    Live* a = live_actor(ref);
    return a && sever(*a, thrown);
}

uint32_t ActorSet::decapitate_art(uint32_t ref) const {
    join();
    const auto it = m_live.find(ref);
    return it != m_live.end() ? it->second->look.decapitate_art : 0;
}

bool ActorSet::sever(Live& a, bool thrown) {
    if (a.severed || !a.animation || !m_physics || !a.look.skeleton || !a.look.project || a.look.stump.mesh.empty()) {
        return false;
    }
    const ActorSkeleton& s = *a.look.skeleton;
    const auto head = s.by_name.find("npc head [head]"), neck = s.by_name.find("npc neck [neck]");
    const anim::Ragdoll* ragdoll = a.look.project->ragdoll();
    if (head == s.by_name.end() || neck == s.by_name.end() || !ragdoll) {
        return false;
    }
    Animation& m = *a.animation;
    const Visual& v = a.visual;
    const glm::mat4 place = glm::translate(glm::mat4(1.0f), v.position) * glm::mat4(v.rotation) * glm::scale(glm::mat4(1.0f), glm::vec3(v.scale));
    const glm::mat4 cut = place * m.nodes[head->second];
    const glm::vec3 from_neck = glm::vec3(cut[3]) - glm::vec3((place * m.nodes[neck->second])[3]);
    const float scale = v.scale * units::k_skyrim_unit_m;
    const int32_t bone = m.bones[head->second];
    int32_t body = -1;
    for (const anim::RagdollMapping& map : ragdoll->to_ragdoll) {
        if (map.bone_a == bone && map.bone_b >= 0 && size_t(map.bone_b) < ragdoll->bone_to_body.size()) {
            body = ragdoll->bone_to_body[size_t(map.bone_b)];
        }
    }
    if (body < 0 || size_t(body) >= ragdoll->bodies.size()) {
        return false;
    }
    const anim::RagdollBody& b = ragdoll->bodies[size_t(body)];
    physics::RagdollSpec spec;
    physics::RagdollBodySpec& h = spec.bodies.emplace_back();
    h.radius = b.radius * scale;
    h.a = b.a * scale;
    h.b = b.b * scale;
    h.mass = b.mass;
    h.inertia = 1.0f / glm::max(b.inverse_inertia, glm::vec3(1e-6f)) * (scale * scale);
    h.friction = b.friction;
    h.restitution = b.restitution;
    h.linear_damping = b.linear_damping;
    h.angular_damping = b.angular_damping;
    h.gravity_factor = b.gravity_factor;
    h.max_linear_velocity = b.max_linear_velocity;
    h.max_angular_velocity = b.max_angular_velocity;
    h.collision_filter = b.collision_filter;
    h.position = glm::vec3(cut[3]);
    glm::mat3 turn(cut);
    for (int i = 0; i < 3; ++i) {
        turn[i] = glm::normalize(turn[i]);
    }
    h.rotation = glm::quat_cast(turn);
    if (thrown && glm::length(from_neck) > 0.0f) {
        // Along the neck, the same as its spin. The game adds the way the blade was moving, which nothing here has.
        h.linear_velocity = h.angular_velocity = glm::normalize(from_neck) * m_data.dismembered_limb_velocity();
    }
    Severed severed;
    severed.ragdoll = m_physics->add_ragdoll(std::move(spec));
    severed.nodes = m.nodes;
    severed.from = glm::inverse(glm::translate(glm::mat4(1.0f), h.position) * glm::mat4(h.rotation)) * place;
    a.severed = std::move(severed);
    m_data.npc_headless(a.placed.ref, true);
    Part p;
    p.source = a.look.stump;
    p.prop = "decapitated neck";
    p.prop_shown = true;
    p.mesh = m_renderer.meshes().acquire(p.source.mesh, 1.0f);
    a.parts.push_back(std::move(p));
    m_props_loading.push_back(a.placed.ref);
    return true;
}

void ActorSet::follow_head(Live& a) {
    Severed& s = *a.severed;
    std::vector<physics::RagdollBodyState>& at = m_head_state;
    if (!m_physics || !m_physics->ragdoll_state(s.ragdoll, at) || at.empty()) {
        return;
    }
    const Visual& v = a.visual;
    const glm::mat4 place = glm::translate(glm::mat4(1.0f), v.position) * glm::mat4(v.rotation) * glm::scale(glm::mat4(1.0f), glm::vec3(v.scale));
    s.now = glm::inverse(place) * glm::translate(glm::mat4(1.0f), at[0].position) * glm::mat4(at[0].rotation) * s.from;
}

void ActorSet::push(uint32_t ref, const glm::vec3& centre, float strength) {
    join();
    Live* a = live_actor(ref);
    if (a && a->animation && a->animation->ragdoll && m_physics) {
        m_physics->push_ragdoll(a->animation->ragdoll, centre, strength);
    }
}

// The bodies a dying actor's animation holds: where they are this frame, in model space.
void ActorSet::keyframe_ragdoll(Live& a) {
    Animation& m = *a.animation;
    const auto& parents = a.look.project->skeleton().parents;
    std::vector<anim::Qs> model(parents.size());
    for (size_t i = 0; i < parents.size() && i < m.output.pose.size(); ++i) {
        model[i] = parents[i] >= 0 ? model[size_t(parents[i])] * m.output.pose[i] : m.output.pose[i];
    }
    m.last_keyframed.swap(m.keyframed);
    a.look.project->ragdoll()->bodies_from_pose(model, m.keyframed);
}

// A corpse the plugin laid out: its entries are the local transforms of the nodes with bodies, as the depth-first walk
// meets them, and a part that doesn't match ends the layout.
bool ActorSet::lay_out(Live& a) {
    const RagdollStart start = m_data.ragdoll_start(a.placed.ref);
    if (start.bones.empty()) {
        return false;
    }
    Animation& m = *a.animation;
    const ActorSkeleton& s = *a.look.skeleton;
    const anim::Ragdoll& ragdoll = *a.look.project->ragdoll();
    std::unordered_map<std::string, uint32_t> parts;
    for (const anim::RagdollBody& b : ragdoll.bodies) {
        parts.emplace(lower(b.name), b.collision_filter >> 8 & 0x1F);
    }
    std::vector<glm::mat4> local = s.rest_local;
    const auto rx = [](float a) { return glm::mat3(glm::rotate(glm::mat4(1.0f), a, glm::vec3(1, 0, 0))); };
    const auto ry = [](float a) { return glm::mat3(glm::rotate(glm::mat4(1.0f), a, glm::vec3(0, 1, 0))); };
    const auto rz = [](float a) { return glm::mat3(glm::rotate(glm::mat4(1.0f), a, glm::vec3(0, 0, 1))); };
    // NiMatrix3's Euler XYZ: Gamebryo's rotations turn the other way. The file agrees: the corpses' joints come nearest
    // the rest pose this way round.
    const auto euler = [&](const glm::vec3& e) { return glm::transpose(rz(e.z) * ry(e.y) * rx(e.x)); };
    size_t next = 0;
    for (size_t i = 0; i < s.nodes.bones.size() && next < start.bones.size(); ++i) {
        const auto part = parts.find(lower(s.nodes.bones[i].name));
        if (part == parts.end()) {
            continue;
        }
        const RagdollStart::Bone& e = start.bones[next++];
        if (e.part != part->second) {
            return false;
        }
        local[i] = glm::mat4(euler(e.rotation) * s.nodes.bones[i].scale);
        local[i][3] = glm::vec4(e.position * units::k_skyrim_unit_m, 1.0f);
    }
    if (start.root_rotation != glm::vec3(0.0f) && local.size() > 1) {
        const glm::vec4 keep = local[1][3];
        local[1] = glm::mat4(euler(start.root_rotation));
        local[1][3] = keep;
    }
    const anim::Skeleton& bones = a.look.project->skeleton();
    std::vector<anim::Qs> model(bones.size());
    for (size_t i = 0; i < bones.size(); ++i) { // bones the NIF lacks sit at their reference pose
        const int16_t p = bones.parents[i];
        model[i] = p >= 0 ? model[size_t(p)] * bones.reference[i] : bones.reference[i];
    }
    std::vector<glm::mat4> nodes(local.size());
    for (size_t i = 0; i < local.size(); ++i) {
        const int32_t parent = s.nodes.bones[i].parent;
        nodes[i] = parent >= 0 ? nodes[size_t(parent)] * local[i] : local[i];
        if (const int32_t b = m.bones[i]; b >= 0 && size_t(b) < model.size()) {
            model[size_t(b)].t = glm::vec3(nodes[i][3]) / units::k_skyrim_unit_m;
            model[size_t(b)].r = glm::quat_cast(glm::mat3(nodes[i]));
        }
    }
    ragdoll.bodies_from_pose(model, m.keyframed);
    m.last_keyframed.clear();
    return true;
}

// Physics takes the bodies from the animation, moving as they were.
void ActorSet::start_ragdoll(Live& a) {
    a.snap = true; // bodies take over from the pose: nothing to blend across
    Animation& m = *a.animation;
    const Visual& v = a.visual;
    const float scale = v.scale * units::k_skyrim_unit_m;
    const glm::quat turn = glm::quat_cast(v.rotation);
    m.bodies.resize(m.keyframed.size());
    for (size_t i = 0; i < m.keyframed.size(); ++i) {
        m.bodies[i].position = v.position + turn * (m.keyframed[i].t * scale);
        m.bodies[i].rotation = turn * m.keyframed[i].r;
    }
    m.corpse = true;
    m.dying = false;
    if (!m_physics) {
        return;
    }
    physics::RagdollSpec spec = ragdoll_spec(a);
    if (m.last_keyframed.size() == m.keyframed.size() && m_dt > 0.0f) {
        for (size_t i = 0; i < m.keyframed.size(); ++i) {
            const anim::Qs &now = m.keyframed[i], &then = m.last_keyframed[i];
            spec.bodies[i].linear_velocity = turn * (now.t - then.t) * (scale / m_dt);
            glm::quat delta = now.r * glm::conjugate(then.r);
            if (delta.w < 0.0f) {
                delta = -delta;
            }
            spec.bodies[i].angular_velocity = turn * (glm::axis(delta) * (glm::angle(delta) / m_dt));
        }
    }
    spec.asleep = false;
    m.ragdoll = m_physics->add_ragdoll(std::move(spec));
}

physics::RagdollSpec ActorSet::ragdoll_spec(const Live& a) const {
    const anim::Ragdoll& ragdoll = *a.look.project->ragdoll();
    // The file's lengths are game units, and the model carries the actor's scale.
    const float scale = a.visual.scale * units::k_skyrim_unit_m;
    physics::RagdollSpec spec;
    spec.asleep = true;
    for (size_t i = 0; i < ragdoll.bodies.size(); ++i) {
        const anim::RagdollBody& b = ragdoll.bodies[i];
        physics::RagdollBodySpec& s = spec.bodies.emplace_back();
        s.radius = b.radius * scale;
        s.a = b.a * scale;
        s.b = b.b * scale;
        s.mass = b.mass;
        s.inertia = 1.0f / glm::max(b.inverse_inertia, glm::vec3(1e-6f)) * (scale * scale);
        s.friction = b.friction;
        s.restitution = b.restitution;
        s.linear_damping = b.linear_damping;
        s.angular_damping = b.angular_damping;
        s.gravity_factor = b.gravity_factor;
        s.max_linear_velocity = b.max_linear_velocity;
        s.max_angular_velocity = b.max_angular_velocity;
        s.force_scale = b.force_scale;
        s.collision_filter = b.collision_filter;
        s.position = a.animation->bodies[i].position;
        s.rotation = a.animation->bodies[i].rotation;
    }
    for (const anim::RagdollConstraint& c : ragdoll.constraints) {
        physics::RagdollConstraintSpec& s = spec.constraints.emplace_back();
        s.body_a = c.body_a;
        s.body_b = c.body_b;
        s.hinge = c.hinge;
        s.pivot_a = c.pivot_a * scale;
        s.pivot_b = c.pivot_b * scale;
        s.axes_a = c.axes_a;
        s.axes_b = c.axes_b;
        s.twist_min = c.twist_min;
        s.twist_max = c.twist_max;
        s.cone_max = c.cone_max;
        s.plane_min = c.plane_min;
        s.plane_max = c.plane_max;
        s.hinge_min = c.hinge_min;
        s.hinge_max = c.hinge_max;
        s.friction = c.friction;
    }
    return spec;
}

// The bodies pose the skeleton, and the model goes where the first of them lies.
void ActorSet::follow_ragdoll(Live& a) {
    Animation& m = *a.animation;
    if (m_physics && !m.ragdoll && m.bodies.size() == a.look.project->ragdoll()->bodies.size()) {
        m.ragdoll = m_physics->add_ragdoll(ragdoll_spec(a)); // the world came after it
    }
    if (m_physics && m.ragdoll) {
        m_physics->ragdoll_state(m.ragdoll, m.bodies);
    }
    if (m.bodies.empty()) {
        return;
    }
    Visual& v = a.visual;
    const float scale = v.scale * units::k_skyrim_unit_m;
    const glm::quat back = glm::conjugate(glm::quat_cast(v.rotation));
    const glm::vec3 centre = m.bodies[0].position;
    float reach = 0.0f;
    std::vector<anim::Qs> bodies(m.bodies.size());
    for (size_t i = 0; i < m.bodies.size(); ++i) {
        bodies[i].t = (back * (m.bodies[i].position - centre)) / scale;
        bodies[i].r = back * m.bodies[i].rotation;
        reach = std::max(reach, glm::distance(m.bodies[i].position, centre));
    }
    a.look.project->ragdoll()->pose_from_bodies(bodies, a.look.project->skeleton().parents, m.output.pose);
    if (centre != v.position) {
        v.position = centre;
        a.placed.position = centre;
        v.moved = true;
    }
    v.sphere = glm::vec4(centre, reach + k_ragdoll_margin);
}

// One step of the graph: channels, update, generate.
void ActorSet::run_graph(Live& a) {
    Animation& m = *a.animation;
    anim::Graph& g = *m.graph;
    // The channel is per the reference's scale; the actions below go by the raw speed.
    const float channel_scale = a.look.height * a.placed.scale;
    const float channel_speed = channel_scale > 1.2e-7f ? m.speed / channel_scale : m.speed;
    g.set_float("Speed", channel_speed);
    g.set_float("TargetSpeed", channel_speed);
    g.set_float("Direction", m.direction);
    g.set_float("Pitch", m.pitch);
    if (a.look.first_person) {
        // FirstPersonState::Update: the arms keep up with the camera through this variable, from the actor's
        // pitch and that of the Cam1 node, which last tick's offset turned (Euler: minus asin of row 2, column 1).
        float node = 0.0f;
        if (const auto cam = a.look.skeleton->by_name.find("camera1st [cam1]");
            cam != a.look.skeleton->by_name.end() && cam->second < m.nodes.size()) {
            node = -glm::degrees(std::asin(std::clamp(glm::normalize(glm::vec3(m.nodes[cam->second][1])).z, -1.0f, 1.0f)));
        }
        g.set_float("PitchOffset", -(node + m.pitch) * k_first_person_pitch_factor);
    }
    g.set_float("TimeDelta", m_dt);
    g.set_float("TurnDelta", m.turn_delta);
    const bool looking = m.looking.load(std::memory_order_relaxed);
    if (looking != m.head_tracking) {
        g.set_bool("bHeadTracking", looking);
        m.head_tracking = looking;
    }
    if (looking) {
        g.set_vector("TargetLocation", glm::vec4(m.look_x.load(std::memory_order_relaxed), m.look_y.load(std::memory_order_relaxed),
                                                 m.look_z.load(std::memory_order_relaxed), 0.0f));
    }
    // The channels' side effects.
    // The actor's draw and sheathe: the hands' types count as equipped only with the weapon out, and the
    // graph's sprint and weapon states choose by them.
    if (m.weapon_drawn != m.hands_out) {
        m.hands_out = m.weapon_drawn;
        int32_t left = 0, right = 0;
        g.get_int("iLeftHandType", left);
        g.get_int("iRightHandType", right);
        constexpr int32_t k_hand_spell = 9;
        g.set_int("iMagicEquipped", m.hands_out && (left == k_hand_spell || right == k_hand_spell) ? 1 : 0);
        g.set_int("iLeftHandEquipped", m.hands_out ? left : 0);
        g.set_int("iRightHandEquipped", m.hands_out ? right : 0);
    }
    // The game keeps the iSync variables from these actions: a state the graph enters later, such
    // as an equip's way out, reads them to rejoin what the body is doing.
    const bool moving = m.speed >= 5.0f;
    if (moving != m.moving) {
        g.notify(moving ? "moveStart" : "moveStop");
        g.set_int("iSyncIdleLocomotion", moving ? 1 : 0);
        m.moving = moving;
    }
    const bool forward = m.direction < 0.25f || m.direction >= 0.75f;
    if (forward != m.forward) {
        g.notify(forward ? "moveForward" : "moveBackward");
        g.set_int("iSyncForwardState", forward ? 0 : 1);
        m.forward = forward;
    }
    const bool right = m.direction > 0.0f && m.direction <= 0.5f;
    if (right != m.right) {
        g.notify(right ? "moveRight" : "moveLeft");
        g.set_int("iSyncStrafeState", right ? 0 : 1);
        m.right = right;
    }
    const bool turning = std::abs(m.turn_delta) > 1.0f;
    if (turning != m.turning) {
        g.notify(!turning ? "turnStop" : m.turn_delta > 0.0f ? "turnLeft" : "turnRight");
        g.set_int("iSyncTurnState", !turning ? 1 : m.turn_delta > 0.0f ? 2 : 0);
        m.turning = turning;
    }
    const Visual& v = a.visual;
    m.character.world_from_model.t = v.position / units::k_skyrim_unit_m;
    m.character.world_from_model.r = glm::quat_cast(v.rotation);
    m.character.world_from_model.s = glm::vec3(v.scale);
    g.update(m_dt * (a.placed.ref == k_player_ref || a.look.first_person ? m_player_graph_rate : m_other_graph_rate));
    if (!m.queued.empty()) {
        constexpr int k_queued_tries = 90;
        if (g.notify(m.queued.front()) || ++m.queued_tries >= k_queued_tries) {
            m.queued.pop_front();
            m.queued_tries = 0;
        }
    }
    if (m.collecting) {
        glm::vec3 translation(0.0f), angles(0.0f);
        if (g.root_motion(m_dt, v.scale, translation, angles)) {
            m.motion += translation;
        }
    }
    g.generate(m.output);
    if (m_physics) {
        m.character.foot_ik_feedback = glm::vec3(0.0f);
        m.character.foot_ik_on = false;
        anim::run_foot_ik_driver(m.character, m.output, m_dt);
        const float before = m.foot_drop.load(std::memory_order_relaxed);
        const bool held = m.character.foot_ik_on && !m.dying && !m.corpse && !m.pair_done; // the marks place it
        const float drop = held ? before + m.character.foot_ik_feedback.z * units::k_skyrim_unit_m : 0.0f;
        if (drop != before) {
            m.foot_drop.store(drop, std::memory_order_relaxed);
            a.visual.position.z += drop - before;
            a.visual.moved = true;
        }
    }
}

void ActorSet::animate(Live& a) {
    animate_graph(a);
    animate_finish(a);
}

// What is the actor's alone, so actors can run side by side.
void ActorSet::animate_graph(Live& a) {
    Animation& m = *a.animation;
    animate_face(a);
    track_eyes(a);
    if (!m.corpse || m.output.pose_fraction <= 0.0f) {
        run_graph(a); // a corpse's unmapped bones keep what it last played
    }
    if (!m.dying && !m.corpse) {
        build_nodes(a);
        a.ready_rows.clear();
        a.ready_drawn = m.weapon_drawn;
        pose_rows(a, m.nodes, a.ready_rows); // what the next tick's pose will ask for, off its thread
    }
}

void ActorSet::run_loop(Live& a) {
    ObjectLoop& l = *a.loop;
    l.time += m_dt;
    Visual& v = a.visual;
    v.hidden = hidden_now(a);
    constexpr uint32_t k_unseen_ticks = 8; // as pose's other callers
    if (!l.played && (v.hidden || (a.posed && !v.seen && (m_tick + a.placed.ref) % k_unseen_ticks != 0))) {
        a.snap = true;
        return;
    }
    l.effect->update(l.time);
    const ActorSkeleton& s = *a.look.skeleton;
    for (size_t i = 0; i < s.nodes.bones.size(); ++i) {
        if (const uint32_t n = l.node_of[i]; n != ose::k_none) {
            glm::mat4 m(l.effect->node_world_rotation(n) * l.effect->node_world_scale(n));
            m[3] = glm::vec4(l.effect->node_world_translation_units(n) * units::k_skyrim_unit_m, 1.0f);
            l.nodes[i] = m;
        }
    }
    if (l.played) {
        ObjectMove move;
        move.ref = a.placed.ref;
        const glm::quat turn = glm::quat_cast(v.rotation);
        for (size_t i = 0; i < l.nodes.size(); ++i) {
            const glm::mat3 linear(l.nodes[i]);
            const float scale = glm::length(linear[0]);
            physics::NodePose p;
            p.node = static_cast<uint32_t>(i);
            p.position = v.position + v.rotation * (glm::vec3(l.nodes[i][3]) * v.scale);
            p.rotation = turn * glm::quat_cast(scale > 0.0f ? linear / scale : glm::mat3(1.0f));
            move.nodes.push_back(p);
        }
        m_object_moves.push_back(std::move(move));
    }
    pose(a);
}

bool ActorSet::play_sequence(uint32_t ref, const std::string& name, bool start_over, float ease_in) {
    join();
    const auto it = m_live.find(ref);
    if (it == m_live.end() || !it->second->loop) {
        return false;
    }
    ObjectLoop& l = *it->second->loop;
    if (!l.effect->sequence_active(name) || start_over) {
        for (const std::string& s : l.effect->sequence_names()) {
            l.effect->deactivate_sequence(s, ease_in);
        }
        l.effect->activate_sequence(name, 1.0f, ease_in);
    }
    l.played = true;
    return true;
}

void ActorSet::build_nodes(Live& a) {
    Animation& m = *a.animation;
    const ActorSkeleton& s = *a.look.skeleton;
    for (size_t i = 0; i < s.nodes.bones.size(); ++i) {
        glm::mat4 local = s.rest_local[i];
        const int32_t b = m.bones[i];
        if (b >= 0 && m.output.pose_fraction > 0.0f && size_t(b) < m.output.pose.size()) {
            const anim::Qs& q = m.output.pose[size_t(b)];
            const float scale = s.nodes.bones[i].scale; // the game writes rotation and translation, not scale
            local = glm::mat4(glm::mat3_cast(q.r) * scale);
            local[3] = glm::vec4(q.t * units::k_skyrim_unit_m, 1.0f);
        }
        const int32_t parent = s.nodes.bones[i].parent;
        m.nodes[i] = parent >= 0 ? m.nodes[size_t(parent)] * local : local;
    }
}

// What touches the world or what all actors share, in a fixed order.
void ActorSet::animate_finish(Live& a) {
    Animation& m = *a.animation;
    if (m.pair_done && m.corpse) {
        // Its graph has stopped, partway through leaving: the other side goes on and ends alone.
        m.character.sync_scene->abandon(&m.character);
        m.character.sync_scene.reset();
        m.pair_done.reset();
    }
    if (m.pair_done) {
        // The clip's marks place whoever isn't leading: in a kill move the killer goes to the victim, whose generator leads.
        if (m.output.world_from_model_fraction > 0.0f && !m.dying && !m.corpse) {
            // Foot IK has put its feedback on the output, for the capsule: not part of the mark.
            a.visual.position = (m.output.world_from_model.t - m.character.foot_ik_feedback) * units::k_skyrim_unit_m;
            a.visual.rotation = glm::mat3_cast(m.output.world_from_model.r);
            a.visual.moved = true;
        }
        if (m.pair_done->load()) {
            m.pair_done.reset();
            m.character.sync_scene.reset();
            m.queued.emplace_back("PairEnd");
        }
    }
    if (m.dying) {
        if (!m.laid_out) {
            keyframe_ragdoll(a);
        }
        // Nothing left playing means no clip end to send `Ragdoll`: on open ground no bone touches anything either.
        std::vector<anim::ActiveClip> clips;
        m.graph->active_clips(clips);
        if (m.ragdoll_event || clips.empty()) {
            start_ragdoll(a);
        }
    }
    if (m.corpse) {
        follow_ragdoll(a);
    }
    if (!a.visual.hidden && m_no_body.contains(a.placed.ref)) {
        a.visual.hidden = true;
    }
    if (m.dying || m.corpse) {
        build_nodes(a);
    }
    if (a.placed.object) {
        Visual& v = a.visual;
        const bool hide = hidden_now(a);
        if (hide != v.hidden) {
            v.hidden = hide;
        }
        if (m.nodes != m.shown_nodes) {
            m.shown_nodes = m.nodes;
            ObjectMove move;
            move.ref = a.placed.ref;
            const glm::quat turn = glm::quat_cast(v.rotation);
            for (size_t i = 0; i < m.nodes.size(); ++i) {
                const glm::mat3 linear(m.nodes[i]);
                const float scale = glm::length(linear[0]);
                physics::NodePose p;
                p.node = static_cast<uint32_t>(i);
                p.position = v.position + v.rotation * (glm::vec3(m.nodes[i][3]) * v.scale);
                p.rotation = turn * glm::quat_cast(scale > 0.0f ? linear / scale : glm::mat3(1.0f));
                move.nodes.push_back(p);
            }
            m_object_moves.push_back(std::move(move));
        } else {
            return; // nothing moved: what the GPU has stands
        }
    }
    // Most of a town is behind the camera. Who wasn't drawn since the last pose, in a view or a shadow, is posed and skinned
    // one tick in eight; what they do goes on, and on coming into view the next tick catches them up.
    constexpr uint32_t k_unseen_ticks = 8;
    if (m_ahead && a.posed && !a.visual.seen && (m_tick + a.placed.ref) % k_unseen_ticks != 0) {
        a.snap = true;
        return;
    }
    pose(a);
}

// Which of the skeleton's nodes each of the model's bones follows: names don't change, so this runs once per part.
void ActorSet::bind_bones(Part& part, const render::SkinnedModel& model, const ActorSkeleton* skeleton) {
    const auto node_of = [&](const std::string& name) {
        const auto it = skeleton->by_name.find(name);
        return it != skeleton->by_name.end() ? int32_t(it->second) : int32_t(-1);
    };
    part.binds.resize(model.bones.size());
    const std::string attach = lower(model.attach);
    for (size_t i = 0; i < model.bones.size(); ++i) {
        const osm::SkinBone& bone = model.bones[i];
        BoneBind& bind = part.binds[i];
        bind.skin = glm::mat4(1.0f);
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) {
                bind.skin[c][r] = bone.skin_to_bone[r][c];
            }
        }
        if (!skeleton) {
            continue;
        }
        if (bone.name.empty()) {
            // A rigid model: on its node, and a weapon in the hand once drawn; a bow in the left (inferred). A quiver or shield stays.
            bind.node = node_of(part.source.left_hand ? "shield" : attach);
            if (part.source.left_hand) {
                bind.drawn_node = bind.node; // Shield, whatever its Prn says
            } else if (attach.starts_with("weapon")) {
                bind.drawn_node = node_of(attach == "weaponbow" ? "shield" : "weapon");
                if (bind.drawn_node < 0) {
                    bind.drawn_node = bind.node;
                }
            }
        } else if (const int32_t node = node_of(lower(bone.name)); node >= 0) {
            bind.node = node;
        } else if (const int32_t at = node_of(attach); at >= 0) {
            bind.node = at; // a node of the model's own: its skin_to_bone goes to the model's root, held at this node
        } else {
            bind.skin = glm::mat4(1.0f); // as authored
        }
    }
}

// The model's bones at the actor's nodes: its animated pose, else the skeleton's rest pose.
void ActorSet::pose_rows(const Live& a, const std::vector<glm::mat4>& nodes, std::vector<glm::vec4>& rows) const {
    const bool drawn = a.animation && a.animation->weapon_drawn;
    for (const RowBone& bone : a.row_plan) {
        const Part& part = a.parts[bone.part];
        const BoneBind& bind = part.binds[bone.bind];
        if ((part.source.spare && part.source.weapon != a.look.right_weapon) || (!part.prop.empty() && !part.prop_shown)) {
            rows.insert(rows.end(), 3, glm::vec4(0.0f)); // folded to a point: nothing to see
            continue;
        }
        if ((part.source.no_scabbard && bind.drawn_node < 0) || (part.source.left_hand && !drawn)) {
            rows.insert(rows.end(), 3, glm::vec4(0.0f)); // a left-hand weapon shows nothing sheathed
            continue;
        }
        const bool in_hand = drawn && (!part.source.weapon || part.source.weapon == a.look.right_weapon);
        const int32_t node = in_hand && bind.drawn_node >= 0 ? bind.drawn_node : bind.node;
        // Face and hair, and what's worn on them, leave with the head.
        constexpr uint32_t k_head_slots = 1u << 0 | 1u << 1 | 1u << 11 | 1u << 12 | 1u << 13; // 30, 31, 41, 42, 43
        const bool on_head = part.source.head || !part.source.face.empty() || (part.source.slots & k_head_slots);
        if (on_head && m_player_head_hidden && a.placed.ref == k_player_ref) {
            rows.insert(rows.end(), 3, glm::vec4(0.0f)); // the eye is in there
            continue;
        }
        const bool cut = a.severed && on_head;
        const std::vector<glm::mat4>& from = cut ? a.severed->nodes : nodes;
        glm::mat4 m = bind.skin;
        if (node >= 0) {
            m = from[size_t(node)] * bind.skin;
        }
        if (cut) {
            m = a.severed->now * m;
        }
        for (int r = 0; r < 3; ++r) {
            rows.emplace_back(m[0][r], m[1][r], m[2][r], m[3][r]);
        }
    }
}

void ActorSet::pose(Live& a) {
    auto& meshes = m_renderer.meshes();
    const ActorSkeleton* skeleton = a.look.skeleton.get();
    const std::vector<glm::mat4>* nodes = a.loop      ? &a.loop->nodes
                                          : a.animation ? &a.animation->nodes
                                          : skeleton    ? &skeleton->rest
                                                        : nullptr;
    auto& jobs = m_data.skin_jobs();
    auto& rows = m_data.skin_bones();
    auto& before = m_data.skin_bones_before();
    const size_t first_row = rows.size();
    const size_t first_job = jobs.size();
    std::vector<RowBone>& plan = m_row_plan;
    plan.clear();
    for (auto& part : a.parts) {
        if (part.variant == render::k_no_variant) {
            continue;
        }
        const render::SkinnedModel* model = meshes.skinned_model(part.mesh);
        for (size_t k = 0; k < model->shapes.size() && k < part.targets.size(); ++k) {
            if (part.targets[k] == 0) {
                continue;
            }
            const render::SkinnedShape& shape = model->shapes[k];
            render::SkinJob job;
            job.source = shape.vertices;
            job.skin = shape.skin;
            job.morph = shape.morph;
            job.target = part.targets[k];
            job.vertices = shape.vertex_count;
            job.morph_weight = part.source.morph_weight;
            job.flags = shape.axes ? render::k_skin_axes : 0;
            job.first_bone = static_cast<uint32_t>(first_row / 3 + plan.size()); // the rows come after this loop, in plan order
            if (k < part.face.size() && part.face[k]) {
                const FaceShape& face = *part.face[k];
                auto& morphs = m_data.skin_face();
                job.first_face = static_cast<uint32_t>(morphs.size());
                morphs.insert(morphs.end(), face.fixed.begin(), face.fixed.end());
                // a weight is the result as it stands, used only in (0, 1]; phonemes
                // reach further than the rest; the look pairs net out.
                const float away = glm::distance(a.visual.position, m_camera) / units::k_skyrim_unit_m;
                if (a.face.morphs) {
                    a.face.shown = a.face.weights;
                }
                std::array<float, k_face_channels> w = a.face.shown;
                constexpr size_t look = k_face_phonemes + 8; // down, left, right, up
                const float up = w[look + 3] - w[look], right = w[look + 2] - w[look + 1];
                w[look + 3] = std::max(up, 0.0f);
                w[look] = std::max(-up, 0.0f);
                w[look + 2] = std::max(right, 0.0f);
                w[look + 1] = std::max(-right, 0.0f);
                const bool close = away <= k_face_lod_distance + 10.0f || a.face.eyes_closed;
                for (int c = 0; c < k_face_channels; ++c) {
                    const float weight = w[size_t(c)];
                    const bool in_reach = c < k_face_phonemes ? away <= k_face_talking_distance + 10.0f : close;
                    if (const int16_t m = face.channels[size_t(c)]; m >= 0 && in_reach && weight > 0.0f && weight <= 1.0f) {
                        morphs.push_back({face.tri->deltas[size_t(m)], weight * face.tri->scales[size_t(m)], 0});
                    }
                }
                job.face_count = static_cast<uint32_t>(morphs.size()) - job.first_face;
            }
            if (part.binds.empty()) {
                bind_bones(part, *model, skeleton);
            }
            for (uint32_t b = 0; b < shape.bone_count; ++b) {
                plan.push_back({uint32_t(&part - a.parts.data()), shape.first_bone + b});
            }
            jobs.push_back(job);
        }
    }
    if (a.look.headless && !a.severed) {
        sever(a, false); // a look made again: the head lies by the body
    }
    if (a.severed) {
        follow_head(a);
    }
    // The graph's worker may have worked them out already, from the same list.
    const bool drawn = a.animation && a.animation->weapon_drawn;
    if (plan == a.row_plan && a.ready_rows.size() == plan.size() * 3 && a.ready_drawn == drawn && a.animation &&
        !a.animation->dying && !a.animation->corpse && !a.severed) {
        rows.insert(rows.end(), a.ready_rows.begin(), a.ready_rows.end());
    } else {
        a.row_plan = plan;
        pose_rows(a, *nodes, rows);
    }
    a.ready_rows.clear();
    // In view, the rows of the tick before go with these, for the frames between; nothing changes in what they mean.
    if (a.visual.seen && !a.snap && a.last_rows.size() == rows.size() - first_row) {
        const uint32_t at = uint32_t(before.size() / 3);
        before.insert(before.end(), a.last_rows.begin(), a.last_rows.end());
        for (size_t j = first_job; j < jobs.size(); ++j) {
            jobs[j].before_bone = at + (jobs[j].first_bone - uint32_t(first_row / 3));
        }
    }
    a.last_rows.assign(rows.begin() + std::ptrdiff_t(first_row), rows.end());
    a.visual.seen = false;
    a.snap = false;
    a.posed = true;
}

void ActorSet::set_player(const PlayerBody& body) {
    join();
    place_player(k_player_ref, body.third_person, false, body);
    place_player(k_player_first_person_ref, body.first_person, true, body);
}

void ActorSet::new_player_body() { end(k_player_ref); }

void ActorSet::place_player(uint32_t ref, bool visible, bool first_person, const PlayerBody& body) {
    auto it = m_live.find(ref);
    if (!visible) {
        if (it != m_live.end()) {
            end(ref);
        }
        return;
    }
    if (it == m_live.end()) {
        PlacedActor p;
        p.ref = ref;
        p.base = k_player_npc;
        p.position = body.feet;
        p.heading = body.heading;
        p.first_person = first_person;
        start(p);
        return;
    }
    // The player's death falls to the third-person body, which may have only just started: first person has no ragdoll.
    Live& live = *it->second;
    if (body.dead && !first_person && live.animation) {
        kill(ref);
    }
    if (!live.animation || !(live.animation->dying || live.animation->corpse || live.animation->no_body)) {
        reposition(live, body);
    }
}

void ActorSet::hold_player(const glm::vec3& eye_feet, const glm::vec3& feet, float heading, bool third) {
    const float c = std::cos(heading), s = std::sin(heading);
    m_first_person_feet = eye_feet;
    m_player_rotation = glm::mat3(glm::vec3(c, -s, 0.0f), glm::vec3(s, c, 0.0f), glm::vec3(0, 0, 1));
    const auto it = third ? m_live.find(k_player_ref) : m_live.end();
    m_third_person_held = it != m_live.end() && it->second->animation && !it->second->animation->dying &&
                          !it->second->animation->corpse;
    if (m_third_person_held) {
        m_third_person_feet = feet + glm::vec3(0.0f, 0.0f, it->second->animation->foot_drop.load(std::memory_order_relaxed));
    }
}

bool ActorSet::held_player(const Visual& v, glm::vec3& position, glm::mat3& rotation) const {
    if (v.ref == k_player_first_person_ref) {
        position = m_first_person_feet;
    } else if (v.ref == k_player_ref && m_third_person_held) {
        position = m_third_person_feet;
    } else {
        return false;
    }
    rotation = m_player_rotation;
    return true;
}

void ActorSet::drive(uint32_t ref, const PlayerBody& body) {
    join();
    if (const auto it = m_live.find(ref); it != m_live.end()) {
        reposition(*it->second, body);
    }
    for (PlacedActor& p : m_placed) {
        if (p.ref == ref) {
            p.position = body.feet;
            p.heading = body.heading;
        }
    }
}

glm::vec3 ActorSet::node_offset(uint32_t ref, std::string_view lower_name) const {
    join();
    const auto it = m_live.find(ref);
    if (it == m_live.end() || !it->second->animation || !it->second->look.skeleton) {
        return glm::vec3(0.0f);
    }
    const Live& a = *it->second;
    const auto node = a.look.skeleton->by_name.find(std::string(lower_name));
    if (node == a.look.skeleton->by_name.end()) {
        return glm::vec3(0.0f);
    }
    return glm::vec3(a.animation->nodes[node->second][3]);
}

glm::vec3 ActorSet::take_motion(uint32_t ref) {
    join();
    auto it = m_live.find(ref);
    if (it == m_live.end() && ref == k_player_body) {
        it = m_live.find(k_player_first_person_ref);
    }
    if (it == m_live.end() || !it->second->animation) {
        return glm::vec3(0.0f);
    }
    Animation& m = *it->second->animation;
    m.collecting = true;
    const glm::vec3 out = m.motion;
    m.motion = glm::vec3(0.0f);
    return out;
}

void ActorSet::move(uint32_t ref, const glm::vec3& feet, float heading, float speed, float pitch) {
    join();
    if (const auto it = m_live.find(ref); it != m_live.end()) {
        PlayerBody body;
        body.feet = feet;
        body.heading = heading;
        body.speed = speed;
        body.pitch = pitch;
        reposition(*it->second, body);
    }
    if (const auto it = m_placed_at.find(ref); it != m_placed_at.end()) {
        PlacedActor& p = m_placed[it->second];
        p.position = feet;
        p.heading = heading;
    }
}

void ActorSet::set_placed(std::vector<PlacedActor> placed) {
    join();
    std::unordered_set<uint32_t> keep;
    for (const PlacedActor& p : placed) {
        keep.insert(p.ref);
    }
    std::vector<uint32_t> gone;
    for (const auto& [ref, live] : m_live) {
        const bool dead = live->placed.dead || live->placed.object || (live->animation && live->animation->corpse);
        if (ref != k_player_ref && ref != k_player_first_person_ref && !dead && !keep.contains(ref)) {
            gone.push_back(ref);
        }
    }
    for (const uint32_t ref : gone) {
        end(ref);
    }
    m_placed = std::move(placed);
    index_placed();
    m_placed_ready = true;
}

bool ActorSet::add_object(uint32_t ref, uint32_t base, const std::string& mesh, const glm::vec3& position) {
    join();
    if (!m_data.object_model(mesh)) {
        return false;
    }
    PlacedActor a;
    a.ref = ref;
    a.base = base;
    a.object = true;
    a.mesh = mesh;
    a.position = position;
    m_placed_dead.push_back(std::move(a));
    return true;
}

void ActorSet::remove_object(uint32_t ref) {
    join();
    if (const auto it = std::ranges::find(m_placed_dead, ref, &PlacedActor::ref);
        it != m_placed_dead.end() && it->object) {
        m_placed_dead.erase(it);
        if (m_live.contains(ref)) {
            end(ref);
        }
    }
}

void ActorSet::index_placed() {
    m_placed_at.clear();
    for (size_t i = 0; i < m_placed.size(); ++i) {
        m_placed_at.emplace(m_placed[i].ref, uint32_t(i));
    }
}

void ActorSet::reposition(Live& a, const PlayerBody& body) {
    if (a.animation && (a.animation->dying || a.animation->corpse || a.animation->pair_done)) {
        return;
    }
    a.placed.position = body.feet;
    a.placed.heading = body.heading;
    if (a.animation) {
        a.animation->speed = body.speed;
        a.animation->direction = body.direction;
        a.animation->pitch = body.pitch;
        a.animation->run_blend = body.run_blend;
        Animation& m = *a.animation;
        m.turn_delta = body.turn_delta;
        if (body.sneaking != m.sneaking) {
            m.graph->set_bool("IsSneaking", body.sneaking);
            m.graph->set_int("iIsInSneak", body.sneaking ? 1 : 0);
            m.graph->notify(body.sneaking ? "SneakStart" : "SneakStop");
            m.sneaking = body.sneaking;
        }
        if (body.sprinting != m.sprinting) {
            m.graph->set_bool("IsSprinting", body.sprinting);
            m.graph->notify(body.sprinting ? "SprintStart" : "SprintStop");
            m.sprinting = body.sprinting;
        }
    }
    if (!a.finished || a.visual.draws.empty()) {
        return;
    }
    Visual& v = a.visual;
    const float c = std::cos(body.heading), s = std::sin(body.heading);
    const glm::mat3 rotation(glm::vec3(c, -s, 0.0f), glm::vec3(s, c, 0.0f), glm::vec3(0, 0, 1));
    const glm::vec3 feet = a.animation ? body.feet + glm::vec3(0.0f, 0.0f, a.animation->foot_drop.load(std::memory_order_relaxed))
                                       : body.feet;
    if (feet != v.position || rotation != v.rotation) {
        // A jump is a jump: a teleport or a door shows no way there.
        constexpr float k_snap_m = 2.0f;
        const bool snap = glm::distance(feet, v.position) > k_snap_m;
        v.before_position = snap ? feet : v.position;
        v.before_rotation = snap ? rotation : v.rotation;
        v.moved_tick = m_tick;
        a.snap = a.snap || snap;
        v.sphere = glm::vec4(glm::vec3(v.sphere) - v.position + feet, v.sphere.w);
        v.position = feet;
        v.rotation = rotation;
        v.moved = true;
    }
}

// The game's queue gate: the 10 nearest actors, the player and whoever is talking.
void ActorSet::gate_faces() {
    constexpr size_t k_morphed = 10;
    std::vector<std::pair<float, Live*>> order;
    for (auto& [ref, live] : m_live) {
        const bool always = live->look.player || (live->animation && live->animation->looking.load(std::memory_order_relaxed));
        live->face.morphs = always;
        if (!always) {
            const glm::vec3 d = live->visual.position - m_camera;
            order.push_back({glm::dot(d, d), live.get()});
        }
    }
    const size_t count = std::min(order.size(), k_morphed);
    std::partial_sort(order.begin(), order.begin() + std::ptrdiff_t(count), order.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t i = 0; i < count; ++i) {
        order[i].second->face.morphs = true;
    }
}

// The game (): the payload names an ANIO; Load brings its model in unseen,
// Draw shows it, on the node its Prn gives. The record's unload event puts it away.
void ActorSet::update_props() {
    join(); // parts are read on the workers
    std::vector<PropEvent> events;
    {
        std::lock_guard lock(m_events_mutex);
        events.swap(m_prop_events);
    }
    auto& meshes = m_renderer.meshes();
    for (const PropEvent& e : events) {
        const auto it = m_live.find(e.ref);
        if (it == m_live.end() || !it->second->finished) {
            continue;
        }
        Live& a = *it->second;
        const bool load = e.tag == "AnimObjLoad", draw = e.tag == "AnimObjDraw";
        if (!load && !draw) {
            const std::string tag = lower(e.tag);
            for (Part& part : a.parts) {
                part.prop_shown = part.prop_shown && part.prop_unload != tag;
            }
            continue;
        }
        const std::string id = lower(e.payload);
        auto part = std::ranges::find(a.parts, id, &Part::prop);
        if (part == a.parts.end()) {
            const ActorData::AnimObject* object = m_data.anim_object(id);
            if (!object) {
                log::warn("actors: {:08X}: no animation object {}", e.ref, e.payload);
                continue;
            }
            Part p;
            p.source.mesh = object->mesh;
            p.source.whole = true;
            p.prop = id;
            p.prop_unload = object->unload;
            p.mesh = meshes.acquire(object->mesh, 1.0f);
            a.parts.push_back(std::move(p));
            part = a.parts.end() - 1;
            m_props_loading.push_back(e.ref);
        }
        part->prop_shown = part->prop_shown || draw;
    }
    std::erase_if(m_props_loading, [&](uint32_t ref) {
        const auto it = m_live.find(ref);
        if (it == m_live.end()) {
            return true;
        }
        Live& a = *it->second;
        bool waiting = false;
        for (size_t i = 0; i < a.parts.size(); ++i) {
            Part& part = a.parts[i];
            if (part.prop.empty() || part.variant != render::k_no_variant || meshes.failed(part.mesh)) {
                continue;
            }
            if (meshes.ready(part.mesh)) {
                glm::vec3 lo(1e30f), hi(-1e30f);
                finish_parts(a, i, i + 1, lo, hi);
            } else {
                waiting = true;
            }
        }
        return !waiting;
    });
}

void ActorSet::update(const glm::vec3& camera) {
    if (m_still) {
        if (m_visuals_dirty) {
            rebuild_visuals(); // whoever ended since the last tick is gone from what draws
        }
        return;
    }
    m_camera = camera;
    update_props();
    gate_faces();
    if (m_placing.valid() && m_placing.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto placed = m_placing.get();
            log::info("actors: {} placed in {}", placed.size(), m_world->edid);
            std::ranges::copy_if(placed, std::back_inserter(m_placed_dead),
                                 [](const PlacedActor& p) { return p.dead || p.object; });
            std::erase_if(placed, [](const PlacedActor& p) { return p.dead || p.object; });
            if (!m_placed_ready) {
                m_placed = std::move(placed);
                index_placed();
            }
        } catch (const std::exception& e) {
            log::warn("actors: {}: {}", m_world->edid, e.what());
        }
        m_placed_ready = true;
    }
    const bool interior = (m_world->flags & osw::world_interior) != 0;
    const float cs = m_world->cell_size;
    const glm::ivec2 cell(int32_t(std::floor(camera.x / cs)), int32_t(std::floor(camera.y / cs)));
    auto near_camera = [&](const glm::vec3& p, int32_t margin) {
        if (interior) {
            return true;
        }
        const glm::ivec2 c(int32_t(std::floor(p.x / cs)), int32_t(std::floor(p.y / cs)));
        const glm::ivec2 d = glm::abs(c - cell);
        return std::max(d.x, d.y) <= m_grid_radius + margin;
    };
    uint32_t started = 0;
    for (const auto* list : {&m_placed, &m_placed_dead}) {
        for (const auto& placed : *list) {
            if (started < k_start_per_frame && near_camera(placed.position, 0) && !m_live.contains(placed.ref)) {
                start(placed);
                ++started;
            }
        }
    }
    m_starting = started == k_start_per_frame;
    SPUD_TRACE("actors finish");
    std::vector<uint32_t> gone;
    uint32_t finished = 0;
    for (auto& [ref, live] : m_live) {
        Live& a = *live;
        if (ref != k_player_ref && ref != k_player_first_person_ref && !near_camera(a.placed.position, 1)) {
            gone.push_back(ref);
            continue;
        }
        if (a.finished) {
            update_weapon_blood(a);
            update_skin_decals(a);
        }
        if (!a.built && a.building.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            a.built = true;
            try {
                a.look = a.building.get();
            } catch (const std::exception& e) {
                log::warn("actors: {:08X}: {}", ref, e.what());
                a.finished = true;
                continue;
            }
            auto& meshes = m_renderer.meshes();
            for (const auto& source : a.look.parts) {
                Part p;
                p.source = source;
                p.mesh = meshes.acquire(source.mesh, 1.0f);
                for (const FacePart& f : source.face) {
                    p.face_morphs.push_back({meshes.acquire_face_morphs(f.tri), meshes.acquire_face_morphs(f.race),
                                             meshes.acquire_face_morphs(f.chargen)});
                }
                a.parts.push_back(std::move(p));
            }
        }
        if (a.built && !a.finished && finished < k_finish_per_frame) {
            const auto& meshes = m_renderer.meshes();
            const bool loaded = std::ranges::all_of(a.parts, [&](const Part& p) {
                return (meshes.ready(p.mesh) || meshes.failed(p.mesh)) &&
                       std::ranges::all_of(p.face_morphs, [&](const auto& sets) {
                           return std::ranges::all_of(sets, [&](uint32_t s) { return meshes.face_morphs_settled(s); });
                       });
            });
            if (loaded) {
                finish(a);
                ++finished;
            }
        }
    }
    {
        SPUD_TRACE("actors leaving");
        for (uint32_t ref : gone) {
            end(ref);
        }
    }
    if (m_hold) {
        const float dt = m_dt;
        physics::World* world = m_physics;
        m_dt = 1.0f / 60.0f;
        m_physics = nullptr; // foot IK waits on collision streaming, which goes by how the camera got here
        for (auto& [ref, live] : m_live) {
            if (live->animation && !live->held) {
                animate(*live);
                live->held = true;
            } else if (live->loop && !live->held) {
                run_loop(*live);
                live->held = true;
            } else if (live->held) {
                pose(*live); // skinned every frame as a moving actor is: one dispatch alone isn't repeatable
            }
        }
        m_dt = dt;
        m_physics = world;
    } else if (m_dt > 0.0f) {
        const auto start = std::chrono::steady_clock::now();
        // On play's clock the graphs run between ticks: what they made last time is posed now, and they start again on the
        // workers with what this tick gave them, while the frames between draw. Actors show one tick later for it.
        join();
        for (Live* live : m_animating) {
            animate_finish(*live);
        }
        m_animating.clear();
        for (auto& [ref, live] : m_live) {
            if (live->loop) {
                run_loop(*live);
            }
        }
        for (auto& [ref, live] : m_live) {
            if (live->animation) {
                m_animating.push_back(live.get());
            }
        }
        if (m_ahead) {
            // Readers take the finished tick's events: these graphs run on while the frame goes on, and an event they
            // raised between a read and its clear was lost.
            join();
            finish_events();
            m_in_flight = true;
            parallel::start(m_animating.size(), [this](size_t i) { animate_graph(*m_animating[i]); });
        } else {
            m_events_from = m_events.size();
            parallel::for_each(m_animating.size(), [this](size_t i) { animate_graph(*m_animating[i]); });
            m_in_flight = true;
            join();
            finish_events();
            for (Live* live : m_animating) {
                animate_finish(*live);
            }
            m_animating.clear();
        }
        m_animate_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    if (m_visuals_dirty) {
        rebuild_visuals();
    }
    ++m_tick;
}

void ActorSet::rebuild_visuals() {
    m_visuals.clear();
    for (auto& [ref, live] : m_live) {
        if (live->finished && !live->visual.draws.empty()) {
            m_visuals.push_back(&live->visual);
        }
    }
    // Sorted: what draws must not depend on the order actors finished loading.
    std::sort(m_visuals.begin(), m_visuals.end(), [](const Visual* a, const Visual* b) { return a->ref < b->ref; });
    m_visuals_dirty = false;
}

ActorSet::Live* ActorSet::live_actor(uint32_t ref) {
    auto it = m_live.find(ref);
    if (it == m_live.end() && ref == k_player_body) {
        it = m_live.find(k_player_first_person_ref);
    }
    return it != m_live.end() && it->second->animation ? it->second.get() : nullptr;
}

bool ActorSet::send_event(uint32_t ref, std::string_view event) {
    join();
    Live* a = live_actor(ref);
    return a && a->animation->graph->notify(event);
}

void ActorSet::queue_event(uint32_t ref, std::string event) {
    join();
    if (Live* a = live_actor(ref)) {
        a->animation->queued.push_back(std::move(event));
    }
}

bool ActorSet::dying(uint32_t ref) {
    join();
    const Live* a = live_actor(ref);
    return a && a->animation && a->animation->dying;
}

bool ActorSet::death_refused(uint32_t ref) {
    join();
    const Live* a = live_actor(ref);
    return a && a->animation && a->animation->ragdoll_event;
}

void ActorSet::death_taken(uint32_t ref) {
    join();
    if (Live* a = live_actor(ref); a && a->animation) {
        a->animation->ragdoll_event = false;
    }
}

void ActorSet::clear_events(uint32_t ref) {
    join();
    if (Live* a = live_actor(ref)) {
        a->animation->queued.clear();
        a->animation->queued_tries = 0;
    }
}

bool ActorSet::play_idle(uint32_t ref, std::string_view idle_editor_id) {
    join();
    const std::string event = m_data.idle_event(idle_editor_id);
    return !event.empty() && send_event(ref, event);
}

std::string ActorSet::behaviour_folder(uint32_t ref) const {
    join();
    auto it = m_live.find(ref);
    if (it == m_live.end() && ref == k_player_body) {
        it = m_live.find(k_player_first_person_ref);
    }
    return it != m_live.end() ? it->second->look.behaviour_folder : std::string();
}

std::vector<uint32_t> ActorSet::weapons(uint32_t ref) const {
    join();
    std::vector<uint32_t> out;
    if (const auto it = m_live.find(ref); it != m_live.end() && it->second->built) {
        for (const auto& [form, type] : it->second->look.weapons) {
            out.push_back(form);
        }
    }
    return out;
}

void ActorSet::swap_weapon(uint32_t ref, uint32_t form) {
    join();
    const auto it = m_live.find(ref);
    if (it == m_live.end() || !it->second->built) {
        return;
    }
    ActorLook& look = it->second->look;
    if (!form) { // out of the hand for good: its part goes
        for (Part& part : it->second->parts) {
            if (part.source.weapon == look.right_weapon) {
                part.source.spare = true;
            }
        }
        std::erase_if(look.weapons, [&look](const auto& w) { return w.first == look.right_weapon; });
        look.right_weapon = 0;
        look.right_hand_type = 0;
        set_variable(ref, "iRightHandType", 0.0f);
        return;
    }
    const auto weapon = std::ranges::find(look.weapons, form, &std::pair<uint32_t, int32_t>::first);
    if (weapon != look.weapons.end()) {
        look.right_weapon = form;
        look.right_hand_type = weapon->second;
        set_variable(ref, "iRightHandType", float(weapon->second));
    }
}

void ActorSet::gear(uint32_t ref, std::vector<uint32_t>& armor, uint32_t& right) const {
    join();
    if (const auto it = m_live.find(ref); it != m_live.end() && it->second->built) {
        armor = it->second->look.armor;
        right = it->second->look.right_weapon;
    }
}

std::vector<std::string> ActorSet::graph_nodes(uint32_t ref) const {
    join();
    auto it = m_live.find(ref);
    if (it == m_live.end() && ref == k_player_body) {
        it = m_live.find(k_player_first_person_ref);
    }
    return it != m_live.end() && it->second->animation ? it->second->animation->graph->active_nodes() : std::vector<std::string>{};
}

bool ActorSet::get_int(uint32_t ref, std::string_view name, int32_t& out) const {
    join();
    auto it = m_live.find(ref);
    if (it == m_live.end() && ref == k_player_body) {
        it = m_live.find(k_player_first_person_ref);
    }
    return it != m_live.end() && it->second->animation && it->second->animation->graph->get_int(name, out);
}

bool ActorSet::get_float(uint32_t ref, std::string_view name, float& out) const {
    join();
    auto it = m_live.find(ref);
    if (it == m_live.end() && ref == k_player_body) {
        it = m_live.find(k_player_first_person_ref);
    }
    return it != m_live.end() && it->second->animation && it->second->animation->graph->get_float(name, out);
}

bool ActorSet::get_bool(uint32_t ref, std::string_view name) const {
    join();
    auto it = m_live.find(ref);
    if (it == m_live.end() && ref == k_player_body) {
        it = m_live.find(k_player_first_person_ref);
    }
    bool value = false;
    return it != m_live.end() && it->second->animation && it->second->animation->graph->get_bool(name, value) && value;
}

std::optional<glm::vec3> ActorSet::head_position(uint32_t ref) {
    join(); // scripts' targets only: rare
    const Live* a = live_actor(ref);
    if (!a || !a->animation || !a->look.skeleton) {
        return std::nullopt;
    }
    const auto head = a->look.skeleton->by_name.find("npc head [head]");
    if (head == a->look.skeleton->by_name.end() || head->second >= a->animation->nodes.size()) {
        return a->visual.position;
    }
    const glm::vec3 local(a->animation->nodes[head->second][3]);
    return a->visual.position + a->visual.rotation * (local * a->visual.scale * units::k_skyrim_unit_m);
}

float ActorSet::foot_drop(uint32_t ref) const {
    const auto it = m_live.find(ref);
    return it != m_live.end() && it->second->animation ? it->second->animation->foot_drop.load(std::memory_order_relaxed) : 0.0f;
}

void ActorSet::set_look_target(uint32_t ref, const glm::vec3* target) {
    Live* a = live_actor(ref);
    if (!a || !a->animation) {
        return;
    }
    Animation& m = *a->animation;
    if (target) {
        const glm::vec3 units = *target / units::k_skyrim_unit_m;
        m.look_x.store(units.x, std::memory_order_relaxed);
        m.look_y.store(units.y, std::memory_order_relaxed);
        m.look_z.store(units.z, std::memory_order_relaxed);
    }
    m.looking.store(target != nullptr, std::memory_order_relaxed);
}

bool ActorSet::set_variable(uint32_t ref, std::string_view name, float value) {
    join();
    Live* a = live_actor(ref);
    if (!a) {
        return false;
    }
    anim::Graph& g = *a->animation->graph;
    const anim::Project& p = g.project();
    const int32_t index = p.root_variable(name);
    if (index < 0) {
        return false;
    }
    switch (p.root().variable_types[size_t(index)]) {
    case anim::VariableType::real: return g.set_float(name, value);
    case anim::VariableType::boolean: return g.set_bool(name, value != 0.0f);
    default: return g.set_int(name, static_cast<int32_t>(value));
    }
}

void ActorSet::finish_events() {
    m_finished_events.insert(m_finished_events.end(), std::make_move_iterator(m_events.begin()), std::make_move_iterator(m_events.end()));
    m_events.clear();
    m_events_from = 0;
}

void ActorSet::join() const {
    if (!m_in_flight) {
        return;
    }
    parallel::wait();
    m_in_flight = false;
    // In the same order however the threads fell.
    std::stable_sort(m_events.begin() + std::ptrdiff_t(std::min(m_events_from, m_events.size())), m_events.end(),
                     [](const ActorEvent& a, const ActorEvent& b) { return a.ref < b.ref; });
}

const std::vector<ActorSet::Who>& ActorSet::who() {
    join();
    m_who.clear();
    for (const auto& [ref, live] : m_live) {
        m_who.push_back({ref, live->look.npc, live->placed.position});
    }
    std::ranges::sort(m_who, {}, &Who::ref);
    return m_who;
}

void ActorSet::solids(std::vector<Solid>& out) const {
    out.clear();
    for (const auto& [ref, live] : m_live) {
        if (live->built && live->look.npc && !live->placed.object && !live->placed.first_person && !dead(ref)) {
            out.push_back({ref, live->look.npc, live->look.race, live->look.female, live->placed.position,
                           live->placed.heading, live->look.height * live->placed.scale});
        }
    }
}

std::vector<ActorSet::Entry> ActorSet::list() const {
    join();
    std::vector<Entry> out;
    for (const auto& [ref, live] : m_live) {
        Entry e{ref, live->look.npc, live->placed.position, {}, {}};
        for (const auto& p : live->parts) {
            const auto slash = p.source.mesh.rfind('/');
            std::string shapes;
            if (const auto* m = m_renderer.meshes().ready(p.mesh) ? m_renderer.meshes().skinned_model(p.mesh) : nullptr) {
                for (uint16_t part : m->parts) {
                    shapes += std::format(" {}", part);
                }
            }
            e.parts.push_back(std::format("{} {:08x}:{}", p.source.mesh.substr(slash + 1), p.source.slots, shapes));
        }
        if (live->animation) {
            std::vector<anim::ActiveClip> clips;
            live->animation->graph->active_clips(clips);
            for (const auto& c : clips) {
                e.clips.push_back(c.name ? *c.name : std::string());
            }
        }
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.ref < b.ref; });
    return out;
}

bool ActorSet::busy() const {
    if (!m_placed_ready || m_starting) {
        return true;
    }
    return std::ranges::any_of(m_live, [](const auto& e) { return !e.second->finished; });
}

} // namespace spud::engine
