#include "formats/ose.hpp"

#include <cstring>
#include <format>

namespace spud::ose {

namespace {

constexpr auto k_info = container::fourcc("INFO");
constexpr auto k_node = container::fourcc("NODE");
constexpr auto k_syst = container::fourcc("SYST");
constexpr auto k_mods = container::fourcc("MODS");
constexpr auto k_flts = container::fourcc("FLTS");
constexpr auto k_subt = container::fourcc("SUBT");
constexpr auto k_emsh = container::fourcc("EMSH");
constexpr auto k_empo = container::fourcc("EMPO");
constexpr auto k_emno = container::fourcc("EMNO");
constexpr auto k_emtr = container::fourcc("EMTR");
constexpr auto k_ctlr = container::fourcc("CTLR");
constexpr auto k_intp = container::fourcc("INTP");
constexpr auto k_keyg = container::fourcc("KEYG");
constexpr auto k_fkey = container::fourcc("FKEY");
constexpr auto k_bkey = container::fourcc("BKEY");
constexpr auto k_seqs = container::fourcc("SEQS");
constexpr auto k_sblk = container::fourcc("SBLK");
constexpr auto k_tkey = container::fourcc("TKEY");
constexpr auto k_matl = container::fourcc("MATL");
constexpr auto k_strs = container::fourcc("STRS");
constexpr auto k_tint = container::fourcc("TINT");
constexpr auto k_vkey = container::fourcc("VKEY");
constexpr auto k_qkey = container::fourcc("QKEY");
constexpr auto k_ctgt = container::fourcc("CTGT");
constexpr auto k_nodx = container::fourcc("NODX");
constexpr auto k_subn = container::fourcc("SUBN");
constexpr auto k_invm = container::fourcc("INVM");
constexpr auto k_atch = container::fourcc("ATCH");
constexpr auto k_addn = container::fourcc("ADDN");

struct Info {
    uint32_t root;
    uint32_t node_count;
    uint32_t system_count;
    uint32_t modifier_count;
    uint32_t controller_count;
    uint32_t interpolator_count;
    uint32_t sequence_count;
    uint32_t material_count;
    uint32_t emitter_mesh_count;
    uint32_t reserved[7];
};
static_assert(sizeof(Info) == 64);

template <typename T> void check_count(const std::vector<T>& v, uint32_t expected, const char* what) {
    if (v.size() != expected) {
        throw ParseError(std::format(".ose {} count {} does not match INFO ({})", what, v.size(), expected));
    }
}

void check_index(uint32_t index, size_t size, const char* what) {
    if (index != k_none && index >= size) {
        throw ParseError(std::format(".ose {} index {} out of range ({})", what, index, size));
    }
}

void check_range(uint32_t first, uint32_t count, size_t size, const char* what) {
    if (uint64_t{first} + count > size) {
        throw ParseError(std::format(".ose {} range {}+{} out of range ({})", what, first, count, size));
    }
}

// Every index a record holds must be in range, so the simulation can trust the data.
void validate(const Effect& e) {
    check_index(e.root, e.nodes.size(), "root");
    for (const auto& n : e.nodes) {
        check_index(n.parent, e.nodes.size(), "node parent");
        e.string(n.name);
    }
    for (const auto& s : e.systems) {
        check_index(s.node, e.nodes.size(), "system node");
        check_index(s.material, e.materials.size(), "system material");
        check_range(s.first_modifier, s.modifier_count, e.modifiers.size(), "system modifiers");
        check_range(s.first_subtexture, s.subtexture_count, e.subtextures.size(), "system subtextures");
    }
    for (const auto& m : e.modifiers) {
        check_index(m.system, e.systems.size(), "modifier system");
        e.string(m.name);
        switch (m.type) {
        case ModifierType::age_death:
            check_index(m.ref, e.modifiers.size(), "age death spawn modifier");
            break;
        case ModifierType::mesh_emitter:
            check_range(m.first, m.count, e.emitter_meshes.size(), "emitter meshes");
            break;
        case ModifierType::scale:
            check_range(m.first, m.count, e.floats.size(), "scale curve");
            break;
        case ModifierType::gravity:
        case ModifierType::drag:
            check_index(m.ref, e.nodes.size(), "modifier node");
            break;
        default:
            break;
        }
    }
    for (const auto& m : e.emitter_meshes) {
        check_index(m.node, e.nodes.size(), "emitter mesh node");
        check_range(m.first_vertex, m.vertex_count, e.mesh_positions.size(), "emitter mesh vertices");
        check_range(m.first_triangle, m.triangle_count, e.mesh_triangles.size(), "emitter mesh triangles");
        for (uint32_t t = 0; t < m.triangle_count; ++t) {
            for (auto v : e.mesh_triangles[m.first_triangle + t].v) {
                if (v >= m.vertex_count) {
                    throw ParseError(std::format(".ose emitter mesh triangle vertex {} out of range", v));
                }
            }
        }
    }
    if (e.mesh_normals.size() != e.mesh_positions.size()) {
        throw ParseError(".ose emitter mesh normals and positions differ in count");
    }
    for (const auto& c : e.controllers) {
        check_index(c.interpolator, e.interpolators.size(), "controller interpolator");
        check_index(c.interpolator2, e.interpolators.size(), "controller interpolator");
        check_index(c.modifier, e.modifiers.size(), "controller modifier");
        e.string(c.name);
        if (c.type == ControllerType::transform) {
            check_range(c.first_target, c.target_count, e.controller_targets.size(), "controller targets");
        }
        switch (c.target_kind) {
        case TargetKind::system:
            check_index(c.target, e.systems.size(), "controller target");
            break;
        case TargetKind::material:
            check_index(c.target, e.materials.size(), "controller target");
            break;
        case TargetKind::node:
            check_index(c.target, e.nodes.size(), "controller target");
            break;
        default:
            break;
        }
    }
    for (const auto& i : e.interpolators) {
        check_index(i.key_group, i.type == InterpolatorType::transform ? e.transforms.size() : e.key_groups.size(),
                    "interpolator keys");
    }
    for (const auto& t : e.transforms) {
        if (t.rotation_type == 4) {
            check_range(t.rotation_first, 3, e.key_groups.size(), "transform rotation keys");
        } else {
            check_range(t.rotation_first, t.rotation_count, e.quat_keys.size(), "transform rotation keys");
        }
        check_range(t.translation_first, t.translation_count, e.vec3_keys.size(), "transform translation keys");
        check_index(t.scale_group, e.key_groups.size(), "transform scale keys");
    }
    for (auto n : e.controller_targets) {
        check_index(n, e.nodes.size(), "controller target");
    }
    if (!e.node_extras.empty() && e.node_extras.size() != e.nodes.size()) {
        throw ParseError(std::format(".ose node extras count {} does not match the nodes ({})", e.node_extras.size(),
                                     e.nodes.size()));
    }
    for (auto n : e.submesh_nodes) {
        check_index(n, e.nodes.size(), "submesh node");
    }
    if (e.inv_marker.size() > 1) {
        throw ParseError(".ose holds more than one inventory marker");
    }
    for (auto s : e.attach_techniques) {
        e.string(s);
    }
    for (const auto& g : e.key_groups) {
        check_range(g.first, g.count,
                    g.kind == KeyKind::float_keys ? e.float_keys.size() : e.bool_keys.size(), "keys");
    }
    for (const auto& s : e.sequences) {
        e.string(s.name);
        check_range(s.first_block, s.block_count, e.blocks.size(), "sequence blocks");
        check_range(s.first_text_key, s.text_key_count, e.text_keys.size(), "sequence text keys");
    }
    for (const auto& b : e.blocks) {
        check_index(b.interpolator, e.interpolators.size(), "block interpolator");
        check_index(b.controller, e.controllers.size(), "block controller");
    }
    for (const auto& t : e.text_keys) {
        e.string(t.text);
    }
    for (const auto& m : e.materials) {
        check_index(m.node, e.nodes.size(), "material node");
        e.string(m.source_texture);
        e.string(m.greyscale_texture);
    }
}

} // namespace

uint32_t Effect::add_string(std::string_view s) {
    std::string key(s);
    if (auto it = m_string_index.find(key); it != m_string_index.end()) {
        return it->second;
    }
    auto offset = static_cast<uint32_t>(m_strings.size());
    m_strings.insert(m_strings.end(), s.begin(), s.end());
    m_strings.push_back('\0');
    m_string_index.emplace(std::move(key), offset);
    return offset;
}

std::string_view Effect::string(uint32_t offset) const {
    return container::StringTable(std::as_bytes(std::span<const char>(m_strings))).get(offset);
}

void Effect::set_string_data(std::vector<char> data) {
    m_strings = std::move(data);
    m_string_index.clear();
}

std::vector<std::byte> write(const Effect& e) {
    validate(e);
    Info info{};
    info.root = e.root;
    info.node_count = static_cast<uint32_t>(e.nodes.size());
    info.system_count = static_cast<uint32_t>(e.systems.size());
    info.modifier_count = static_cast<uint32_t>(e.modifiers.size());
    info.controller_count = static_cast<uint32_t>(e.controllers.size());
    info.interpolator_count = static_cast<uint32_t>(e.interpolators.size());
    info.sequence_count = static_cast<uint32_t>(e.sequences.size());
    info.material_count = static_cast<uint32_t>(e.materials.size());
    info.emitter_mesh_count = static_cast<uint32_t>(e.emitter_meshes.size());

    container::Writer w(k_kind, k_version);
    w.add_struct(k_info, info);
    w.add_array(k_node, e.nodes);
    w.add_array(k_syst, e.systems);
    w.add_array(k_mods, e.modifiers);
    w.add_array(k_flts, e.floats);
    w.add_array(k_subt, e.subtextures);
    w.add_array(k_emsh, e.emitter_meshes);
    w.add_array(k_empo, e.mesh_positions);
    w.add_array(k_emno, e.mesh_normals);
    w.add_array(k_emtr, e.mesh_triangles);
    w.add_array(k_ctlr, e.controllers);
    w.add_array(k_intp, e.interpolators);
    w.add_array(k_keyg, e.key_groups);
    w.add_array(k_fkey, e.float_keys);
    w.add_array(k_bkey, e.bool_keys);
    w.add_array(k_seqs, e.sequences);
    w.add_array(k_sblk, e.blocks);
    w.add_array(k_tkey, e.text_keys);
    w.add_array(k_matl, e.materials);
    w.add_array(k_tint, e.transforms);
    w.add_array(k_vkey, e.vec3_keys);
    w.add_array(k_qkey, e.quat_keys);
    w.add_array(k_ctgt, e.controller_targets);
    w.add_array(k_nodx, e.node_extras);
    w.add_array(k_subn, e.submesh_nodes);
    w.add_array(k_invm, e.inv_marker);
    w.add_array(k_atch, e.attach_techniques);
    if (!e.addon_nodes.empty()) {
        w.add_array(k_addn, e.addon_nodes);
    }
    w.add(k_strs, std::as_bytes(std::span<const char>(e.string_data())));
    return w.finish();
}

Effect read(std::span<const std::byte> file) {
    container::Reader r(file, k_kind);
    if (r.version() < k_min_version || r.version() > k_version) {
        throw ParseError("unsupported .ose version " + std::to_string(r.version()));
    }
    auto info = r.read_struct<Info>(k_info);
    Effect e;
    e.root = info.root;
    e.nodes = r.read_array<Node>(k_node);
    e.systems = r.read_array<System>(k_syst);
    e.modifiers = r.read_array<Modifier>(k_mods);
    e.floats = r.read_array<float>(k_flts);
    e.subtextures = r.read_array<std::array<float, 4>>(k_subt);
    e.emitter_meshes = r.read_array<EmitterMesh>(k_emsh);
    e.mesh_positions = r.read_array<Vec3>(k_empo);
    e.mesh_normals = r.read_array<Vec3>(k_emno);
    e.mesh_triangles = r.read_array<Triangle>(k_emtr);
    e.controllers = r.read_array<Controller>(k_ctlr);
    e.interpolators = r.read_array<Interpolator>(k_intp);
    e.key_groups = r.read_array<KeyGroup>(k_keyg);
    e.float_keys = r.read_array<FloatKey>(k_fkey);
    e.bool_keys = r.read_array<BoolKey>(k_bkey);
    e.sequences = r.read_array<Sequence>(k_seqs);
    e.blocks = r.read_array<Block>(k_sblk);
    e.text_keys = r.read_array<TextKey>(k_tkey);
    e.materials = r.read_array<EffectMaterial>(k_matl);
    if (r.version() >= 2) {
        e.transforms = r.read_array<TransformInterp>(k_tint);
        e.vec3_keys = r.read_array<Vec3Key>(k_vkey);
        e.quat_keys = r.read_array<QuatKey>(k_qkey);
        e.controller_targets = r.read_array<uint32_t>(k_ctgt);
    } else {
        // Version 1 kept these words of every controller reserved.
        for (auto& c : e.controllers) {
            c.first_target = c.target_count = 0;
        }
    }
    if (r.version() >= 3) {
        e.node_extras = r.read_array<NodeExtra>(k_nodx);
        e.submesh_nodes = r.read_array<uint32_t>(k_subn);
        e.inv_marker = r.read_array<InvMarker>(k_invm);
    }
    if (r.version() >= 5) {
        e.attach_techniques = r.read_array<uint32_t>(k_atch);
    }
    if (r.find(k_addn)) {
        e.addon_nodes = r.read_array<AddonNode>(k_addn);
    }
    auto strings = r.require(k_strs);
    std::vector<char> data(strings.size());
    std::memcpy(data.data(), strings.data(), strings.size());
    e.set_string_data(std::move(data));

    check_count(e.nodes, info.node_count, "node");
    check_count(e.systems, info.system_count, "system");
    check_count(e.modifiers, info.modifier_count, "modifier");
    check_count(e.controllers, info.controller_count, "controller");
    check_count(e.interpolators, info.interpolator_count, "interpolator");
    check_count(e.sequences, info.sequence_count, "sequence");
    check_count(e.materials, info.material_count, "material");
    check_count(e.emitter_meshes, info.emitter_mesh_count, "emitter mesh");
    validate(e);
    return e;
}

} // namespace spud::ose
