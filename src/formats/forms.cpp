#include "formats/forms.hpp"

#include "core/error.hpp"
#include "core/file.hpp"

#include <algorithm>
#include <cstring>
#include <format>

namespace spud::forms {

std::string_view Field::text() const {
    std::string_view s(reinterpret_cast<const char*>(data.data()), data.size());
    return s.substr(0, s.find('\0'));
}

void FieldIterator::load() {
    if (m_rest.size() < 8) {
        m_done = true;
        return;
    }
    uint32_t header[2];
    std::memcpy(header, m_rest.data(), 8);
    if (header[1] > m_rest.size() - 8) {
        throw ParseError(std::format("field {} overruns its form", sig_name(header[0])));
    }
    m_field.sig = header[0];
    m_field.data = m_rest.subspan(8, header[1]);
    m_rest = m_rest.subspan(8 + header[1]);
    m_done = false;
}

struct Database::Mapping {
    MappedFile file;
};

Database::Database(const std::filesystem::path& path) : m_mapping(std::make_unique<Mapping>()) {
    m_mapping->file = MappedFile(path);
    open(m_mapping->file.bytes());
}

Database::Database(std::vector<std::byte> bytes) : m_owned(std::move(bytes)) {
    open(m_owned);
}

Database::~Database() = default;

void Database::open(std::span<const std::byte> bytes) {
    container::Reader r(bytes, k_kind);
    if (r.version() != k_version) {
        throw ParseError(std::format("forms database version {} (expected {})", r.version(), k_version));
    }
    auto schema = r.require(k_chunk_schema);
    m_schema_text = std::string_view(reinterpret_cast<const char*>(schema.data()), schema.size());
    m_schema.parse(m_schema_text, "forms database");
    m_schema.finish();
    m_header = r.read_struct<Header>(k_chunk_header);
    auto cast = [&]<typename T>(container::FourCC id, std::span<const T>& out) {
        auto b = r.require(id);
        if (b.size() % sizeof(T) != 0) {
            throw ParseError("chunk " + container::to_string(id) + " has a partial element");
        }
        out = std::span<const T>(reinterpret_cast<const T*>(b.data()), b.size() / sizeof(T));
    };
    cast(k_chunk_plugins, m_plugins);
    cast(k_chunk_masters, m_masters);
    cast(k_chunk_forms, m_forms);
    cast(k_chunk_sources, m_sources);
    cast(k_chunk_source_groups, m_source_groups);
    cast(k_chunk_fixups, m_fixups);
    m_strings = container::StringTable(r.require(k_chunk_strings));
    m_data = r.require(k_chunk_data);
    if (m_source_groups.size() != m_sources.size()) {
        throw ParseError("SGRP and SRCS have different lengths");
    }
    for (const auto& f : m_forms) {
        if (f.data_offset + f.data_size > m_data.size() || f.first_source + f.source_count > m_sources.size() ||
            uint64_t(f.first_fixup) + f.fixup_count > m_fixups.size() || f.plugin >= m_plugins.size()) {
            throw ParseError(std::format("form {:08X} points outside the database", f.form_id));
        }
    }
    build_index();
}

std::string_view Database::language() const {
    return m_strings.get(m_header.language);
}

std::string_view Database::plugin_name(size_t plugin) const {
    return m_strings.get(m_plugins[plugin].name);
}

std::span<const uint16_t> Database::masters(const PluginEntry& p) const {
    return m_masters.subspan(p.first_master, p.master_count);
}

FieldRange Database::header_fields(const PluginEntry& p) const {
    return {m_data.subspan(p.header_offset, p.header_size)};
}

std::span<const uint16_t> Database::sources(const FormEntry& f) const {
    return m_sources.subspan(f.first_source, f.source_count);
}

std::span<const uint8_t> Database::source_groups(const FormEntry& f) const {
    return m_source_groups.subspan(f.first_source, f.source_count);
}

std::span<const uint32_t> Database::fixups(const FormEntry& f) const {
    return m_fixups.subspan(f.first_fixup, f.fixup_count);
}

// Conditions and quest fills ask this thousands of times in a tick. A binary search of a table this size is a cache miss a
// step, so ids go through an open-addressed table: one or two misses. A slot holds the id and its entry's index + 1.
void Database::build_index() {
    size_t slots = 1024;
    m_index_shift = 54;
    while (slots < m_forms.size() * 2) {
        slots *= 2;
        --m_index_shift;
    }
    m_index.assign(slots, 0);
    for (size_t i = 0; i < m_forms.size(); ++i) {
        const uint32_t id = m_forms[i].form_id;
        size_t at = size_t((uint64_t(id) * 0x9E3779B97F4A7C15ull) >> m_index_shift);
        while (m_index[at] != 0) {
            at = (at + 1) & (slots - 1);
        }
        m_index[at] = uint64_t(id) << 32 | uint64_t(i + 1);
    }
}

const FormEntry* Database::find(uint32_t form_id) const {
    const size_t mask = m_index.size() - 1;
    for (size_t at = size_t((uint64_t(form_id) * 0x9E3779B97F4A7C15ull) >> m_index_shift);; at = (at + 1) & mask) {
        const uint64_t slot = m_index[at];
        if (slot == 0) {
            return nullptr;
        }
        if (uint32_t(slot >> 32) == form_id) {
            return &m_forms[size_t(uint32_t(slot)) - 1];
        }
    }
}

FieldRange Database::fields(const FormEntry& f) const {
    return {m_data.subspan(f.data_offset, f.data_size)};
}

std::optional<Field> Database::field(const FormEntry& f, Sig s) const {
    for (const auto& fl : fields(f)) {
        if (fl.sig == s) {
            return fl;
        }
    }
    return std::nullopt;
}

std::string_view Database::editor_id(const FormEntry& f) const {
    if (auto e = field(f, sig("EDID"))) {
        return e->text();
    }
    return {};
}

const std::vector<const FormEntry*>& Database::of_type(Sig type) const {
    std::call_once(m_types_once, [this] {
        for (const auto& f : m_forms) {
            if (!(f.flags & form_deleted)) {
                m_types[uint32_t(f.type)].push_back(&f);
            }
        }
    });
    static const std::vector<const FormEntry*> none;
    const auto it = m_types.find(uint32_t(type));
    return it != m_types.end() ? it->second : none;
}

} // namespace spud::forms
