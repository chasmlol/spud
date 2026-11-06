#include "core/game_random.hpp"
#include "script/natives.hpp"

#include "formats/forms_typed.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <random>

namespace spud::script {

namespace {

constexpr float k_degrees = 180.0f / std::numbers::pi_v<float>;

// 3.2 and 7.2 of: the flags are the game's, whoever implements the native.
struct Flagged {
    const char* script;
    const char* names; // space separated
};

constexpr Flagged k_latent[] = {
    {"Actor", "MoveToPackageLocation PathToReference Resurrect ShowGiftMenu"},
    {"Debug", "CenterOnCellAndWait PlayerMoveToAndWait GetConfigName GetPlatformName GetVersionNumber"},
    {"Faction", "SendPlayerToJail"},
    {"Game", "AddHavokBallAndSocketConstraint RemoveHavokConstraints GetGameSettingString"},
    {"Keyword", "SendStoryEventAndWait"},
    {"MagicEffect", "GetAssociatedSkill"},
    {"Message", "Show"},
    {"ObjectReference",
     "AddItem ApplyHavokImpulse DamageObject Delete Disable DropObject Enable ForceAddRagdollToWorld "
     "ForceRemoveRagdollFromWorld MoveTo MoveToInteractionLocation MoveToMyEditorLocation MoveToNode "
     "PlayAnimationAndWait PlaySyncedAnimationAndWaitSS RemoveItem Reset SetAngle SetMotionType SetPosition SetScale "
     "WaitForAnimationEvent"},
    {"Quest", "SetCurrentStageID Start"},
    {"Scroll", "Cast"},
    {"Sound", "PlayAndWait"},
    {"Spell", "Cast RemoteCast"},
    {"Utility", "GameTimeToString Wait WaitGameTime WaitMenuMode"},
};

constexpr Flagged k_tasklet[] = {
    {"ActiveMagicEffect", "AddInventoryEventFilter RemoveAllInventoryEventFilters RemoveInventoryEventFilter "
                          "StartObjectProfiling StopObjectProfiling"},
    {"Alias", "GetOwningQuest RegisterForAnimationEvent RegisterForLOS RegisterForSingleLOSGain RegisterForSingleLOSLost "
              "RegisterForSingleUpdate RegisterForSingleUpdateGameTime RegisterForSleep RegisterForTrackedStatsEvent "
              "RegisterForUpdate RegisterForUpdateGameTime StartObjectProfiling StopObjectProfiling "
              "UnregisterForAnimationEvent UnregisterForLOS UnregisterForSleep UnregisterForTrackedStatsEvent "
              "UnregisterForUpdate UnregisterForUpdateGameTime"},
    {"Debug", "CloseUserLog DumpAliasData OpenUserLog StartScriptProfiling StartStackProfiling StopScriptProfiling "
              "StopStackProfiling Trace TraceStack TraceUser"},
    {"Form", "StartObjectProfiling StopObjectProfiling"},
    {"Game", "GetForm GetFormFromFile"},
    {"GlobalVariable", "GetValue SetValue"},
    {"LocationAlias", "Clear ForceLocationTo GetLocation"},
    {"Math", "abs acos asin atan Ceiling cos DegreesToRadians Floor pow RadiansToDegrees sin sqrt tan"},
    {"ObjectReference", "AddInventoryEventFilter AddItem Disable DisableNoWait Enable EnableNoWait "
                        "RemoveAllInventoryEventFilters RemoveInventoryEventFilter SetContainerAllowStolenItems"},
    {"ReferenceAlias", "AddInventoryEventFilter Clear ForceRefTo GetReference RemoveAllInventoryEventFilters "
                       "RemoveInventoryEventFilter"},
    {"Scroll", "Cast"},
    {"Spell", "Cast RemoteCast"},
};

bool listed(std::span<const Flagged> table, std::string_view script, std::string_view name) {
    for (const Flagged& f : table) {
        if (_stricmp(f.script, std::string(script).c_str()) != 0) {
            continue;
        }
        std::string_view names = f.names;
        while (!names.empty()) {
            const size_t end = names.find(' ');
            const std::string_view word = names.substr(0, end);
            if (word.size() == name.size() && _strnicmp(word.data(), name.data(), name.size()) == 0) {
                return true;
            }
            names = end == names.npos ? std::string_view{} : names.substr(end + 1);
        }
    }
    return false;
}

uint32_t flags_of(std::string_view script, std::string_view name) {
    return (listed(k_tasklet, script, name) ? native_tasklet : 0u) | (listed(k_latent, script, name) ? native_latent : 0u);
}

const char* class_of_record(forms::Sig type) {
    struct Row {
        const char* sig;
        const char* script;
    };
    static constexpr Row k_rows[] = {
        {"ACHR", "Actor"}, {"REFR", "ObjectReference"}, {"NPC_", "ActorBase"}, {"ACTI", "Activator"}, {"ALCH", "Potion"},
        {"AMMO", "Ammo"}, {"APPA", "Apparatus"}, {"ARMO", "Armor"}, {"ARMA", "ArmorAddon"}, {"ASTP", "AssociationType"},
        {"BOOK", "Book"}, {"CELL", "Cell"}, {"CLAS", "Class"}, {"COBJ", "ConstructibleObject"}, {"CONT", "Container"},
        {"CSTY", "CombatStyle"}, {"DIAL", "Topic"}, {"DOOR", "Door"}, {"EFSH", "EffectShader"}, {"ENCH", "Enchantment"},
        {"EQUP", "EquipSlot"}, {"EXPL", "Explosion"}, {"FACT", "Faction"}, {"FLOR", "Flora"}, {"FLST", "FormList"},
        {"FURN", "Furniture"}, {"GLOB", "GlobalVariable"}, {"HAZD", "Hazard"}, {"HDPT", "HeadPart"}, {"IDLE", "Idle"},
        {"IMAD", "ImageSpaceModifier"}, {"INFO", "TopicInfo"}, {"INGR", "Ingredient"}, {"KEYM", "Key"}, {"KYWD", "Keyword"},
        {"LCRT", "LocationRefType"}, {"LCTN", "Location"}, {"LIGH", "Light"}, {"LVLI", "LeveledItem"}, {"LVLN", "LeveledActor"},
        {"LVSP", "LeveledSpell"}, {"MESG", "Message"}, {"MGEF", "MagicEffect"}, {"MISC", "MiscObject"}, {"MUSC", "MusicType"},
        {"OTFT", "Outfit"}, {"PACK", "Package"}, {"PERK", "Perk"}, {"PROJ", "Projectile"}, {"QUST", "Quest"}, {"RACE", "Race"},
        {"SCEN", "Scene"}, {"SCRL", "Scroll"}, {"SHOU", "Shout"}, {"SLGM", "SoulGem"}, {"SNCT", "SoundCategory"},
        {"SNDR", "SoundDescriptor"}, {"SOUN", "Sound"}, {"SPEL", "Spell"}, {"SPGD", "ShaderParticleGeometry"}, {"STAT", "Static"},
        {"TACT", "TalkingActivator"}, {"TREE", "TreeObject"}, {"VTYP", "VoiceType"}, {"WEAP", "Weapon"}, {"WOOP", "WordOfPower"},
        {"WRLD", "WorldSpace"}, {"WTHR", "Weather"}, {"RFCT", "VisualEffect"}, {"ARTO", "Art"}, {"AACT", "Action"},
        {"CLFM", "ColorForm"}, {"ECZN", "EncounterZone"}, {"IPDS", "ImpactDataSet"}, {"TXST", "TextureSet"},
    };
    for (const Row& r : k_rows) {
        if (forms::parse_sig(r.sig) == type) {
            return r.script;
        }
    }
    return "Form";
}

// The form behind an object argument, 0 for None.
uint32_t form_of(const Value& v) {
    return v.tag == Tag::object && v.object ? handle_form(v.object->handle) : 0;
}

uint32_t form_of(const Object* o) {
    return o ? handle_form(o->handle) : 0;
}

std::string_view str(VirtualMachine& vm, const Value& v) {
    return v.tag == Tag::string ? vm.text(v.string) : std::string_view{};
}

class Binder {
public:
    Binder(VirtualMachine& vm, GameHost& host) : vm(vm), host(host) {}

