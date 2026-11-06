#include "commands.hpp"

#include "core/log.hpp"
#include "formats/forms_typed.hpp"
#include "game/dialogue.hpp"
#include "game/quests.hpp"
#include "importer/convert/script_import.hpp"
#include "importer/legacy/data_source.hpp"
#include "importer/legacy/esm.hpp"
#include "script/natives.hpp"
#include "script/vm.hpp"

#include "core/file.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace skytool {

namespace {

int script_import(Args args) {
    const auto out = args.size() > 1 ? std::filesystem::path(args[1]) : cache_dir() / "scripts" / "scripts.oss";
    const auto install = open_install();
    spud::legacy::EsmDatabase db(install);
    spud::legacy::DataSource data(install);
    auto result = spud::importer::import_scripts(db, data);
    for (const auto& w : result.warnings) {
        spud::log::warn("{}", w);
    }
    std::filesystem::create_directories(out.parent_path());
    const auto bytes = spud::oss::write(result.scripts);
    std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::ssize(bytes));
    std::printf("%zu scripts, %zu functions, %zu instructions, %zu attachments, %zu warnings, %.1f MB\n",
                result.scripts.scripts.size(), result.scripts.functions.size(), result.scripts.instructions.size(),
                result.scripts.attachments.size(), result.warnings.size(), bytes.size() / 1e6);
    return 0;
}

// Runs one global function in the VM at 60 frames a second, printing what it traces. The check against the real
// game runs the same.pex there.
int script_run(Args args) {
    const auto positional = positionals(args, {"--add", "--frames", "--save-at"});
    if (positional.size() < 2) {
        std::fputs("usage: skytool script run Script.Function [--add X.pex] [--frames N]\n", stderr);
        return 2;
    }
    const std::string& target = positional[1];
    const size_t dot = target.find('.');
    const auto install = open_install();
    spud::legacy::DataSource data(install);
    spud::importer::ScriptCompiler compiler;
    for (const std::string& path : data.list("scripts/", ".pex")) {
        if (const auto bytes = data.read(path)) {
            compiler.add(*bytes);
        }
    }
    if (const auto extra = option(args, "--add")) {
        std::ifstream file(*extra, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(file)), {});
        compiler.add(std::as_bytes(std::span(text)));
    }
    const spud::oss::Scripts scripts = compiler.finish();
    const auto forms_path = cache_dir() / "forms.osf";
    std::unique_ptr<spud::forms::Database> forms;
    if (std::filesystem::exists(forms_path)) {
        forms = std::make_unique<spud::forms::Database>(forms_path);
    }
    spud::script::GameHost host(forms.get());
    uint64_t frame = 0;
    host.on_log = [](int severity, std::string_view text) { std::printf("[%d] %.*s\n", severity, static_cast<int>(text.size()), text.data()); };
    host.real_time = [&frame] { return static_cast<double>(frame * 1000 / 60) * 0.001; };
    auto vm = std::make_unique<spud::script::VirtualMachine>(scripts, host);
    spud::script::bind_natives(*vm, host);
    bool finished = false;
    vm->call_static(target.substr(0, dot), target.substr(dot + 1), {}, [&](const spud::script::Value& v) {
        std::printf("returned %s at frame %llu\n", vm->to_string(v).c_str(), static_cast<unsigned long long>(frame));
        finished = true;
    });
    // --save-at N: at frame N, save and go on in a new VM, as a load would.
    const auto save_option = option(args, "--save-at");
    const uint64_t save_at = save_option ? std::stoull(*save_option) : UINT64_MAX;
    const auto frames_option = option(args, "--frames");
    const uint64_t frames = frames_option ? std::stoull(*frames_option) : 3600;
    uint32_t last_ms = 0;
    for (; frame < frames && !finished; ++frame) {
        const auto now_ms = static_cast<uint32_t>(frame * 1000 / 60);
        if (frame == save_at) {
            const auto saved = vm->save();
            std::printf("saved %zu bytes at frame %llu\n", saved.size(), static_cast<unsigned long long>(frame));
            vm = std::make_unique<spud::script::VirtualMachine>(scripts, host);
            spud::script::bind_natives(*vm, host);
            vm->load(saved);
        }
        vm->update(now_ms - last_ms);
        last_ms = now_ms;
        vm->dispatch();
        vm->dispatch();
    }
    std::printf("%llu frames, %zu stacks running, %zu suspended\n", static_cast<unsigned long long>(frame), vm->running_stacks(), vm->suspended_stacks());
    return frame < frames || finished ? 0 : 1;
}

