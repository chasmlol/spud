#include "audio/conditions.hpp"

#include <bit>

namespace spud::audio {

namespace {

constexpr uint8_t k_or = 0x01;

// The value a condition function returns for the player. Function indices are the game's condition table
// Functions that need systems spud
// doesn't have yet (actors, races, factions, perks, combat targets, quests, globals) return 0: the value the
// game gives a player with none of those.
float function_value(const osd::Condition& c, const ConditionContext& ctx) {
    switch (c.function) {
    case 18: // GetCurrentTime
        return ctx.game_hour;
    case 62: // IsRaining
        return ctx.raining ? 1.0f : 0.0f;
    case 75: // IsSnowing
        return ctx.snowing ? 1.0f : 0.0f;
    case 149: // GetIsCurrentWeather
        return ctx.weather != 0 && ctx.weather == c.param1 ? 1.0f : 0.0f;
    case 300: // IsInInterior
        return ctx.interior ? 1.0f : 0.0f;
    case 707: // GetCombatTargetHasKeyword
        return 0.0f;
    default:
        return 0.0f;
    }
}

bool compare(const osd::Condition& c, const ConditionContext& ctx) {
    const float value = function_value(c, ctx);
    const float against = (c.op & 0x04) ? 0.0f : std::bit_cast<float>(c.comparison);
    switch (c.op >> 5) {
    case 0:
        return value == against;
    case 1:
        return value != against;
    case 2:
        return value > against;
    case 3:
        return value >= against;
    case 4:
        return value < against;
    case 5:
        return value <= against;
    default:
        return false;
    }
}

} // namespace

bool evaluate(std::span<const osd::Condition> conditions, const ConditionContext& context) {
    bool group = false;
    for (size_t i = 0; i < conditions.size(); ++i) {
        group = group || compare(conditions[i], context);
        const bool last_of_group = !(conditions[i].op & k_or) || i + 1 == conditions.size();
        if (last_of_group) {
            if (!group) {
                return false;
            }
            group = false;
        }
    }
    return true;
}

} // namespace spud::audio
