#include "game/settings.hpp"

#include "formats/forms_typed.hpp"

namespace spud::game {

Settings::Settings(const forms::Database& db) {
    for (const forms::FormEntry* e : db.of_type(forms::parse_sig("GMST").value())) {
        const forms::Form f(db, *e);
        const auto value = forms::setting_value(f);
        if (!value) {
            continue;
        }
        std::string name(f.editor_id());
        if (value->kind == forms::Value::Kind::text) {
            m_texts.emplace(std::move(name), value->text);
        } else if (value->kind == forms::Value::Kind::real) {
            m_numbers.emplace(std::move(name), float(value->real));
        } else if (value->kind == forms::Value::Kind::integer) {
            m_numbers.emplace(std::move(name), float(value->integer));
        }
    }
}

float Settings::number(std::string_view name, float fallback) const {
    const auto it = m_numbers.find(std::string(name));
    return it == m_numbers.end() ? fallback : it->second;
}

const std::string& Settings::text(std::string_view name) const {
    static const std::string none;
    const auto it = m_texts.find(std::string(name));
    return it == m_texts.end() ? none : it->second;
}

} // namespace spud::game
