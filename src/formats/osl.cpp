#include "formats/osl.hpp"

#include <cstring>
#include <format>

namespace spud::osl {

namespace {

constexpr auto k_scrn = container::fourcc("SCRN");
constexpr auto k_cond = container::fourcc("COND");
constexpr auto k_locn = container::fourcc("LOCN");
constexpr auto k_wrld = container::fourcc("WRLD");
constexpr auto k_wcel = container::fourcc("WCEL");
constexpr auto k_icel = container::fourcc("ICEL");
constexpr auto k_qust = container::fourcc("QUST");
constexpr auto k_qstg = container::fourcc("QSTG");
constexpr auto k_glob = container::fourcc("GLOB");
constexpr auto k_qvar = container::fourcc("QVAR");
constexpr auto k_pkwd = container::fourcc("PKWD");
constexpr auto k_dead = container::fourcc("DEAD");
constexpr auto k_flst = container::fourcc("FLST");
constexpr auto k_strs = container::fourcc("STRS");

void check_range(uint32_t first, uint32_t count, size_t size, const char* what) {
    if (uint64_t{first} + count > size) {
        throw ParseError(std::format(".osl {} range {}+{} out of range ({})", what, first, count, size));
    }
}

void validate(const LoadingScreens& t) {
    for (const auto& s : t.screens) {
        check_range(s.first_condition, s.condition_count, t.conditions.size(), "screen conditions");
        for (uint32_t str : {s.edid, s.text, s.model, s.camera_path}) {
            t.string(str);
        }
    }
    for (const auto& c : t.conditions) {
        t.string(c.string_param);
    }
    for (const auto& q : t.quests) {
        check_range(q.first_stage, q.stage_count, t.stages.size(), "quest stages");
        t.string(q.edid);
    }
    for (const auto& l : t.locations) {
        t.string(l.edid);
    }
    for (const auto& w : t.worldspaces) {
        t.string(w.edid);
    }
    for (const auto& c : t.interior_cells) {
        t.string(c.edid);
    }
    for (const auto& g : t.globals) {
        t.string(g.edid);
    }
    for (const auto& v : t.quest_variables) {
        t.string(v.name);
    }
}

} // namespace

uint32_t LoadingScreens::add_string(std::string_view s) {
    std::string key(s);
    if (auto it = m_string_index.find(key); it != m_string_index.end()) {
        return it->second;
    }
    auto offset = static_cast<uint32_t>(m_strings.size());
    m_strings.insert(m_strings.end(), s.begin(), s.end());
    m_strings.push_back('\0');
    m_string_index.emplace(std::move(key), offset);
    return offset;
}

std::string_view LoadingScreens::string(uint32_t offset) const {
    return container::StringTable(std::as_bytes(std::span<const char>(m_strings))).get(offset);
}

void LoadingScreens::set_string_data(std::vector<char> data) {
    m_strings = std::move(data);
    m_string_index.clear();
}

std::vector<std::byte> write(const LoadingScreens& t) {
    validate(t);
    container::Writer w(k_kind, k_version);
    w.add_array(k_scrn, t.screens);
    w.add_array(k_cond, t.conditions);
    w.add_array(k_locn, t.locations);
    w.add_array(k_wrld, t.worldspaces);
    w.add_array(k_wcel, t.world_cells);
    w.add_array(k_icel, t.interior_cells);
    w.add_array(k_qust, t.quests);
    w.add_array(k_qstg, t.stages);
    w.add_array(k_glob, t.globals);
    w.add_array(k_qvar, t.quest_variables);
    w.add_array(k_pkwd, t.player_keywords);
    w.add_array(k_dead, t.dead_references);
    w.add_array(k_flst, t.form_lists);
    w.add(k_strs, std::as_bytes(std::span<const char>(t.string_data())));
    return w.finish();
}

LoadingScreens read(std::span<const std::byte> file) {
    container::Reader r(file, k_kind);
    if (r.version() != k_version) {
        throw ParseError("unsupported .osl version " + std::to_string(r.version()));
    }
    LoadingScreens t;
    t.screens = r.read_array<LoadScreen>(k_scrn);
    t.conditions = r.read_array<Condition>(k_cond);
    t.locations = r.read_array<Location>(k_locn);
    t.worldspaces = r.read_array<Worldspace>(k_wrld);
    t.world_cells = r.read_array<WorldCell>(k_wcel);
    t.interior_cells = r.read_array<InteriorCell>(k_icel);
    t.quests = r.read_array<Quest>(k_qust);
    t.stages = r.read_array<QuestStage>(k_qstg);
    t.globals = r.read_array<Global>(k_glob);
    t.quest_variables = r.read_array<QuestVariable>(k_qvar);
    t.player_keywords = r.read_array<uint32_t>(k_pkwd);
    t.dead_references = r.read_array<uint32_t>(k_dead);
    t.form_lists = r.read_array<FormListEntry>(k_flst);
    auto strings = r.require(k_strs);
    std::vector<char> data(strings.size());
    std::memcpy(data.data(), strings.data(), strings.size());
    t.set_string_data(std::move(data));
    validate(t);
    return t;
}

} // namespace spud::osl
