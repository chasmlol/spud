#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace spud {

std::vector<std::byte> read_file(const std::filesystem::path& path);

// Writes via a temporary file and rename, creating parent directories, so a
// crash never leaves a half-written file at `path`.
void write_file(const std::filesystem::path& path, std::span<const std::byte> data);

// Read-only memory mapping. Used for multi-gigabyte archives that we only
// touch a few pages of.
class MappedFile {
public:
    MappedFile() = default;
    explicit MappedFile(const std::filesystem::path& path);
    ~MappedFile();

    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    std::span<const std::byte> bytes() const { return {m_data, m_size}; }
    bool is_open() const { return m_data != nullptr; }

private:
    void close();

    const std::byte* m_data = nullptr;
    size_t m_size = 0;
    void* m_file = nullptr;
    void* m_mapping = nullptr;
};

} // namespace spud
