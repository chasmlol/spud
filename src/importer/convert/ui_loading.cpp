// LSCR records and what their conditions read -> .osl (docs/formats/osl.md); the facts about how the game uses
// each field are,.7.104.
//
// Record layouts (UESP "Skyrim Mod:Mod File Format", xEdit definitions): LSCR EDID, DESC (localized string),
// CTDA (+ CIS1/CIS2), NNAM (STAT), SNAM (float), RNAM (3 x int16), ONAM (2 x int16), XNAM (3 x float), MOD2;
// LCTN PNAM (parent), LCEC (worldspace, then packed cell coordinates); WRLD XLCN, XEZN; CELL DATA (flag 0x1
// interior), XLCN, XEZN; ECZN DATA (owner, location, ...); QUST DNAM (flags first), INDX (index, flags);
// GLOB FLTV; VMAD (Papyrus scripts and properties); NPC_ RNAM, KWDA; RACE KWDA; FLST LNAM.

#include "importer/convert/ui_loading.hpp"

#include "core/binary.hpp"
#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "formats/osm.hpp"
#include "formats/ose.hpp"
#include "importer/convert/mesh_import.hpp"
#include "importer/convert/particle_import.hpp"
#include "importer/convert/ui_strings.hpp"
#include "importer/legacy/data_source.hpp"
#include "importer/legacy/esm.hpp"
#include "importer/legacy/nif.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace spud::importer {

