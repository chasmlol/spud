// The save and load calls the Journal and the Main Menu share: the movie's
// SaveLoadPanel holds the list and asks every question itself; the engine fills the list, answers for the selected save
// and acts.

#include "ui/menu.hpp"
#include "ui/player/player.hpp"
#include "ui/ui.hpp"

#include <memory>

namespace spud::ui {

namespace {

constexpr const char* k_screenshot_image = "BGSSaveLoadHeader_Screenshot";

} // namespace

void Menu::handle_save_load() {
    using Value = player::Value;
    SaveSource* source = m_ui.host().ui_saves();
    // The movie's list while it is up: SAVE and LOAD pass it, and the filler writes into it.
    auto kept = std::make_shared<Value>();
    auto write_list = [this, source, kept] {
        auto& vm = player()->vm;
        const Value list = *kept;
        size_t count = 0;
        if (source && list.is_object()) {
            list.o->elements.clear();
            for (const SaveListEntry& e : source->save_list()) {
                const Value entry = Value::object(vm.make_object());
                vm.set(entry, "fileNum", Value::string(e.file_num));
                vm.set(entry, "text", Value::string(e.text));
                list.o->elements.push_back(entry);
                ++count;
            }
        }
        send("onSaveLoadBatchComplete", {Value::boolean(true), Value::number(double(count)), Value::number(double(count))});
    };
    // The list is the menu's update message's work, not the call's: the movie reaches its list state after SAVE returns.
    auto fill = [this, kept, write_list](Call& c) {
        *kept = c.arg(0);
        m_next_advance = write_list;
    };
    handle("SAVE", fill);
    handle("LOAD", fill);
    handle("PrepSaveGameScreenshot", [this, source](Call& c) {
        auto& vm = player()->vm;
        const int index = int(vm.to_number(c.arg(0)));
        const Value entry = c.arg(1);
        if (!source || !entry.is_object() || index < 0) {
            return;
        }
        const SaveDetails d = source->save_details(size_t(index));
        vm.set(entry, "corrupt", Value::boolean(d.corrupt));
        vm.set(entry, "obsolete", Value::boolean(d.obsolete));
        vm.set(entry, "name", Value::string(d.name));
        vm.set(entry, "raceName", Value::string(d.race));
        vm.set(entry, "level", Value::number(d.level));
        vm.set(entry, "playTime", Value::string(d.play_time));
        vm.set(entry, "dateString", Value::string(d.date));
        if (!d.image.empty()) {
            m_ui.set_image_pixels(k_screenshot_image, d.key, d.width, d.height, d.image);
        }
        send("ScreenshotReady");
    });
    handle("SaveGame", [this, source](Call& c) {
        if (source) {
            source->save_over(int(player()->vm.to_number(c.arg(0))));
        }
    });
    handle("LoadGame", [this, source](Call& c) {
        if (source) {
            source->load_save(size_t(player()->vm.to_number(c.arg(0))));
        }
    });
    // The movie's own question follows ("Load this game? All unsaved progress will be lost.").
    handle("IsOKtoLoad", [this](Call&) { send("ConfirmOKToLoad"); });
    handle("DeleteSave", [this, source](Call& c) {
        if (source) {
            source->delete_save(size_t(player()->vm.to_number(c.arg(0))));
        }
    });
    handle("QuickSave", [source](Call&) {
        if (source) {
            source->quick_save();
        }
    });
    handle("PopulateCharacterList", [this, source](Call& c) {
        auto& vm = player()->vm;
        const Value list = c.arg(0);
        if (source && list.is_object()) {
            list.o->elements.clear();
            for (const SaveCharacter& ch : source->save_characters()) {
                const Value entry = Value::object(vm.make_object());
                vm.set(entry, "text", Value::string(ch.text));
                vm.set(entry, "id", Value::number(ch.id));
                vm.set(entry, "flags", Value::number(0));
                list.o->elements.push_back(entry);
            }
        }
        send("onFillCharacterListComplete");
    });
    handle("CharacterSelected", [this, source, write_list](Call& c) {
        if (source) {
            source->select_character(uint32_t(player()->vm.to_number(c.arg(0))));
            m_next_advance = write_list; // the filter is set and the list made again
        }
    });
    for (const char* name : {"ORBISDeleteSave", "ForceStopSaveListLoading"}) {
        handle(name, [](Call&) {});
    }
    m_ui.library().images.add_render_target(k_screenshot_image);
}

} // namespace spud::ui
