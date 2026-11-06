#pragma once

// A sampling profiler for the main thread: with SPUD_SAMPLE=<file>, a second thread stops it about a thousand times a
// second, walks its stack and writes the return addresses to <file> when the process ends.
// turns that into the functions the time went to. Off (and free) otherwise.

#include <cstdint>

namespace spud::sampler {

// Call on the thread to sample. Does nothing unless SPUD_SAMPLE is set.
void start();
// Writes what was sampled; also runs at exit.
void stop();
// A crash leaves a dump in the local app data folder, spud/crash, so a rare one can be read after the fact.
void catch_crashes();
void catch_terminate(); // for this thread
// A number stored with each sample: which part of the run it came from (a tour's section).
void set_tag(uint32_t tag);

} // namespace spud::sampler
