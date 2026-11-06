#include "formats/container.hpp"

#include <cstring>
#include <format>

namespace spud::container {

std::string to_string(FourCC id) {
    std::string s(4, ' ');
    for (int i = 0; i < 4; ++i) {
        char c = static_cast<char>((id >> (8 * i)) & 0xFF);
        s[i] = (c >= 32 && c < 127) ? c : '?';
    }
    return s;
}

void Writer::add(FourCC id, std::span<const std::byte> payload) {
    m_chunks.push_back({id, std::vector<std::byte>(payload.begin(), payload.end())});
}

std::vector<std::byte> Writer::finish() const {
    BinaryWriter w;
    w.write(FileHeader{k_magic, m_kind, m_version, static_cast<uint32_t>(m_chunks.size())});
    for (const auto& chunk : m_chunks) {
        w.write(ChunkHeader{chunk.id, 0, chunk.payload.size()});
        w.write_bytes(chunk.payload);
        w.pad_to(16);
    }
    return w.take();
}

Reader::Reader(std::span<const std::byte> file, FourCC expected_kind) {
    BinaryReader r(file);
    auto header = r.read<FileHeader>();
    if (header.magic != k_magic) {
        throw ParseError("not an spud file (bad magic)");
    }
    if (header.kind != expected_kind) {
        throw ParseError(std::format("expected a {} file, found {}", to_string(expected_kind), to_string(header.kind)));
    }
    m_version = header.version;
    for (uint32_t i = 0; i < header.chunk_count; ++i) {
        auto chunk = r.read<ChunkHeader>();
        m_chunks.emplace_back(chunk.id, r.take(static_cast<size_t>(chunk.size)));
        auto aligned = (r.tell() + 15) & ~size_t{15};
        r.seek(std::min(aligned, r.size()));
    }
}

std::optional<std::span<const std::byte>> Reader::find(FourCC id) const {
    for (const auto& [chunk_id, bytes] : m_chunks) {
        if (chunk_id == id) {
            return bytes;
        }
    }
    return std::nullopt;
}

std::span<const std::byte> Reader::require(FourCC id) const {
    auto chunk = find(id);
    if (!chunk) {
        throw ParseError("missing chunk " + to_string(id));
    }
    return *chunk;
}

uint32_t StringTableBuilder::add(std::string_view s) {
    std::string key(s);
    if (auto it = m_offsets.find(key); it != m_offsets.end()) {
        return it->second;
    }
    auto offset = static_cast<uint32_t>(m_data.size());
    m_data.insert(m_data.end(), s.begin(), s.end());
    m_data.push_back('\0');
    m_offsets.emplace(std::move(key), offset);
    return offset;
}

std::string_view StringTable::get(uint32_t offset) const {
    if (offset == k_no_string) {
        return {};
    }
    if (offset >= m_bytes.size()) {
        throw ParseError(std::format("string offset {} out of range", offset));
    }
    auto start = reinterpret_cast<const char*>(m_bytes.data()) + offset;
    auto max = m_bytes.size() - offset;
    return std::string_view(start, strnlen(start, max));
}

} // namespace spud::container
