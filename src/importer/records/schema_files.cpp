#include "importer/records/schema_files.hpp"

#include "core/file.hpp"
#include "core/hash.hpp"

#include <algorithm>
#include <vector>

namespace spud::records {

struct EmbeddedFile {
    const char* name;
    const unsigned char* data;
    size_t size;
};
extern const EmbeddedFile k_embedded_schema[];

LoadedSchema load_schema(const std::optional<std::filesystem::path>& dir) {
    std::vector<std::pair<std::string, std::string>> files;
    if (dir) {
        for (const auto& e : std::filesystem::directory_iterator(*dir)) {
            if (e.path().extension() == ".txt") {
                auto bytes = read_file(e.path());
                files.emplace_back(e.path().filename().string(),
                                   std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
            }
        }
    } else {
        for (auto* f = k_embedded_schema; f->name; ++f) {
            files.emplace_back(f->name, std::string(reinterpret_cast<const char*>(f->data), f->size));
        }
    }
    // common.txt first: the other files use its types and field sets; then name order.
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        bool ca = a.first == "common.txt";
        bool cb = b.first == "common.txt";
        return ca != cb ? ca : a.first < b.first;
    });
    LoadedSchema out;
    for (const auto& [name, text] : files) {
        if (name == "conditions.txt") {
            out.conditions_text = text;
            continue;
        }
        out.schema.parse(text, name);
        out.text += "# ---- " + name + "\n" + text;
        if (!text.ends_with('\n')) {
            out.text += '\n';
        }
    }
    out.schema.finish();
    out.conditions.parse(out.conditions_text);
    Hasher h;
    h.add(out.text);
    h.add(out.conditions_text);
    out.hash = h.value();
    return out;
}

} // namespace spud::records
