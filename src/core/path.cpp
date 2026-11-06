#include "core/path.hpp"

#include <cctype>

namespace spud::path {

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string normalize(std::string_view p) {
    std::string out;
    out.reserve(p.size());
    for (char c : p) {
        if (c == '\\') {
            c = '/';
        }
        if (c == '/' && (out.empty() || out.back() == '/')) {
            continue;
        }
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\0')) {
        out.pop_back();
    }
    // Some shipped meshes keep the authoring machine's absolute path
    // ("skyrimhd\build\pc\data\textures\..."); resolve from the last data folder like the game does.
    if (auto pos = out.rfind("/data/"); pos != std::string::npos) {
        out.erase(0, pos + 6);
    } else if (out.starts_with("data/")) {
        out.erase(0, 5);
    }
    return out;
}

std::string with_root(std::string_view normalized, std::string_view root) {
    std::string prefix = std::string(root) + "/";
    if (normalized.starts_with(prefix)) {
        return std::string(normalized);
    }
    return prefix + std::string(normalized);
}

std::string replace_extension(std::string_view p, std::string_view extension) {
    auto slash = p.rfind('/');
    auto dot = p.rfind('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
        return std::string(p) + std::string(extension);
    }
    return std::string(p.substr(0, dot)) + std::string(extension);
}

} // namespace spud::path