namespace {

// There, and in a version the engine can still read: an older file would leave its loading screen without a model.
bool readable_model(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    uint32_t head[3] = {};
    in.read(reinterpret_cast<char*>(head), sizeof head);
    return in && head[2] >= osm::k_min_version;
}

using legacy::record_type;

constexpr auto k_lscr = record_type("LSCR");
constexpr auto k_ctda = record_type("CTDA");
constexpr auto k_cis1 = record_type("CIS1");
constexpr auto k_cis2 = record_type("CIS2");

// The condition functions whose parameters are forms, by function index (opcode - 0x1000): which of the two
// parameters to resolve through the record's plugin. The load order's LSCRs use exactly these (the design lists
// them with their uses); any other function keeps its raw parameters and is reported.
struct FunctionParams {
    uint16_t function;
    bool form1, form2;
};
constexpr FunctionParams k_functions[] = {
    {46, false, false},  // GetDead
    {56, true, false},   // GetQuestRunning (quest)
    {58, true, false},   // GetStage (quest)
    {59, true, false},   // GetStageDone (quest, stage)
    {74, true, false},   // GetGlobalValue (global)
    {77, false, false},  // GetRandomPercent
    {310, true, false},  // GetInWorldspace (worldspace or form list)
    {359, true, false},  // GetInCurrentLoc (location)
    {543, true, false},  // GetQuestCompleted (quest)
    {560, true, false},  // HasKeyword (keyword)
    {629, true, false},  // GetVMQuestVariable (quest, variable name in CIS2)
};

const FunctionParams* function_params(uint16_t f) {
    for (const auto& p : k_functions) {
        if (p.function == f) {
            return &p;
        }
    }
    return nullptr;
}

std::string mesh_path(std::string_view model) {
    return path::with_root(path::normalize(model), "meshes");
}

// ---- Papyrus: VMAD properties and the compiled scripts' initial values -----------------------------------------

std::string wstring(BinaryReader& r) {
    const uint16_t n = r.read<uint16_t>();
    std::string s(n, '\0');
    for (uint16_t i = 0; i < n; ++i) {
        s[i] = static_cast<char>(r.read<uint8_t>());
    }
    return s;
}

struct ScriptValue {
    bool found = false;
    float value = 0.0f;
};

// VMAD (UESP "Skyrim Mod:Mod File Format/VMAD Field"): version, object format, scripts with their properties.
// Returns the scripts' names, and the value of property `property` (int, float or bool) if one sets it.
std::vector<std::string> vmad_scripts(std::span<const std::byte> vmad, std::string_view property, ScriptValue& out) {
    std::vector<std::string> scripts;
    BinaryReader r(vmad);
    const int16_t version = r.read<int16_t>();
    const int16_t format = r.read<int16_t>();
    const uint16_t count = r.read<uint16_t>();
    for (uint16_t s = 0; s < count; ++s) {
        scripts.push_back(wstring(r));
        if (version >= 4) {
            r.skip(1); // status
        }
        const uint16_t properties = r.read<uint16_t>();
        for (uint16_t p = 0; p < properties; ++p) {
            const std::string name = wstring(r);
            const uint8_t type = r.read<uint8_t>();
            if (version >= 4) {
                r.skip(1);
            }
            auto skip_value = [&](uint8_t t) {
                switch (t) {
                case 1: r.skip(8); break; // object: (format 2) alias, unused, form ID / (format 1) form ID, alias, unused
                case 2: wstring(r); break;
                case 3: r.skip(4); break;
                case 4: r.skip(4); break;
                case 5: r.skip(1); break;
                default: throw ParseError(std::format("VMAD property type {}", t));
                }
            };
            if (type >= 11 && type <= 15) {
                const uint32_t n = r.read<uint32_t>();
                for (uint32_t i = 0; i < n; ++i) {
                    skip_value(static_cast<uint8_t>(type - 10));
                }
                continue;
            }
            const bool wanted = path::to_lower(name) == path::to_lower(property);
            if (wanted && (type == 3 || type == 4 || type == 5)) {
                out.found = true;
                out.value = type == 3   ? static_cast<float>(r.read<int32_t>())
                            : type == 4 ? r.read<float>()
                                        : (r.read<uint8_t>() != 0 ? 1.0f : 0.0f);
                continue;
            }
            skip_value(type);
        }
    }
    (void)format;
    return scripts;
}

// Compiled Papyrus (.pex, big-endian; UESP "Skyrim Mod:Compiled Script File Format"): the initial value of an
// object variable, if the script declares it.
class BigEndian {
public:
    explicit BigEndian(std::span<const std::byte> d) : m_d(d) {}
    template <typename T>
    T read() {
        if (m_at + sizeof(T) > m_d.size()) {
            throw ParseError("pex: truncated");
        }
        uint8_t b[sizeof(T)];
        for (size_t i = 0; i < sizeof(T); ++i) {
            b[i] = static_cast<uint8_t>(m_d[m_at + sizeof(T) - 1 - i]);
        }
        m_at += sizeof(T);
        T v;
        std::memcpy(&v, b, sizeof(T));
        return v;
    }
    void skip(size_t n) { m_at += n; }
    std::string string() {
        const uint16_t n = read<uint16_t>();
        if (m_at + n > m_d.size()) {
            throw ParseError("pex: truncated string");
        }
        std::string s(reinterpret_cast<const char*>(m_d.data()) + m_at, n);
        m_at += n;
        return s;
    }

private:
    std::span<const std::byte> m_d;
    size_t m_at = 0;
};

ScriptValue pex_initial_value(std::span<const std::byte> pex, std::string_view variable) {
    BigEndian r(pex);
    if (r.read<uint32_t>() != 0xFA57C0DE) {
        throw ParseError("pex: bad magic");
    }
    r.skip(2 + 2 + 8); // version, game ID, compile time
    r.string();        // source file name
    r.string();        // user
    r.string();        // machine
    const uint16_t strings = r.read<uint16_t>();
    std::vector<std::string> table(strings);
    for (auto& s : table) {
        s = r.string();
    }
    auto name = [&](uint16_t i) { return i < table.size() ? table[i] : std::string(); };
    if (r.read<uint8_t>() != 0) {
        r.skip(8); // modification time
        const uint16_t functions = r.read<uint16_t>();
        for (uint16_t f = 0; f < functions; ++f) {
            r.skip(2 + 2 + 2 + 1);
            const uint16_t lines = r.read<uint16_t>();
            r.skip(size_t(lines) * 2);
        }
    }
    const uint16_t flags = r.read<uint16_t>();
    r.skip(size_t(flags) * 3);
    const uint16_t objects = r.read<uint16_t>();
    ScriptValue out;
    for (uint16_t o = 0; o < objects && !out.found; ++o) {
        r.skip(2 + 4 + 2 + 2 + 4 + 2); // name, size, parent, docstring, user flags, auto state
        const uint16_t variables = r.read<uint16_t>();
        for (uint16_t v = 0; v < variables; ++v) {
            const std::string var = name(r.read<uint16_t>());
            r.skip(2 + 4); // type name, user flags
            const uint8_t type = r.read<uint8_t>();
            float value = 0.0f;
            switch (type) {
            case 0: break;
            case 1:
            case 2: r.skip(2); break;
            case 3: value = static_cast<float>(r.read<int32_t>()); break;
            case 4: value = r.read<float>(); break;
            case 5: value = r.read<uint8_t>() != 0 ? 1.0f : 0.0f; break;
            default: throw ParseError(std::format("pex: variable data type {}", type));
            }
            if (path::to_lower(var) == path::to_lower(variable)) {
                out.found = true;
                out.value = value;
                break;
            }
        }
        break; // the variables of the first object are all this needs; the rest of the file is not parsed
    }
    return out;
}

class Importer {
public:
    Importer(const legacy::EsmDatabase& db, const legacy::DataSource& data, LoadingImport& out)
        : m_db(db), m_data(data), m_out(out), m_t(out.table), m_strings(db, data, "english") {}

