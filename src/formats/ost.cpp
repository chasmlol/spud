#include "formats/ost.hpp"

#include <cstring>

namespace spud::ost {

namespace {

constexpr auto k_head = container::fourcc("HEAD");
constexpr auto k_text = container::fourcc("TEXT");
constexpr auto k_plug = container::fourcc("PLUG");
constexpr auto k_thmb = container::fourcc("THMB");
constexpr auto k_list = container::fourcc("SECT");

} // namespace

const std::vector<std::byte>* Save::section(container::FourCC id) const {
    for (const auto& [section_id, bytes] : sections) {
        if (section_id == id) {
            return &bytes;
        }
    }
    return nullptr;
}

void put(BinaryWriter& w, std::string_view s) {
    w.write<uint32_t>(uint32_t(s.size()));
    w.write_string(s);
}

std::string get_string(BinaryReader& r) {
    const auto n = r.read<uint32_t>();
    const auto bytes = r.read_array<char>(n);
    return std::string(bytes.begin(), bytes.end());
}

std::vector<std::byte> write(const Save& save) {
    container::Writer out(k_kind, k_version);
    out.add(k_head, std::as_bytes(std::span(&save.header, 1)));
    BinaryWriter text;
    put(text, save.player_name);
    put(text, save.location);
    put(text, save.race);
    put(text, save.space);
    out.add(k_text, text.buffer());
    BinaryWriter plugins;
    plugins.write<uint32_t>(uint32_t(save.plugins.size()));
    for (const Plugin& p : save.plugins) {
        put(plugins, p.name);
        plugins.write<uint64_t>(p.hash);
    }
    out.add(k_plug, plugins.buffer());
    if (!save.thumbnail.empty()) {
        out.add(k_thmb, std::as_bytes(std::span(save.thumbnail)));
    }
    std::vector<uint32_t> ids;
    for (const auto& [id, bytes] : save.sections) {
        out.add(id, bytes);
        ids.push_back(id);
    }
    out.add_array(k_list, ids);
    return out.finish();
}

Save read(std::span<const std::byte> file, bool sections) {
    container::Reader r(file, k_kind);
    if (r.version() != k_version) {
        throw ParseError("unsupported .ost version " + std::to_string(r.version()));
    }
    Save s;
    const auto head = r.require(k_head);
    if (head.size() != sizeof(Header)) {
        throw ParseError(".ost header size");
    }
    std::memcpy(&s.header, head.data(), sizeof(Header));
    BinaryReader text(r.require(k_text));
    s.player_name = get_string(text);
    s.location = get_string(text);
    s.race = get_string(text);
    s.space = get_string(text);
    BinaryReader plugins(r.require(k_plug));
    const auto count = plugins.read<uint32_t>();
    for (uint32_t i = 0; i < count; ++i) {
        Plugin p;
        p.name = get_string(plugins);
        p.hash = plugins.read<uint64_t>();
        s.plugins.push_back(std::move(p));
    }
    if (const auto thumb = r.find(k_thmb)) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(thumb->data());
        s.thumbnail.assign(bytes, bytes + thumb->size());
    }
    if (sections) {
        for (const uint32_t id : r.read_array<uint32_t>(k_list)) {
            const auto bytes = r.require(id);
            s.sections.emplace_back(id, std::vector<std::byte>(bytes.begin(), bytes.end()));
        }
    }
    return s;
}

} // namespace spud::ost
