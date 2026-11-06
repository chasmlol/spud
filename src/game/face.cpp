#include "game/face.hpp"

#include "game/character.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace spud::game {

namespace {

using forms::Value;

enum HeadPartFlag : uint8_t { part_playable = 0x1, part_male = 0x2, part_female = 0x4, part_extra = 0x8 };

template <class T>
T read(const forms::Field& f) {
    return f.as<T>();
}

} // namespace

const RaceFace& Faces::race(uint32_t race, bool female) const {
    const std::pair key{race, female};
    if (const auto it = m_races.find(key); it != m_races.end()) {
        return it->second;
    }
    RaceFace out;
    for (auto& mask : out.morph_masks) {
        mask.fill(0xFFFFFFFF);
    }
    if (const forms::FormEntry* e = m_db->find(race)) {
        bool head = false, mine = false;
        uint32_t morph = 0;
        TintTemplate* tint = nullptr;
        for (const forms::Field& f : m_db->fields(*e)) {
            switch (f.sig) {
            case forms::sig("NAM0"): head = true; break;
            case forms::sig("NAM1"): case forms::sig("NAM3"): head = false; break;
            case forms::sig("MNAM"): mine = !female; break;
            case forms::sig("FNAM"): mine = female; break;
            case forms::sig("HCLF"):
                if (f.data.size() >= 8) {
                    std::memcpy(&out.default_hair_color, f.data.data() + (female ? 4 : 0), 4);
                }
                break;
            default: break;
            }
            if (!head || !mine) {
                continue;
            }
            switch (f.sig) {
            case forms::sig("HEAD"): out.default_parts.push_back(read<uint32_t>(f)); break;
            case forms::sig("MPAI"): morph = read<uint32_t>(f); break;
            case forms::sig("MPAV"):
                if (morph < 4 && f.data.size() >= 32) {
                    std::memcpy(out.morph_masks[morph].data(), f.data.data(), 32);
                }
                break;
            case forms::sig("RPRM"): case forms::sig("RPRF"): out.presets.push_back(read<uint32_t>(f)); break;
            case forms::sig("AHCM"): case forms::sig("AHCF"): out.hair_colors.push_back(read<uint32_t>(f)); break;
            case forms::sig("FTSM"): case forms::sig("FTSF"): out.face_details.push_back(read<uint32_t>(f)); break;
            case forms::sig("DFTM"): case forms::sig("DFTF"): out.default_face_details = read<uint32_t>(f); break;
            case forms::sig("TINI"):
                tint = &out.tints.emplace_back();
                tint->index = read<uint16_t>(f);
                break;
            case forms::sig("TINT"):
                if (tint) {
                    tint->mask = f.text();
                }
                break;
            case forms::sig("TINP"):
                if (tint) {
                    tint->type = uint8_t(read<uint16_t>(f));
                }
                break;
            case forms::sig("TIND"):
                if (tint) {
                    tint->default_color = read<uint32_t>(f);
                }
                break;
            case forms::sig("TINC"):
                if (tint) {
                    tint->colors.push_back(read<uint32_t>(f));
                }
                break;
            case forms::sig("TINV"):
                if (tint) {
                    tint->values.push_back(read<float>(f));
                }
                break;
            default: break;
            }
        }
    }
    return m_races.emplace(key, std::move(out)).first->second;
}

int Faces::part_type(uint32_t head_part) const {
    const forms::FormEntry* e = m_db->find(head_part);
    const auto f = e ? m_db->field(*e, forms::sig("PNAM")) : std::nullopt;
    return f ? int(f->as<uint32_t>()) : -1;
}

