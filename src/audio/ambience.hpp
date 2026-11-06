#pragma once

// Ambience: acoustic spaces and reverb, region sounds, weather sounds, placed emitters, and the footstep and
// impact triggers.

#include "formats/osd.hpp"

#include <glm/vec3.hpp>

#include "audio/system.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace spud::audio {

class Ambience {
  public:
    explicit Ambience(AudioSystem::Impl& audio);
    ~Ambience();
    void update(float dt);
    void footstep(uint32_t footstep_set, osd::FootstepGait gait, std::string_view tag, uint32_t material,
                  const glm::vec3& position, bool player, bool first_person);
    void impact(uint32_t impact_set, uint32_t material, const glm::vec3& position);
    void combat_impact(uint32_t impact_set, uint32_t material, const glm::vec3& position,
                       bool player_attacker, bool normal_hit);
    void collision(uint32_t material_a, uint32_t material_b, const glm::vec3& position, float speed);
    std::string state_json() const;

    struct State;

  private:
    friend struct AmbienceOps;
    static bool path_equal(std::string_view a, std::string_view b);
    AudioSystem::Impl& m_audio;
    std::unique_ptr<State> m_state;
};

} // namespace spud::audio
