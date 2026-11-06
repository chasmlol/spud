#include "engine/face_tint.hpp"

#include "core/error.hpp"
#include "core/file.hpp"
#include "core/log.hpp"
#include "formats/dds.hpp"

#include <algorithm>
#include <cstring>
#include <glm/glm.hpp>

namespace spud::engine {

namespace {

constexpr int k_tint_slots = 16;

// The mask's red channel, a byte a texel, at the top mip. Tint masks are 24-bit files, which dds::parse widens; one ships
// as BC3, whose colour block holds it.
std::vector<uint8_t> mask_of(const dds::Image& image, uint32_t& width, uint32_t& height) {
    const dds::Subresource& top = image.subresource(0, 0);
    width = top.width;
    height = top.height;
    const auto* data = reinterpret_cast<const uint8_t*>(image.data.data()) + top.offset;
    std::vector<uint8_t> out(size_t(width) * height);
    switch (image.format) {
    case dds::Format::rgba8:
        for (size_t i = 0; i < out.size(); ++i) {
            out[i] = data[i * 4];
        }
        break;
    case dds::Format::bgra8:
    case dds::Format::bgrx8:
        for (size_t i = 0; i < out.size(); ++i) {
            out[i] = data[i * 4 + 2];
        }
        break;
    case dds::Format::r8:
    case dds::Format::l8:
        std::copy_n(data, out.size(), out.begin());
        break;
    case dds::Format::bc3:
        for (uint32_t by = 0; by < height / 4; ++by) {
            for (uint32_t bx = 0; bx < width / 4; ++bx) {
                const uint8_t* b = data + (size_t(by) * (width / 4) + bx) * 16 + 8;
                const int r0 = (b[1] >> 3) * 255 / 31, r1 = (b[3] >> 3) * 255 / 31;
                const int red[4] = {r0, r1, (2 * r0 + r1) / 3, (r0 + 2 * r1) / 3};
                for (int t = 0; t < 16; ++t) {
                    const int code = (b[4 + t / 4] >> (2 * (t % 4))) & 3;
                    out[size_t(by * 4 + t / 4) * width + bx * 4 + t % 4] = static_cast<uint8_t>(red[code]);
                }
            }
        }
        break;
    default:
        throw Error("a tint mask of an unexpected format");
    }
    return out;
}

void put32(std::byte* at, uint32_t value) { std::memcpy(at, &value, 4); }

std::vector<std::byte> rgba_dds(const std::vector<glm::vec3>& pixels, uint32_t width, uint32_t height) {
    constexpr size_t k_header = 128;
    std::vector<std::byte> out(k_header + pixels.size() * 4);
    std::memcpy(out.data(), "DDS ", 4);
    put32(out.data() + 4, 124);
    put32(out.data() + 8, 0x100F); // caps, height, width, pitch, pixel format
    put32(out.data() + 12, height);
    put32(out.data() + 16, width);
    put32(out.data() + 20, width * 4);
    put32(out.data() + 76, 32);
    put32(out.data() + 80, 0x41); // RGB with alpha
    put32(out.data() + 88, 32);
    put32(out.data() + 92, 0x000000FF);
    put32(out.data() + 96, 0x0000FF00);
    put32(out.data() + 100, 0x00FF0000);
    put32(out.data() + 104, 0xFF000000);
    put32(out.data() + 108, 0x1000); // texture
    auto* texel = reinterpret_cast<uint8_t*>(out.data() + k_header);
    for (const glm::vec3& c : pixels) {
        const glm::vec3 byte = glm::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f;
        *texel++ = static_cast<uint8_t>(byte.r);
        *texel++ = static_cast<uint8_t>(byte.g);
        *texel++ = static_cast<uint8_t>(byte.b);
        *texel++ = 255;
    }
    return out;
}

} // namespace

std::vector<std::byte> plain_face_tint(const glm::vec3& colour) { return rgba_dds(std::vector<glm::vec3>(16, colour), 4, 4); }

std::vector<std::byte> compose_face_tint(const std::filesystem::path& cache, std::span<const game::TintLayer> layers) {
    std::vector<glm::vec3> pixels;
    uint32_t width = 0, height = 0;
    int slots = 0;
    for (const game::TintLayer& layer : layers) {
        if (layer.alpha <= 0.0f || layer.mask.empty() || slots == k_tint_slots) {
            continue;
        }
        std::vector<uint8_t> mask;
        uint32_t w = 0, h = 0;
        try {
            std::string file = layer.mask;
            std::ranges::transform(file, file.begin(), [](unsigned char c) { return c == '\\' ? '/' : char(std::tolower(c)); });
            mask = mask_of(dds::parse(read_file(cache / "textures" / file)), w, h);
        } catch (const std::exception& e) {
            log::warn("face tint: {}: {}", layer.mask, e.what());
            continue;
        }
        if (pixels.empty()) {
            width = w;
            height = h;
            pixels.assign(size_t(w) * h, glm::vec3(0.5f)); // mid grey under the first layer only
        }
        if (w != width || h != height) {
            log::warn("face tint: {} isn't the size of the others", layer.mask);
            continue;
        }
        const glm::vec3 colour(float(layer.rgba & 0xFF) / 255.0f, float(layer.rgba >> 8 & 0xFF) / 255.0f,
                               float(layer.rgba >> 16 & 0xFF) / 255.0f);
        for (size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = glm::mix(pixels[i], colour, float(mask[i]) / 255.0f * layer.alpha);
        }
        ++slots;
    }
    return pixels.empty() ? std::vector<std::byte>{} : rgba_dds(pixels, width, height);
}

} // namespace spud::engine
