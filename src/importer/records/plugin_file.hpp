#pragma once

// One plugin file read on its own, independent of the load order: every record with its group context and
// FormIDs exactly as written in the file (master-relative). The forms import compiles each plugin from this
// alone, so a plugin's compiled unit stays valid however the load order around it changes.

#include "core/file.hpp"
#include "formats/forms_schema.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace spud::records {

using forms::Sig;

struct PluginHeader {
    uint32_t flags = 0;        // TES4 record flags: 0x1 master, 0x80 localized, 0x200 light
    uint16_t form_version = 0; // TES4 record header
    float version = 0.0f;      // HEDR
    uint32_t record_count = 0; // HEDR
    uint32_t next_object_id = 0;
    std::vector<std::string> masters;
    std::vector<uint64_t> master_sizes;   // DATA after each MAST
    std::vector<uint32_t> overrides;      // ONAM
    std::string author;                   // CNAM
    std::string description;              // SNAM
    uint32_t intv = 0;                    // INTV
    bool has_intv = false;
    uint32_t incc = 0;                    // INCC
    bool has_incc = false;
    std::span<const std::byte> raw;       // the whole TES4 payload
};

struct RawRecord {
    Sig type;
    uint32_t flags;
    uint32_t form_id; // as written (master-relative)
    uint32_t vc_stamp;
    uint16_t form_version;
    uint16_t vc_unknown;
    uint32_t parent;  // as written: FormID owning the innermost world/cell/topic group, 0 if none
    uint8_t group;    // innermost group type, 0xFF for none
    uint64_t offset;  // of the 24-byte header in the file
    std::span<const std::byte> payload; // as stored (compressed if flags & 0x40000)
};

struct RawSubrecord {
    Sig type;
    std::span<const std::byte> data;
};

class PluginFile {
public:
    // Maps and walks the file. Throws ParseError on a damaged file (the game refuses those too).
    explicit PluginFile(const std::filesystem::path& path);

    const PluginHeader& header() const { return m_header; }
    std::span<const RawRecord> records() const { return m_records; }
    std::span<const std::byte> bytes() const { return m_file.bytes(); }
    uint32_t group_count() const { return m_groups; }

    // The record's payload, decompressed into `storage` if needed, split into subrecords (XXXX applied).
    static std::vector<RawSubrecord> subrecords(const RawRecord& r, std::vector<std::byte>& storage);

private:
    MappedFile m_file;
    PluginHeader m_header;
    std::vector<RawRecord> m_records;
    uint32_t m_groups = 0;
};

} // namespace spud::records