    void add(std::string_view script, std::string_view name, NativeFunction fn) {
        vm.bind_native(script, name, std::move(fn), flags_of(script, name));
    }

    void math() {
        auto unary = [this](const char* name, float (*fn)(float)) {
            add("Math", name, [fn](NativeCall& c) { c.result = Value::of(fn(c.args[0].real)); });
        };
        unary("abs", [](float x) { return std::fabs(x); });
        unary("sqrt", [](float x) { return std::sqrt(x); });
        unary("sin", [](float x) { return std::sin(x / k_degrees); });
        unary("cos", [](float x) { return std::cos(x / k_degrees); });
        unary("tan", [](float x) { return std::tan(x / k_degrees); });
        unary("asin", [](float x) { return std::asin(x) * k_degrees; });
        unary("acos", [](float x) { return std::acos(x) * k_degrees; });
        unary("atan", [](float x) { return std::atan(x) * k_degrees; });
        unary("DegreesToRadians", [](float x) { return x / k_degrees; });
        unary("RadiansToDegrees", [](float x) { return x * k_degrees; });
        add("Math", "pow", [](NativeCall& c) { c.result = Value::of(std::pow(c.args[0].real, c.args[1].real)); });
        add("Math", "Floor", [](NativeCall& c) { c.result = Value::of(static_cast<int32_t>(std::floor(c.args[0].real))); });
        add("Math", "Ceiling", [](NativeCall& c) { c.result = Value::of(static_cast<int32_t>(std::ceil(c.args[0].real))); });
        // SKSE.
        add("Math", "LeftShift", [](NativeCall& c) { c.result = Value::of(c.args[1].integer >= 32 ? 0 : c.args[0].integer << c.args[1].integer); });
        add("Math", "RightShift", [](NativeCall& c) {
            c.result = Value::of(c.args[1].integer >= 32 ? 0 : static_cast<int32_t>(static_cast<uint32_t>(c.args[0].integer) >> c.args[1].integer));
        });
        add("Math", "LogicalAnd", [](NativeCall& c) { c.result = Value::of(c.args[0].integer & c.args[1].integer); });
        add("Math", "LogicalOr", [](NativeCall& c) { c.result = Value::of(c.args[0].integer | c.args[1].integer); });
        add("Math", "LogicalXor", [](NativeCall& c) { c.result = Value::of(c.args[0].integer ^ c.args[1].integer); });
        add("Math", "LogicalNot", [](NativeCall& c) { c.result = Value::of(~c.args[0].integer); });
        add("Math", "Log", [](NativeCall& c) { c.result = Value::of(std::log(c.args[0].real)); });
    }

