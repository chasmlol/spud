#pragma once

// The record schema (src/importer/schema/*.txt, compiled in) and the condition function table.

#include "formats/forms_schema.hpp"
#include "importer/records/convert.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace spud::records {

struct LoadedSchema {
    forms::Schema schema;
    std::string text;            // every schema file, concatenated in name order (stored in databases)
    std::string conditions_text; // conditions.txt
    ConditionFunctions conditions;
    uint64_t hash = 0;           // of text + conditions_text
};

// The compiled-in schema, or the *.txt files of `dir` when given (for editing the schema without rebuilding).
// Throws ParseError on a schema error.
LoadedSchema load_schema(const std::optional<std::filesystem::path>& dir = std::nullopt);

} // namespace spud::records
