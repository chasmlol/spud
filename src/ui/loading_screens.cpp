#include "ui/loading_screens.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"

#include <algorithm>

namespace spud::ui {

namespace {

// Condition function indices (opcode - 0x1000; the SCRIPT_FUNCTION table, 0x50 bytes per entry).
enum Function : uint16_t {
    fn_get_dead = 46,
    fn_get_quest_running = 56,
    fn_get_stage = 58,
    fn_get_stage_done = 59,
    fn_get_global_value = 74,
    fn_get_random_percent = 77,
    fn_get_in_worldspace = 310,
    fn_get_in_current_loc = 359,
    fn_get_quest_completed = 543,
    fn_has_keyword = 560,
    fn_get_vm_quest_variable = 629,
};

uint64_t cell_key(uint32_t world, int x, int y) {
    return (uint64_t{world} << 32) | (uint32_t(uint16_t(int16_t(x))) << 16) | uint16_t(int16_t(y));
}

} // namespace

// ---- starting state -------------------------------------------------------------------------------------------

StartingState::StartingState(const osl::LoadingScreens& data) : m_data(data) {
    // The game starts as the record's DNAM flags (low byte), whose bit 0, "start game enabled", is the running
    // bit: measured at a new game's first SetupModel, every quest's flags equal its
    // DNAM's low byte (0x11 for the start-game-enabled ones, 0x04, 0x08, 0) and its stage is 0, the start-up stages
    // (BYOHHouse*, DLC2Init: 10) not set yet.
    for (const auto& q : m_data.quests) {
        m_quests[q.form_id].flags = static_cast<uint8_t>(q.flags & 0xFF);
        m_quests[q.form_id].running = (q.flags & 0x01) != 0;
    }
}

void StartingState::start_quest(uint32_t quest, std::optional<uint16_t> stage) {
    auto& q = m_quests[quest];
    q.running = true;
    q.flags |= 0x01;
    for (const auto& rec : m_data.quests) {
        if (rec.form_id == quest && rec.startup_stage >= 0) {
            q.stage = static_cast<uint16_t>(rec.startup_stage);
            q.done.push_back(q.stage);
        }
    }
    if (stage) {
        q.stage = *stage;
        q.done.push_back(*stage);
    }
}

uint16_t StartingState::quest_stage(uint32_t quest) const {
    auto it = m_quests.find(quest);
    return it == m_quests.end() ? 0 : it->second.stage;
}

bool StartingState::quest_stage_done(uint32_t quest, uint16_t stage) const {
    auto it = m_quests.find(quest);
    return it != m_quests.end() && std::ranges::find(it->second.done, stage) != it->second.done.end();
}

void StartingState::set_quest_flags(uint32_t quest, uint8_t flags) {
    auto& q = m_quests[quest];
    q.flags = flags;
    q.running = (flags & 0x01) != 0;
}

void StartingState::set_quest_stage(uint32_t quest, uint16_t stage) {
    m_quests[quest].stage = stage;
}

bool StartingState::quest_running(uint32_t quest) const {
    // The game: bit 0 set and bit 7 clear (and no pending stop).
    auto it = m_quests.find(quest);
    return it != m_quests.end() && (it->second.flags & 0x01) && !(it->second.flags & 0x80);
}

bool StartingState::quest_completed(uint32_t quest) const {
    // bit 1 (no quest starts completed).
    auto it = m_quests.find(quest);
    return it != m_quests.end() && (it->second.flags & 0x02);
}

void StartingState::set_global(uint32_t global, float value) {
    m_globals[global] = value;
}

std::optional<float> StartingState::global_value(uint32_t global) const {
    if (auto it = m_globals.find(global); it != m_globals.end()) {
        return it->second;
    }
    for (const auto& g : m_data.globals) {
        if (g.form_id == global) {
            return g.value;
        }
    }
    return std::nullopt;
}

std::optional<float> StartingState::quest_variable(uint32_t quest, std::string_view name) const {
    for (const auto& v : m_data.quest_variables) {
        if (v.quest == quest && m_data.string(v.name) == name) {
            return v.source ? std::optional<float>(v.value) : std::nullopt;
        }
    }
    return std::nullopt;
}

bool StartingState::player_has_keyword(uint32_t keyword) const {
    return std::ranges::binary_search(m_data.player_keywords, keyword);
}

bool StartingState::reference_dead(uint32_t reference) const {
    return std::ranges::binary_search(m_data.dead_references, reference);
}

// ---- the table ------------------------------------------------------------------------------------------------

std::unique_ptr<LoadingScreens> LoadingScreens::load(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        return nullptr;
    }
    return std::make_unique<LoadingScreens>(osl::read(read_file(file)));
}

