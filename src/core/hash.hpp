#pragma once

// XXH64 (xxHash, 64-bit variant; algorithm per the xxHash specification). Used to identify file contents
// for incremental imports.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

namespace spud {

uint64_t xxh64(std::span<const std::byte> data, uint64_t seed = 0);
inline uint64_t xxh64(std::string_view s, uint64_t seed = 0) {
    return xxh64(std::as_bytes(std::span(s.data(), s.size())), seed);
}

// Incremental hashing of several inputs (the order matters).
class Hasher {
public:
    void add(std::span<const std::byte> data) { m_state = xxh64(data, m_state ^ 0x9E3779B97F4A7C15ull); }
    void add(std::string_view s) { add(std::as_bytes(std::span(s.data(), s.size()))); }
    void add(uint64_t v) { add(std::as_bytes(std::span(&v, 1))); }
    uint64_t value() const { return m_state; }

private:
    uint64_t m_state = 0;
};

// XXH64 of a whole file (memory-mapped). Throws Error if it can't be read.
uint64_t hash_file(const std::filesystem::path& path);

} // namespace spud
