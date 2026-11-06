#pragma once

// Bindless texture cache. A TextureId is the descriptor slot, valid from
// acquire() on: it shows a fallback until the DDS has been read and parsed on
// a worker, uploaded on the transfer queue and finalised on the render thread.
// Unreferenced textures stay cached until the memory budget is exceeded (LRU).

#include "render/bindless.hpp"
#include "render/device.hpp"
#include "render/jobs.hpp"
#include "render/uploader.hpp"

#include <cstdint>
#include <filesystem>
#include <list>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spud::render {

using TextureId = uint32_t;
inline constexpr TextureId k_invalid_slot = ~0u;

enum class TextureKind : uint8_t {
    color,  // grey while loading, magenta checker if missing
    normal, // flat normal while loading or missing
    cube,   // environment cube map: black while loading or missing (shaders read it as textureCube)
};

struct TextureStats {
    uint32_t live = 0;       // slots in use (excluding built-ins)
    uint32_t ready = 0;
    uint32_t loading = 0;    // queued, in flight or awaiting upload
    uint32_t missing = 0;
    uint32_t referenced = 0; // refcount > 0
    uint64_t gpu_bytes = 0;
};

class TextureManager {
public:
    TextureManager(Device& device, Bindless& bindless, Uploader& uploader, JobSystem& jobs,
                   std::filesystem::path root);
    ~TextureManager();

    // Past loading: it shows what it will show.
    bool settled(TextureId id) const {
        const State s = m_entries[id].state;
        return s != State::queued && s != State::loading;
    }
    TextureId white() const { return m_white; }
    TextureId black() const { return m_black; }
    TextureId black_cube() const { return m_black_cube; }
    TextureId grey() const { return m_grey; }
    TextureId flat_normal() const { return m_flat_normal; }
    TextureId facegen_detail() const { return m_facegen_detail; } // 64 each
    TextureId missing() const { return m_missing; }

    // Empty paths return the kind's neutral built-in. Each acquire needs a release.
    TextureId acquire(std::string_view path, TextureKind kind, float priority);
    // A DDS file made at run time: the next acquire of `path` reads this, not the disk. Any thread.
    void provide(std::string path, std::vector<std::byte> file);
    void release(TextureId id);

    // An image someone else owns (a UI page render target) under a slot of its own, sampled like any texture in
    // the layout the owner keeps it in (SHADER_READ_ONLY_OPTIMAL). unregister_view frees the slot; the owner keeps
    // the view alive until no frame in flight samples it.
    TextureId register_view(VkImageView view);
    // A loaded texture's view (the UI's book pages copy one), null while it is still loading or missing.
    VkImageView view(TextureId id) const {
        return id < m_entries.size() && m_entries[id].state == State::ready ? m_entries[id].view : VK_NULL_HANDLE;
    }
    void unregister_view(TextureId id);

    // Render thread, once per frame: schedules loads (bounded in-flight count) and evicts.
    void update();

    size_t busy() const { return m_queued + m_in_flight; }
    TextureStats stats() const;
    void set_budget(uint64_t bytes) { m_budget = bytes; }
    // Preallocates device memory for textures at startup. A new device-memory block
    // allocated while streaming stalls the GPU for 100+ ms on Windows, so it must not happen.
    void reserve(uint64_t bytes);

private:
    enum class State : uint8_t { free, queued, loading, ready, missing, builtin, external };

    struct Entry {
        std::string path;
        TextureKind kind = TextureKind::color;
        State state = State::free;
        uint32_t refs = 0;
        float priority = 0.0f;
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkImageView view = VK_NULL_HANDLE;
        uint64_t bytes = 0;
        bool in_lru = false;
        std::list<TextureId>::iterator lru;
    };

    std::mutex m_provided_mutex;
    std::unordered_map<std::string, std::vector<std::byte>> m_provided;

    struct QueueItem {
        float priority;
        uint64_t sequence;
        TextureId id;
        bool operator<(const QueueItem& o) const {
            if (priority != o.priority) {
                return priority > o.priority;
            }
            return sequence > o.sequence;
        }
    };

    TextureId create_builtin(uint32_t w, uint32_t h, const uint32_t* rgba, bool cube = false);
    TextureId allocate_slot();
    void load_job(TextureId id, std::string path, TextureKind kind);
    void finish_load(TextureId id, VkImage image, VmaAllocation allocation, VkImageView view, uint64_t bytes);
    void fail_load(TextureId id, const std::string& error, bool not_found);
    void evict_over_budget();
    void touch_lru(TextureId id);
    VkImageView fallback_view(TextureKind kind) const;

    Device& m_device;
    Bindless& m_bindless;
    Uploader& m_uploader;
    JobSystem& m_jobs;
    std::filesystem::path m_root;

    std::vector<Entry> m_entries;
    std::vector<TextureId> m_free_slots;
    std::unordered_map<std::string, TextureId> m_by_path;
    std::list<TextureId> m_lru; // unreferenced ready/missing textures, oldest first
    std::priority_queue<QueueItem> m_queue;
    uint64_t m_sequence = 0;
    size_t m_queued = 0;
    size_t m_in_flight = 0;
    size_t m_max_in_flight = 96;
    uint64_t m_gpu_bytes = 0;
    uint64_t m_budget = 8ull << 30;
    std::unordered_set<std::string> m_logged_missing;

    // Completion messages from workers that failed before reaching the uploader.
    std::mutex m_fail_mutex;
    struct Failure {
        TextureId id;
        std::string error;
        bool not_found;
    };
    std::vector<Failure> m_failures;

    VmaPool m_pool = nullptr;
    TextureId m_white = 0, m_black = 0, m_grey = 0, m_flat_normal = 0, m_missing = 0, m_black_cube = 0;
    TextureId m_facegen_detail = 0;
};

} // namespace spud::render