std::vector<uint32_t> Faces::head_parts(uint32_t race, bool female, int type) const {
    std::vector<std::pair<std::string_view, uint32_t>> found;
    for (const forms::FormEntry* e : m_db->of_type(forms::sig("HDPT"))) {
        const auto data = m_db->field(*e, forms::sig("DATA"));
        const auto pnam = m_db->field(*e, forms::sig("PNAM"));
        if (!data || !pnam || int(pnam->as<uint32_t>()) != type) {
            continue;
        }
        const auto flags = data->as<uint8_t>();
        // Flagged only for the other sex: a male needs bit 2 clear, a female bit 1.
        if (!(flags & part_playable) || (flags & part_extra) || (flags & (female ? part_male : part_female))) {
            continue;
        }
        if (const auto r = m_db->field(*e, forms::sig("RNAM"))) {
            const forms::FormEntry* list = m_db->find(r->as<uint32_t>());
            if (list) {
                const auto ids = forms::form_list(forms::Form(*m_db, *list));
                if (std::find(ids.begin(), ids.end(), race) == ids.end()) {
                    continue;
                }
            }
        }
        found.emplace_back(m_db->editor_id(*e), e->form_id);
    }
    std::sort(found.begin(), found.end());
    std::vector<uint32_t> out;
    for (const auto& [id, form] : found) {
        out.push_back(form);
    }
    return out;
}

uint32_t Faces::color(uint32_t clfm) const {
    const forms::FormEntry* e = m_db->find(clfm);
    const auto c = e ? m_db->field(*e, forms::sig("CNAM")) : std::nullopt;
    return c ? c->as<uint32_t>() : 0;
}

FaceData Faces::from_npc(uint32_t npc) const {
    FaceData out;
    out.morphs.fill(k_morph_unset);
    const forms::FormEntry* e = m_db->find(npc);
    if (!e) {
        return out;
    }
    const forms::Form f(*m_db, *e);
    if (const Value* v = f.get("RNAM")) {
        out.race = v->form;
    }
    if (const Value* v = f.get("ACBS.flags")) {
        out.female = (v->integer & 1) != 0;
    }
    if (const Value* v = f.get("NAM7")) {
        out.weight = float(v->real);
    }
    if (const Value* v = f.get("NAM6")) {
        out.height = v->real == 0.0 ? 1.0f : float(v->real);
    }
    for (const Value* p : f.all("PNAM")) {
        out.head_parts.push_back(p->form);
    }
    if (const Value* v = f.get("HCLF")) {
        out.hair_color = v->form;
    }
    if (const Value* v = f.get("FTST")) {
        out.face_details = v->form;
    }
    if (const auto d = m_db->field(*e, forms::sig("NAM9")); d && d->data.size() >= sizeof(float) * k_face_morphs) {
        std::memcpy(out.morphs.data(), d->data.data(), sizeof(float) * k_face_morphs);
    }
    if (const auto d = m_db->field(*e, forms::sig("NAMA")); d && d->data.size() >= 16) {
        std::memcpy(out.presets.data(), d->data.data(), 16);
    }
    // the NPC's layers by index, over the race's defaults.
    const RaceFace& rf = race(out.race, out.female);
    for (const TintTemplate& t : rf.tints) {
        TintLayer l;
        l.template_index = t.index;
        l.type = t.type;
        l.mask = t.mask;
        l.rgba = color(t.default_color);
        const auto at = std::find(t.colors.begin(), t.colors.end(), t.default_color);
        const auto i = size_t(at - t.colors.begin());
        l.alpha = i < t.values.size() ? t.values[i] : 0.0f;
        out.tints.push_back(std::move(l));
    }
    uint16_t index = 0;
    TintLayer* layer = nullptr;
    for (const forms::Field& field : m_db->fields(*e)) {
        if (field.sig == forms::sig("TINI")) {
            index = field.as<uint16_t>();
            const auto it = std::find_if(out.tints.begin(), out.tints.end(), [&](const TintLayer& l) { return l.template_index == index; });
            layer = it == out.tints.end() ? nullptr : &*it;
        } else if (layer && field.sig == forms::sig("TINC")) {
            layer->rgba = field.as<uint32_t>();
        } else if (layer && field.sig == forms::sig("TINV")) {
            layer->alpha = float(field.as<uint32_t>()) / 100.0f;
        }
    }
    return out;
}

// ---- the editor --------------------------------------------------------------------------------------------------