    void run() {
        screens();
        locations();
        worldspaces();
        interior_cells();
        quests();
        globals();
        quest_variables();
        player_keywords();
        dead_references();
        form_lists();
        log::info("ui loading: {} screens ({} with conditions, {} conditions), {} locations, {} worldspace cells, "
                  "{} interior cells, {} quests, {} globals, {} quest variables, {} models, {} camera paths",
                  m_t.screens.size(),
                  std::ranges::count_if(m_t.screens, [](const auto& s) { return s.condition_count > 0; }),
                  m_t.conditions.size(), m_t.locations.size(), m_t.world_cells.size(), m_t.interior_cells.size(),
                  m_t.quests.size(), m_t.globals.size(), m_t.quest_variables.size(), m_out.models.size(),
                  m_out.camera_paths.size());
    }

private:
    void warn(std::string w) {
        log::warn("ui loading: {}", w);
        m_out.warnings.push_back(std::move(w));
    }

    uint32_t str(std::string_view s) { return m_t.add_string(s); }

    // A form's EDID and type, from its winning record.
    std::optional<legacy::Record> record(uint32_t id) const { return id ? m_db.get(id) : std::nullopt; }

    // ---- LSCR

    void screens() {
        std::unordered_set<uint32_t> seen;
        std::set<std::string> models, paths;
        for (const legacy::RecordEntry& e : m_db.entries()) {
            if (e.type != k_lscr || !seen.insert(e.form_id).second) {
                continue;
            }
            auto rec = m_db.get(e.form_id);
            if (!rec) {
                continue; // deleted by a later plugin
            }
            osl::LoadScreen s{};
            s.form_id = rec->form_id();
            s.edid = str(rec->edid());
            s.record_flags = rec->flags();
            s.text = str(m_strings.text(*rec, record_type("DESC")));
            s.model = s.camera_path = osl::k_none;
            s.scale = 1.0f;
            s.rotation_min = -180;
            s.rotation_max = 180;
            s.first_condition = static_cast<uint32_t>(m_t.conditions.size());
            const auto subs = rec->subrecords();
            for (size_t i = 0; i < subs.size(); ++i) {
                const auto& sr = subs[i];
                if (sr.type == k_ctda) {
                    condition(*rec, sr, i + 1 < subs.size() ? &subs[i + 1] : nullptr);
                } else if (sr.type == record_type("NNAM")) {
                    s.static_form = rec->form(record_type("NNAM"));
                } else if (sr.type == record_type("SNAM")) {
                    s.scale = sr.as<float>();
                } else if (sr.type == record_type("RNAM")) {
                    BinaryReader r = sr.reader();
                    for (auto& a : s.rotation) {
                        a = r.read<int16_t>();
                    }
                } else if (sr.type == record_type("ONAM")) {
                    BinaryReader r = sr.reader();
                    s.rotation_min = r.read<int16_t>();
                    s.rotation_max = r.read<int16_t>();
                } else if (sr.type == record_type("XNAM")) {
                    BinaryReader r = sr.reader();
                    for (auto& f : s.offset) {
                        f = r.read<float>();
                    }
                } else if (sr.type == record_type("MOD2")) {
                    const std::string p = mesh_path(sr.zstring());
                    if (p != "meshes/" && !sr.zstring().empty()) {
                        s.camera_path = str(path::replace_extension(p, ".ose"));
                        paths.insert(p);
                    }
                }
            }
            s.condition_count = static_cast<uint32_t>(m_t.conditions.size()) - s.first_condition;
            if (auto stat = record(s.static_form)) {
                const std::string_view modl = stat->string(record_type("MODL"));
                if (!modl.empty()) {
                    const std::string p = mesh_path(modl);
                    s.model = str(path::replace_extension(p, ".osm"));
                    models.insert(p);
                }
            } else if (s.static_form) {
                warn(std::format("LSCR {:08X}: NNAM {:08X} not found", s.form_id, s.static_form));
            }
            m_t.screens.push_back(s);
        }
        m_out.models.assign(models.begin(), models.end());
        m_out.camera_paths.assign(paths.begin(), paths.end());
    }