    void utility() {
        add("Utility", "RandomInt", [this](NativeCall& c) {
            const int32_t lo = c.args[0].integer, hi = c.args[1].integer;
            c.result = Value::of(lo >= hi ? lo : std::uniform_int_distribution<int32_t>(lo, hi)(m_random));
        });
        add("Utility", "RandomFloat", [this](NativeCall& c) {
            const float lo = c.args[0].real, hi = c.args[1].real;
            c.result = Value::of(lo >= hi ? lo : std::uniform_real_distribution<float>(lo, hi)(m_random));
        });
        add("Utility", "GetCurrentRealTime", [this](NativeCall& c) {
            c.result = Value::of(static_cast<float>(host.real_time ? host.real_time() : 0.0));
        });
        add("Utility", "GetCurrentGameTime", [this](NativeCall& c) { c.result = Value::of(static_cast<float>(host.days_passed)); });
        add("Utility", "IsInMenuMode", [this](NativeCall& c) { c.result = Value::of(host.menu_mode); });
        auto wait = [this](const char* name, VirtualMachine::Clock clock) {
            add("Utility", name, [this, clock](NativeCall& c) {
                const float seconds = c.args[0].real;
                if (seconds <= 0.0f) {
                    c.vm.return_from_latent(c.stack, {});
                    return;
                }
                const auto ms = static_cast<int64_t>(std::min<double>(seconds * 1000.0, 86400000.0));
                c.vm.wait(c.stack, clock, c.vm.clock(clock) + ms);
            });
        };
        wait("Wait", VirtualMachine::Clock::gameplay);
        wait("WaitMenuMode", VirtualMachine::Clock::real);
        add("Utility", "WaitGameTime", [this](NativeCall& c) {
            const float hours = c.args[0].real;
            if (hours <= 0.0f) {
                c.vm.return_from_latent(c.stack, {});
                return;
            }
            c.vm.wait(c.stack, VirtualMachine::Clock::game, static_cast<int64_t>((host.days_passed + hours / 24.0) * 1000.0));
        });
    }

