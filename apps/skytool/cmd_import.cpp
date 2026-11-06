#include "commands.hpp"

#include "core/log.hpp"
#include "importer/far/far_view.hpp"
#include "importer/pipeline.hpp"

#include <cstdio>
#include <cstdlib>

namespace skytool {

int cmd_far(Args args) {
    spud::importer::far::FarViewOptions options;
    options.cache_dir = cache_dir();
    if (auto t = option(args, "--threads")) {
        options.threads = static_cast<unsigned>(std::atoi(t->c_str()));
    }
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--world") {
            options.worlds.push_back(args[i + 1]);
        }
    }
    spud::importer::far::build_far_views(options);
    return 0;
}

int cmd_import(Args args) {
    if (has_flag(args, "--help")) {
        std::puts("import [--world <edid>]... [--interior <edid>]... [--no-interiors] [--threads N] [--force]\n"
                  "       [--no-forms] [--audio-only | --no-audio] [--actors-only] [--doors-only] [--rules-only]\n"
                  "  Converts the install into the spud cache (default %LOCALAPPDATA%/spud/cache/skyrim).\n"
                  "  With no --world, imports every worldspace that has exterior cells; every interior cell unless\n"
                  "  --no-interiors, or only those --interior names. Also writes forms.osf (every\n"
                  "  form of the load order) unless --no-forms. Unchanged inputs are not converted again.");
        return 0;
    }
    spud::importer::ImportOptions options;
    options.cache_dir = cache_dir();
    options.force = has_flag(args, "--force");
    options.forms = !has_flag(args, "--no-forms");
    options.only_audio = has_flag(args, "--audio-only");
    options.only_actors = has_flag(args, "--actors-only");
    options.only_doors = has_flag(args, "--doors-only");
    options.only_rules = has_flag(args, "--rules-only");
    options.only_effects = has_flag(args, "--effects-only");
    options.audio = !has_flag(args, "--no-audio");
    options.interiors = !has_flag(args, "--no-interiors");
    if (auto t = option(args, "--threads")) {
        options.threads = static_cast<unsigned>(std::atoi(t->c_str()));
    }
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--world") {
            options.worlds.push_back(args[i + 1]);
        }
        if (args[i] == "--interior") {
            options.interior_cells.push_back(args[i + 1]);
        }
    }
    auto summary = spud::importer::run_import(open_install(), options);
    return summary.meshes_failed == 0 && summary.audio_failed == 0 ? 0 : 2;
}

} // namespace skytool
