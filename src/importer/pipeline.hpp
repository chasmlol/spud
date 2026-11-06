#pragma once

// The import pipeline: turns a Skyrim install into a self-contained spud
// cache (worlds, meshes, textures). See docs/ARCHITECTURE.md.

#include "importer/legacy/install.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace spud::importer {

struct ImportOptions {
    std::filesystem::path cache_dir;
    std::vector<std::string> worlds; // worldspace EditorIDs; empty = every worldspace with exterior cells
    bool interiors = true;           // every interior cell as a space (interiors/<name>.osw)
    std::vector<std::string> interior_cells; // only these interiors (EditorIDs); empty = all
    unsigned threads = 0;            // 0 = hardware concurrency
    bool force = false;              // re-convert meshes and re-extract textures that already exist
    bool forms = true;               // write the forms database (forms.osf)
    bool audio = true;               // audio records (audio/sound.osd) and sound/music files
    bool only_audio = false;         // skip worlds, meshes and textures
    bool only_doors = false;         // only rules/doors.oda
    bool only_rules = false;         // game settings, key bindings and the like only: rules/game.osr
    bool only_actors = false;        // only what actors are drawn with (skinned models, skeletons, their textures)
    bool only_effects = false;       // only what the records' effects draw (impacts, projectiles, explosions, hazards)
};

struct ImportSummary {
    size_t worlds = 0;
    size_t refs = 0;
    size_t refs_dropped = 0; // refs whose mesh was missing, failed or had nothing renderable
    size_t meshes = 0;
    size_t meshes_empty = 0;
    size_t meshes_missing = 0;
    size_t meshes_failed = 0;
    size_t textures = 0;
    size_t textures_missing = 0;
    size_t reused = 0;    // meshes and textures whose cached output was still valid
    size_t forms = 0;     // forms in forms.osf
    size_t forms_units_compiled = 0;
    size_t forms_units_reused = 0;
    size_t effects = 0; // .ose files written or kept (NIFs with particle systems)
    size_t audio_files = 0;
    size_t audio_failed = 0;
    size_t actor_models = 0;
    size_t actor_failed = 0;
    double seconds = 0.0;
};

ImportSummary run_import(const legacy::Install& install, const ImportOptions& options);

} // namespace spud::importer