    void condition(const legacy::Record& rec, const legacy::Subrecord& sr, const legacy::Subrecord* next) {
        BinaryReader r = sr.reader();
        osl::Condition c{};
        c.flags = r.read<uint8_t>();
        r.skip(3);
        const uint32_t value_bits = r.read<uint32_t>();
        c.function = r.read<uint16_t>();
        r.skip(2);
        c.param1 = r.read<uint32_t>();
        c.param2 = r.read<uint32_t>();
        c.run_on = static_cast<uint8_t>(r.read<uint32_t>());
        c.reference = r.read<uint32_t>();
        c.param3 = sr.data.size() >= 32 ? r.read<int32_t>() : -1;
        if (c.flags & 0x04) {
            c.value_global = rec.resolve(value_bits);
            m_globals.insert(c.value_global);
            c.value = 0.0f;
        } else {
            std::memcpy(&c.value, &value_bits, 4);
        }
        const FunctionParams* fp = function_params(c.function);
        if (!fp) {
            warn(std::format("LSCR {:08X}: condition function {} is not one the loading screens use", rec.form_id(),
                             c.function));
        } else {
            if (fp->form1) {
                c.param1 = rec.resolve(c.param1);
            }
            if (fp->form2) {
                c.param2 = rec.resolve(c.param2);
            }
        }
        if (c.run_on == 2) {
            c.reference = rec.resolve(c.reference);
            m_references.insert(c.reference);
        }
        c.string_param = osl::k_none;
        if (next && (next->type == k_cis1 || next->type == k_cis2)) {
            c.string_param = str(next->zstring());
        }
        switch (c.function) {
        case 56:
        case 58:
        case 59:
        case 543:
        case 629: m_quests.insert(c.param1); break;
        case 74: m_globals.insert(c.param1); break;
        case 310: m_worldspace_params.insert(c.param1); break;
        default: break;
        }
        if (c.function == 629 && c.string_param != osl::k_none) {
            m_variables.emplace(c.param1, std::string(m_t.string(c.string_param)));
        }
        m_t.conditions.push_back(c);
    }

    // ---- where a destination lies

    // An encounter zone's location (ECZN DATA: owner, location, ...).
    uint32_t zone_location(uint32_t zone) const {
        auto z = record(zone);
        if (!z || z->type() != record_type("ECZN")) {
            return 0;
        }
        const auto* d = z->find(record_type("DATA"));
        return d && d->data.size() >= 8 ? z->resolve(BinaryReader(d->data.subspan(4)).read<uint32_t>()) : 0;
    }

