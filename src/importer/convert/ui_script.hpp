#pragma once

// ActionScript 2 (AVM1) byte code -> .osu instructions: constant pools resolved to strings, branch offsets to
// instruction indices, and every function body split into its own osu::Function (docs/formats/osu.md).

#include "formats/osu.hpp"

#include <span>
#include <string>
#include <vector>

namespace spud::importer {

class ScriptTranslator {
public:
    ScriptTranslator(osu::Movie& movie, container::StringTableBuilder& strings) : m_movie(movie), m_strings(strings) {}

    // Translates one action block (frame, init, clip-event or button actions); returns its function index.
    uint32_t block(std::span<const std::byte> code);

    const std::vector<std::string>& warnings() const { return m_warnings; }

private:
    // `pool`: the constant pool in effect where the function is defined (bodies inherit it).
    uint32_t translate(std::span<const std::byte> code, osu::FunctionKind kind, std::string_view name,
                       const std::vector<std::pair<std::string, uint32_t>>& params, uint8_t registers,
                       uint16_t flags, std::vector<std::string> pool);

    osu::Movie& m_movie;
    container::StringTableBuilder& m_strings;
    std::vector<std::string> m_warnings;
};

} // namespace spud::importer