    void debug() {
        add("Debug", "Trace", [this](NativeCall& c) {
            const int32_t severity = c.args[1].integer;
            if (severity >= 3 || severity < 0) {
                c.vm.log_error(c.stack, std::format("Trace error severity is out of range: {}", severity));
            } else {
                host.log(severity, str(c.vm, c.args[0]));
            }
        });
        add("Debug", "TraceStack", [](NativeCall& c) { c.vm.log_error(c.stack, c.vm.text(c.args[0].string), c.args[1].integer); });
        add("Debug", "Notification", [this](NativeCall& c) {
            if (host.on_notification) {
                host.on_notification(str(c.vm, c.args[0]));
            }
        });
        add("Debug", "MessageBox", [this](NativeCall& c) {
            if (host.on_message_box) {
                host.on_message_box(str(c.vm, c.args[0]));
            }
        });
    }

    void registrations(const char* script) {
        add(script, "RegisterForUpdate", [](NativeCall& c) { c.vm.register_update(c.self->handle, c.args[0].real, false); });
        add(script, "RegisterForSingleUpdate", [](NativeCall& c) { c.vm.register_update(c.self->handle, c.args[0].real, true); });
        add(script, "UnregisterForUpdate", [](NativeCall& c) { c.vm.unregister_update(c.self->handle); });
        add(script, "RegisterForUpdateGameTime", [](NativeCall& c) { c.vm.register_game_time_update(c.self->handle, c.args[0].real, false); });
        add(script, "RegisterForSingleUpdateGameTime", [](NativeCall& c) { c.vm.register_game_time_update(c.self->handle, c.args[0].real, true); });
        add(script, "UnregisterForUpdateGameTime", [](NativeCall& c) { c.vm.unregister_game_time_update(c.self->handle); });
    }

    void form() {
        registrations("Form");
        registrations("Alias");
        registrations("ActiveMagicEffect");
        add("Form", "GetFormID", [](NativeCall& c) { c.result = Value::of(static_cast<int32_t>(form_of(c.self))); });
        add("Form", "GetGoldValue", [this](NativeCall& c) {
            const auto* entry = host.forms() ? host.forms()->find(form_of(c.self)) : nullptr;
            int32_t gold = -1;
            if (entry) {
                gold = static_cast<int32_t>(forms::item_value(forms::Form(*host.forms(), *entry)).value_or(-1));
            }
            c.result = Value::of(gold);
        });
        add("Form", "HasKeyword", [this](NativeCall& c) {
            const auto* entry = host.forms() ? host.forms()->find(form_of(c.self)) : nullptr;
            const uint32_t keyword = form_of(c.args[0]);
            bool has = false;
            if (entry && keyword) {
                const auto words = forms::Form(*host.forms(), *entry).keywords();
                has = std::ranges::find(words, keyword) != words.end();
            }
            c.result = Value::of(has);
        });
        add("Form", "GetName", [this](NativeCall& c) {
            const auto* entry = host.forms() ? host.forms()->find(form_of(c.self)) : nullptr;
            c.result = Value::of_string(c.vm.intern(entry ? forms::Form(*host.forms(), *entry).name() : std::string_view{}));
        });
    }

    void game() {
        add("Game", "GetPlayer", [](NativeCall& c) { c.result = c.vm.form_value(k_player_ref, "Actor"); });
        add("Game", "GetForm", [this](NativeCall& c) {
            const auto id = static_cast<uint32_t>(c.args[0].integer);
            const std::string_view type = host.form_class(id);
            c.result = type.empty() ? Value{} : c.vm.form_value(id, "Form");
        });
        add("Game", "GetFormFromFile", [this](NativeCall& c) {
            const forms::Database* db = host.forms();
            if (!db) {
                return;
            }
            const std::string_view file = str(c.vm, c.args[1]);
            const auto plugins = db->plugins();
            for (size_t i = 0; i < plugins.size(); ++i) {
                if (_stricmp(std::string(db->plugin_name(i)).c_str(), std::string(file).c_str()) == 0) {
                    const uint32_t id = (static_cast<uint32_t>(i) << 24) | (static_cast<uint32_t>(c.args[0].integer) & 0xFFFFFF);
                    if (db->find(id)) {
                        c.result = c.vm.form_value(id, "Form");
                    }
                    return;
                }
            }
        });
    }

