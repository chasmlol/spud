#include "core/hash.hpp"

#include "core/file.hpp"

#include <bit>
#include <cstring>

namespace spud {

namespace {

constexpr uint64_t k_p1 = 0x9E3779B185EBCA87ull;
constexpr uint64_t k_p2 = 0xC2B2AE3D27D4EB4Full;
constexpr uint64_t k_p3 = 0x165667B19E3779F9ull;
constexpr uint64_t k_p4 = 0x85EBCA77C2B2AE63ull;
constexpr uint64_t k_p5 = 0x27D4EB2F165667C5ull;

uint64_t read64(const std::byte* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

uint32_t read32(const std::byte* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

uint64_t round(uint64_t acc, uint64_t lane) {
    acc += lane * k_p2;
    acc = std::rotl(acc, 31);
    return acc * k_p1;
}

uint64_t merge(uint64_t acc, uint64_t v) {
    acc ^= round(0, v);
    return acc * k_p1 + k_p4;
}

} // namespace

uint64_t xxh64(std::span<const std::byte> data, uint64_t seed) {
    const std::byte* p = data.data();
    const std::byte* end = p + data.size();
    uint64_t h;
    if (data.size() >= 32) {
        uint64_t v1 = seed + k_p1 + k_p2;
        uint64_t v2 = seed + k_p2;
        uint64_t v3 = seed;
        uint64_t v4 = seed - k_p1;
        const std::byte* limit = end - 32;
        do {
            v1 = round(v1, read64(p));
            v2 = round(v2, read64(p + 8));
            v3 = round(v3, read64(p + 16));
            v4 = round(v4, read64(p + 24));
            p += 32;
        } while (p <= limit);
        h = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
        h = merge(h, v1);
        h = merge(h, v2);
        h = merge(h, v3);
        h = merge(h, v4);
    } else {
        h = seed + k_p5;
    }
    h += data.size();
    while (p + 8 <= end) {
        h ^= round(0, read64(p));
        h = std::rotl(h, 27) * k_p1 + k_p4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= uint64_t(read32(p)) * k_p1;
        h = std::rotl(h, 23) * k_p2 + k_p3;
        p += 4;
    }
    while (p < end) {
        h ^= uint64_t(std::to_integer<uint8_t>(*p)) * k_p5;
        h = std::rotl(h, 11) * k_p1;
        ++p;
    }
    h ^= h >> 33;
    h *= k_p2;
    h ^= h >> 29;
    h *= k_p3;
    h ^= h >> 32;
    return h;
}

uint64_t hash_file(const std::filesystem::path& path) {
    if (std::filesystem::file_size(path) == 0) {
        return xxh64(std::span<const std::byte>());
    }
    MappedFile f(path);
    return xxh64(f.bytes());
}

} // namespace spud
