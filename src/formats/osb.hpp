#pragma once

// .osb: spud behaviour data. Spec: docs/formats/osb.md
//
// A project, character or behaviour graph as an object graph: objects of named classes whose members are named values.
// The class and member names are those of. The animation runtime builds its nodes
// from them when it loads a graph.

#include "formats/container.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spud::osb {

inline constexpr container::FourCC k_kind = container::fourcc("BEHV");
inline constexpr uint32_t k_version = 1;
inline constexpr uint32_t k_no_object = ~0u;

enum class Kind : uint8_t {
    none,    // a null pointer or a member with no value
    integer, // integer, bool, enum, flags
    real,
    text,
    object,  // index into Graph::objects
    list,    // arrays of objects, strings and structs: `list`
    ints,    // arrays of integers: `ints`
    floats,  // arrays of reals, and vectors, quaternions and transforms as their floats: `floats`
    bytes,   // arrays of bytes: `bytes`
};

struct Value {
    Kind kind = Kind::none;
    int64_t integer = 0;
    float real = 0.0f;
    uint32_t object = k_no_object;
    std::string text;
    std::vector<Value> list;
    std::vector<int32_t> ints;
    std::vector<float> floats;
    std::vector<uint8_t> bytes;
};

struct Object {
    std::string cls;
    std::vector<std::pair<std::string, Value>> members;

    const Value* get(std::string_view name) const {
        for (const auto& [n, v] : members) {
            if (n == name) {
                return &v;
            }
        }
        return nullptr;
    }
};

struct Graph {
    std::vector<Object> objects;
    uint32_t root = k_no_object;
};

std::vector<std::byte> write(const Graph& graph);
Graph read(std::span<const std::byte> file);

} // namespace spud::osb
