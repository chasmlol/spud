#pragma once

// Compiled Papyrus (.pex) and record VMADs -> scripts/scripts.oss. See docs/formats/oss.md.

#include "formats/oss.hpp"
#include "importer/legacy/data_source.hpp"
#include "importer/legacy/esm.hpp"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace spud::importer {

struct ScriptImport {
    oss::Scripts scripts;
    std::vector<std::string> warnings;
};

// `extra`: one more .pex, for checks that run a script of their own.
ScriptImport import_scripts(const legacy::EsmDatabase& db, const legacy::DataSource& data, std::span<const std::byte> extra = {});

// One .pex added to a table being built; for tools and tests that run a script outside a load order.
class ScriptCompiler {
public:
    ScriptCompiler();
    ~ScriptCompiler();
    // Throws ParseError on a file that isn't well-formed PEX.
    void add(std::span<const std::byte> pex);
    // One record's VMAD: scripts, fragments, quest alias scripts. Form IDs through `plugin`.
    void add_attachments(uint32_t form_id, legacy::RecordType type, std::span<const std::byte> vmad,
                         const legacy::PluginInfo& plugin);
    oss::Scripts finish();
    std::vector<std::string> warnings;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace spud::importer
