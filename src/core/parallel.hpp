#pragma once

// A loop spread over a few threads for work inside a frame: animation, and the like. Separate from the streaming pool,
// whose jobs can run for milliseconds. The caller works too, and the call returns when every index is done.

#include <cstddef>
#include <functional>

namespace spud::parallel {

// Calls fn(i) for each i in [0, n), in any order, on any of the pool's threads. One caller at a time.
void for_each(size_t n, const std::function<void(size_t)>& fn);
// The same loop in the background: returns at once, and wait() is when it is done. for_each waits for it first.
void start(size_t n, std::function<void(size_t)> fn);
void wait();

} // namespace spud::parallel