RaceSexEditor::RaceSexEditor(const forms::Database& db, const Settings& settings, Character& character)
    : m_db(&db), m_settings(&settings), m_character(&character), m_faces(db) {
    for (const forms::FormEntry* e : db.of_type(forms::sig("RACE"))) {
        const forms::Form f(db, *e);
        const Value* flags = f.get("DATA.flags");
        if (flags && (flags->integer & 1)) { // playable
            m_races.push_back({e->form_id, std::string(f.name()), std::string(f.description())});
        }
    }
    m_face = m_faces.from_npc(k_player_base);
    m_face.race = character.race();
    m_face.female = character.female();
}

int RaceSexEditor::current_race() const {
    for (size_t i = 0; i < m_races.size(); ++i) {
        if (m_races[i].race == m_face.race) {
            return int(i);
        }
    }
    return -1;
}

void RaceSexEditor::changed() {
    if (m_host) {
        m_host->face_changed(m_face);
    }
}

void RaceSexEditor::init_tints() {
    // a layer per template, its default colour, the value paired with it.
    m_face.tints.clear();
    for (const TintTemplate& t : race_face().tints) {
        TintLayer l;
        l.template_index = t.index;
        l.type = t.type;
        l.mask = t.mask;
        l.rgba = m_faces.color(t.default_color);
        const auto i = size_t(std::find(t.colors.begin(), t.colors.end(), t.default_color) - t.colors.begin());
        l.alpha = i < t.values.size() ? t.values[i] : 0.0f;
        m_face.tints.push_back(std::move(l));
    }
}

void RaceSexEditor::adopt(const FaceData& preset) {
    m_face.head_parts = preset.head_parts;
    m_face.morphs = preset.morphs;
    m_face.presets = preset.presets;
    m_face.weight = preset.weight;
    if (preset.hair_color) {
        m_face.hair_color = preset.hair_color;
    }
    if (preset.face_details) {
        m_face.face_details = preset.face_details;
    }
    m_face.tints = preset.tints;
    m_face.tint_edited = true;
}

uint32_t RaceSexEditor::part_of(int type) const {
    for (uint32_t p : m_face.head_parts) {
        if (m_faces.part_type(p) == type) {
            return p;
        }
    }
    return 0;
}

void RaceSexEditor::set_part(int type, uint32_t part) {
    std::erase_if(m_face.head_parts, [&](uint32_t p) { return m_faces.part_type(p) == type; });
    if (part) {
        m_face.head_parts.push_back(part);
    }
}

const TintTemplate* RaceSexEditor::tint_template(int type, int variant) const {
    for (const TintTemplate& t : race_face().tints) {
        if (t.type == type && variant-- == 0) {
            return &t;
        }
    }
    return nullptr;
}

int RaceSexEditor::template_count(int type) const {
    const auto& list = race_face().tints;
    return int(std::count_if(list.begin(), list.end(), [type](const TintTemplate& t) { return t.type == type; }));
}

TintLayer* RaceSexEditor::layer(int type, int variant) {
    for (TintLayer& l : m_face.tints) {
        if (l.type == type && variant-- == 0) {
            return &l;
        }
    }
    return nullptr;
}

void RaceSexEditor::find_tint(int type, int& variant, int& index) const {
    // the first (variant, index >= 1) with the layer's colour and value.
    variant = -1;
    index = 0;
    int v = 0;
    for (const TintLayer& l : m_face.tints) {
        if (l.type != type) {
            continue;
        }
        if (const TintTemplate* t = tint_template(type, v)) {
            for (size_t i = 1; i < t->colors.size() && i < t->values.size(); ++i) {
                if (m_faces.color(t->colors[i]) == l.rgba && t->values[i] == l.alpha) {
                    variant = v;
                    index = int(i);
                    return;
                }
            }
        }
        ++v;
    }
}

void RaceSexEditor::set_layer(int type, int variant, int index) {
    const TintTemplate* t = tint_template(type, variant);
    TintLayer* l = layer(type, variant);
    if (!t || !l || t->colors.empty()) {
        return;
    }
    const size_t i = size_t(std::clamp(index, 0, int(t->colors.size()) - 1));
    l->rgba = m_faces.color(t->colors[i]);
    l->alpha = i < t->values.size() ? t->values[i] : 0.0f;
}