    void locations() {
        for (const legacy::RecordEntry* e : m_db.of_type(record_type("LCTN"))) {
            const legacy::Record rec = m_db.read(*e);
            m_t.locations.push_back({rec.form_id(), str(rec.edid()), rec.form(record_type("PNAM")), 0});
        }
        // The worldspaces' cell -> location maps, as the game's loading builds them: every version of
        // every location in load order, LCEC and ACEC (a worldspace, then cells) insert or replace each cell with this
        // location, RCEC removes the cells it lists.
        std::map<std::tuple<uint32_t, int16_t, int16_t>, uint32_t> cells;
        for (const legacy::RecordEntry& e : m_db.entries()) {
            if (e.type != record_type("LCTN") || e.deleted()) {
                continue;
            }
            const legacy::Record rec = m_db.read(e);
            for (const auto& sr : rec.subrecords()) {
                const bool add = sr.type == record_type("LCEC") || sr.type == record_type("ACEC");
                const bool remove = sr.type == record_type("RCEC");
                if ((!add && !remove) || sr.data.size() < 8) {
                    continue;
                }
                BinaryReader r = sr.reader();
                const uint32_t world = rec.resolve(r.read<uint32_t>());
                auto w = m_db.find(world);
                if (!w || w->type != record_type("WRLD")) {
                    continue;
                }
                while (r.remaining() >= 4) {
                    const uint32_t key = r.read<uint32_t>(); // the map key itself: x << 16 | (y & 0xFFFF)
                    const std::tuple k{world, static_cast<int16_t>(key >> 16), static_cast<int16_t>(key & 0xFFFF)};
                    if (add) {
                        cells[k] = rec.form_id();
                    } else {
                        cells.erase(k);
                    }
                }
            }
        }
        for (const auto& [k, loc] : cells) {
            m_t.world_cells.push_back({std::get<0>(k), std::get<1>(k), std::get<2>(k), loc, 0});
        }
    }

    void worldspaces() {
        for (const legacy::RecordEntry* e : m_db.of_type(record_type("WRLD"))) {
            const legacy::Record rec = m_db.read(*e);
            uint32_t loc = rec.form(record_type("XLCN"));
            if (!loc) {
                loc = zone_location(rec.form(record_type("XEZN")));
            }
            m_t.worldspaces.push_back({rec.form_id(), str(rec.edid()), loc, 0});
        }
    }

    void interior_cells() {
        for (const legacy::RecordEntry* e : m_db.of_type(record_type("CELL"))) {
            if (e->parent != 0) {
                continue; // exterior: in a worldspace's group
            }
            const legacy::Record rec = m_db.read(*e);
            const auto* d = rec.find(record_type("DATA"));
            if (!d || d->data.empty() || (static_cast<uint8_t>(d->data[0]) & 0x01) == 0) {
                continue;
            }
            uint32_t loc = zone_location(rec.form(record_type("XEZN")));
            if (!loc) {
                loc = rec.form(record_type("XLCN"));
            }
            m_t.interior_cells.push_back({rec.form_id(), str(rec.edid()), loc, 0});
        }
    }

    // ---- what a new game starts with

    void quests() {
        for (uint32_t id : m_quests) {
            auto rec = record(id);
            if (!rec || rec->type() != record_type("QUST")) {
                warn(std::format("condition quest {:08X} not found", id));
                continue;
            }
            osl::Quest q{};
            q.form_id = id;
            q.edid = str(rec->edid());
            if (const auto* d = rec->find(record_type("DNAM")); d && d->data.size() >= 2) {
                q.flags = d->as<uint16_t>();
            }
            q.startup_stage = -1;
            q.first_stage = static_cast<uint32_t>(m_t.stages.size());
            for (const auto& sr : rec->subrecords()) {
                if (sr.type == record_type("INDX") && sr.data.size() >= 3) {
                    BinaryReader r = sr.reader();
                    osl::QuestStage st{};
                    st.index = r.read<uint16_t>();
                    st.flags = r.read<uint8_t>();
                    if ((st.flags & 0x02) && q.startup_stage < 0) {
                        q.startup_stage = st.index;
                    }
                    m_t.stages.push_back(st);
                }
            }
            q.stage_count = static_cast<uint32_t>(m_t.stages.size()) - q.first_stage;
            m_t.quests.push_back(q);
        }
    }

    void globals() {
        for (uint32_t id : m_globals) {
            auto rec = record(id);
            const auto* v = rec ? rec->find(record_type("FLTV")) : nullptr;
            if (!rec || !v || v->data.size() < 4) {
                warn(std::format("condition global {:08X} not found", id));
                continue;
            }
            m_t.globals.push_back({id, str(rec->edid()), v->as<float>(), 0});
        }
    }

