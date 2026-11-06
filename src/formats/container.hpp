#pragma once

// SPUD container: the chunked envelope shared by every binary spud format.
// Spec: docs/formats/container.md

#include "core/binary.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spud::container {

using FourCC = uint32_t;

constexpr FourCC fourcc(const char (&s)[5]) {
    return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24);
}

std::string to_string(FourCC id);

inline constexpr FourCC k_magic = fourcc("SPUD");
inline constexpr uint32_t k_no_string = 0xFFFFFFFFu;

struct FileHeader {
    FourCC magic;
    FourCC kind;
    uint32_t version;
    uint32_t chunk_count;
};
static_assert(sizeof(FileHeader) == 16);

struct ChunkHeader {
    FourCC id;
    uint32_t reserved;
    uint64_t size; // payload bytes, excluding padding
};
static_assert(sizeof(ChunkHeader) == 16);

class Writer {
public:
    Writer(FourCC kind, uint32_t version) : m_kind(kind), m_version(version) {}

    void add(FourCC id, std::span<const std::byte> payload);

    template <Pod T>
    void add_array(FourCC id, std::span<const T> items) {
        add(id, std::as_bytes(items));
    }

    template <Pod T>
    void add_array(FourCC id, const std::vector<T>& items) {
        add(id, std::as_bytes(std::span<const T>(items)));
    }

    template <Pod T>
    void add_struct(FourCC id, const T& value) {
        add(id, std::as_bytes(std::span<const T>(&value, 1)));
    }

    std::vector<std::byte> finish() const;

private:
    struct Chunk {
        FourCC id;
        std::vector<std::byte> payload;
    };
    FourCC m_kind;
    uint32_t m_version;
    std::vector<Chunk> m_chunks;
};

class Reader {
public:
    // Throws ParseError if the magic or kind don't match.
    Reader(std::span<const std::byte> file, FourCC expected_kind);

    uint32_t version() const { return m_version; }
    std::optional<std::span<const std::byte>> find(FourCC id) const;
    std::span<const std::byte> require(FourCC id) const;

    template <Pod T>
    std::vector<T> read_array(FourCC id) const {
        auto bytes = require(id);
        if (bytes.size() % sizeof(T) != 0) {
            throw ParseError("chunk " + to_string(id) + " size is not a multiple of its element size");
        }
        BinaryReader r(bytes);
        return r.read_array<T>(bytes.size() / sizeof(T));
    }

    template <Pod T>
    T read_struct(FourCC id) const {
        BinaryReader r(require(id));
        return r.read<T>();
    }

private:
    uint32_t m_version = 0;
    std::vector<std::pair<FourCC, std::span<const std::byte>>> m_chunks;
};

// Deduplicating string table: NUL-terminated UTF-8 strings addressed by byte offset.
class StringTableBuilder {
public:
    uint32_t add(std::string_view s);
    std::span<const std::byte> bytes() const { return std::as_bytes(std::span<const char>(m_data)); }

private:
    std::vector<char> m_data;
    std::unordered_map<std::string, uint32_t> m_offsets;
};

class StringTable {
public:
    StringTable() = default;
    explicit StringTable(std::span<const std::byte> bytes) : m_bytes(bytes) {}
    // Returns "" for k_no_string; throws ParseError for out-of-range offsets.
    std::string_view get(uint32_t offset) const;

private:
    std::span<const std::byte> m_bytes;
};

} // namespace spud::container
