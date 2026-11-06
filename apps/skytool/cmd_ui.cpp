// skytool ui: convert and inspect the game's interface movies (docs/formats/osu.md).

#include "commands.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "formats/osl.hpp"
#include "fx/effect.hpp"
#include "formats/osu.hpp"
#include "importer/convert/ui_import.hpp"
#include "importer/convert/ui_images.hpp"
#include "importer/convert/ui_loading.hpp"
#include "importer/convert/ui_strings.hpp"
#include "importer/convert/ui_worlds.hpp"
#include "importer/convert/world_tables.hpp"
#include "importer/legacy/esm.hpp"
#include "importer/legacy/data_source.hpp"
#include "ui/player/draw.hpp"
#include "ui/player/player.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <format>
#include <map>

namespace skytool {

namespace {

using spud::legacy::DataSource;

std::vector<std::byte> read_source(const DataSource* data, const std::string& path) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        return spud::read_file(path);
    }
    if (data) {
        if (auto b = data->read(spud::path::normalize(path))) {
            return std::move(*b);
        }
    }
    throw spud::Error("not found: " + path);
}

void summary(const spud::osu::Movie& m) {
    std::printf("stage %.0fx%.0f @ %.0f fps, SWF %u\n", m.header.stage_width, m.header.stage_height,
                m.header.frame_rate, m.header.source_version);
    std::printf("characters %zu: shapes %zu (meshes %zu, %zu vertices, %zu triangles), sprites %zu, texts %zu, "
                "fonts %zu (%zu glyphs), bitmaps %zu (%.1f MiB), buttons %zu\n",
                m.characters.size(), m.shapes.size(), m.meshes.size(), m.vertices.size(), m.indices.size() / 3,
                m.sprites.size(), m.texts.size(), m.fonts.size(), m.glyphs.size(), m.bitmaps.size(),
                m.bitmap_data.size() / 1048576.0, m.buttons.size());
    std::printf("frames %zu, place commands %zu, filters %zu, labels %zu, clip events %zu\n", m.frames.size(),
                m.commands.size(), m.filters.size(), m.labels.size(), m.events.size());
    std::map<int, int> kinds, blends;
    for (const auto& f : m.filters) {
        ++kinds[int(f.kind)];
    }
    for (const auto& c : m.commands) {
        if (c.flags & spud::osu::place_blend) {
            ++blends[int(c.blend)];
        }
    }
    static const char* k_filter_names[] = {"drop_shadow", "blur", "glow", "bevel", "gradient_glow", "convolution",
                                           "color_matrix", "gradient_bevel"};
    static const char* k_blend_names[] = {"normal", "layer", "multiply", "screen", "lighten", "darken", "difference",
                                          "add", "subtract", "invert", "alpha", "erase", "overlay", "hardlight"};
    if (!kinds.empty() || !blends.empty()) {
        std::printf("filter kinds:");
        for (const auto& [k, n] : kinds) {
            std::printf(" %s x%d", k < 8 ? k_filter_names[k] : "?", n);
        }
        std::printf("; blend modes:");
        for (const auto& [b, n] : blends) {
            std::printf(" %s x%d", b < 14 ? k_blend_names[b] : "?", n);
        }
        std::printf("\n");
        std::map<std::string, int> distinct;
        for (const auto& f : m.filters) {
            ++distinct[std::format("{} color {:08x} blur {}x{} strength {} angle {:.2f} distance {} passes {} flags {}",
                                   int(f.kind) < 8 ? k_filter_names[int(f.kind)] : "?", f.color, f.blur_x, f.blur_y,
                                   f.strength, f.angle, f.distance, f.passes, f.flags)];
        }
        for (const auto& [d, n] : distinct) {
            std::printf("  %dx %s\n", n, d.c_str());
        }
    }
    std::printf("script: %zu functions, %zu instructions, %zu values; %zu init blocks\n", m.functions.size(),
                m.instructions.size(), m.values.size(), m.init_actions.size());
    std::printf("exports %zu, imports %zu:", m.exports.size(), m.imports.size());
    for (const auto& i : m.imports) {
        std::printf(" %.*s<-%.*s", int(m.string(i.name).size()), m.string(i.name).data(),
                    int(m.string(i.url).size()), m.string(i.url).data());
    }
    std::printf("\n");
    for (const auto& f : m.fonts) {
        std::printf("font '%.*s' flags %u, %u glyphs\n", int(m.string(f.name).size()), m.string(f.name).data(),
                    f.flags, f.glyph_count);
    }
}

