#pragma once

// Faces as records and as character creation edits them: what a race offers, the player's
// FaceData (what the game keep), and the RaceSex Menu's sliders. The head and its morphs are drawn
// from FaceData by whoever implements FaceHost.

#include "formats/forms_typed.hpp"
#include "game/settings.hpp"

#include <array>
#include <cfloat>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace spud::game {

class Character;

inline constexpr int k_face_morphs = 19; // NAM9: the slider morphs, then VampireMorph
inline constexpr float k_morph_unset = FLT_MAX;

// The game's tint layer: one per race template, in their order.
struct TintLayer {
    uint16_t template_index = 0; // TINI
    uint8_t type = 0;            // TINP
    uint32_t rgba = 0;           // red in the low byte
    float alpha = 0;
    std::string mask;            // TINT, under textures/
};

struct FaceData {
    uint32_t race = 0;
    bool female = false;
    float weight = 50, height = 1;
    std::vector<uint32_t> head_parts; // PNAM
    uint32_t hair_color = 0;          // HCLF, a CLFM
    uint32_t face_details = 0;        // FTST, a TXST
    std::array<float, k_face_morphs> morphs{};
    std::array<int32_t, 4> presets{-1, -1, -1, -1}; // NAMA: nose, brow, eyes, lip
    std::array<uint8_t, 3> skin_tone{128, 128, 128};
    std::vector<TintLayer> tints;
    bool tint_edited = false; // the menu rebuilds the tint map on an edit; until then the head has the default map
};

struct TintTemplate {
    uint16_t index = 0;
    uint8_t type = 0;
    std::string mask;
    uint32_t default_color = 0; // TIND, a CLFM
    std::vector<uint32_t> colors; // TINC, CLFMs
    std::vector<float> values;    // TINV
};

// One sex of a race's head data.
struct RaceFace {
    std::array<std::array<uint32_t, 8>, 4> morph_masks; // MPAV, all ones when absent
    std::vector<TintTemplate> tints;
    std::vector<uint32_t> face_details;  // FTSM, FTSF
    uint32_t default_face_details = 0;   // DFTM, DFTF
    std::vector<uint32_t> presets;       // RPRM, RPRF
    std::vector<uint32_t> hair_colors;   // AHCM, AHCF
    uint32_t default_hair_color = 0;     // HCLF
    std::vector<uint32_t> default_parts; // HEAD
};

class Faces {
public:
    explicit Faces(const forms::Database& db) : m_db(&db) {}

    const RaceFace& race(uint32_t race, bool female) const;
    // HDPT forms the menu lists for a part type: playable, not extra, for this race and sex, by EditorID bytes.
    std::vector<uint32_t> head_parts(uint32_t race, bool female, int type) const;
    int part_type(uint32_t head_part) const;
    FaceData from_npc(uint32_t npc) const; // a preset NPC_, its tint layers in the template list's order
    uint32_t color(uint32_t clfm) const;   // CLFM's RGBA

private:
    const forms::Database* m_db;
    mutable std::map<std::pair<uint32_t, bool>, RaceFace> m_races;
};

// What draws the player's head and body. The Actors workstream implements it; the menu runs without one.
class FaceHost {
public:
    virtual ~FaceHost() = default;
    virtual void face_changed(const FaceData& face) = 0; // rebuild what changed, morphs from zero
    virtual bool face_ready() const { return true; }     // the 3D shows the last face: the loading icon goes
    virtual void zoom(bool face) { (void)face; }         // ZoomPC: the face pose, the body pose
};

// The RaceSex Menu's logic: the game and the setters.
class RaceSexEditor {
public:
    struct Slider {
        std::string label; // the GMST's text
        int filter = 0;    // the category flag
        std::string callback;
        float min = 0, max = 0, position = 0, step = 1;
        int id = 0;
        int key = 0;    // a part type, tint type, morph or preset index
        int link = -1;  // the slider paired with this one, by id
    };
    struct RaceChoice {
        uint32_t race = 0;
        std::string name, description;
    };

    RaceSexEditor(const forms::Database& db, const Settings& settings, Character& character);

    void set_host(FaceHost* host) { m_host = host; }
    const std::vector<RaceChoice>& races() const { return m_races; }
    int current_race() const;
    const FaceData& face() const { return m_face; }
    const std::vector<Slider>& sliders() const { return m_sliders; }

    void build_sliders(bool preset = false);
    void change_race(int index);
    void change_sex(bool female);
    void change(std::string_view callback, float value, int id);
    bool preset_would_lose_changes() const { return m_changed; }
    void apply_preset(int index);

private:
    struct Key {
        uint32_t race;
        bool female;
        auto operator<=>(const Key&) const = default;
    };
    const RaceFace& race_face() const { return m_faces.race(m_face.race, m_face.female); }
    void init_tints();
    void adopt(const FaceData& preset);
    void set_part(int type, uint32_t part);
    uint32_t part_of(int type) const;
    TintLayer* layer(int type, int variant);
    const TintTemplate* tint_template(int type, int variant) const;
    int template_count(int type) const;
    void find_tint(int type, int& variant, int& index) const;
    void set_layer(int type, int variant, int index);
    void refresh_skin_tone();
    void changed();
    int nth_morph(int group, int rank) const;

    const forms::Database* m_db;
    const Settings* m_settings;
    Character* m_character;
    Faces m_faces;
    FaceHost* m_host = nullptr;
    FaceData m_face;
    std::vector<RaceChoice> m_races;
    std::vector<Slider> m_sliders;
    std::map<Key, FaceData> m_built; // a race and sex already seen keeps what was chosen there
    int m_preset = 0;
    bool m_changed = false;
};

} // namespace spud::game
