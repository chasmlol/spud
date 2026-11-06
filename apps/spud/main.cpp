#include "engine/app.hpp"

#include <SDL3/SDL_main.h>

int main(int argc, char** argv) {
    const auto start = std::chrono::steady_clock::now();
    return spud::engine::run(argc, argv, start);
}
