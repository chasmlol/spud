#pragma once

// What vm.cpp and vm_save.cpp share.

#include "script/vm.hpp"

#include <deque>
#include <unordered_map>
#include <vector>

namespace spud::script {

enum class CallKind : uint8_t { method, parent, global, property_get, property_set };

struct Request {
    CallKind kind = CallKind::method;
    Value self;
    const Class* search = nullptr;
    StringId name = 0;
    std::vector<Value> args;
};

struct Frame {
    const Function* function = nullptr;
    Value self;
    Object* object = nullptr;
    uint32_t ip = 0;
    uint32_t bytes = 0;
    std::vector<Value> slots;
};

struct Stack {
    StackId id = 0;
    StackType type = StackType::normal;
    StackState state = StackState::running;
    std::vector<Frame> frames;
    Value ret;
    Request pending;
    bool resume = false;  // store the return register at the call and step past it
    bool queued = false;  // on the ready list
    bool in_latent = false;
    bool pop_pending = true;
    Object* held = nullptr; // the lock (3.1)
    Object* init_object = nullptr;
    VirtualMachine::Callback callback;
};

struct VirtualMachine::Message {
    StackId stack;
    bool is_return;
    bool pop;
};

struct VirtualMachine::Timer {
    int64_t target;
    StackId stack;
    bool operator>(const Timer& o) const { return target > o.target; }
};

struct VirtualMachine::UpdateRegistration {
    Handle handle;
    int64_t next;
    int64_t interval;
    bool single;
};

struct VirtualMachine::Queues {
    std::deque<Message> messages;
    std::vector<Message> overflow;
    struct Side {
        std::deque<StackId> ring;
        std::vector<StackId> overflow;
        size_t size() const { return ring.size() + overflow.size(); }
    };
    Side sides[2];
    int write = 0;
    std::vector<Timer> waits[3]; // gameplay, real, game: min-heaps
    std::vector<UpdateRegistration> updates;
    std::vector<UpdateRegistration> game_updates;
    std::vector<Message> messages_as_vector() const { return {messages.begin(), messages.end()}; }
    std::unordered_map<uint64_t, NativeFunction> natives;
    std::unordered_map<uint64_t, uint32_t> native_flags;
};


} // namespace spud::script
