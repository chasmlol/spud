// skytool conditions: condition functions answered from the records, for comparing with the game's own

#include "commands.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "game/conditions.hpp"

#include <charconv>
#include <cstdio>
#include <format>
#include <fstream>
#include <sstream>

namespace skytool {

int cmd_conditions(Args args) {
    const auto pos = positionals(args);
    if (pos.size() < 2) {
        std::puts("conditions <in.csv> <out.csv> [hour weather [worldspace]]\n"
                  "  in: function,subject,param1,param2 (hex form IDs and integers); out: the same, then the value");
        return 1;
    }
    spud::forms::Database db(cache_dir() / "forms.osf");
    spud::game::WorldState world;
    if (pos.size() >= 4) {
        world.game_hour = std::stof(pos[2]);
        world.weather = uint32_t(std::stoul(pos[3], nullptr, 16));
        if (const auto* w = db.find(world.weather)) {
            const spud::forms::Form f(db, *w);
            const auto* data = f.get("DATA");
            const auto* speed = data ? data->member("wind_speed") : nullptr;
            world.wind_speed = speed ? float(speed->integer) / 255.0f : 0.0f;
        }
    }
    if (pos.size() >= 5) {
        world.player_worldspace = uint32_t(std::stoul(pos[4], nullptr, 16));
    }
    spud::game::Conditions conditions(db, world);
    std::ifstream in(pos[0]);
    std::ofstream out(pos[1]);
    std::string line;
    size_t rows = 0;
    while (std::getline(in, line)) {
        uint32_t f = 0, subject = 0, p1 = 0, p2 = 0;
        if (std::sscanf(line.c_str(), "%u,%x,%x,%x", &f, &subject, &p1, &p2) != 4) {
            continue;
        }
        bool known = false;
        const double v = conditions.value(uint16_t(f), subject, 0, p1, p2, &known);
        out << std::format("{},{:08X},{:08X},{:08X},{},{}\n", f, subject, p1, p2, v, known ? 1 : 0);
        ++rows;
    }
    spud::log::info("conditions: {} rows", rows);
    return 0;
}

} // namespace skytool
