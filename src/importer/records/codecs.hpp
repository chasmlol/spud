#pragma once

// Fields whose layout the schema language can't express, decoded by code: `codec(name)` in the schema.
// Each codec writes the field's canonical form and adds a fixup for every FormID in it.
//   ctda  condition item: parameter kinds depend on the condition function (conditions.txt)
//   vmad  script attachments: scripts, properties, and the per-record-type fragment block

#include "core/binary.hpp"
#include "importer/records/convert.hpp"

#include <string_view>
#include <vector>

namespace spud::records {

struct CodecContext {
    const ConvertContext& ctx;
    const RawRecord& record;
    std::vector<std::byte>& out;
    size_t base; // record data start in `out`
    std::vector<Fixup>& fixups;
};

// Throws ParseError for an unknown codec or data that doesn't decode.
void run_codec(std::string_view name, CodecContext& c, BinaryReader& in);

} // namespace spud::records