void RaceSexEditor::refresh_skin_tone() {
    // from the first skin layer, over mid grey as its alpha falls.
    const TintLayer* skin = layer(6, 0);
    for (int c = 0; c < 3; ++c) {
        if (!skin) {
            m_face.skin_tone[size_t(c)] = 128;
            continue;
        }
        const float value = float((skin->rgba >> (8 * c)) & 0xFF) / 255.0f;
        const float a = skin->alpha;
        m_face.skin_tone[size_t(c)] = uint8_t(std::clamp(value * a + (1.0f - std::max(a, 0.0f)) * 0.5f, 0.0f, 1.0f) * 255.0f);
    }
}

int RaceSexEditor::nth_morph(int group, int rank) const {
    // the rank-th allowed morph number.
    int seen = 0;
    const auto& mask = race_face().morph_masks[size_t(group)];
    for (int bit = 0; bit < 256; ++bit) {
        if (mask[size_t(bit / 32)] & (1u << (bit % 32))) {
            if (seen++ == rank) {
                return bit;
            }
        }
    }
    return 0;
}

void RaceSexEditor::build_sliders(bool preset) {
    const RaceFace& rf = race_face();
    const Key key{m_face.race, m_face.female};
    const bool seen = m_built.contains(key);
    if (preset) {
        // ChangeHeadPreset: the preset's values become the defaults.
    } else if (seen) {
        const FaceData kept = m_built[key];
        m_face = kept;
    } else if (!rf.presets.empty()) {
        // First time in a race: its first preset, with the race's default tints.
        adopt(m_faces.from_npc(rf.presets[0]));
        init_tints();
    } else {
        init_tints();
    }
    m_sliders.clear();
    const auto text = [this](const char* gmst) { return m_settings->text(gmst); };
    int id = 0;
    const auto add = [&](const char* label, int filter, const char* callback, float min, float max, float position, float step,
                         int slider_key, int link = -1) -> Slider& {
        Slider s;
        s.label = text(label);
        s.filter = filter;
        s.callback = callback;
        s.min = min;
        s.max = max;
        s.position = position;
        s.step = step;
        s.id = id++;
        s.key = slider_key;
        s.link = link;
        m_sliders.push_back(std::move(s));
        return m_sliders.back();
    };
    const auto part_slider = [&](const char* label, int filter, int type, bool optional) {
        const std::vector<uint32_t> list = m_faces.head_parts(m_face.race, m_face.female, type);
        if (optional && list.size() <= 1) {
            return;
        }
        const auto at = std::find(list.begin(), list.end(), part_of(type));
        add(label, filter, "ChangeHeadPart", 0, float(list.empty() ? 0 : list.size() - 1), at == list.end() ? 0.0f : float(at - list.begin()), 1,
            type);
    };
    const auto tint_slider = [&](const char* label, int filter, int type) {
        const TintTemplate* t = tint_template(type, 0);
        int variant = 0, index = 0;
        find_tint(type, variant, index);
        add(label, filter, "ChangeTintingMask", 0, t ? float(t->colors.empty() ? 0 : t->colors.size() - 1) : 0.0f, float(index), 1, type);
    };
    const auto mask_sliders = [&](const char* label, const char* color_label, int filter, int type) {
        int variant = 0, index = 0;
        find_tint(type, variant, index);
        const TintTemplate* t = tint_template(type, std::max(variant, 0));
        const int link = id + 1;
        add(label, filter, "ChangeMask", -1, float(template_count(type) - 1), float(variant), 1, type, link);
        add(color_label, filter, "ChangeMaskColor", 1, t ? float(int(t->colors.size()) - 1) : 1.0f, float(index), 1, type, link - 1);
    };
    const auto preset_slider = [&](const char* label, int filter, int group, const char* count_setting, float count_default) {
        int bits = 0;
        const int count = int(m_settings->number(count_setting, count_default));
        const auto& mask = rf.morph_masks[size_t(group)];
        for (int bit = 0; bit < std::min(count, 256); ++bit) {
            bits += (mask[size_t(bit / 32)] >> (bit % 32)) & 1;
        }
        add(label, filter, "ChangePreset", 0, float(std::max(bits - 1, 0)), float(std::max(m_face.presets[size_t(group)], 0)), 1, group);
    };
    const auto morph = [&](const char* label, int filter, int index) {
        const float v = m_face.morphs[size_t(index)];
        add(label, filter, "ChangeDoubleMorph", -1, 1, v == k_morph_unset ? 0.0f : v, 0.1f, index);
    };

    // Sex has id -1 and isn't there when limited (the menu leaves it out then).
    Slider sex;
    sex.label = text("sRSMSex");
    sex.filter = 4;
    sex.callback = "ChangeSex";
    sex.max = 1;
    sex.position = m_face.female ? 1.0f : 0.0f;
    sex.id = -1;
    m_sliders.push_back(std::move(sex));
    add("sRSMHeadPresets", 4, "ChangeHeadPreset", 0, float(rf.presets.empty() ? 0 : rf.presets.size() - 1), float(m_preset), 1, 0);
    tint_slider("sRSMSkinColor", 4, 6);
    add("sRSMWeight", 4, "ChangeWeight", 0, 1, m_face.weight / 100.0f, 0.1f, 0);
    if (!rf.face_details.empty()) {
        const uint32_t details = m_face.face_details ? m_face.face_details : rf.default_face_details;
        const auto at = std::find(rf.face_details.begin(), rf.face_details.end(), details);
        add("sRSMComplexion", 8, "ChangeFaceDetails", 0, float(rf.face_details.size() - 1),
            at == rf.face_details.end() ? 0.0f : float(at - rf.face_details.begin()), 1, 0);
    }
    mask_sliders("sRSMDirt", "sRSMDirtColor", 8, 14);
    part_slider("sRSMScars", 8, 5, false);
    mask_sliders("sRSMPaint", "sRSMPaintColor", 8, 7);
    part_slider("sRSMHairPresets", 256, 3, false);
    part_slider("sRSMFacialHairPresets", 256, 4, true);
    if (!rf.hair_colors.empty()) {
        const auto at = std::find(rf.hair_colors.begin(), rf.hair_colors.end(), m_face.hair_color);
        add("sRSMHairColorPresets", 256, "ChangeHairColorPreset", 0, float(rf.hair_colors.size() - 1),
            at == rf.hair_colors.end() ? 0.0f : float(at - rf.hair_colors.begin()), 1, 0);
    }
    preset_slider("sRSMEyeTypes", 32, 2, "iEyeMorphCount", 10);
    part_slider("sRSMEyeColor", 32, 2, false);
    morph("sRSMEyeHeight", 32, 7);
    morph("sRSMEyeWidth", 32, 8);
    morph("sRSMEyeDepth", 32, 17);
    tint_slider("sRSMEyelinerColor", 32, 3);
    tint_slider("sRSMEyeSocketUpperColor", 32, 4);
    tint_slider("sRSMEyeSocketLowerColor", 32, 5);
    part_slider("sRSMBrowTypes", 64, 6, true);
    morph("sRSMBrowHeight", 64, 9);
    morph("sRSMBrowWidth", 64, 10);
    morph("sRSMBrowForward", 64, 11);
    preset_slider("sRSMNoseTypes", 16, 0, "iNoseMorphCount", 10);
    morph("sRSMNoseHeight", 16, 1);
    morph("sRSMNoseLength", 16, 0);
    morph("sRSMJawWidth", 16, 3);
    morph("sRSMJawHeight", 16, 2);
    morph("sRSMJawForward", 16, 4);
    morph("sRSMCheekboneHeight", 16, 5);
    morph("sRSMCheekboneWidth", 16, 6);
    tint_slider("sRSMCheekColor", 16, 2);
    tint_slider("sRSMLaughLines", 16, 8);
    tint_slider("sRSMCheekColorLower", 16, 9);
    tint_slider("sRSMNoseColor", 16, 10);
    tint_slider("sRSMChinColor", 16, 11);
    tint_slider("sRSMNeckColor", 16, 12);
    tint_slider("sRSMForeheadColor", 16, 13);
    preset_slider("sRSMMouthTypes", 128, 3, "iLipMorphCount", 10);
    morph("sRSMMouthHeight", 128, 12);
    morph("sRSMMouthForward", 128, 13);
    morph("sRSMChinWidth", 128, 14);
    morph("sRSMChinLength", 128, 15);
    morph("sRSMChinForward", 128, 16);
    tint_slider("sRSMLipColor", 128, 1);

    refresh_skin_tone();
    m_built[key] = m_face;
    changed();
}

