#pragma once

// The game's calendar. One clock for the sky, the menus, Papyrus and the world's
// timers. Values are floats like the globals they stand for (GameYear .. TimeScale), so rounding follows the game's.

#include "core/binary.hpp"

#include <cstdint>

namespace spud {

class GameClock {
public:
    // A session's start: Calendar::Init leaves the whole days at 0, and GameDaysPassed is hour / 24 plus them.
    void start(float year, float month, float day, float hour, float whole_days = 0.0f);

    // Calendar::Update: `seconds` of real time at TimeScale.
    void update(float seconds);
    // The same in game hours: Sleep/Wait and fast travel pass time through the same steps.
    void pass_hours(float hours);

    // The GameHour global written directly (scripts, `set time_of_day`): no day rolls over.
    void set_hour(float hour) { m_hour = hour; }
    void set_time_scale(float scale) { m_time_scale = scale; }

    float year() const { return m_year; }
    float month() const { return m_month; }
    float day() const { return m_day; }
    float hour() const { return m_hour; }
    float time_scale() const { return m_time_scale; }
    float days_passed() const { return m_days_passed; }
    float whole_days() const { return m_whole_days; }
    // Midnights since the session began (Calendar).
    uint32_t midnights() const { return m_midnights; }
    // Whole game hours passed: the time stamp of cells, encounter zones and locations.
    uint32_t hours_passed() const;

    void save(BinaryWriter& w) const;
    void load(BinaryReader& r);

    // The calendar globals by FormID (GameYear 0x35 .. TimeScale 0x3A), for GlobalVariable.GetValue and SetValue.
    bool read_global(uint32_t form_id, float& value) const;
    bool write_global(uint32_t form_id, float value);

private:
    float m_year = 77.0f, m_month = 7.0f, m_day = 17.0f; // The game's values without the globals
    float m_hour = 12.0f;
    float m_days_passed = 0.5f;
    float m_time_scale = 20.0f;
    float m_whole_days = 0.0f;
    uint32_t m_midnights = 0;
};

} // namespace spud