    void world() {
        // ObjectReference natives on map markers and the game's fast travel.
        add("ObjectReference", "AddToMap", [this](NativeCall& c) {
            if (host.world.add_to_map) {
                host.world.add_to_map(form_of(c.self), c.args[0].integer != 0);
                if (host.on_notification) {
                    host.on_notification("Map updated."); // sMapMarkerAdded
                }
            }
        });
        add("ObjectReference", "IsMapMarkerVisible", [this](NativeCall& c) {
            c.result = Value::of(host.world.marker_flags && (host.world.marker_flags(form_of(c.self)) & 1) != 0);
        });
        add("ObjectReference", "CanFastTravelToMarker", [this](NativeCall& c) {
            c.result = Value::of(host.world.marker_flags && (host.world.marker_flags(form_of(c.self)) & 2) != 0);
        });
        add("ObjectReference", "EnableFastTravel", [this](NativeCall& c) {
            if (host.world.set_marker_travel) {
                host.world.set_marker_travel(form_of(c.self), c.args[0].integer != 0);
            }
        });
        add("Game", "FastTravel", [this](NativeCall& c) {
            const uint32_t to = form_of(c.args[0]);
            if (!to) {
                c.vm.log_error(c.stack, "Cannot fast travel to a None destination");
            } else if (host.world.fast_travel) {
                host.world.fast_travel(to);
            }
        });
        add("Game", "EnableFastTravel", [this](NativeCall& c) {
            if (host.world.set_fast_travel_controls) {
                host.world.set_fast_travel_controls(c.args[0].integer != 0);
            }
        });
        add("Game", "IsFastTravelControlsEnabled", [this](NativeCall& c) {
            c.result = Value::of(host.world.fast_travel_controls && host.world.fast_travel_controls());
        });
        add("Game", "IsFastTravelEnabled", [this](NativeCall& c) {
            c.result = Value::of(host.world.can_fast_travel && host.world.can_fast_travel());
        });
        add("Game", "SetInChargen", [this](NativeCall& c) {
            if (host.world.set_in_chargen) {
                host.world.set_in_chargen(c.args[0].integer != 0);
            }
        });
        add("Game", "RequestAutosave", [this](NativeCall&) {
            if (host.world.request_autosave) {
                host.world.request_autosave();
            }
        });
        add("Game", "RequestSave", [this](NativeCall&) {
            if (host.world.request_save) {
                host.world.request_save();
            }
        });
        add("Cell", "Reset", [this](NativeCall& c) {
            if (host.world.reset_cell) {
                host.world.reset_cell(form_of(c.self));
            }
        });
        add("Location", "IsCleared", [this](NativeCall& c) {
            c.result = Value::of(host.world.location_cleared && host.world.location_cleared(form_of(c.self)));
        });
        add("Location", "SetCleared", [this](NativeCall& c) {
            if (host.world.set_location_cleared) {
                host.world.set_location_cleared(form_of(c.self), c.args[0].integer != 0);
            }
        });
    }