    void quest_variables() {
        for (const auto& [quest, variable] : m_variables) {
            osl::QuestVariable v{quest, str(variable), 0.0f, 0};
            // "::Name_var" is the hidden variable behind the auto property "Name".
            std::string property = variable;
            if (property.starts_with("::")) {
                property = property.substr(2);
            }
            if (property.ends_with("_var")) {
                property.resize(property.size() - 4);
            }
            auto rec = record(quest);
            const auto* vmad = rec ? rec->find(record_type("VMAD")) : nullptr;
            ScriptValue sv;
            std::vector<std::string> scripts;
            try {
                if (vmad) {
                    scripts = vmad_scripts(vmad->data, property, sv);
                }
            } catch (const std::exception& e) {
                warn(std::format("quest {:08X} VMAD: {}", quest, e.what()));
            }
            if (sv.found) {
                v.value = sv.value;
                v.source = 1;
            } else {
                for (const auto& script : scripts) {
                    auto pex = m_data.read("scripts/" + path::to_lower(script) + ".pex");
                    if (!pex) {
                        continue;
                    }
                    try {
                        const ScriptValue init = pex_initial_value(*pex, variable);
                        if (init.found) {
                            v.value = init.value;
                            v.source = 2;
                            break;
                        }
                    } catch (const std::exception& e) {
                        warn(std::format("scripts/{}.pex: {}", script, e.what()));
                    }
                }
            }
            if (!v.source) {
                warn(std::format("quest {:08X} variable {}: no VMAD property or script variable", quest, variable));
            }
            m_t.quest_variables.push_back(v);
        }
    }

    void player_keywords() {
        // The player's base (NPC_ 0x7) and its race (RNAM), as a new game creates the player.
        auto keywords = [&](const legacy::Record& r) {
            const auto* k = r.find(record_type("KWDA"));
            if (!k) {
                return;
            }
            BinaryReader br = k->reader();
            while (br.remaining() >= 4) {
                m_t.player_keywords.push_back(r.resolve(br.read<uint32_t>()));
            }
        };
        auto npc = record(0x7);
        if (!npc) {
            warn("no player NPC_ (00000007)");
            return;
        }
        keywords(*npc);
        if (auto race = record(npc->form(record_type("RNAM")))) {
            keywords(*race);
        }
        std::ranges::sort(m_t.player_keywords);
    }

    void dead_references() {
        for (uint32_t id : m_references) {
            const legacy::RecordEntry* e = m_db.find(id);
            if (e && (e->flags & 0x200)) { // ACHR "starts dead"
                m_t.dead_references.push_back(id);
            }
        }
        std::ranges::sort(m_t.dead_references);
    }

    void form_lists() {
        for (uint32_t id : m_worldspace_params) {
            auto rec = record(id);
            if (!rec || rec->type() != record_type("FLST")) {
                continue;
            }
            for (const auto& sr : rec->subrecords()) {
                if (sr.type == record_type("LNAM") && sr.data.size() >= 4) {
                    m_t.form_lists.push_back({id, rec->resolve(sr.as<uint32_t>())});
                }
            }
        }
    }

    const legacy::EsmDatabase& m_db;
    const legacy::DataSource& m_data;
    LoadingImport& m_out;
    osl::LoadingScreens& m_t;
    GameStrings m_strings;
    std::set<uint32_t> m_quests, m_globals, m_references, m_worldspace_params;
    std::set<std::pair<uint32_t, std::string>> m_variables;
};

} // namespace

LoadingImport import_loading_screens(const legacy::EsmDatabase& db, const legacy::DataSource& data) {
    LoadingImport out;
    Importer(db, data, out).run();
    return out;
}

