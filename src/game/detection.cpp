#include "game/detection.hpp"

#include <algorithm>
#include <cmath>

namespace spud::game {

namespace {

float max_distance(const Settings& s, bool exterior) {
    return s.number("fSneakMaxDistance", 2500.0f) * (exterior ? s.number("fSneakExteriorDistanceMult", 2.1f) : 1.0f);
}

} // namespace

float player_detection_skill(const Settings& s, float sneak) {
    return s.number("fPlayerDetectionSneakMult", 0.4f) * sneak + s.number("fPlayerDetectionSneakBase", 10.0f);
}

float detection_sight_range(const Settings& s, bool exterior, float angle) {
    const float range = max_distance(s, exterior);
    const float start = s.number("fDetectionLOSDistanceAngle", 60.0f);
    if (angle <= start) {
        return range;
    }
    const float far = exterior ? s.number("fDetectionLOSDistanceMultExterior", 0.5f)
                               : s.number("fDetectionLOSDistanceMultInterior", 0.75f);
    return range * (1.0f + (far - 1.0f) * (angle - start) / (180.0f - start));
}

bool event_heard(const Settings& s, int level, float distance, bool exterior, bool line, bool asleep) {
    const bool very_loud = level >= int(s.number("iSoundLevelVeryLoud", 200.0f));
    const float radius = s.number("fDetectEventDistancePlayer", 1536.0f) * (very_loud ? s.number("fDetectEventDistanceVeryLoudMult", 2.0f) : 1.0f);
    if (distance > radius) {
        return false;
    }
    const float range = very_loud ? s.number("fDetectEventSneakDistanceVeryLoud", 3072.0f) * (exterior ? s.number("fSneakExteriorDistanceMult", 2.1f) : 1.0f)
                                  : max_distance(s, exterior);
    const float attenuation = std::pow(std::max((range - distance) / range, 0.0f), s.number("fSneakDistanceAttenuationExponent", 2.0f));
    const float sound = float(level) * 2.0f * attenuation * (line ? 1.0f : s.number("fSneakSoundLosMult", 0.3f));
    const float light = exterior ? s.number("iDetectEventLightLevelExterior", 75.0f) : s.number("iDetectEventLightLevelInterior", 225.0f);
    const float light_mult = exterior ? s.number("fSneakLightExteriorMult", 0.5f) : s.number("fSneakLightMult", 0.33f);
    const float sight = line && !asleep ? attenuation * (light * light_mult + 15.0f) : 0.0f;
    return sound + sight + s.number("fSneakBaseValue", -15.0f) > 0.0f;
}

int detection_level(const Settings& s, const DetectionInput& in) {
    const float range = max_distance(s, in.exterior);
    if (in.distance >= range) {
        return -1000;
    }
    const float attenuation = std::pow(std::max((range - in.distance) / range, 0.0f),
                                       s.number("fSneakDistanceAttenuationExponent", 2.0f));
    const bool in_cone = in.los && in.angle < s.number("fDetectionViewCone", 190.0f) / 2.0f;

    // Sound.
    const float heard = attenuation * (in.los ? 1.0f : s.number("fSneakSoundLosMult", 0.3f));
    const float gear = in.moving ? (s.number("fSneakEquippedWeightMult", 0.5f) * in.worn_weight +
                                    s.number("fSneakEquippedWeightBase", 12.0f)) *
                                       std::max(in.noise_mult, 0.0f)
                                 : 0.0f;
    const float sound = std::max((float(in.action_sound) * float(int(s.number("fSneakActionMult", 2.0f))) +
                                  (in.running ? s.number("fSneakRunningMult", 2.0f) : 1.0f) * gear) *
                                     s.number("fSneakSoundsMult", 1.0f) * heard *
                                     (in.observer_asleep ? 1.0f + s.number("fSneakSleepBonus", 0.0f) : 1.0f),
                                 0.0f);

    // Sight.
    const float movement = 1.0f + (in.moving ? s.number("fSneakLightMoveMult", 0.01f) : 0.0f) +
                           (in.running ? s.number("fSneakLightRunMult", 0.2f) : 0.0f);
    const float light_mult = in.exterior ? s.number("fSneakLightExteriorMult", 0.5f) : s.number("fSneakLightMult", 0.33f);
    const float sight_ok = in.observer_asleep ? 0.0f : (100.0f - in.blindness) / 100.0f;
    const float sight = std::max((in_cone && !in.invisible ? 1.0f : 0.0f) * attenuation * movement *
                                     (in.light * light_mult + s.number("fDetectionSneakLightMod", 15.0f)) * sight_ok *
                                     in.size * s.number("fDetectionLargeActorSizeMult", 1.0f),
                                 0.0f);

    // Skill.
    const float low = s.number("fSneakPerceptionSkillMin", 0.0f), high = s.number("fSneakPerceptionSkillMax", 100.0f);
    const float perception = low + (high - low) * in.observer_skill / 100.0f;
    const float alert = 1.0f + (in.observer_in_combat ? s.number("fSneakAlertMod", 0.0f) : 0.0f) +
                        (in.observer_asleep ? s.number("fSneakSleepMod", 0.0f) : 0.0f) +
                        (in.observer_busy ? s.number("fSneakCombatMod", -0.4f) : 0.0f);
    const float skill = (attenuation * perception * alert - (in.sneaking ? in.target_skill : 0.0f)) *
                        s.number("fSneakSkillMult", 0.5f);

    const float level = sound + sight + skill + s.number("fSneakBaseValue", -15.0f);
    if (level > 0.0f && level < 1.0f) {
        return 1;
    }
    const int out = int(level);
    return out == 0 ? -1 : out; // exactly 0 is "heard", which sight never gives
}

} // namespace spud::game