    void lists() {
        add("GlobalVariable", "GetValue", [this](NativeCall& c) {
            const uint32_t id = form_of(c.self);
            float value = 0.0f;
            if (host.read_global && host.read_global(id, value)) {
                c.result = Value::of(value);
                return;
            }
            if (auto it = host.global_values.find(id); it != host.global_values.end()) {
                c.result = Value::of(it->second);
                return;
            }
            if (const auto* entry = host.forms() ? host.forms()->find(id) : nullptr) {
                const forms::Form form(*host.forms(), *entry);
                if (const auto* v = form.get("FLTV")) {
                    value = static_cast<float>(v->kind == forms::Value::Kind::real ? v->real : static_cast<double>(v->integer));
                }
            }
            c.result = Value::of(value);
        });
        add("GlobalVariable", "SetValue", [this](NativeCall& c) {
            if (!host.write_global || !host.write_global(form_of(c.self), c.args[0].real)) {
                host.global_values[form_of(c.self)] = c.args[0].real;
            }
        });
        add("FormList", "GetSize", [this](NativeCall& c) { c.result = Value::of(static_cast<int32_t>(list_members(form_of(c.self)).size())); });
        add("FormList", "GetAt", [this](NativeCall& c) {
            const auto members = list_members(form_of(c.self));
            const int32_t i = c.args[0].integer;
            if (i >= 0 && static_cast<size_t>(i) < members.size()) {
                c.result = c.vm.form_value(members[static_cast<size_t>(i)], "Form");
            }
        });
        add("FormList", "Find", [this](NativeCall& c) {
            const auto members = list_members(form_of(c.self));
            const auto it = std::ranges::find(members, form_of(c.args[0]));
            c.result = Value::of(it == members.end() ? -1 : static_cast<int32_t>(it - members.begin()));
        });
        add("FormList", "HasForm", [this](NativeCall& c) {
            const auto members = list_members(form_of(c.self));
            c.result = Value::of(std::ranges::find(members, form_of(c.args[0])) != members.end());
        });
        add("FormList", "AddForm", [this](NativeCall& c) { host.added_to_lists[form_of(c.self)].push_back(form_of(c.args[0])); });
        add("FormList", "RemoveAddedForm", [this](NativeCall& c) { std::erase(host.added_to_lists[form_of(c.self)], form_of(c.args[0])); });
        add("FormList", "Revert", [this](NativeCall& c) { host.added_to_lists.erase(form_of(c.self)); });
    }

    std::vector<uint32_t> list_members(uint32_t list) {
        std::vector<uint32_t> out;
        if (const auto* entry = host.forms() ? host.forms()->find(list) : nullptr) {
            const forms::Form form(*host.forms(), *entry);
            for (const auto* v : form.all("LNAM")) {
                out.push_back(v->form);
            }
        }
        if (auto it = host.added_to_lists.find(list); it != host.added_to_lists.end()) {
            out.insert(out.end(), it->second.begin(), it->second.end());
        }
        return out;
    }

    const forms::Value* game_setting(std::string_view name) {
        if (m_settings.empty() && host.forms()) {
            for (const forms::FormEntry* entry : host.forms()->of_type(forms::sig("GMST"))) {
                const forms::Form form(*host.forms(), *entry);
                if (const auto* v = form.get("DATA")) {
                    m_settings.emplace(lower_text(form.editor_id()), *v);
                }
            }
        }
        auto it = m_settings.find(lower_text(name));
        return it == m_settings.end() ? nullptr : &it->second;
    }

    static std::string lower_text(std::string_view s) {
        std::string r(s);
        std::ranges::transform(r, r.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return r;
    }

    void settings() {
        add("Game", "GetGameSettingFloat", [this](NativeCall& c) {
            const auto* v = game_setting(str(c.vm, c.args[0]));
            c.result = Value::of(v ? static_cast<float>(v->kind == forms::Value::Kind::real ? v->real : static_cast<double>(v->integer)) : 0.0f);
        });
        add("Game", "GetGameSettingInt", [this](NativeCall& c) {
            const auto* v = game_setting(str(c.vm, c.args[0]));
            c.result = Value::of(v ? static_cast<int32_t>(v->kind == forms::Value::Kind::real ? v->real : static_cast<double>(v->integer)) : 0);
        });
        add("Game", "GetGameSettingString", [this](NativeCall& c) { // latent (3.2)
            const auto* v = game_setting(str(c.vm, c.args[0]));
            c.vm.return_from_latent(c.stack, Value::of_string(c.vm.intern(v ? std::string_view(v->text) : std::string_view{})));
        });
    }

    void strings() {
        add("StringUtil", "GetLength", [](NativeCall& c) { c.result = Value::of(static_cast<int32_t>(str(c.vm, c.args[0]).size())); });
        add("StringUtil", "Find", [](NativeCall& c) {
            const std::string haystack(str(c.vm, c.args[0]));
            const std::string needle(str(c.vm, c.args[1]));
            auto same = [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); };
            const size_t start = static_cast<size_t>(std::max(c.args[2].integer, 0));
            if (start > haystack.size()) {
                c.result = Value::of(-1);
                return;
            }
            const auto it = std::search(haystack.begin() + static_cast<ptrdiff_t>(start), haystack.end(), needle.begin(), needle.end(), same);
            c.result = Value::of(it == haystack.end() ? -1 : static_cast<int32_t>(it - haystack.begin()));
        });
        add("StringUtil", "Substring", [](NativeCall& c) {
            const std::string_view text = str(c.vm, c.args[0]);
            const size_t start = static_cast<size_t>(std::max(c.args[1].integer, 0));
            const int32_t length = c.args[2].integer;
            if (start >= text.size()) {
                c.result = Value::of_string(0);
                return;
            }
            c.result = Value::of_string(c.vm.intern(text.substr(start, length <= 0 ? text.npos : static_cast<size_t>(length))));
        });
    }