// A new game's quests after N frames, one per line compares this with the real game.
int script_quests(Args args) {
    const auto positional = positionals(args, {"--frames", "--add", "--call", "--talk"});
    const auto frames_option = option(args, "--frames");
    const int frames = frames_option ? std::stoi(*frames_option) : 300;
    spud::oss::Scripts scripts;
    if (const auto extra = option(args, "--add")) {
        const auto install = open_install();
        spud::legacy::EsmDatabase db(install);
        spud::legacy::DataSource data(install);
        scripts = spud::importer::import_scripts(db, data, spud::read_file(*extra)).scripts;
    } else {
        scripts = spud::oss::read(spud::read_file(cache_dir() / "scripts" / "scripts.oss"));
    }
    spud::forms::Database forms(cache_dir() / "forms.osf");
    spud::script::GameHost host(&forms);
    std::unordered_map<std::string, int> warnings;
    const auto call = option(args, "--call");
    host.on_log = [&warnings, &call](int severity, std::string_view text) {
        if (severity >= 1) {
            ++warnings[std::string(text.substr(0, text.find('\n')))];
        } else if (call) {
            std::printf("[0] %.*s\n", static_cast<int>(text.size()), text.data());
        }
    };
    uint64_t frame = 0;
    host.real_time = [&frame] { return static_cast<double>(frame * 1000 / 60) * 0.001; };
    spud::script::VirtualMachine vm(scripts, host);
    spud::script::bind_natives(vm, host);
    spud::game::WorldState world;
    spud::game::Conditions conditions(forms, world);
    spud::game::QuestWorld quest_world(forms);
    spud::game::QuestSystem quests(forms, scripts, vm, conditions, quest_world);
    // What a conversation shows, in text.
    struct TextOutput final : spud::game::DialogueOutput {
        void open_menu(std::string_view name) override { std::printf("menu: %.*s\n", static_cast<int>(name.size()), name.data()); }
        void show_text(std::string_view text) override { std::printf("says: %.*s\n", static_cast<int>(text.size()), text.data()); }
        void show_topics(const std::vector<Row>& rows, bool) override {
            for (const Row& r : rows) {
                std::printf("topic %d%s: %s\n", r.index, r.is_new ? " (new)" : "", r.text.c_str());
            }
        }
        void close_menu() override { std::puts("menu closed"); }
    } text_output;
    spud::game::Dialogue dialogue(quests, text_output);
    quests.new_game();
    // As the game's frame: the teller's job, then the VM's; a second dispatch after render.
    uint32_t last_ms = 0;
    for (; static_cast<int>(frame) < frames; ++frame) {
        if (const auto talk = option(args, "--talk"); talk && frame == 200) {
            const auto form = static_cast<uint32_t>(std::stoul(*talk, nullptr, 16));
            const uint32_t ref = quest_world.ref_of_unique_npc(form);
            if (has_flag(args, "--explain")) {
                for (const std::string& line : dialogue.explain(ref ? ref : form)) {
                    std::printf("%s\n", line.c_str());
                }
            }
            dialogue.start_conversation(ref ? ref : form);
        }
        if (call && frame == 120) {
            const size_t dot = call->find('.');
            vm.call_static(call->substr(0, dot), call->substr(dot + 1));
        }
        const auto now_ms = static_cast<uint32_t>(frame * 1000 / 60);
        quests.update(1.0f / 60.0f, 0.0, false);
        dialogue.update(1.0f / 60.0f, 0.0);
        vm.update(now_ms - last_ms);
        last_ms = now_ms;
        vm.dispatch();
        vm.dispatch();
    }
    const auto out_path = positional.size() > 1 ? positional[1] : std::string("quests.csv");
    FILE* out = std::fopen(out_path.c_str(), "w");
    std::fputs("form,flags,stage,editor_id\n", out);
    for (const spud::forms::FormEntry* e : forms.of_type(spud::forms::sig("QUST"))) {
        const auto* q = quests.quest(e->form_id);
        std::fprintf(out, "%08X,%u,%u,%s\n", e->form_id, q->flags, q->current_stage, q->editor_id.c_str());
    }
    std::fclose(out);
    for (const spud::forms::FormEntry* e : forms.of_type(spud::forms::sig("QUST"))) {
        if (const auto* q = quests.quest(e->form_id); !q->fill_failure.empty()) {
            std::printf("fill failed: %s: %s\n", q->editor_id.c_str(), q->fill_failure.c_str());
        }
    }
    std::vector<std::pair<int, std::string>> sorted;
    for (const auto& [text, n] : warnings) {
        sorted.emplace_back(n, text);
    }
    std::ranges::sort(sorted, std::greater<>{});
    for (size_t i = 0; i < sorted.size() && i < 25; ++i) {
        std::printf("%6d %s\n", sorted[i].first, sorted[i].second.substr(0, 110).c_str());
    }
    std::printf("%zu running, %zu stacks, %zu suspended\n", quests.running().size(), vm.running_stacks(), vm.suspended_stacks());
    return 0;
}

} // namespace

int cmd_script(Args args) {
    if (!args.empty() && args[0] == "import") {
        return script_import(args);
    }
    if (!args.empty() && args[0] == "run") {
        return script_run(args);
    }
    if (!args.empty() && args[0] == "quests") {
        return script_quests(args);
    }
    std::fputs("usage: skytool script import [OUT.oss] | run Script.Function [--add X.pex] [--frames N]\n", stderr);
    return 2;
}

} // namespace skytool