const char* op_name(uint8_t op) {
    switch (op) {
    case 0x04: return "NextFrame"; case 0x05: return "PrevFrame"; case 0x06: return "Play"; case 0x07: return "Stop";
    case 0x08: return "ToggleQuality"; case 0x09: return "StopSounds"; case 0x0A: return "Add";
    case 0x0B: return "Subtract"; case 0x0C: return "Multiply"; case 0x0D: return "Divide"; case 0x0E: return "Equals";
    case 0x0F: return "Less"; case 0x10: return "And"; case 0x11: return "Or"; case 0x12: return "Not";
    case 0x13: return "StringEquals"; case 0x14: return "StringLength"; case 0x15: return "StringExtract";
    case 0x17: return "Pop"; case 0x18: return "ToInteger"; case 0x1C: return "GetVariable";
    case 0x1D: return "SetVariable"; case 0x20: return "SetTarget2"; case 0x21: return "StringAdd";
    case 0x22: return "GetProperty"; case 0x23: return "SetProperty"; case 0x24: return "CloneSprite";
    case 0x25: return "RemoveSprite"; case 0x26: return "Trace"; case 0x27: return "StartDrag";
    case 0x28: return "EndDrag"; case 0x29: return "StringLess"; case 0x2A: return "Throw"; case 0x2B: return "CastOp";
    case 0x2C: return "ImplementsOp"; case 0x30: return "RandomNumber"; case 0x31: return "MBStringLength";
    case 0x32: return "CharToAscii"; case 0x33: return "AsciiToChar"; case 0x34: return "GetTime";
    case 0x35: return "MBStringExtract"; case 0x36: return "MBCharToAscii"; case 0x37: return "MBAsciiToChar";
    case 0x3A: return "Delete"; case 0x3B: return "Delete2"; case 0x3C: return "DefineLocal";
    case 0x3D: return "CallFunction"; case 0x3E: return "Return"; case 0x3F: return "Modulo";
    case 0x40: return "NewObject"; case 0x41: return "DefineLocal2"; case 0x42: return "InitArray";
    case 0x43: return "InitObject"; case 0x44: return "TypeOf"; case 0x45: return "TargetPath";
    case 0x46: return "Enumerate"; case 0x47: return "Add2"; case 0x48: return "Less2"; case 0x49: return "Equals2";
    case 0x4A: return "ToNumber"; case 0x4B: return "ToString"; case 0x4C: return "PushDuplicate";
    case 0x4D: return "StackSwap"; case 0x4E: return "GetMember"; case 0x4F: return "SetMember";
    case 0x50: return "Increment"; case 0x51: return "Decrement"; case 0x52: return "CallMethod";
    case 0x53: return "NewMethod"; case 0x54: return "InstanceOf"; case 0x55: return "Enumerate2";
    case 0x60: return "BitAnd"; case 0x61: return "BitOr"; case 0x62: return "BitXor"; case 0x63: return "BitLShift";
    case 0x64: return "BitRShift"; case 0x65: return "BitURShift"; case 0x66: return "StrictEquals";
    case 0x67: return "Greater"; case 0x68: return "StringGreater"; case 0x69: return "Extends";
    case 0x81: return "GotoFrame"; case 0x83: return "GetURL"; case 0x87: return "StoreRegister";
    case 0x88: return "ConstantPool"; case 0x8A: return "WaitForFrame"; case 0x8B: return "SetTarget";
    case 0x8C: return "GoToLabel"; case 0x8D: return "WaitForFrame2"; case 0x8E: return "DefineFunction2";
    case 0x8F: return "Try"; case 0x94: return "With"; case 0x96: return "Push"; case 0x99: return "Jump";
    case 0x9A: return "GetURL2"; case 0x9B: return "DefineFunction"; case 0x9D: return "If"; case 0x9E: return "Call";
    case 0x9F: return "GotoFrame2";
    default: return "?";
    }
}

