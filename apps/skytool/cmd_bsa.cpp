#include "commands.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "importer/legacy/bsa.hpp"
#include "importer/legacy/data_source.hpp"

#include <cstdio>

namespace skytool {

namespace {

std::filesystem::path resolve_archive(const std::string& name) {
    std::filesystem::path p(name);
    if (std::filesystem::exists(p)) {
        return p;
    }
    return open_install().data_dir / p;
}

} // namespace

int cmd_bsa(Args args) {
    auto pos = positionals(args);
    if (pos.empty()) {
        std::puts("bsa list <archive> [prefix]     list files (archive path or name in Data/)\n"
                  "bsa extract <archive> <path> <out>\n"
                  "bsa find <path>                 which source the merged data view resolves <path> from\n"
                  "bsa verify <archive>            decompress every file");
        return 1;
    }
    const auto& sub = pos[0];
    if (sub == "list" && pos.size() >= 2) {
        spud::legacy::BsaArchive bsa(resolve_archive(pos[1]));
        auto prefix = pos.size() >= 3 ? spud::path::normalize(pos[2]) : std::string();
        size_t shown = 0;
        for (const auto& e : bsa.entries()) {
            if (e.path.starts_with(prefix)) {
                std::printf("%10u %c %s\n", e.size, e.compressed ? 'z' : ' ', e.path.c_str());
                ++shown;
            }
        }
        spud::log::info("{} of {} files (BSA v{})", shown, bsa.entries().size(), bsa.version());
        return 0;
    }
    if (sub == "extract" && pos.size() >= 4) {
        spud::legacy::BsaArchive bsa(resolve_archive(pos[1]));
        auto index = bsa.find(spud::path::normalize(pos[2]));
        if (!index) {
            spud::log::error("{} not in archive", pos[2]);
            return 1;
        }
        auto data = bsa.read(*index);
        spud::write_file(pos[3], data);
        spud::log::info("wrote {} bytes to {}", data.size(), pos[3]);
        return 0;
    }
    if (sub == "find" && pos.size() >= 2) {
        spud::legacy::DataSource data(open_install());
        auto p = spud::path::normalize(pos[1]);
        auto origin = data.origin(p);
        std::printf("%s: %s\n", p.c_str(), origin.empty() ? "(not found)" : origin.c_str());
        return origin.empty() ? 1 : 0;
    }
    if (sub == "verify" && pos.size() >= 2) {
        spud::legacy::BsaArchive bsa(resolve_archive(pos[1]));
        size_t bytes = 0;
        size_t failed = 0;
        for (size_t i = 0; i < bsa.entries().size(); ++i) {
            try {
                bytes += bsa.read(i).size();
            } catch (const std::exception& e) {
                spud::log::error("{}", e.what());
                ++failed;
            }
        }
        spud::log::info("{} files, {} bytes decompressed, {} failures", bsa.entries().size(), bytes, failed);
        return failed == 0 ? 0 : 1;
    }
    spud::log::error("unknown bsa subcommand");
    return 1;
}

} // namespace skytool