    // Everything left in the load order's scripts: warn once, return the default.
    void stand_ins() {
        const oss::Scripts& scripts = vm.scripts();
        for (const oss::Script& s : scripts.scripts) {
            for (uint32_t i = 0; i < s.state_count; ++i) {
                const oss::State& st = scripts.states[s.first_state + i];
                for (uint32_t f = 0; f < st.function_count; ++f) {
                    const oss::Function& fn = scripts.functions[st.first_function + f];
                    if (fn.flags & oss::function_native) {
                        declare(scripts.string(s.name), scripts.string(fn.name));
                    }
                }
            }
        }
    }

    VirtualMachine& vm;
    GameHost& host;

private:
    void declare(std::string_view script, std::string_view name) {
        if (vm.has_native(script, name)) {
            return;
        }
        const uint32_t flags = flags_of(script, name);
        std::string full = std::format("{}.{}", script, name);
        vm.bind_native(script, name, [this, full, flags, warned = false](NativeCall& c) mutable {
            if (!warned) {
                warned = true;
                host.log(1, std::format("warning: native {} waits for a game system", full));
            }
            if (flags & native_latent) {
                c.started = false;
            }
        }, flags);
        vm.mark_stub(script, name, std::move(full));
    }

    std::mt19937 m_random{session_seed()};
    std::unordered_map<std::string, forms::Value> m_settings;
};

} // namespace

void GameHost::log(int severity, std::string_view text) {
    if (on_log) {
        on_log(severity, text);
    }
}

std::string_view GameHost::form_class(uint32_t form_id) {
    if (form_id == k_player_ref) {
        return "Actor"; // no record places the player: the game creates PlayerRef
    }
    if (form_id >= 0xFF000000) {
        return runtime_actor && runtime_actor(form_id) ? "Actor" : "ObjectReference"; // made at run time
    }
    if (!m_forms) {
        return {};
    }
    const auto* entry = m_forms->find(form_id);
    return entry ? class_of_record(entry->type) : std::string_view{};
}

uint32_t GameHost::reference_base(uint32_t form_id) {
    const auto* entry = m_forms ? m_forms->find(form_id) : nullptr;
    const auto name = entry ? m_forms->field(*entry, forms::parse_sig("NAME").value()) : std::nullopt;
    if (name && name->data.size() >= 4) {
        return name->as<uint32_t>();
    }
    return created_base ? created_base(form_id) : 0; // made at run time: no record
}

std::string_view GameHost::handle_class(Handle handle) {
    switch (handle_kind(handle)) {
    case k_handle_ref_alias: return "ReferenceAlias";
    case k_handle_location_alias: return "LocationAlias";
    case k_handle_active_effect: return "ActiveMagicEffect";
    case k_handle_inventory: return "Form";
    default: return form_class(handle_form(handle));
    }
}

std::string GameHost::handle_text(Handle handle) {
    if (handle == k_no_handle) {
        return "None";
    }
    return std::format(" ({:08X})", handle_form(handle));
}

uint32_t native_flags(std::string_view script, std::string_view name) {
    return flags_of(script, name);
}

void bind_natives(VirtualMachine& vm, GameHost& host) {
    static std::vector<std::unique_ptr<Binder>> keep;
    auto binder = std::make_unique<Binder>(vm, host);
    binder->math();
    binder->utility();
    binder->debug();
    binder->form();
    binder->game();
    binder->lists();
    binder->world();
    binder->strings();
    binder->settings();
    binder->stand_ins();
    keep.push_back(std::move(binder));
}

} // namespace spud::script