LoadingScreens::LoadingScreens(osl::LoadingScreens data) : m_data(std::move(data)) {
    for (size_t i = 0; i < m_data.screens.size(); ++i) {
        m_screen_of.emplace(m_data.screens[i].form_id, i);
    }
    for (const auto& l : m_data.locations) {
        m_parent.emplace(l.form_id, l.parent);
        m_location_name.emplace(l.form_id, l.edid);
    }
    for (const auto& w : m_data.worldspaces) {
        m_world_by_edid.emplace(path::to_lower(m_data.string(w.edid)), w.form_id);
        m_world_location.emplace(w.form_id, w.location);
        m_worldspaces.insert(w.form_id);
    }
    for (const auto& c : m_data.world_cells) {
        m_world_cell[cell_key(c.worldspace, c.x, c.y)] = c.location;
    }
    for (const auto& c : m_data.interior_cells) {
        m_interior.emplace(path::to_lower(m_data.string(c.edid)), c.location);
    }
    for (const auto& f : m_data.form_lists) {
        m_form_lists[f.list].push_back(f.form);
    }
}

const osl::LoadScreen* LoadingScreens::find(uint32_t form_id) const {
    auto it = m_screen_of.find(form_id);
    return it == m_screen_of.end() ? nullptr : &m_data.screens[it->second];
}

uint32_t LoadingScreens::worldspace(std::string_view edid) const {
    auto it = m_world_by_edid.find(path::to_lower(edid));
    return it == m_world_by_edid.end() ? 0 : it->second;
}

uint32_t LoadingScreens::location_at(std::string_view worldspace_edid, int cell_x, int cell_y) const {
    const uint32_t world = worldspace(worldspace_edid);
    if (!world) {
        return 0;
    }
    if (auto it = m_world_cell.find(cell_key(world, cell_x, cell_y)); it != m_world_cell.end() && it->second) {
        return it->second;
    }
    auto it = m_world_location.find(world);
    return it == m_world_location.end() ? 0 : it->second;
}

uint32_t LoadingScreens::interior_location(std::string_view cell_edid) const {
    auto it = m_interior.find(path::to_lower(cell_edid));
    return it == m_interior.end() ? 0 : it->second;
}

std::string_view LoadingScreens::location_edid(uint32_t location) const {
    auto it = m_location_name.find(location);
    return it == m_location_name.end() ? std::string_view() : m_data.string(it->second);
}

// GetInCurrentLoc ( ->): the location, or one of its parents, is the one
// asked for.
bool LoadingScreens::location_within(uint32_t location, uint32_t ancestor) const {
    for (int guard = 0; location && guard < 64; ++guard) {
        if (location == ancestor) {
            return true;
        }
        auto it = m_parent.find(location);
        location = it == m_parent.end() ? 0 : it->second;
    }
    return false;
}

