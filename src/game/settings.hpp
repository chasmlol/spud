#pragma once

// Game settings (GMST) by EditorID, from the forms database: the values the plugins give. Settings the plugins leave
// out keep the value compiled into the game, which the caller passes as `fallback`.

#include "formats/forms.hpp"

#include <string>
#include <string_view>
#include <unordered_map>

namespace spud::game {

class Settings {
public:
    explicit Settings(const forms::Database& db);

    float number(std::string_view name, float fallback) const; // f and i settings
    const std::string& text(std::string_view name) const;      // s settings, "" when absent

private:
    std::unordered_map<std::string, float> m_numbers;
    std::unordered_map<std::string, std::string> m_texts;
};

} // namespace spud::game