namespace {

// The BSInvMarker named "INV" among the root's extra data (the game looks it up by that name): its
// rotation, three uint16 the game multiplies by 0.001.
std::optional<std::array<float, 3>> inv_marker(const legacy::NifFile& nif, std::span<const std::byte> bytes) {
    if (nif.roots.empty()) {
        return std::nullopt;
    }
    const legacy::NiAVObject* root = nif.av_object(nif.roots[0]);
    if (!root) {
        return std::nullopt;
    }
    for (legacy::NifRef ref : root->extra_data) {
        if (!nif.valid(ref) || nif.type_name(ref) != "BSInvMarker") {
            continue;
        }
        const auto& b = nif.blocks[static_cast<size_t>(ref)];
        BinaryReader r(bytes.subspan(b.offset, b.size));
        const int32_t name = r.read<int32_t>();
        if (name < 0 || static_cast<size_t>(name) >= nif.header.strings.size() || nif.header.strings[size_t(name)] != "INV") {
            continue;
        }
        std::array<float, 3> rot{};
        for (float& f : rot) {
            f = static_cast<float>(r.read<uint16_t>()) * 0.001f;
        }
        return rot;
    }
    return std::nullopt;
}

} // namespace

size_t write_loading_screens(const legacy::EsmDatabase& db, const legacy::DataSource& data,
                             const std::string& cache_dir, bool force) {
    const std::filesystem::path cache(cache_dir);
    LoadingImport imp = import_loading_screens(db, data);
    // The models' inventory markers, which decide their base rotation.
    std::map<std::string, std::array<float, 3>> markers;
    for (const std::string& src : imp.models) {
        try {
            if (auto nif = data.read(src)) {
                if (auto m = inv_marker(legacy::read_nif(*nif), *nif)) {
                    markers[path::replace_extension(src, ".osm")] = *m;
                }
            }
        } catch (const std::exception& e) {
            log::warn("ui loading: {}: {}", src, e.what());
        }
    }
    for (auto& s : imp.table.screens) {
        if (s.model == osl::k_none) {
            continue;
        }
        if (auto it = markers.find(std::string(imp.table.string(s.model))); it != markers.end()) {
            s.has_inv_marker = 1;
            std::memcpy(s.inv_rotation, it->second.data(), sizeof s.inv_rotation);
        }
    }
    log::info("ui loading: {} models carry an INV marker", markers.size());
    write_file(cache / "interface" / "loadingscreens.osl", osl::write(imp.table));
    size_t models = 0, animated = 0;
    for (const std::string& src : imp.models) {
        const auto out = cache / path::replace_extension(src, ".osm");
        if (!force && readable_model(out)) {
            ++models;
            continue;
        }
        try {
            auto nif = data.read(src);
            if (!nif) {
                log::warn("ui loading: {} not found", src);
                continue;
            }
            const legacy::NifFile parsed = legacy::read_nif(*nif);
            if (has_particle_systems(parsed) || has_node_animation(parsed)) {
                ++animated; // the Mist Menu updates the slot's controllers; reported, drawn static (see the design)
                log::info("ui loading: {} has particle systems or node animation", src);
            }
            auto result = import_nif(parsed, src);
            write_file(out, osm::write(result.mesh));
            for (const auto& t : result.textures) {
                const auto tex = cache / t;
                if (!force && std::filesystem::exists(tex)) {
                    continue;
                }
                if (auto bytes = data.read(t)) {
                    write_file(tex, *bytes);
                } else {
                    log::warn("ui loading: {}: texture {} not found", src, t);
                }
            }
            ++models;
        } catch (const std::exception& e) {
            log::warn("ui loading: {}: {}", src, e.what());
        }
    }
    size_t paths = 0;
    for (const std::string& src : imp.camera_paths) {
        const auto out = cache / path::replace_extension(src, ".ose");
        if (!force && std::filesystem::exists(out)) {
            ++paths;
            continue;
        }
        try {
            auto nif = data.read(src);
            if (!nif) {
                log::warn("ui loading: camera path {} not found", src);
                continue;
            }
            auto anim = import_node_animation(legacy::read_nif(*nif), src);
            for (const auto& w : anim.warnings) {
                log::warn("ui loading: {}: {}", src, w);
            }
            write_file(out, ose::write(anim.effect));
            ++paths;
        } catch (const std::exception& e) {
            log::warn("ui loading: {}: {}", src, e.what());
        }
    }
    log::info("ui loading: wrote {} screens, {} models ({} animated), {} camera paths", imp.table.screens.size(),
              models, animated, paths);
    return imp.table.screens.size();
}

} // namespace spud::importer
