#include "ui/player/json.hpp"

#include "ui/player/vm.hpp"

namespace spud::ui::player {

Value json_to_value(Vm& vm, const json::Value& v) {
    switch (v.kind) {
    case json::Value::null: return Value::null();
    case json::Value::undefined: return Value{};
    case json::Value::boolean: return Value::boolean(v.b);
    case json::Value::number: return Value::number(v.n);
    case json::Value::string: return Value::string(v.s);
    case json::Value::array: {
        std::vector<Value> items;
        items.reserve(v.items.size());
        for (const auto& i : v.items) {
            items.push_back(json_to_value(vm, i));
        }
        return Value::object(vm.make_array(std::move(items)));
    }
    case json::Value::object: {
        Object* o = vm.make_object();
        for (const auto& [name, m] : v.members) {
            vm.set(Value::object(o), name, json_to_value(vm, m));
        }
        return Value::object(o);
    }
    }
    return {};
}

} // namespace spud::ui::player
