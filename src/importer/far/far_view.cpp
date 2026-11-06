#include "importer/far/far_view.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "importer/far/texels.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace spud::importer::far {

void build_far_views(const FarViewOptions& options) {
    const unsigned threads = options.threads ? options.threads : std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::filesystem::path> files;
    if (options.worlds.empty()) {
        for (const auto& e : std::filesystem::directory_iterator(options.cache_dir / "worlds")) {
            if (e.path().extension() == ".osw") {
                files.push_back(e.path());
            }
        }
        std::sort(files.begin(), files.end());
    } else {
        for (const auto& w : options.worlds) {
            files.push_back(options.cache_dir / "worlds" / (path::to_lower(w) + ".osw"));
        }
    }
    TexelCache texels(options.cache_dir);
    for (const auto& file : files) {
        const auto start = std::chrono::steady_clock::now();
        const osw::World world = osw::read(read_file(file));
        osh::FarView view;
        bake_land(world, texels, options.cache_dir, view, threads);
        if (view.colour.empty()) {
            continue; // no land: nothing to see from afar
        }
        bake_trees(world, texels, options.cache_dir, view, threads);
        bake_proxies(world, texels, options.cache_dir, view, threads);
        write_file(options.cache_dir / "far" / (path::to_lower(world.edid) + ".osh"), osh::write(view));
        log::info("far view: {} in {:.1f} s", world.edid,
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    }
}

} // namespace spud::importer::far
