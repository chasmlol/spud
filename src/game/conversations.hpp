#pragma once

// What a random scene asks of the actors in it: the two-actor talk of, between Story Manager, Scenes and Ai.

#include <cstdint>

namespace spud::game {

class Conversations {
public:
    virtual ~Conversations() = default;
    virtual void event_processed(uint32_t event, bool started) = 0; // 1.4: the ADIA's answer
    virtual bool can_talk(uint32_t a, uint32_t b) = 0;              // both ways
    virtual uint32_t initiator(uint32_t a, uint32_t b) = 0;         // high, else the second
    virtual void start_talk(uint32_t speaker, uint32_t target) = 0; // vtable slot 216,
    virtual bool talk_ready(uint32_t speaker) = 0;                  // what clears the scene's Hold
    virtual bool talk_broken(uint32_t a, uint32_t b) = 0;           // a partner gone
    virtual void end_talk(uint32_t a, uint32_t b) = 0;              // vtable slot 218,
    virtual float setting(const char* name, float fallback) = 0;
};

} // namespace spud::game