std::string push_values(const spud::osu::Movie& m, const spud::osu::Instruction& ins) {
    std::string out;
    for (uint32_t i = 0; i < ins.count; ++i) {
        const auto& v = m.values[ins.arg + i];
        out += i ? ", " : " ";
        switch (v.kind) {
        case spud::osu::ValueKind::string: out += "\"" + std::string(m.string(v.u)) + "\""; break;
        case spud::osu::ValueKind::number: out += std::format("{}", v.number); break;
        case spud::osu::ValueKind::null: out += "null"; break;
        case spud::osu::ValueKind::undefined: out += "undefined"; break;
        case spud::osu::ValueKind::reg: out += std::format("r{}", v.u); break;
        case spud::osu::ValueKind::boolean: out += v.u ? "true" : "false"; break;
        }
    }
    return out;
}

void disasm_function(const spud::osu::Movie& m, uint32_t fi) {
    const auto& f = m.functions[fi];
    std::string params;
    for (uint32_t i = 0; i < f.param_count; ++i) {
        const auto& p = m.params[f.first_param + i];
        params += std::format("{}{}{}", i ? ", " : "", m.string(p.name), p.reg ? std::format(":r{}", p.reg) : "");
    }
    std::printf("function #%u %s(%s) kind %d regs %u flags 0x%x\n", fi,
                f.name == spud::osu::k_none ? "<anon>" : std::string(m.string(f.name)).c_str(), params.c_str(),
                int(f.kind), f.registers, f.flags);
    for (uint32_t i = 0; i < f.instruction_count; ++i) {
        const auto& ins = m.instructions[f.first_instruction + i];
        std::string arg;
        switch (ins.op) {
        case 0x96: arg = push_values(m, ins); break;
        case 0x99: case 0x9D: arg = std::format(" -> {}", ins.arg); break;
        case 0x9B: case 0x8E: arg = std::format(" #{}", ins.arg); break;
        case 0x87: arg = std::format(" r{}", ins.arg); break;
        case 0x81: arg = std::format(" {}", ins.arg); break;
        case 0x8C: case 0x8B: arg = " \"" + std::string(m.string(ins.arg)) + "\""; break;
        case 0x9F: arg = std::format(" play {} bias {}", ins.count, ins.arg); break;
        default: break;
        }
        std::printf("  %4u %s%s\n", i, op_name(ins.op), arg.c_str());
    }
}

// Lists the functions whose name contains `filter`, or that push a string containing `grep`.
void disasm(const spud::osu::Movie& m, const std::string& filter, const std::string& grep) {
    for (uint32_t fi = 0; fi < m.functions.size(); ++fi) {
        const auto& f = m.functions[fi];
        bool show = false;
        if (!filter.empty() && f.name != spud::osu::k_none) {
            show = std::string(m.string(f.name)).find(filter) != std::string::npos;
        }
        if (!grep.empty()) {
            for (uint32_t i = 0; i < f.instruction_count && !show; ++i) {
                const auto& ins = m.instructions[f.first_instruction + i];
                if (ins.op == 0x96 && push_values(m, ins).find(grep) != std::string::npos) {
                    show = true;
                }
            }
        }
        if (filter.empty() && grep.empty()) {
            show = true;
        }
        if (show) {
            disasm_function(m, fi);
        }
    }
}