// One condition item: the run-on subject, the function's value, the comparison (the double result
// converted to float against the item's float or its global's value).
bool LoadingScreens::item_true(const osl::Condition& c, const ConditionContext& ctx, fx::BsRandom& rng) const {
    const ConditionState& st = *ctx.state;
    // Subject and target are both the player; run-on 2 names a reference. The other run-on kinds (combat target,
    // linked reference, quest alias, package and event data) have nothing behind them before a game is loaded.
    enum class Subject { none, player, reference } subject = Subject::none;
    switch (c.run_on) {
    case 0:
    case 1: subject = Subject::player; break;
    case 2: subject = c.reference ? Subject::reference : Subject::none; break;
    default: subject = Subject::none; break;
    }
    double result = 0.0;
    bool ok = true;
    switch (c.function) {
    case fn_get_dead:
        // Actors only (form type 62): the player is alive; a reference is dead when it starts dead.
        if (subject == Subject::none) {
            ok = false;
        } else {
            result = subject == Subject::reference && st.reference_dead(c.reference) ? 1.0 : 0.0;
        }
        break;
    case fn_get_quest_running: result = st.quest_running(c.param1) ? 1.0 : 0.0; break;
    case fn_get_stage: result = double(float(int(st.quest_stage(c.param1)))); break;
    case fn_get_stage_done:
        result = st.quest_stage_done(c.param1, static_cast<uint16_t>(c.param2)) ? 1.0 : 0.0;
        break;
    case fn_get_global_value: result = st.global_value(c.param1).value_or(-1.0f); break;
    case fn_get_random_percent: result = double(int(rng.next(100))); break;
    case fn_get_in_worldspace: {
        // The subject's worldspace: the player's, who has not moved yet when the screen is chosen.
        const uint32_t w = subject == Subject::player ? ctx.player_worldspace : 0;
        if (m_worldspaces.contains(c.param1)) {
            result = w && w == c.param1 ? 1.0 : 0.0;
        } else if (auto it = m_form_lists.find(c.param1); it != m_form_lists.end()) {
            result = w && std::ranges::find(it->second, w) != it->second.end() ? 1.0 : 0.0;
        }
        break;
    }
    case fn_get_in_current_loc:
        // The player's current location is the destination's while the screen is chosen.
        result = subject == Subject::player && ctx.location && c.param1 && location_within(ctx.location, c.param1)
                     ? 1.0
                     : 0.0;
        break;
    case fn_get_quest_completed: result = st.quest_completed(c.param1) ? 1.0 : 0.0; break;
    case fn_has_keyword:
        if (subject == Subject::none) {
            ok = false;
        } else {
            result = subject == Subject::player && st.player_has_keyword(c.param1) ? 1.0 : 0.0;
        }
        break;
    case fn_get_vm_quest_variable:
        result = st.quest_variable(c.param1, m_data.string(c.string_param)).value_or(0.0f);
        break;
    default:
        ok = false; // not a function the loading screens use (the importer reports it)
        break;
    }
    if (!ok) {
        return false;
    }
    const float v = static_cast<float>(result);
    const float target = (c.flags & 0x04) ? st.global_value(c.value_global).value_or(0.0f) : c.value;
    switch (c.flags >> 5) {
    case 0: return v == target;
    case 1: return v != target;
    case 2: return v > target;
    case 3: return v >= target;
    case 4: return v < target;
    case 5: return v <= target;
    default: return true;
    }
}

// The game: a run of items flagged OR, with the item after them, is one group, true when
// any of them is; the groups and plain items must all be true. Evaluation stops at the first false item or group,
// and inside a group at its first true item, so GetRandomPercent only draws when reached.
bool LoadingScreens::conditions_true(const osl::LoadScreen& s, const ConditionContext& ctx, fx::BsRandom& rng) const {
    bool all = true;
    bool value = false;
    bool in_group = false;
    for (uint32_t k = 0; k < s.condition_count; ++k) {
        const osl::Condition& c = m_data.conditions[s.first_condition + k];
        const bool is_or = (c.flags & 0x01) != 0;
        if (!in_group) {
            value = item_true(c, ctx, rng);
            if (is_or) {
                in_group = true;
            } else {
                all = all && value;
                if (!all) {
                    return false;
                }
            }
            continue;
        }
        if (!value) {
            value = item_true(c, ctx, rng);
        }
        if (is_or) {
            continue;
        }
        in_group = false;
        if (!(all && value)) {
            return false;
        }
    }
    return in_group ? all && value : all;
}

