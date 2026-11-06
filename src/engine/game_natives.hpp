#pragma once

namespace spud::game {
class Game;
}
namespace spud::script {
class VirtualMachine;
}
namespace spud::ui {
class Ui;
}

#include <array>
#include <cstdint>
#include <functional>
#include <glm/vec3.hpp>
#include <string>
#include <string_view>

namespace spud::engine {

// TranslateTo and SplineTranslateTo: position in Skyrim units, angle in degrees, speed in units a second, rotation in
// degrees a second, 0 for no limit.
struct Translation {
    uint32_t ref = 0;
    std::array<float, 3> position{}, angle{};
    float speed = 0.0f, max_rotation = 0.0f;
    float start_distance = 0.0f;
    bool almost_sent = false;
};

// What ObjectReference natives ask of the world.
struct ReferenceHost {
    std::function<void(uint32_t actor, uint16_t subtype)> say_favor; // a line of the Favors category, to the player
    std::function<std::array<float, 3>()> player_position; // Skyrim units
    std::function<uint32_t()> player_cell;
    std::function<bool(uint32_t ref)> loaded;
    std::function<void(uint32_t ref, uint32_t activator, bool default_only)> activate;
    std::function<bool()> in_interior;
    std::function<void(uint32_t ref, const std::string& event, bool value)> bool_event;
    std::function<float(uint32_t ref, const std::string& variable)> graph_float;
    // References within a radius of a point, nearest first: of those bases, or actors.
    std::function<std::vector<uint32_t>(const std::array<float, 3>& at, float radius, std::vector<uint32_t> bases, bool actors)> refs_near;
    std::function<bool()> using_gamepad;
    std::function<void(bool driven)> player_ai_driven;
    std::function<void(uint32_t weather, bool accelerate)> set_weather;
    std::function<void(uint32_t music, bool add)> music;
    std::function<bool(uint32_t location)> location_loaded;
    std::function<uint32_t(uint32_t sound)> play_form_sound; // the instance
    std::function<uint32_t(int which)> weather; // 0 current, 1 outgoing
    std::function<float()> weather_transition;
    std::function<uint32_t()> current_location;
    std::function<void(const std::array<float, 3>&)> move_player;
    std::function<void(uint64_t handle, bool on)> register_sleep;
    std::function<uint32_t(uint32_t base, const std::array<float, 3>& position)> place; // Skyrim units; the new reference
    std::function<void(uint32_t ref)> remove;
    std::function<void(uint32_t ref)> moved;
    std::function<void(uint64_t handle, uint32_t form, int what)> inventory_filter; // 1 add, 0 remove, -1 clear
    std::function<void(uint32_t ref, const std::array<float, 3>& direction, float magnitude)> havok_impulse;
    std::function<bool(uint32_t ref)> kill;
    std::function<bool(uint32_t ref)> is_dead;
    std::function<bool(uint32_t ref)> weapon_drawn;
    std::function<bool(uint32_t ref)> resurrect; // false: dead from the editor
    std::function<void(uint32_t ref)> redress;   // its outfit changed
    std::function<void(uint32_t ref, uint32_t form, bool on)> npc_equip; // a script puts it on an NPC, or takes it off
    std::function<void(uint32_t ref)> npc_bare;  // ... takes off everything
    std::function<bool(uint32_t ref, std::string_view event)> animation_event;
    // A stack in a latent native, woken with true when the reference's graph sends the event.
    std::function<void(uint32_t ref, std::string_view event, uint32_t stack)> wait_animation;
    // 0 played, 1 no 3D, 2 no controller manager, 3 no such sequence.
    std::function<int(uint32_t ref, std::string_view name, bool start_over, float ease_in)> play_gamebryo;
    std::function<bool(uint32_t ref, std::string_view idle)> play_idle;
    std::function<void(uint32_t actor, uint32_t target)> look_at; // 0: stops
    std::function<bool(uint32_t ref, std::string_view name, float value)> graph_variable;
    std::function<void(uint64_t handle, uint32_t sender, std::string_view event, bool on)> register_animation;
    std::function<void(const std::string& edid, float strength, float fade_in)> apply_modifier;
    std::function<void(const std::string& edid, float fade_out)> remove_modifier;
    std::function<void(uint32_t id)> stop_sound;
    std::function<void(uint32_t source, float strength, float duration)> shake_camera; // source 0: the player
    std::function<void(uint32_t ref)> critical_end;   // a goo or disintegrate end: its 3D goes
    std::function<void(uint64_t handle, uint32_t viewer, uint32_t target, int type)> register_los; // 0 gain, 1 lost, 2 both
    std::function<void(uint64_t handle, uint32_t viewer, uint32_t target)> unregister_los;
    std::function<void(const std::string& event, const std::string& text, float duration, float interval, int times)>
        help_message;
    std::function<void(const std::string& event)> help_reset;
    std::function<void(uint32_t trainer)> open_training;
    std::function<void(uint32_t ref, bool giving, uint32_t filter, bool stolen, std::function<void(int)> done)> open_gift;
    std::function<void(uint32_t id, float volume)> sound_volume;
    std::function<void(uint32_t cart, uint32_t horse)> tether;
    std::function<void(uint32_t cart, bool held)> hold_cart;
    std::function<void(uint32_t rider, uint32_t vehicle)> set_vehicle;
    std::function<void(float degrees)> sitting_rotation;
    std::function<void(uint32_t door, bool open)> script_door;
    std::function<void(const char* mode, bool on)> hud_mode;
    std::function<void(const std::string& sequence, bool show)> title_sequence;
    std::function<void(bool show)> first_person_geometry;
    std::function<void(uint32_t ref, uint32_t target, const std::array<float, 3>& at)> move_agent; // an actor's AI goes too
    std::function<void(uint32_t target, const std::array<float, 3>& at, bool match_rotation, uint32_t stack)> move_player_to;
    std::function<void(uint32_t spgd, float fade, bool on)> particles;
    std::function<bool(uint32_t cell)> cell_attached;
    std::function<float(uint32_t ref)> mass;
    std::function<uint32_t(int type)> find_weather;
    std::function<int()> sky_mode;
    std::function<void()> quit_to_menu;
    std::function<void(uint32_t weather)> force_weather;
    std::function<void()> release_weather;
    std::function<void(bool first_person)> force_view;
    std::function<void(bool fade_out, bool black, float wait, float seconds)> fade;
    std::function<void(uint32_t npc)> open_barter;
    std::function<void(uint32_t ref)> open_inventory;
    std::function<int32_t(uint32_t ref)> trigger_count;
    std::function<void(const Translation&)> translate;
    std::function<void(uint32_t ref)> stop_translation;
    std::function<std::array<float, 3>(uint32_t ref, std::string_view node)> node_offset; // Skyrim units from the ref
    std::function<void(uint32_t ref, float amount)> damage_object;
    std::function<void(uint32_t target, uint32_t trap, float damage, float pushback, const glm::vec3& velocity, const glm::vec3& position)>
        trap_hit;
    std::function<void(uint32_t ref, int index, float strength)> expression_override; // index < 0: cleared
    std::function<void(uint32_t ref)> clear_destruction;
    std::function<int32_t(uint32_t ref)> destruction_stage;
};

// The natives that read and change the game's state. The game must outlive the VM's use of them.
void bind_game_natives(script::VirtualMachine& vm, game::Game& game);
// ... and the character's (character_natives.cpp).
void bind_character_natives(script::VirtualMachine& vm, game::Game& game, ui::Ui& ui);
// ... and references' (reference_natives.cpp).
void bind_misc_natives(script::VirtualMachine& vm, game::Game& g, const ReferenceHost& host);
void bind_reference_natives(script::VirtualMachine& vm, game::Game& game, ReferenceHost host);
// ... and actors' and factions' (actor_natives.cpp).
void bind_actor_natives(script::VirtualMachine& vm, game::Game& game, const ReferenceHost& host);
// ... and what the world answers (presentation_natives.cpp).
void bind_presentation_natives(script::VirtualMachine& vm, game::Game& game, const ReferenceHost& host);

} // namespace spud::engine
