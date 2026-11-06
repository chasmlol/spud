#include "formats/osb.hpp"

#include <cstring>

namespace spud::osb {

namespace {

constexpr auto k_info = container::fourcc("INFO");
constexpr auto k_objs = container::fourcc("OBJS");
constexpr auto k_vals = container::fourcc("VALS");
constexpr auto k_ints = container::fourcc("INTS");
constexpr auto k_flts = container::fourcc("FLTS");
constexpr auto k_byts = container::fourcc("BYTS");
constexpr auto k_strs = container::fourcc("STRS");

struct Info {
    uint32_t root;
};

struct ObjectRecord {
    uint32_t cls;
    uint32_t first_value;
    uint32_t value_count;
};

// A value, its name for an object's member. `a` and `b` by kind: integer the low and high words, real the bits, text
// the string, object the index, list the first value and count, ints, floats and bytes the first element and count.
struct ValueRecord {
    uint32_t name;
    uint32_t kind;
    uint32_t a;
    uint32_t b;
};
static_assert(sizeof(ValueRecord) == 16);

struct Writer {
    container::StringTableBuilder strings;
    std::vector<ValueRecord> values;
    std::vector<int32_t> ints;
    std::vector<float> floats;
    std::vector<uint8_t> bytes;

    // Writes `v`'s record into values[at], with what it holds appended after.
    void put(size_t at, uint32_t name, const Value& v) {
        ValueRecord r{name, static_cast<uint32_t>(v.kind), 0, 0};
        switch (v.kind) {
        case Kind::none: break;
        case Kind::integer:
            r.a = static_cast<uint32_t>(static_cast<uint64_t>(v.integer));
            r.b = static_cast<uint32_t>(static_cast<uint64_t>(v.integer) >> 32);
            break;
        case Kind::real: std::memcpy(&r.a, &v.real, 4); break;
        case Kind::text: r.a = strings.add(v.text); break;
        case Kind::object: r.a = v.object; break;
        case Kind::list: {
            r.a = static_cast<uint32_t>(values.size());
            r.b = static_cast<uint32_t>(v.list.size());
            values.resize(values.size() + v.list.size());
            for (size_t i = 0; i < v.list.size(); ++i) {
                put(r.a + i, container::k_no_string, v.list[i]);
            }
            break;
        }
        case Kind::ints:
            r.a = static_cast<uint32_t>(ints.size());
            r.b = static_cast<uint32_t>(v.ints.size());
            ints.insert(ints.end(), v.ints.begin(), v.ints.end());
            break;
        case Kind::floats:
            r.a = static_cast<uint32_t>(floats.size());
            r.b = static_cast<uint32_t>(v.floats.size());
            floats.insert(floats.end(), v.floats.begin(), v.floats.end());
            break;
        case Kind::bytes:
            r.a = static_cast<uint32_t>(bytes.size());
            r.b = static_cast<uint32_t>(v.bytes.size());
            bytes.insert(bytes.end(), v.bytes.begin(), v.bytes.end());
            break;
        }
        values[at] = r;
    }
};

} // namespace

std::vector<std::byte> write(const Graph& graph) {
    Writer w;
    std::vector<ObjectRecord> objects;
    objects.reserve(graph.objects.size());
    for (const auto& o : graph.objects) {
        ObjectRecord rec{w.strings.add(o.cls), static_cast<uint32_t>(w.values.size()),
                         static_cast<uint32_t>(o.members.size())};
        w.values.resize(w.values.size() + o.members.size());
        for (size_t i = 0; i < o.members.size(); ++i) {
            w.put(rec.first_value + i, w.strings.add(o.members[i].first), o.members[i].second);
        }
        objects.push_back(rec);
    }
    container::Writer out(k_kind, k_version);
    out.add_struct(k_info, Info{graph.root});
    out.add_array(k_objs, objects);
    out.add_array(k_vals, w.values);
    out.add_array(k_ints, w.ints);
    out.add_array(k_flts, w.floats);
    out.add_array(k_byts, w.bytes);
    out.add(k_strs, w.strings.bytes());
    return out.finish();
}

Graph read(std::span<const std::byte> file) {
    container::Reader r(file, k_kind);
    if (r.version() != k_version) {
        throw ParseError("unsupported .osb version " + std::to_string(r.version()));
    }
    container::StringTable strings(r.require(k_strs));
    const auto values = r.read_array<ValueRecord>(k_vals);
    const auto ints = r.read_array<int32_t>(k_ints);
    const auto floats = r.read_array<float>(k_flts);
    const auto bytes = r.read_array<uint8_t>(k_byts);

    auto span_ok = [](uint32_t a, uint32_t b, size_t size) {
        if (size_t{a} + b > size) {
            throw ParseError("behaviour value out of range");
        }
    };
    auto take = [&](const ValueRecord& rec, auto&& self) -> Value {
        Value v;
        v.kind = static_cast<Kind>(rec.kind);
        switch (v.kind) {
        case Kind::none: break;
        case Kind::integer: v.integer = static_cast<int64_t>(uint64_t(rec.a) | uint64_t(rec.b) << 32); break;
        case Kind::real: std::memcpy(&v.real, &rec.a, 4); break;
        case Kind::text: v.text = std::string(strings.get(rec.a)); break;
        case Kind::object: v.object = rec.a; break;
        case Kind::list:
            span_ok(rec.a, rec.b, values.size());
            v.list.reserve(rec.b);
            for (uint32_t i = 0; i < rec.b; ++i) {
                v.list.push_back(self(values[rec.a + i], self));
            }
            break;
        case Kind::ints:
            span_ok(rec.a, rec.b, ints.size());
            v.ints.assign(ints.begin() + rec.a, ints.begin() + rec.a + rec.b);
            break;
        case Kind::floats:
            span_ok(rec.a, rec.b, floats.size());
            v.floats.assign(floats.begin() + rec.a, floats.begin() + rec.a + rec.b);
            break;
        case Kind::bytes:
            span_ok(rec.a, rec.b, bytes.size());
            v.bytes.assign(bytes.begin() + rec.a, bytes.begin() + rec.a + rec.b);
            break;
        default: throw ParseError("unknown behaviour value kind");
        }
        return v;
    };

    Graph g;
    g.root = r.read_struct<Info>(k_info).root;
    const auto objects = r.read_array<ObjectRecord>(k_objs);
    g.objects.reserve(objects.size());
    for (const auto& rec : objects) {
        span_ok(rec.first_value, rec.value_count, values.size());
        Object o;
        o.cls = std::string(strings.get(rec.cls));
        o.members.reserve(rec.value_count);
        for (uint32_t i = 0; i < rec.value_count; ++i) {
            const auto& v = values[rec.first_value + i];
            o.members.emplace_back(std::string(strings.get(v.name)), take(v, take));
        }
        g.objects.push_back(std::move(o));
    }
    return g;
}

} // namespace spud::osb