// Logs what a movie asks of the engine; answers nothing.
class PrintHost final : public spud::ui::player::PlayerHost {
public:
    bool quiet = false;
    spud::ui::player::Value external_call(spud::ui::player::Player& p, const std::string& method,
                                          const std::vector<spud::ui::player::Value>& args) override {
        if (!quiet) {
            std::string line = "  engine call " + method + "(";
            for (size_t i = 0; i < args.size(); ++i) {
                line += (i ? ", " : "") + *p.vm.to_string(args[i]);
            }
            std::printf("%s)\n", line.c_str());
        }
        return {};
    }
};

void print_tree(spud::ui::player::DisplayObject* d, int indent, int max_depth) {
    using namespace spud::ui::player;
    if (indent > max_depth) {
        return;
    }
    const char* kind = d->kind == DisplayKind::clip    ? "clip"
                       : d->kind == DisplayKind::shape ? "shape"
                       : d->kind == DisplayKind::text  ? "text"
                                                       : "button";
    std::string extra;
    if (d->kind == DisplayKind::clip) {
        auto* c = static_cast<MovieClip*>(d);
        extra = std::format(" frame {}/{}{}", c->current_frame + 1, c->frame_count(), c->playing ? " playing" : "");
    } else if (d->kind == DisplayKind::text) {
        extra = " \"" + u32_to_utf8(static_cast<TextFieldObject*>(d)->plain_text()).substr(0, 60) + "\"";
    }
    std::printf("%*s%s %s @%d (%.1f, %.1f) a%.2f%s%s\n", indent * 2, "", kind, d->name.c_str(), d->depth,
                d->matrix.tx, d->matrix.ty, d->cxform.mul[3], d->visible ? "" : " hidden", extra.c_str());
    if (d->kind == DisplayKind::clip) {
        for (auto& [depth, c] : static_cast<MovieClip*>(d)->children) {
            print_tree(c.get(), indent + 1, max_depth);
        }
    }
}

// Runs a movie as the game opens a menu (first frame, Advance(0), InitExtensions, SetPlatform(0, false); see
//) and then `frames` frames. Returns the script errors; `notes` gets the
// calls of undefined names.
std::vector<std::string> play_movie(spud::ui::player::Library& lib, const std::string& name, int frames,
                                    bool quiet, bool tree, int tree_depth, std::vector<std::string>& notes) {
    using namespace spud::ui::player;
    auto def = lib.movie(name);
    if (!def) {
        return {"no such movie: " + name};
    }
    PrintHost host;
    host.quiet = quiet;
    Player p(lib, def, &host);
    p.vm.report_missing = true;
    p.start();
    p.advance(0);
    if (p.has_path("InitExtensions")) {
        p.invoke("InitExtensions", {});
    }
    if (p.has_path("SetPlatform")) {
        p.invoke("SetPlatform", {Value::number(0), Value::boolean(false)});
    }
    for (int i = 0; i < frames; ++i) {
        p.step_frame();
    }
    DrawList list;
    p.render(list);
    if (tree) {
        print_tree(p.root.get(), 0, tree_depth);
    }
    if (!quiet) {
        std::printf("%d frames, %zu draw items, %zu live objects\n", frames, list.items.size(), p.vm.heap.live());
    }
    notes = p.vm.notes;
    return p.errors();
}

} // namespace