namespace {

bool no_conditions(const osl::LoadScreen* s) {
    return s->condition_count == 0;
}

void quicksort(std::vector<const osl::LoadScreen*>& a, uint32_t lo, uint32_t hi) {
    if (lo >= hi) {
        return;
    }
    const uint32_t end = hi;
    uint32_t first = lo;
    for (;;) {
        // "x goes after the pivot": x has no conditions and the pivot (whatever sits in the first slot) has.
        auto after = [&](uint32_t x) { return no_conditions(a[x]) && !no_conditions(a[first]); };
        uint32_t i = first, j = end;
        bool crossed = false;
        do {
            for (;;) {
                if (after(j)) {
                    break;
                }
                while (!after(i)) {
                    if (++i >= j) {
                        crossed = true;
                        break;
                    }
                }
                if (crossed) {
                    break;
                }
                std::swap(a[i], a[j]);
            }
            if (crossed) {
                break;
            }
            --j;
        } while (i < j);
        std::swap(a[first], a[j]);
        if (j) {
            quicksort(a, first, j - 1);
        }
        if (j >= a.size() - 1) {
            break;
        }
        first = j + 1;
        if (!(j + 1 < end)) {
            break;
        }
    }
}

} // namespace

void sort_conditioned_first(std::vector<const osl::LoadScreen*>& list) {
    if (list.size() > 1) {
        quicksort(list, 0, static_cast<uint32_t>(list.size() - 1));
    }
}

// The game with its visitor over every LSCR in form-array order.
LoadingScreens::Choice LoadingScreens::choose(const ConditionContext& ctx, fx::BsRandom& rng) const {
    Choice c;
    std::vector<const osl::LoadScreen*> candidates;
    uint32_t conditioned_with_model = 0;
    for (const osl::LoadScreen& s : m_data.screens) {
        const bool ok = ctx.session_reset ? (s.record_flags & 0x400) != 0 : conditions_true(s, ctx, rng);
        if (!ok) {
            continue;
        }
        if (s.condition_count == 0) {
            if (s.static_form) {
                candidates.push_back(&s);
            }
        } else {
            ++c.conditioned;
            if (s.static_form) {
                candidates.push_back(&s);
                ++conditioned_with_model;
            }
        }
        c.eligible.push_back(&s);
    }
    sort_conditioned_first(c.eligible);
    sort_conditioned_first(candidates);
    if (candidates.empty()) {
        return c;
    }
    const uint32_t n = conditioned_with_model ? conditioned_with_model : static_cast<uint32_t>(candidates.size());
    c.chosen = candidates[rng.next(n)];
    if (auto it = std::ranges::find(c.eligible, c.chosen); it != c.eligible.end()) {
        c.eligible.erase(it);
    }
    return c;
}

std::optional<std::string> LoadingScreens::next_text(Choice& c, bool no_screen, bool use_all_non_default_first,
                                                     fx::BsRandom& rng) const {
    std::optional<std::string> out;
    if (c.chosen) {
        if (!no_screen) {
            out = std::string(text(*c.chosen));
        }
        c.chosen = nullptr;
        c.conditioned = 0;
        return out;
    }
    if (c.eligible.empty()) {
        return out;
    }
    const uint32_t n = c.conditioned ? c.conditioned : static_cast<uint32_t>(c.eligible.size());
    const uint32_t i = rng.next(n);
    if (!no_screen) {
        out = std::string(text(*c.eligible[i]));
    }
    c.eligible.erase(c.eligible.begin() + i);
    if (c.conditioned) {
        if (use_all_non_default_first) {
            --c.conditioned;
        } else {
            c.conditioned = 0;
        }
    }
    return out;
}

} // namespace spud::ui