void RaceSexEditor::change_race(int index) {
    if (m_races.empty()) {
        return;
    }
    const RaceChoice& r = m_races[size_t(std::clamp(index, 0, int(m_races.size()) - 1))];
    m_built[{m_face.race, m_face.female}] = m_face;
    m_character->switch_race(r.race, false); // skills and attributes follow the race
    m_face.race = r.race;
    m_face.head_parts.clear();
    m_preset = 0;
    m_changed = false;
}

void RaceSexEditor::change_sex(bool female) {
    if (female == m_face.female) {
        return;
    }
    m_built[{m_face.race, m_face.female}] = m_face;
    m_character->set_female(female);
    m_face.female = female;
    m_face.head_parts.clear();
    m_preset = 0;
    m_changed = false;
}

void RaceSexEditor::apply_preset(int index) {
    const RaceFace& rf = race_face();
    if (rf.presets.empty()) {
        return;
    }
    m_preset = std::clamp(index, 0, int(rf.presets.size()) - 1);
    adopt(m_faces.from_npc(rf.presets[size_t(m_preset)]));
    m_changed = false;
    build_sliders(true);
}

void RaceSexEditor::change(std::string_view callback, float value, int id) {
    const auto it = std::find_if(m_sliders.begin(), m_sliders.end(), [id](const Slider& s) { return s.id == id; });
    if (it == m_sliders.end()) {
        return;
    }
    if (callback != "ChangeDoubleMorph" && it->position == value) {
        return; // a change to what's stored does nothing
    }
    it->position = value;
    const RaceFace& rf = race_face();
    const int i = int(value);
    if (callback == "ChangeDoubleMorph") {
        m_face.morphs[size_t(it->key)] = value;
    } else if (callback == "ChangePreset") {
        m_face.presets[size_t(it->key)] = nth_morph(it->key, i);
    } else if (callback == "ChangeWeight") {
        m_face.weight = std::clamp(value, 0.0f, 1.0f) * 100.0f;
    } else if (callback == "ChangeHeadPart") {
        const std::vector<uint32_t> list = m_faces.head_parts(m_face.race, m_face.female, it->key);
        set_part(it->key, i >= 0 && i < int(list.size()) ? list[size_t(i)] : 0);
    } else if (callback == "ChangeHairColorPreset") {
        if (i >= 0 && i < int(rf.hair_colors.size())) {
            m_face.hair_color = rf.hair_colors[size_t(i)];
        }
    } else if (callback == "ChangeFaceDetails") {
        if (i >= 0 && i < int(rf.face_details.size())) {
            m_face.face_details = rf.face_details[size_t(i)];
        }
    } else if (callback == "ChangeTintingMask") {
        m_face.tint_edited = true;
        set_layer(it->key, 0, i);
        if (it->key == 6) {
            refresh_skin_tone();
        }
    } else if (callback == "ChangeMask") {
        m_face.tint_edited = true;
        // The old variant goes; the new one takes the linked slider's colour, at least its first.
        for (int v = 0; v < template_count(it->key); ++v) {
            if (TintLayer* l = layer(it->key, v)) {
                l->alpha = 0;
            }
        }
        if (i >= 0) {
            const auto colour = std::find_if(m_sliders.begin(), m_sliders.end(), [&](const Slider& s) { return s.id == it->link; });
            set_layer(it->key, i, colour == m_sliders.end() ? 1 : std::max(int(colour->position), 1));
        }
    } else if (callback == "ChangeMaskColor") {
        m_face.tint_edited = true;
        const auto mask = std::find_if(m_sliders.begin(), m_sliders.end(), [&](const Slider& s) { return s.id == it->link; });
        if (mask != m_sliders.end() && mask->position >= 0.0f) {
            set_layer(it->key, int(mask->position), i);
        }
    } else {
        return;
    }
    m_changed = true;
    m_built[{m_face.race, m_face.female}] = m_face;
    changed();
}

} // namespace spud::game
