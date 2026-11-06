#pragma once

// The Lockpicking Menu's lock and pick: the two models, their
// NiControllerManager sequences as LockpickingMenu drives them, and the pick's placement
// Unlike the other scenes nothing here runs on its own clock:
// the menu poses the sequences from the lock angle and the pick's health.

#include "ui/scene.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <string>

namespace spud::fx {
class EffectInstance;
}

namespace spud::ui {

class Ui;

class Lock3D {
  public:
    explicit Lock3D(Ui& ui);
    ~Lock3D();

    // Both models, asked for here so they load behind the opening (SetupModels finds them preloaded).
    void open(bool skeleton_key);
    // Both models have arrived or never will: the menu's first update can go on.
    bool settled();
    // SetupModels: placed, every sequence off; the lock's and the pick's Intro start.
    void start();

    struct Ended {
        bool lock_intro = false, pick_intro = false, pick_break = false;
    };
    // AdvanceSequences: what's playing moves on by the frame's time, and what ended is reported.
    Ended update(double dt);

    void pose_lock(double angle);   // Rotate at begin + (end - begin) x angle / fLockMaxAngle
    void pose_pick(double angle, double lock_angle); // UpdatePickTransform
    void pose_damage(double health); // PickDamage at end - (end - begin) x health / 100
    void break_pick();               // PickDamage off, PickBreak plays
    void insert_pick();              // InsertPick: the Intro again

    bool scene(UiScene& out) const;

  private:
    struct Held;
    void play(Held& h, const char* sequence, bool running);
    void place(Held& h, const glm::mat3& rotation, const glm::vec3& translation);
    void draw(const Held& h, UiScene& out) const;

    Ui& m_ui;
    std::unique_ptr<Held> m_lock, m_pick;
    float m_y = -1300.0f; // fLockPositionY, Wide past 4:3
    glm::vec3 m_pivot{0.0f}; // P, stored once
    bool m_started = false;
};

} // namespace spud::ui
