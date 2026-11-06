#pragma once

// A sound's way to the headphones when HRTF is on:
//
//   source voice --(gain, distance filter)--> this mono submix, HRTF effect --> mastering voice FL, FR
//
// The source voice's other send, to the reverb submix, stays as it is: the room is diffuse, the direct sound is
// what carries direction. Voices are made per sound, so with the option off none exists and the game's graph is
// exactly what it was.

#include <glm/vec3.hpp>

#include <cstdint>

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SubmixVoice;

namespace spud::audio {

class HrtfSet;
class BinauralXapo;

class BinauralVoice {
  public:
    // Throws Error when XAudio2 refuses the voice.
    BinauralVoice(IXAudio2* xaudio, IXAudio2MasteringVoice* master, uint32_t master_channels, const HrtfSet& set,
                  uint32_t sample_rate);
    ~BinauralVoice();
    BinauralVoice(const BinauralVoice&) = delete;
    BinauralVoice& operator=(const BinauralVoice&) = delete;

    IXAudio2SubmixVoice* voice() const { return m_voice; }
    // A unit vector in the head's frame and how directional the sound is (BinauralRenderer::set_target).
    void set_direction(const glm::vec3& direction, float focus);

  private:
    IXAudio2SubmixVoice* m_voice = nullptr;
    BinauralXapo* m_xapo = nullptr;
};

} // namespace spud::audio
