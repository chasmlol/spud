#include "engine/game_clock.hpp"

#include <array>
#include <cmath>

namespace spud {

namespace {

constexpr uint32_t k_game_year = 0x35, k_game_month = 0x36, k_game_day = 0x37, k_game_hour = 0x38,
                   k_game_days_passed = 0x39, k_time_scale = 0x3A;
constexpr std::array<uint16_t, 12> k_month_days = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

} // namespace

void GameClock::start(float year, float month, float day, float hour, float whole_days) {
    m_year = year;
    m_month = month;
    m_day = day;
    m_hour = hour;
    m_whole_days = whole_days;
    m_days_passed = hour * 0.041666668f + whole_days;
    m_midnights = 0;
}

void GameClock::update(float seconds) {
    pass_hours(seconds * m_time_scale * 0.00027777778f);
}

void GameClock::pass_hours(float hours) {
    // Calendar::Update. An hour of exactly 24 stays; however many days pass in one step the month rolls
    // once, and the count of midnights and the whole days move by one.
    float hour = hours + m_hour;
    if (!std::isfinite(hour) || hour >= 1e7f) {
        return; // past what a float can count down in 24s
    }
    if (hour > 24.0f) {
        const int month = static_cast<int>(m_month);
        const float month_days = month >= 0 && month < 12 ? float(k_month_days[size_t(month)]) : 0.0f;
        float day = m_day;
        do {
            hour -= 24.0f;
            day += 1.0f;
        } while (hour > 24.0f);
        if (day > month_days) {
            day -= month_days;
            m_month += 1.0f;
            if (m_month >= 12.0f) {
                m_month -= 12.0f;
                m_year += 1.0f;
            }
        }
        m_day = day;
        ++m_midnights;
        m_whole_days += 1.0f;
    }
    m_days_passed = hour * 0.041666668f + m_whole_days;
    m_hour = hour;
}

void GameClock::save(BinaryWriter& w) const {
    for (const float v : {m_year, m_month, m_day, m_hour, m_days_passed, m_time_scale, m_whole_days}) {
        w.write<float>(v);
    }
    w.write<uint32_t>(m_midnights);
}

void GameClock::load(BinaryReader& r) {
    for (float* v : {&m_year, &m_month, &m_day, &m_hour, &m_days_passed, &m_time_scale, &m_whole_days}) {
        *v = r.read<float>();
    }
    m_midnights = r.read<uint32_t>();
}

uint32_t GameClock::hours_passed() const {
    return static_cast<uint32_t>(std::floor(m_days_passed * 24.0f));
}

bool GameClock::read_global(uint32_t form_id, float& value) const {
    switch (form_id) {
    case k_game_year: value = m_year; return true;
    case k_game_month: value = m_month; return true;
    case k_game_day: value = m_day; return true;
    case k_game_hour: value = m_hour; return true;
    case k_game_days_passed: value = m_days_passed; return true;
    case k_time_scale: value = m_time_scale; return true;
    default: return false;
    }
}

bool GameClock::write_global(uint32_t form_id, float value) {
    switch (form_id) {
    case k_game_year: m_year = value; return true;
    case k_game_month: m_month = value; return true;
    case k_game_day: m_day = value; return true;
    case k_game_hour: m_hour = value; return true;
    case k_game_days_passed: m_days_passed = value; return true;
    case k_time_scale: m_time_scale = value; return true;
    default: return false;
    }
}

} // namespace spud
