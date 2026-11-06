#pragma once

#include "core/error.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace spud {

static_assert(std::endian::native == std::endian::little, "spud assumes a little-endian host");

template <typename T>
concept Pod = std::is_trivially_copyable_v<T>;

// Bounds-checked little-endian reader over a byte span. Every read past the
// end throws ParseError, so parsers can be written straight-line.
class BinaryReader {
public:
    BinaryReader() = default;
    explicit BinaryReader(std::span<const std::byte> data) : m_data(data) {}

    template <Pod T>
    T read() {
        T value;
        std::memcpy(&value, take(sizeof(T)).data(), sizeof(T));
        return value;
    }

    template <Pod T>
    void read_into(std::span<T> out) {
        auto bytes = take(out.size_bytes());
        std::memcpy(out.data(), bytes.data(), bytes.size());
    }

    template <Pod T>
    std::vector<T> read_array(size_t count) {
        std::vector<T> out(count);
        read_into(std::span<T>(out));
        return out;
    }

    std::span<const std::byte> take(size_t n) {
        require(n);
        auto s = m_data.subspan(m_pos, n);
        m_pos += n;
        return s;
    }

    // Reads exactly n bytes as characters, dropping trailing NULs.
    std::string read_string(size_t n) {
        auto bytes = take(n);
        std::string s(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        while (!s.empty() && s.back() == '\0') {
            s.pop_back();
        }
        return s;
    }

    void skip(size_t n) {
        require(n);
        m_pos += n;
    }

    void seek(size_t pos) {
        if (pos > m_data.size()) {
            throw ParseError(std::format("seek to {} past end of {}-byte buffer", pos, m_data.size()));
        }
        m_pos = pos;
    }

    size_t tell() const { return m_pos; }
    size_t size() const { return m_data.size(); }
    size_t remaining() const { return m_data.size() - m_pos; }
    bool eof() const { return m_pos >= m_data.size(); }
    std::span<const std::byte> data() const { return m_data; }
    std::span<const std::byte> rest() const { return m_data.subspan(m_pos); }

private:
    void require(size_t n) const {
        if (n > m_data.size() - m_pos) {
            throw ParseError(std::format("read of {} bytes at offset {} overruns {}-byte buffer", n, m_pos,
                                         m_data.size()));
        }
    }

    std::span<const std::byte> m_data;
    size_t m_pos = 0;
};

// Growable little-endian writer.
class BinaryWriter {
public:
    template <Pod T>
    void write(const T& value) {
        append(&value, sizeof(T));
    }

    template <Pod T>
    void write_span(std::span<const T> items) {
        append(items.data(), items.size_bytes());
    }

    void write_bytes(std::span<const std::byte> bytes) { append(bytes.data(), bytes.size()); }

    void write_string(std::string_view s) { append(s.data(), s.size()); }

    void pad_to(size_t alignment) {
        while (m_buffer.size() % alignment != 0) {
            m_buffer.push_back(std::byte{0});
        }
    }

    template <Pod T>
    void patch(size_t offset, const T& value) {
        std::memcpy(m_buffer.data() + offset, &value, sizeof(T));
    }

    size_t size() const { return m_buffer.size(); }
    const std::vector<std::byte>& buffer() const { return m_buffer; }
    std::vector<std::byte> take() { return std::move(m_buffer); }

private:
    void append(const void* data, size_t n) {
        auto p = static_cast<const std::byte*>(data);
        m_buffer.insert(m_buffer.end(), p, p + n);
    }

    std::vector<std::byte> m_buffer;
};

template <Pod T>
std::span<const std::byte> as_bytes_of(const std::vector<T>& v) {
    return std::as_bytes(std::span<const T>(v));
}

} // namespace spud
