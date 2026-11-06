#include "core/file.hpp"

#include "core/error.hpp"

#include <format>
#include <fstream>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace spud {

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        throw Error(std::format("cannot open {}", path.string()));
    }
    auto size = static_cast<size_t>(in.tellg());
    std::vector<std::byte> data(size);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    if (!in) {
        throw Error(std::format("cannot read {}", path.string()));
    }
    return data;
}

void write_file(const std::filesystem::path& path, std::span<const std::byte> data) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw Error(std::format("cannot create {}", tmp.string()));
        }
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) {
            throw Error(std::format("cannot write {}", tmp.string()));
        }
    }
    std::filesystem::rename(tmp, path);
}

#ifdef _WIN32

MappedFile::MappedFile(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw Error(std::format("cannot open {}", path.string()));
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    m_file = file;
    m_size = static_cast<size_t>(size.QuadPart);
    if (m_size == 0) {
        static const std::byte empty{};
        m_data = &empty;
        return;
    }
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) {
        close();
        throw Error(std::format("cannot map {}", path.string()));
    }
    m_mapping = mapping;
    m_data = static_cast<const std::byte*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    if (!m_data) {
        close();
        throw Error(std::format("cannot map view of {}", path.string()));
    }
}

void MappedFile::close() {
    if (m_data && m_mapping) {
        UnmapViewOfFile(m_data);
    }
    if (m_mapping) {
        CloseHandle(m_mapping);
    }
    if (m_file) {
        CloseHandle(m_file);
    }
    m_data = nullptr;
    m_mapping = nullptr;
    m_file = nullptr;
    m_size = 0;
}

#else
#error "MappedFile is only implemented for Windows so far"
#endif

MappedFile::~MappedFile() { close(); }

MappedFile::MappedFile(MappedFile&& other) noexcept
    : m_data(std::exchange(other.m_data, nullptr)), m_size(std::exchange(other.m_size, 0)),
      m_file(std::exchange(other.m_file, nullptr)), m_mapping(std::exchange(other.m_mapping, nullptr)) {}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        close();
        m_data = std::exchange(other.m_data, nullptr);
        m_size = std::exchange(other.m_size, 0);
        m_file = std::exchange(other.m_file, nullptr);
        m_mapping = std::exchange(other.m_mapping, nullptr);
    }
    return *this;
}

} // namespace spud
