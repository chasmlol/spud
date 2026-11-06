#pragma once

// The music system: music types, their tracks, priorities, transitions, ducking and fades (
// The game).

#include "audio/system.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace spud::audio {

class MusicManager {
  public:
    explicit MusicManager(AudioSystem::Impl& audio);
    ~MusicManager();
    void add(uint32_t music_type);
    void remove(uint32_t music_type);
    void stop_all();
    // The player's cell music changed.
    void set_cell_music(uint32_t music_type); // osd::k_none: none
    void update(float dt);
    std::string state_json() const;

    struct State;

  private:
    friend struct MusicOps;
    AudioSystem::Impl& m_audio;
    std::unique_ptr<State> m_state;
};

} // namespace spud::audio