int cmd_ui(Args args) {
    const auto pos = positionals(args, {"--grep", "--times"});
    if (pos.empty() || has_flag(args, "--help")) {
        std::puts("ui convert <movie.swf|interface/x.swf> <out.osu>   convert one movie and summarise it\n"
                  "ui dump <file.osu>                                 summarise a converted movie\n"
                  "ui check                                           convert every interface movie in memory, "
                  "report warnings\n"
                  "ui import [--force]                                convert the interface into the cache\n"
                  "ui model <nif>... [--force]                        convert models a UI 3D scene shows (an item's,\n"
                  "                                                   a book's) into the cache with their scene graph\n"
                  "ui loading [filter]                                the converted loading screens (.osl)\n"
                  "ui play <movie> [frames] [--tree [depth]] [--quiet]  run a cached movie headless, report errors\n"
                  "ui play --all [frames]                             run every cached movie, report errors\n"
                  "ui disasm <file.osu> [name-filter] [--grep text]   list a movie's script");
        return pos.empty() ? 1 : 0;
    }
    const std::string& sub = pos[0];
    if (sub == "convert" && pos.size() == 3) {
        std::unique_ptr<DataSource> data;
        std::error_code ec;
        if (!std::filesystem::exists(pos[1], ec)) {
            data = std::make_unique<DataSource>(open_install());
        }
        auto m = spud::importer::import_movie(read_source(data.get(), pos[1]));
        spud::write_file(pos[2], spud::osu::write(m.movie));
        summary(spud::osu::read(spud::read_file(pos[2])));
        for (const auto& w : m.warnings) {
            std::printf("warning: %s\n", w.c_str());
        }
        return 0;
    }
    if (sub == "dump" && pos.size() == 2) {
        summary(spud::osu::read(spud::read_file(pos[1])));
        return 0;
    }
    if (sub == "check") {
        DataSource data(open_install());
        size_t failed = 0;
        std::map<std::string, size_t> warning_kinds;
        for (const auto& file : data.list("interface/", ".swf")) {
            try {
                auto m = spud::importer::import_movie(*data.read(file));
                spud::osu::read(spud::osu::write(m.movie)); // round trip
                std::printf("ok    %-50s %5zu functions %6zu instructions %4zu warnings\n", file.c_str(),
                            m.movie.functions.size(), m.movie.instructions.size(), m.warnings.size());
                for (const auto& w : m.warnings) {
                    ++warning_kinds[w.substr(0, w.find_first_of("0123456789"))];
                }
            } catch (const std::exception& e) {
                ++failed;
                std::printf("FAIL  %-50s %s\n", file.c_str(), e.what());
            }
        }
        for (const auto& [k, n] : warning_kinds) {
            std::printf("warning x%zu: %s\n", n, k.c_str());
        }
        return failed ? 2 : 0;
    }
    if (sub == "disasm" && pos.size() >= 2) {
        std::string path = pos[1];
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            path = (cache_dir() / "interface" / path).string();
        }
        const auto grep = option(args, "--grep");
        disasm(spud::osu::read(spud::read_file(path)), pos.size() >= 3 ? pos[2] : std::string(), grep ? *grep : "");
        return 0;
    }
    if (sub == "play") {
        spud::ui::player::Library lib(cache_dir() / "interface");
        const bool all = has_flag(args, "--all");
        const bool tree = has_flag(args, "--tree");
        const bool quiet = has_flag(args, "--quiet") || all;
        std::vector<std::string> movies;
        int frames = 30;
        size_t next = 1;
        if (all) {
            for (const auto& e : std::filesystem::recursive_directory_iterator(lib.root())) {
                const auto rel = std::filesystem::relative(e.path(), lib.root()).generic_string();
                if (!e.is_regular_file() || !rel.ends_with(".osu")) {
                    continue;
                }
                // Movies only: the cache also holds translations, font maps, controls and game settings.
                std::ifstream in(e.path(), std::ios::binary);
                spud::container::FileHeader h{};
                if (in.read(reinterpret_cast<char*>(&h), sizeof(h)) && h.kind == spud::osu::k_kind_movie) {
                    movies.push_back(rel);
                }
            }
        } else if (pos.size() >= 2) {
            movies.push_back(pos[1]);
            next = 2;
        }
        if (pos.size() > next) {
            frames = std::atoi(pos[next].c_str());
        }
        const int tree_depth = tree && pos.size() > next + 1 ? std::atoi(pos[next + 1].c_str()) : 99;
        size_t failed = 0;
        for (const auto& m : movies) {
            std::vector<std::string> notes;
            const auto errors = play_movie(lib, m, frames, quiet, tree, tree_depth, notes);
            std::map<std::string, int> counts, note_counts;
            for (const auto& e : errors) {
                ++counts[e];
            }
            for (const auto& e : notes) {
                ++note_counts[e];
            }
            std::printf("%s %-46s %zu errors, %zu notes\n", errors.empty() ? "ok  " : "FAIL", m.c_str(), errors.size(),
                        notes.size());
            for (const auto& [e, n] : counts) {
                std::printf("      %dx %s\n", n, e.c_str());
            }
            for (const auto& [e, n] : note_counts) {
                std::printf("      note %dx %s\n", n, e.c_str());
            }
            failed += !errors.empty();
        }
        return failed ? 2 : 0;
    }
    if (sub == "import") {
        DataSource data(open_install());
        auto s = spud::importer::import_interface(data, cache_dir().string(), has_flag(args, "--force"));
        const spud::legacy::Install install = open_install();
        const spud::legacy::EsmDatabase db(install);
        spud::importer::write_game_strings(db, data, cache_dir().string());
        spud::importer::write_loading_screens(db, data, cache_dir().string(), has_flag(args, "--force"));
        spud::importer::write_interface_images(db, data, cache_dir().string(), has_flag(args, "--force"));
        spud::importer::write_gfx_default_image(install.game_dir, cache_dir().string());
        spud::importer::write_worldspace_maps(db, cache_dir().string());
        return s.movies_failed ? 2 : 0;
    }
    if (sub == "world-tables") {
        // ui world-tables: rules/world.osg alone, without a whole import.
        DataSource data(open_install());
        const spud::legacy::Install install = open_install();
        const spud::legacy::EsmDatabase db(install);
        spud::importer::write_world_tables(db, data, cache_dir().string());
        return 0;
    }
    if (sub == "worldmap") {
        // ui worldmap [--force]: the world map's land, objects and clouds into the cache.
        DataSource data(open_install());
        std::printf("%zu blocks\n", spud::importer::convert_world_map(data, cache_dir().string(), has_flag(args, "--force")));
        return 0;
    }
    if (sub == "model" && pos.size() >= 2) {
        // ui model <nif>...: models the UI 3D scenes show that the world import may not have converted, or not with
        // their scene graph (an item's model, a book's): .osm, .ose (version 3) and textures into the cache.
        DataSource data(open_install());
        size_t failed = 0;
        for (size_t i = 1; i < pos.size(); ++i) {
            const bool ok =
                spud::importer::convert_scene_model(data, cache_dir().string(), pos[i], has_flag(args, "--force"));
            std::printf("%s %s\n", ok ? "ok  " : "FAIL", pos[i].c_str());
            failed += !ok;
        }
        return failed ? 2 : 0;
    }
    if (sub == "campath" && pos.size() >= 2) {
        // ui campath <camera path .ose> [time...]: the loading-screen camera as the Mist Menu places it from the path's
        // CameraPathNode: sequence CameraPath activated, the root updated at
        // 0.001 s, then at each given time (seconds of the UI clock since the path started), one JSON line each.
        std::string file = pos[1];
        std::error_code ec;
        if (!std::filesystem::exists(file, ec)) {
            file = (cache_dir() / file).string();
        }
        auto effect = spud::fx::load_effect(spud::read_file(file));
        spud::fx::EffectInstance inst(effect);
        inst.activate_sequence("CameraPath", 1.0f, 0.0f, 0);
        inst.update(0.001f);
        const uint32_t node = inst.find_node("CameraPathNode");
        if (node == spud::ose::k_none) {
            std::puts("no CameraPathNode");
            return 2;
        }
        std::vector<float> times;
        for (size_t i = 2; i < pos.size(); ++i) {
            times.push_back(std::stof(pos[i]));
        }
        if (const auto tf = option(args, "--times")) {
            std::ifstream in(*tf);
            for (float t; in >> t;) {
                times.push_back(t);
            }
        }
        for (float t : times) {
            inst.update(t);
            const auto p = inst.node_world_translation_units(node);
            const auto r = inst.node_world_rotation(node);
            std::printf("{\"t\": %.9g, \"sequence_time\": %.9g, \"node\": [%.9g, %.9g, %.9g], \"camera\": [%.9g, %.9g, "
                        "%.9g], \"node_rotation\": [%.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g]}\n",
                        t, inst.sequence_time("CameraPath"), p.x, p.y, p.z, -50.0f - p.x, 600.0f - p.y, 80.0f + p.z,
                        r[0][0], r[1][0], r[2][0], r[0][1], r[1][1], r[2][1], r[0][2], r[1][2], r[2][2]);
        }
        return 0;
    }
    if (sub == "loading") {
        const auto t = spud::osl::read(spud::read_file(cache_dir() / "interface" / "loadingscreens.osl"));
        const std::string filter = pos.size() >= 2 ? spud::path::to_lower(pos[1]) : std::string();
        size_t shown = 0;
        for (const auto& sc : t.screens) {
            const std::string edid(t.string(sc.edid));
            const std::string id = spud::path::to_lower(std::format("{:08X}", sc.form_id));
            if (!filter.empty() && spud::path::to_lower(edid).find(filter) == std::string::npos && id != filter) {
                continue;
            }
            ++shown;
            std::printf("%08X %-36s flags %08X scale %.3g rot (%d %d %d) limits %d..%d offset (%.3g %.3g %.3g)\n"
                        "         model %s\n         camera %s\n         text \"%s\"\n",
                        sc.form_id, edid.c_str(), sc.record_flags, sc.scale, sc.rotation[0], sc.rotation[1],
                        sc.rotation[2], sc.rotation_min, sc.rotation_max, sc.offset[0], sc.offset[1], sc.offset[2],
                        std::string(t.string(sc.model)).c_str(), std::string(t.string(sc.camera_path)).c_str(),
                        std::string(t.string(sc.text)).c_str());
            for (uint32_t i = 0; i < sc.condition_count; ++i) {
                const auto& c = t.conditions[sc.first_condition + i];
                std::printf("         CTDA fn %u op %u flags %02X run_on %u value %.9g global %08X p1 %08X p2 %08X "
                            "ref %08X %s\n",
                            c.function, c.flags >> 5, c.flags & 0x1F, c.run_on, c.value, c.value_global, c.param1,
                            c.param2, c.reference, std::string(t.string(c.string_param)).c_str());
            }
        }
        std::printf("%zu of %zu screens; %zu locations, %zu worldspace cells, %zu interior cells, %zu quests, %zu "
                    "globals, %zu quest variables, %zu player keywords, %zu dead references\n",
                    shown, t.screens.size(), t.locations.size(), t.world_cells.size(), t.interior_cells.size(),
                    t.quests.size(), t.globals.size(), t.quest_variables.size(), t.player_keywords.size(),
                    t.dead_references.size());
        for (const auto& v : t.quest_variables) {
            std::printf("quest variable %08X %s = %g (source %u)\n", v.quest, std::string(t.string(v.name)).c_str(),
                        v.value, v.source);
        }
        for (const auto& q : t.quests) {
            std::printf("quest %08X %s flags %04X startup stage %d, %u stages\n", q.form_id,
                        std::string(t.string(q.edid)).c_str(), q.flags, q.startup_stage, q.stage_count);
        }
        return 0;
    }
    std::puts("unknown ui command (ui --help)");
    return 1;
}

} // namespace skytool
