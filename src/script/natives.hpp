#pragma once

// The native function library: what the game's Register functions bind, for the classes that need nothing beyond the
// forms database and the VM. lists every native and what a missing one is waiting for.

#include "formats/forms.hpp"
#include "script/vm.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spud::script {

inline constexpr uint32_t k_player_ref = 0x14;

// Host over the forms database. The game runs through the callbacks; state with no system of its own yet (global
// values, form lists scripts added to) lives here.
class GameHost : public Host {
public:
    explicit GameHost(const forms::Database* forms) : m_forms(forms) {}

    std::function<void(int, std::string_view)> on_log;
    std::function<void(std::string_view)> on_notification;
    std::function<void(std::string_view)> on_message_box;
    std::function<double()> real_time; // seconds, the frame timer's clock
    std::function<bool(uint32_t form)> runtime_actor; // a reference made at run time that is an actor
    // The world simulation, once one is attached.
    struct WorldCalls {
        std::function<void(uint32_t, bool)> add_to_map;
        std::function<uint8_t(uint32_t)> marker_flags;
        std::function<void(uint32_t, bool)> set_marker_travel;
        std::function<void(uint32_t)> fast_travel;
        std::function<void(bool)> set_fast_travel_controls;
        std::function<bool()> fast_travel_controls;
        std::function<bool()> can_fast_travel;
        std::function<void(uint32_t)> reset_cell;
        std::function<bool(uint32_t)> location_cleared;
        std::function<void(uint32_t, bool)> set_location_cleared;
        std::function<void(bool)> set_in_chargen; // abDisableSaving
        std::function<void()> request_autosave;
        std::function<void()> request_save;
    };
    WorldCalls world;
    // Globals a game system owns (the calendar's): false when it isn't one of them.
    std::function<bool(uint32_t, float&)> read_global;
    std::function<bool(uint32_t, float)> write_global;
    bool menu_mode = false;
    double days_passed = 0.0;

    void log(int severity, std::string_view text) override;
    std::string_view form_class(uint32_t form_id) override;
    std::string_view handle_class(Handle handle) override;
    uint32_t reference_base(uint32_t form_id) override;
    std::function<uint32_t(uint32_t ref)> created_base; // what a reference made at run time was made from
    std::string handle_text(Handle handle) override;
    bool in_menu_mode() override { return menu_mode; }
    double game_days_passed() override { return days_passed; }

    const forms::Database* forms() const { return m_forms; }
    std::unordered_map<uint32_t, float> global_values;
    std::unordered_map<uint32_t, std::vector<uint32_t>> added_to_lists;

private:
    const forms::Database* m_forms;
};

// Binds every native we have, then a stand-in for each one left: it warns once and returns the type's default, and a
// latent one returns at once. Call it before any script runs.
void bind_natives(VirtualMachine& vm, GameHost& host);

// The game's flags for a native (latent, callable from tasklets), for systems that bind their own.
uint32_t native_flags(std::string_view script, std::string_view name);

} // namespace spud::script
