#pragma once

#include <chrono>

namespace spud::engine {

// Entry point of spud.exe (argument parsing, window, main loop).
int run(int argc, char** argv, std::chrono::steady_clock::time_point process_start);

} // namespace spud::engine
