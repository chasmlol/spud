// hkbStateMachine, its transition effect and conditions, and the expression engine

#include "anim/state_machine.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace spud::anim {

namespace {

constexpr float k_epsilon = 1.1920929e-7f;
constexpr float k_deg_to_rad = 0.017453292f;
constexpr float k_rad_to_deg = 57.29578f;

// The graph's generator. Spelled out here because the design fixes how the seed becomes a float, and the
// expressions and the weighted choice must advance the same seed.
float next_random(Context& ctx) {
    uint32_t& seed = ctx.character.random_state;
    seed = 1664525u * seed + 1013904223u;
    return static_cast<float>(seed) * 2.3283064e-10f;
}

int32_t root_slot(const Behavior* behavior, int32_t local) {
    if (!behavior || local < 0 || size_t(local) >= behavior->variable_to_root.size()) {
        return -1;
    }
    return behavior->variable_to_root[size_t(local)];
}

// Out of range is undefined in C++; the game's cvttss2si gives INT_MIN there.
int32_t truncate(float v) {
    const bool in_range = v > -2147483904.0f && v < 2147483648.0f;
    return in_range ? static_cast<int32_t>(v) : std::numeric_limits<int32_t>::min();
}

} // namespace

namespace {

enum class Op : uint8_t {
    number, variable, property,
    add, subtract, multiply, divide, remainder,
    equal, not_equal, less, greater, less_equal, greater_equal,
    logical_and, logical_or, logical_not, negate, plus,
    sin, sind, cos, cosd, asin, asind, acos, acosd, atan2, atan2d,
    sqrt, sqrt_inverse, fabs, ceil, floor, sign, deg2rad, rad2deg,
    pow, max, min, clamp, lerp, clerp, cond, rand01, rand_range,
};

struct Token {
    Op op = Op::number;
    uint8_t operands = 0;
    int32_t index = -1; // a variable's or character property's index
    float value = 0.0f;
};

struct Function {
    std::string_view name;
    Op op;
    uint8_t operands;
};

// In the order they are tried: each is matched as a prefix, so `sind` must come before `sin`.
constexpr Function k_functions[] = {
    {"rand01", Op::rand01, 0},   {"sind", Op::sind, 1},        {"sin", Op::sin, 1},
    {"cosd", Op::cosd, 1},       {"cos", Op::cos, 1},          {"asind", Op::asind, 1},
    {"asin", Op::asin, 1},       {"acosd", Op::acosd, 1},      {"acos", Op::acos, 1},
    {"sqrt", Op::sqrt, 1},       {"fabs", Op::fabs, 1},        {"ceil", Op::ceil, 1},
    {"floor", Op::floor, 1},     {"sqrtInverse", Op::sqrt_inverse, 1}, {"pow", Op::pow, 2},
    {"max", Op::max, 2},         {"min", Op::min, 2},          {"randRange", Op::rand_range, 2},
    {"atan2d", Op::atan2d, 2},   {"atan2", Op::atan2, 2},      {"deg2rad", Op::deg2rad, 1},
    {"rad2deg", Op::rad2deg, 1}, {"clamp", Op::clamp, 3},      {"sign", Op::sign, 1},
    {"lerp", Op::lerp, 3},       {"clerp", Op::clerp, 5},      {"cond", Op::cond, 3},
};

constexpr std::string_view k_operator_chars = "*/+-|&><=!%";

bool is_operator_char(char c) { return k_operator_chars.find(c) != std::string_view::npos; }
bool ends_name(char c) { return c == '(' || c == ')' || c == ',' || is_operator_char(c); }

float havok_atan2(float y, float x) {
    const auto p = [](float r) { return r - 0.121079f * r * r - 0.093522817f * r * r * r; };
    const float ay = std::fabs(y), ax = std::fabs(x);
    float angle = ay > ax ? 1.5707964f - p(ax / (ay + k_epsilon)) : p(ay / (ax + k_epsilon));
    if (x < 0.0f) {
        angle = 3.1415927f - angle;
    }
    return y < 0.0f ? -angle : angle;
}

float saturated_asin(float v) { return std::asin(std::clamp(v, -1.0f, 1.0f)); }
float saturated_acos(float v) { return std::acos(std::clamp(v, -1.0f, 1.0f)); }

float truncated_ceil(float v) {
    if (!(std::fabs(v) < 2147483648.0f)) {
        return v;
    }
    const float t = static_cast<float>(static_cast<int32_t>(v));
    return t < v ? t + 1.0f : t;
}

float truncated_floor(float v) {
    if (!(std::fabs(v) < 2147483648.0f)) {
        return v;
    }
    const float t = static_cast<float>(static_cast<int32_t>(v));
    return t > v ? t - 1.0f : t;
}

float circular_lerp(float a, float b, float t, float lo, float hi) {
    const float direct = std::fabs(a - b);
    if (direct <= (hi - lo) - direct) {
        return (b - a) * t + a;
    }
    if (a > b) {
        std::swap(a, b);
        t = 1.0f - t;
    }
    const float result = a - ((hi - b) + (a - lo)) * t;
    return result < lo ? hi - (lo - result) : result;
}

float word_value(VariableType type, int32_t word) {
    switch (type) {
    case VariableType::boolean: return word != 0 ? 1.0f : 0.0f;
    case VariableType::int8: return static_cast<float>(static_cast<int8_t>(word));
    case VariableType::int16: return static_cast<float>(static_cast<int16_t>(word));
    case VariableType::int32: return static_cast<float>(word);
    case VariableType::real: return std::bit_cast<float>(word);
    default: return 0.0f;
    }
}

float read_variable(Context& ctx, int32_t local) {
    const Behavior* b = ctx.behavior;
    const int32_t slot = root_slot(b, local);
    const auto& words = ctx.graph.values().words;
    if (slot < 0 || size_t(slot) >= words.size()) {
        return -1.0f;
    }
    // The declared type is the node's own behaviour's, as for a binding: the value is the root's.
    const auto i = size_t(local);
    const VariableType type = i < b->variable_types.size() ? b->variable_types[i] : VariableType::real;
    return word_value(type, words[size_t(slot)]);
}

float read_property(Context& ctx, int32_t index) {
    const Project& project = ctx.graph.project();
    const auto i = size_t(index);
    if (index < 0 || i >= project.character_types().size()) {
        return -1.0f;
    }
    const VariableType type = project.character_types()[i];
    if (type == VariableType::pointer) {
        const auto& weights = project.character_bone_weights();
        return i < weights.size() && !weights[i].empty() ? 1.0f : 0.0f;
    }
    const auto& words = project.character_values().words;
    return i < words.size() ? word_value(type, words[i]) : 0.0f;
}

} // namespace

// hkbCompiledExpressionSet: one reverse-Polish list for all its expressions, built at link time.
struct CompiledExpressions {
    std::vector<Token> tokens;
    std::vector<uint32_t> starts; // per expression
    uint32_t max_depth = 0;

    bool evaluate(Context& ctx, size_t expression, float& result) const;
};

bool CompiledExpressions::evaluate(Context& ctx, size_t expression, float& result) const {
    if (expression >= starts.size()) {
        return false;
    }
    const size_t begin = starts[expression];
    const size_t end = expression + 1 < starts.size() ? starts[expression + 1] : tokens.size();
    if (begin == end) {
        return false; // a failed compile leaves an empty range
    }
    std::array<float, 32> local;
    std::vector<float> large;
    float* stack = local.data();
    if (max_depth > local.size()) {
        large.resize(max_depth);
        stack = large.data();
    }
    size_t depth = 0;
    for (size_t i = begin; i < end; ++i) {
        const Token& t = tokens[i];
        depth -= t.operands;
        const float* a = stack + depth;
        float v = 0.0f;
        switch (t.op) {
        case Op::number: v = t.value; break;
        case Op::variable: v = read_variable(ctx, t.index); break;
        case Op::property: v = read_property(ctx, t.index); break;
        case Op::add: v = a[0] + a[1]; break;
        case Op::subtract: v = a[0] - a[1]; break;
        case Op::multiply: v = a[0] * a[1]; break;
        case Op::divide: v = a[0] / a[1]; break;
        case Op::remainder: {
            // A zero divisor faults in the game; no expression reaches it. Here it fails the evaluation.
            const int32_t x = truncate(a[0]), y = truncate(a[1]);
            if (y == 0) {
                return false;
            }
            v = y == -1 ? 0.0f : static_cast<float>(x % y);
            break;
        }
        case Op::equal: v = a[0] == a[1] ? 1.0f : 0.0f; break;
        case Op::not_equal: v = a[0] != a[1] ? 1.0f : 0.0f; break;
        case Op::less: v = a[0] < a[1] ? 1.0f : 0.0f; break;
        case Op::greater: v = a[0] > a[1] ? 1.0f : 0.0f; break;
        case Op::less_equal: v = a[0] <= a[1] ? 1.0f : 0.0f; break;
        case Op::greater_equal: v = a[0] >= a[1] ? 1.0f : 0.0f; break;
        case Op::logical_and: v = a[0] != 0.0f && a[1] != 0.0f ? 1.0f : 0.0f; break;
        case Op::logical_or: v = a[0] != 0.0f || a[1] != 0.0f ? 1.0f : 0.0f; break;
        case Op::logical_not: v = a[0] != 0.0f ? 0.0f : 1.0f; break;
        case Op::negate: v = -a[0]; break;
        case Op::plus: v = a[0]; break;
        case Op::sin: v = std::sin(a[0]); break;
        case Op::sind: v = std::sin(a[0] * k_deg_to_rad); break;
        case Op::cos: v = std::cos(a[0]); break;
        case Op::cosd: v = std::cos(a[0] * k_deg_to_rad); break;
        case Op::asin: v = saturated_asin(a[0]); break;
        case Op::asind: v = saturated_asin(a[0]) * k_rad_to_deg; break;
        case Op::acos: v = saturated_acos(a[0]); break;
        case Op::acosd: v = saturated_acos(a[0]) * k_rad_to_deg; break;
        case Op::atan2: v = havok_atan2(a[0], a[1]); break;
        case Op::atan2d: v = havok_atan2(a[0], a[1]) * k_rad_to_deg; break;
        case Op::sqrt: v = std::sqrt(a[0]); break;
        case Op::sqrt_inverse: v = 1.0f / std::sqrt(a[0]); break;
        case Op::fabs: v = std::fabs(a[0]); break;
        case Op::ceil: v = truncated_ceil(a[0]); break;
        case Op::floor: v = truncated_floor(a[0]); break;
        case Op::sign: v = a[0] > 0.0f ? 1.0f : a[0] < 0.0f ? -1.0f : 0.0f; break;
        case Op::deg2rad: v = a[0] * k_deg_to_rad; break;
        case Op::rad2deg: v = a[0] * k_rad_to_deg; break;
        case Op::pow: v = std::pow(a[0], a[1]); break;
        case Op::max: v = a[0] > a[1] ? a[0] : a[1]; break;
        case Op::min: v = a[0] < a[1] ? a[0] : a[1]; break;
        case Op::clamp: v = a[0] < a[1] ? a[1] : (a[0] < a[2] ? a[0] : a[2]); break;
        case Op::lerp: v = (a[1] - a[0]) * a[2] + a[0]; break;
        case Op::clerp: v = circular_lerp(a[0], a[1], a[2], a[3], a[4]); break;
        case Op::cond: v = a[0] == 0.0f ? a[2] : a[1]; break;
        case Op::rand01: v = next_random(ctx); break;
        case Op::rand_range: v = next_random(ctx) * (a[1] - a[0]) + a[0]; break;
        }
        stack[depth++] = v;
    }
    if (depth != 1) {
        return false;
    }
    result = stack[0];
    return true;
}

// What a behaviour's expressions compile against.
struct ExpressionNames {
    const Project& project;
    const Behavior& behavior;
    std::unordered_map<std::string_view, int32_t> variables;
    std::unordered_map<std::string_view, int32_t> events;

    ExpressionNames(const Project& p, const Behavior& b) : project(p), behavior(b) {
        for (size_t i = 0; i < b.variable_names.size(); ++i) {
            variables.emplace(b.variable_names[i], static_cast<int32_t>(i));
        }
        for (size_t i = 0; i < b.event_names.size(); ++i) {
            events.emplace(b.event_names[i], static_cast<int32_t>(i));
        }
    }
};

namespace {

// The shunting-yard parser. Its precedence is the game's, not C's: logical operators bind tighter than
// comparisons and share one level, and vanilla depends on that (`blockbehavior.hkx`).
class ExpressionCompiler {
public:
    ExpressionCompiler(const ExpressionNames& names, CompiledExpressions& set) : m_names(names), m_set(set) {}

    bool run(std::string_view s) {
        size_t i = 0;
        while (true) {
            i = skip_spaces(s, i);
            if (i >= s.size()) {
                break;
            }
            const char c = s[i];
            if (c == '(') {
                if (m_operand) {
                    return false; // adjacent operands
                }
                m_stack.push_back({Item::paren});
                ++i;
            } else if (c == ')') {
                if (!close_paren()) {
                    return false;
                }
                ++i;
            } else if (is_operator_char(c)) {
                if (!read_operator(s, i)) {
                    return false;
                }
            } else if (c == ',') {
                if (!comma()) {
                    return false;
                }
                ++i;
            } else if ((c >= '0' && c <= '9') || c == '.') {
                if (!read_number(s, i)) {
                    return false;
                }
            } else if (const Function* f = match_function(s, i)) {
                if (!read_function(*f, s, i)) {
                    return false;
                }
            } else if (!read_name(s, i)) {
                return false;
            }
        }
        if (!m_operand) {
            return false; // nothing at all, or a trailing operator
        }
        while (!m_stack.empty()) {
            if (m_stack.back().kind == Item::paren || !emit(m_stack.back())) {
                return false;
            }
            m_stack.pop_back();
        }
        return m_depth == 1;
    }

private:
    struct Item {
        enum Kind : uint8_t { paren, function, unary, binary } kind = paren;
        Op op = Op::number;
        uint8_t level = 0;
        uint8_t operands = 0;
        uint32_t commas = 0;
    };

    static size_t skip_spaces(std::string_view s, size_t i) {
        while (i < s.size() && s[i] == ' ') {
            ++i;
        }
        return i;
    }

    bool push_operand(Token t) {
        if (m_operand) {
            return false; // adjacent operands
        }
        m_operand = true;
        m_set.tokens.push_back(t);
        m_set.max_depth = std::max(m_set.max_depth, ++m_depth);
        return true;
    }

    // Checking the depth here is what lets evaluation run without bounds checks: nothing compiled can
    // underflow.
    bool emit(const Item& item) {
        if (m_depth < item.operands) {
            return false;
        }
        m_depth -= item.operands;
        m_set.tokens.push_back({item.op, item.operands});
        m_set.max_depth = std::max(m_set.max_depth, ++m_depth);
        return true;
    }

    bool pop_to_paren() {
        while (!m_stack.empty() && m_stack.back().kind != Item::paren) {
            if (!emit(m_stack.back())) {
                return false;
            }
            m_stack.pop_back();
        }
        return !m_stack.empty();
    }

    bool close_paren() {
        if (!m_operand || !pop_to_paren()) {
            return false; // empty parentheses, a trailing operator, or unbalanced
        }
        const Item paren = m_stack.back();
        m_stack.pop_back();
        if (!m_stack.empty() && m_stack.back().kind == Item::function) {
            if (paren.commas + 1 != m_stack.back().operands || !emit(m_stack.back())) {
                return false;
            }
            m_stack.pop_back();
        }
        return true;
    }

    bool comma() {
        if (!m_operand || !pop_to_paren()) {
            return false;
        }
        if (m_stack.size() < 2 || m_stack[m_stack.size() - 2].kind != Item::function) {
            return false; // a comma outside a function
        }
        ++m_stack.back().commas;
        m_operand = false;
        return true;
    }

    bool read_operator(std::string_view s, size_t& i) {
        struct Symbol {
            std::string_view text;
            Op op;
            uint8_t level;
        };
        static constexpr Symbol symbols[] = {
            {"==", Op::equal, 1},         {"!=", Op::not_equal, 1},   {"<=", Op::less_equal, 1},
            {">=", Op::greater_equal, 1}, {"&&", Op::logical_and, 2}, {"||", Op::logical_or, 2},
            {"<", Op::less, 1},           {">", Op::greater, 1},      {"!", Op::logical_not, 5},
            {"*", Op::multiply, 4},       {"/", Op::divide, 4},       {"%", Op::remainder, 4},
            {"+", Op::add, 3},            {"-", Op::subtract, 3},
        };
        const std::string_view rest = s.substr(i);
        const auto it = std::ranges::find_if(symbols, [rest](const Symbol& x) {
            return rest.starts_with(x.text);
        });
        if (it == std::end(symbols)) {
            return false; // a lone =, & or |
        }
        i += it->text.size();
        if (it->op == Op::logical_not || (!m_operand && (it->op == Op::add || it->op == Op::subtract))) {
            if (m_operand) {
                return false; // a unary operator with a left operand
            }
            const Op op = it->op == Op::add ? Op::plus : it->op == Op::subtract ? Op::negate : it->op;
            m_stack.push_back({Item::unary, op, 5, 1});
            return true;
        }
        if (!m_operand) {
            return false; // a binary operator with no left operand
        }
        while (!m_stack.empty() && m_stack.back().kind != Item::paren && m_stack.back().level >= it->level) {
            if (!emit(m_stack.back())) {
                return false;
            }
            m_stack.pop_back();
        }
        m_stack.push_back({Item::binary, it->op, it->level, 2});
        m_operand = false;
        return true;
    }

    bool read_number(std::string_view s, size_t& i) {
        size_t end = i;
        while (end < s.size() && ((s[end] >= '0' && s[end] <= '9') || s[end] == '.')) {
            ++end;
        }
        if (end < s.size() && s[end] != ' ' && !ends_name(s[end])) {
            return false; // so a name can't begin with a digit
        }
        float value = 0.0f;
        std::from_chars(s.data() + i, s.data() + end, value);
        i = end;
        return push_operand({Op::number, 0, -1, value});
    }

    static const Function* match_function(std::string_view s, size_t i) {
        for (const Function& f : k_functions) {
            if (s.substr(i).starts_with(f.name)) {
                const size_t next = skip_spaces(s, i + f.name.size());
                if (next >= s.size() || ends_name(s[next])) {
                    return &f;
                }
            }
        }
        return nullptr;
    }

    bool read_function(const Function& f, std::string_view s, size_t& i) {
        i += f.name.size();
        if (f.operands == 0) {
            // rand01 has no arguments, and works with or without ().
            const size_t open = skip_spaces(s, i);
            if (open < s.size() && s[open] == '(') {
                const size_t close = skip_spaces(s, open + 1);
                if (close >= s.size() || s[close] != ')') {
                    return false;
                }
                i = close + 1;
            }
            return push_operand({f.op});
        }
        if (m_operand) {
            return false; // adjacent operands
        }
        m_stack.push_back({Item::function, f.op, 6, f.operands});
        return true;
    }

    bool read_name(std::string_view s, size_t& i) {
        size_t end = i;
        while (end < s.size() && !ends_name(s[end])) {
            ++end;
        }
        std::string_view name = s.substr(i, end - i);
        while (name.ends_with(' ')) {
            name.remove_suffix(1);
        }
        i = end;
        if (auto it = m_names.variables.find(name); it != m_names.variables.end()) {
            return push_operand({Op::variable, 0, it->second});
        }
        if (const int32_t property = m_names.project.character_property(name); property >= 0) {
            return push_operand({Op::property, 0, property});
        }
        log::warn("Invalid token '{}'", name);
        return false;
    }

    const ExpressionNames& m_names;
    CompiledExpressions& m_set;
    std::vector<Item> m_stack;
    uint32_t m_depth = 0;
    bool m_operand = false; // the last token ended an operand
};

// A failed compile cuts the list back but keeps the entry, so that later expressions keep their indices.
void compile_expression(std::string_view text, const ExpressionNames& names, CompiledExpressions& set) {
    const size_t begin = set.tokens.size();
    set.starts.push_back(static_cast<uint32_t>(begin));
    if (!ExpressionCompiler(names, set).run(text)) {
        set.tokens.resize(begin);
        log::warn("behaviour {}: expression '{}' doesn't compile", names.behavior.path, text);
    }
}

std::string_view trimmed(std::string_view s) {
    while (s.starts_with(' ')) {
        s.remove_prefix(1);
    }
    while (s.ends_with(' ')) {
        s.remove_suffix(1);
    }
    return s;
}

// hkbExpressionData.
struct ExpressionData : Bindable {
    std::string expression;
    int32_t assignment_variable_index = -1;
    int32_t assignment_event_index = -1;
    int8_t event_mode = 0;

    void describe(Describer& d) override {
        d.member("expression", expression);
        d.member("assignmentVariableIndex", assignment_variable_index);
        d.member("assignmentEventIndex", assignment_event_index);
        d.member("eventMode", event_mode);
    }
};

// hkbExpressionDataArray.
struct ExpressionDataArray : Bindable {
    std::vector<ExpressionData> expressions_data;
    void describe(Describer& d) override { d.objects("expressionsData", expressions_data); }
};

class EvaluateExpressionModifier final : public Modifier {
public:
    std::shared_ptr<ExpressionDataArray> expressions;

    std::unique_ptr<Node> clone() const override {
        return std::make_unique<EvaluateExpressionModifier>(*this);
    }

    void describe(Describer& d) override {
        Modifier::describe(d);
        if (!expressions) {
            expressions = std::make_shared<ExpressionDataArray>();
        }
        d.object("expressions", *expressions);
    }

    // The files leave the assignment indices at -1: linking parses the targets out of the strings.
    void link(const ExpressionNames& names) {
        auto set = std::make_shared<CompiledExpressions>();
        for (ExpressionData& e : expressions->expressions_data) {
            std::string text = e.expression;
            std::ranges::replace_if(text, [](char c) { return c == '\t' || c == '\n'; }, ' ');
            std::string_view view = text;
            size_t at = view.find(" if ");
            size_t skip = 4;
            if (at == std::string_view::npos) {
                at = view.find(" if(");
                skip = 3;
            }
            if (at != std::string_view::npos) {
                const std::string_view target = trimmed(view.substr(0, at));
                if (auto it = names.events.find(target); it != names.events.end()) {
                    e.assignment_event_index = it->second;
                } else {
                    log::warn("behaviour {}: no event '{}' for '{}'", names.behavior.path, target, view);
                }
                view.remove_prefix(at + skip);
            } else if (at = view.find('='); at != std::string_view::npos) {
                // An unknown target is reported and the expression still runs, writing nothing.
                // falmerbehavior.hkx has one, `State = cond (...)`: the game's behaviour, not something to
                // repair.
                const std::string_view target = trimmed(view.substr(0, at));
                if (auto it = names.variables.find(target); it != names.variables.end()) {
                    e.assignment_variable_index = it->second;
                } else {
                    log::warn("behaviour {}: no variable '{}' for '{}'", names.behavior.path, target, view);
                }
                view.remove_prefix(at + 1);
            }
            compile_expression(view, names, *set);
        }
        m_compiled = std::move(set);
    }

    void activate(Context&) override {
        m_entries.assign(expressions ? expressions->expressions_data.size() : 0, Entry{});
    }

    void update(Context& ctx, float) override {
        if (!m_compiled) {
            return;
        }
        const auto& data = expressions->expressions_data;
        for (size_t i = 0; i < data.size() && i < m_entries.size(); ++i) {
            float result = 0.0f;
            if (!m_compiled->evaluate(ctx, i, result)) {
                continue;
            }
            const ExpressionData& e = data[i];
            Entry& entry = m_entries[i];
            if (e.assignment_variable_index >= 0) {
                write_variable(ctx, e.assignment_variable_index, result);
            }
            // Not the condition's test: here -1 is false.
            const bool truth = result > 0.0f;
            if (e.assignment_event_index >= 0) {
                bool send = false;
                switch (e.event_mode) {
                case 0: send = truth && !entry.raised_event; break;
                case 1: send = truth; break;
                case 2: send = truth && !entry.was_true_in_previous_frame; break;
                case 3: send = truth || entry.raised_event; break;
                default: break;
                }
                if (send) {
                    ctx.graph.raise(ctx, e.assignment_event_index);
                    entry.raised_event = true;
                }
            }
            entry.was_true_in_previous_frame = truth;
        }
    }

private:
    struct Entry {
        bool raised_event = false;
        bool was_true_in_previous_frame = false;
    };

    static void write_variable(Context& ctx, int32_t local, float value) {
        const Behavior* b = ctx.behavior;
        const int32_t slot = root_slot(b, local);
        auto& words = ctx.graph.values().words;
        if (slot < 0 || size_t(slot) >= words.size() || size_t(local) >= b->variable_types.size()) {
            return;
        }
        int32_t& word = words[size_t(slot)];
        switch (b->variable_types[size_t(local)]) {
        case VariableType::boolean: word = value != 0.0f ? 1 : 0; break;
        case VariableType::int8: word = static_cast<int8_t>(truncate(value)); break;
        case VariableType::int16: word = static_cast<int16_t>(truncate(value)); break;
        case VariableType::int32: word = truncate(value); break;
        case VariableType::real: word = std::bit_cast<int32_t>(value); break;
        default: break;
        }
    }

    std::shared_ptr<const CompiledExpressions> m_compiled;
    std::vector<Entry> m_entries;
};

} // namespace

bool TransitionCondition::is_true(Context& ctx) const {
    // hkbStringCondition: the game's callback for it is never set and its default is true, so the string is
    // ignored, though it reads like an expression. It must not be compiled.
    if (!condition_string.empty()) {
        return true;
    }
    float result = 0.0f;
    return compiled && compiled->evaluate(ctx, 0, result) && result != 0.0f;
}

namespace {

struct SyncPosition {
    int32_t first = -1;
    int32_t second = -1;
    float fraction = 0.0f;
};

float period_of(const SyncInfo& s) { return s.base_frequency != 0.0f ? 1.0f / s.base_frequency : 0.0f; }

// "Sync interval of a sync info at time t".
SyncPosition sync_position(const SyncInfo& s, float t) {
    SyncPosition out;
    const uint32_t n = s.point_count;
    if (n == 0) {
        out.fraction = std::clamp(t * s.base_frequency, 0.0f, 1.0f);
        return out;
    }
    const float period = period_of(s);
    if (n == 1 && s.cyclic) {
        const float point = s.points[0].time;
        out.first = out.second = s.points[0].id;
        out.fraction = std::clamp(((t < point ? t + period : t) - point) * s.base_frequency, 0.0f, 1.0f);
        return out;
    }
    uint32_t chosen = 0;
    while (chosen < n && s.points[chosen].time < t) {
        ++chosen;
    }
    const bool past_last = chosen == n;
    if (past_last) {
        chosen = 0;
    }
    int32_t later_id = s.points[chosen].id;
    float later = s.points[chosen].time;
    int32_t earlier_id = -1;
    float earlier = 0.0f;
    if (chosen > 0) {
        earlier_id = s.points[chosen - 1].id;
        earlier = s.points[chosen - 1].time;
    } else if (s.cyclic) {
        earlier_id = s.points[n - 1].id;
        earlier = s.points[n - 1].time;
        if (past_last) {
            later += period;
        } else {
            earlier -= period;
        }
    } else if (past_last) {
        earlier_id = s.points[n - 1].id;
        earlier = s.points[n - 1].time;
        later_id = -1;
        later = period;
    } else if (later == 0.0f) {
        if (n == 1) {
            return out;
        }
        earlier_id = s.points[0].id;
        later_id = s.points[1].id;
        later = s.points[1].time;
    }
    out.first = earlier_id;
    out.second = later_id;
    out.fraction = later > earlier ? std::clamp((t - earlier) / (later - earlier), 0.0f, 1.0f) : 0.0f;
    return out;
}

// "Time for an interval": false when no point matches.
bool time_at(const SyncInfo& s, const SyncPosition& p, float& out) {
    const float period = period_of(s);
    const auto find = [&s](int32_t id) -> const SyncInfo::Point* {
        for (uint32_t i = 0; i < s.point_count; ++i) {
            if (s.points[i].id == id) {
                return &s.points[i];
            }
        }
        return nullptr;
    };
    if (p.first == -1 && p.second == -1) {
        out = s.base_frequency != 0.0f ? p.fraction / s.base_frequency : 0.0f;
        return true;
    }
    const SyncInfo::Point* a = p.first != -1 ? find(p.first) : nullptr;
    const SyncInfo::Point* b = p.second != -1 ? find(p.second) : nullptr;
    if (p.first == -1 && b) {
        out = b->time * p.fraction;
        return true;
    }
    if (p.second == -1 && a) {
        out = (period - a->time) * p.fraction + a->time;
        return true;
    }
    if (a && b && p.first != p.second) {
        if (b->time > a->time) {
            out = (b->time - a->time) * p.fraction + a->time;
            return true;
        }
        out = (b->time + period - a->time) * p.fraction + a->time;
    } else if (a || b) {
        out = period * p.fraction + (a ? a : b)->time;
    } else {
        return false;
    }
    if (out + 0.00001f >= period) {
        out = std::clamp(out - period, 0.0f, period);
    }
    return true;
}

float point_time(const SyncInfo& s, int32_t id) {
    for (uint32_t i = 0; i < s.point_count; ++i) {
        if (s.points[i].id == id) {
            return s.points[i].time;
        }
    }
    return 0.0f;
}

// "Combining children", for two children and a threshold of 0. The first child
// must be the from generator: on the first sync its position becomes the effect's, and the to generator is
// then aligned to that.
void combine_sync(SyncInfo& self, const SyncInfo& from, float from_weight, const SyncInfo& to,
                  float to_weight, bool first_sync) {
    std::array<int32_t, k_max_sync_points> common{};
    uint32_t count = 0;
    for (uint32_t i = 0; i < from.point_count; ++i) {
        for (uint32_t k = 0; k < to.point_count; ++k) {
            if (from.points[i].id == to.points[k].id) {
                common[count++] = from.points[i].id;
                break;
            }
        }
    }
    if (first_sync || self.point_count != count) {
        self.cyclic = from.cyclic;
    }
    const SyncInfo source = first_sync || self.point_count != count ? from : self;
    if (source.base_frequency == 0.0f) {
        self.base_frequency = self.local_time = 0.0f;
        self.playback_speed = 1.0f;
        self.point_count = 0;
        return;
    }
    SyncInfo restricted = source;
    restricted.point_count = 0;
    for (uint32_t i = 0; i < source.point_count; ++i) {
        const auto end = common.begin() + count;
        if (std::find(common.begin(), end, source.points[i].id) != end) {
            restricted.points[restricted.point_count++] = source.points[i];
        }
    }
    const SyncPosition position = sync_position(restricted, source.local_time);

    const std::array<const SyncInfo*, 2> children{&from, &to};
    const std::array<float, 2> weights{from_weight, to_weight};
    self.point_count = count;
    if (count > 0 && !(self.cyclic && count == 1)) {
        std::array<float, k_max_sync_points> intervals{};
        float tail = 0.0f, total = 0.0f;
        for (size_t c = 0; c < children.size(); ++c) {
            if (weights[c] <= 0.0f) {
                continue;
            }
            const SyncInfo& child = *children[c];
            const float period = period_of(child);
            total += weights[c];
            for (uint32_t i = count; i-- > 0;) {
                const float end = point_time(child, common[i]);
                float length;
                if (i == 0 && !self.cyclic) {
                    length = end;
                } else {
                    const float start = point_time(child, common[i == 0 ? count - 1 : i - 1]);
                    length = end > start ? end - start : end - start + period;
                }
                intervals[i] += weights[c] * length;
            }
            if (!self.cyclic) {
                tail += weights[c] * (period - point_time(child, common[count - 1]));
            }
        }
        float running = 0.0f;
        for (uint32_t i = 0; i < count; ++i) {
            running += total > 0.0f ? intervals[i] / total : 0.0f;
            self.points[i] = {common[i], running};
        }
        const float period = running + (!self.cyclic && total > 0.0f ? tail / total : 0.0f);
        self.playback_speed = 1.0f;
        self.base_frequency = period > 0.0f ? 1.0f / period : 0.0f;
    } else {
        float w = 0.0f, p = 0.0f, v = 0.0f;
        for (size_t c = 0; c < children.size(); ++c) {
            if (weights[c] > 0.0f && children[c]->base_frequency != 0.0f) {
                w += weights[c];
                p += weights[c] / children[c]->base_frequency;
                v += weights[c] * children[c]->playback_speed;
            }
        }
        if (w == 0.0f) {
            self.base_frequency = self.local_time = 0.0f;
            self.playback_speed = 1.0f;
            self.point_count = 0;
            return;
        }
        self.playback_speed = v / w;
        self.base_frequency = w / p;
        if (count == 1) {
            self.points[0] = {common[0], 0.0f};
        }
    }
    time_at(self, position, self.local_time);
    self.mirrored = false;
}

// "Joining": a node is placed once at time t, and its sync children follow on their own timelines.
void join(Context& ctx, const Node* node, float time) {
    Node* live = ctx.graph.clone_of(node);
    Record* own = ctx.graph.sync_record(ctx.graph.record_of(node));
    if (!live || !own) {
        return;
    }
    if (live->is_generator()) {
        static_cast<Generator*>(live)->set_local_time(ctx, time);
    }
    // The next timesteps measure from this, before any updateSync has seen the new time.
    own->sync.local_time = time;
    std::vector<Child> kids;
    live->children(children_active_only, kids);
    for (const Child& k : kids) {
        if (!k.node || !k.sync_to_parent) {
            continue;
        }
        Record* child = ctx.graph.sync_record(ctx.graph.record_of(k.node));
        float child_time = time;
        if (child && own->sync.base_frequency != 0.0f && child->sync.base_frequency != 0.0f) {
            time_at(child->sync, sync_position(own->sync, time), child_time);
        }
        join(ctx, k.node, child_time);
    }
}

// startEcho on every active node of a subtree.
void echo(Context& ctx, const Node* root, float duration) {
    std::vector<const Node*> pending{root};
    std::unordered_set<const Node*> seen;
    std::vector<Child> kids;
    while (!pending.empty()) {
        const Node* n = pending.back();
        pending.pop_back();
        Node* live = ctx.graph.clone_of(n);
        if (!live || !seen.insert(n).second) {
            continue;
        }
        if (live->is_generator()) {
            static_cast<Generator*>(live)->start_echo(ctx, duration);
        }
        kids.clear();
        live->children(children_active_only, kids);
        for (const Child& k : kids) {
            if (k.node) {
                pending.push_back(k.node);
            }
        }
    }
}

const Node* inner_generator(const Node* n) {
    while (n) {
        if (const Behavior* b = n->behavior_below()) {
            n = b->root_generator;
        } else if (const Node* wrapped = n->wrapped_generator()) {
            n = wrapped;
        } else {
            break;
        }
    }
    return n;
}

const SyncInfo* sync_of(Context& ctx, const Node* node) {
    const Record* r = ctx.graph.sync_record(ctx.graph.record_of(node));
    return r ? &r->sync : nullptr;
}

enum SelfTransition : int8_t { self_by_cycle = 0, self_continue = 1, self_reset = 2, self_blend = 3 };
enum EffectEventMode : int8_t { event_mode_ignore_from = 2, event_mode_ignore_to = 3 };
enum EndMode : int8_t { end_mode_none = 0, end_mode_until_end_of_from = 1, end_mode_cap_at_end_of_from = 2 };

glm::quat weighted_rotation(const glm::quat& a, float wa, const glm::quat& b, float wb) {
    const glm::quat first = a * wa;
    return glm::dot(b, first) < 0.0f ? first - b * wb : first + b * wb;
}

Qs blend_transforms(const Qs& from, float from_weight, const Qs& to, float to_weight) {
    Qs out;
    out.t = from.t * from_weight + to.t * to_weight;
    out.s = from.s * from_weight + to.s * to_weight;
    out.r = glm::normalize(weighted_rotation(to.r, to_weight, from.r, from_weight));
    return out;
}

bool has_bone(const Output& o, size_t bone) { return o.all_present || o.present[bone] != 0; }

// "Add bones": the additive source applies on the parent side.
void add_bones(const Output& additive, float v, Output& out, size_t count) {
    for (size_t b = 0; b < count; ++b) {
        if (!has_bone(additive, b)) {
            continue;
        }
        const Qs& s = additive.pose[b];
        Qs& p = out.pose[b];
        const glm::vec3 translation = s.t + s.r * p.t;
        const glm::quat rotation = s.r * p.r;
        p.s = (1.0f - v) * p.s + v * (s.s * p.s);
        p.t = (1.0f - v) * p.t + v * translation;
        p.r = glm::normalize(weighted_rotation(p.r, 1.0f - v, rotation, v));
        out.present[b] = 1;
    }
}

// Two inputs make the blender's accumulators unnecessary: a bone has at most two contributions, so each is
// finished in one step, and a bone only one side wrote keeps that side's value whatever its weight.
void blend_pose(const Skeleton* skeleton, const Output& from, float from_weight, const Output& to,
                float to_weight, Output& out) {
    const size_t count = std::min({out.pose.size(), from.pose.size(), to.pose.size()});
    const bool from_additive = from.pose_fraction > 0.0f && from.additive;
    const bool to_additive = to.pose_fraction > 0.0f && to.additive;
    const bool all_additive = count > 0 && from_additive && to_additive;
    const bool use_from = !from_additive && from_weight > k_epsilon;
    const bool use_to = !to_additive && to_weight > k_epsilon;
    bool every = true;
    for (size_t b = 0; b < count; ++b) {
        const bool f = use_from && has_bone(from, b);
        const bool t = use_to && has_bone(to, b);
        out.present[b] = f || t ? 1 : 0;
        if (f && t) {
            const float total = from_weight + to_weight;
            out.pose[b] = blend_transforms(from.pose[b], from_weight / total, to.pose[b], to_weight / total);
        } else if (f) {
            out.pose[b] = from.pose[b];
        } else if (t) {
            out.pose[b] = to.pose[b];
        } else {
            every = false;
            const bool identity = all_additive || !skeleton || b >= skeleton->reference.size();
            out.pose[b] = identity ? Qs{} : skeleton->reference[b];
        }
    }
    bool dense = (use_from && from.all_present) || (use_to && to.all_present);
    if (to_additive && to_weight > k_epsilon) {
        add_bones(to, to_weight, out, count);
        dense = dense || to.all_present;
    }
    if (from_additive && from_weight > k_epsilon) {
        add_bones(from, from_weight, out, count);
        dense = dense || from.all_present;
    }
    out.all_present = dense || every;
    if (out.all_present) {
        std::fill(out.present.begin(), out.present.end(), uint8_t{1});
    }
    out.additive = all_additive;
    out.pose_fraction = 1.0f;
}

// The float track of "Other tracks": an unset value leaves the other side alone, as if it had been present at
// the same weight. A pass-0 child comes before an additive one, whatever the order of the inputs.
void blend_floats(const Output& from, float from_weight, const Output& to, float to_weight, Output& out) {
    constexpr uint32_t unset_bits = 0x7f7fffeeu;
    const float unset = std::bit_cast<float>(unset_bits);
    const auto is_set = [](float v) { return std::bit_cast<uint32_t>(v) != unset_bits; };
    const size_t count = std::min({out.floats.size(), from.floats.size(), to.floats.size()});
    std::fill(out.floats.begin(), out.floats.end(), unset);
    out.floats_fraction = 0.0f;
    float total = 0.0f;
    float children_total = 0.0f;
    using Weighted = std::pair<const Output*, float>;
    std::array<Weighted, 2> order{Weighted{&to, to_weight}, Weighted{&from, from_weight}};
    if (to.additive && !from.additive) {
        std::swap(order[0], order[1]);
    }
    for (const auto& [child, w] : order) {
        if (child->additive && w <= k_epsilon) {
            continue; // only the normal pass blends a weight of nothing
        }
        if (!child->additive && w > k_epsilon) {
            children_total += w;
        }
        if (child->floats_fraction <= 0.0f) {
            continue;
        }
        out.floats_fraction += child->floats_fraction * w;
        total += w;
        for (size_t i = 0; i < count; ++i) {
            const float src = child->floats[i];
            const float dst = out.floats[i];
            const float b = is_set(dst) ? 1.0f : 0.0f;
            const float a = is_set(src) && !(child->additive && src == 0.0f && b == 0.0f) ? 1.0f : 0.0f;
            if (a == 0.0f && b == 0.0f) {
                continue;
            }
            const float taken = a != 0.0f ? ((1.0f - b) * total + b * w) * src : 0.0f;
            const float kept = b != 0.0f ? ((1.0f - a) * w + 1.0f) * dst : 0.0f;
            out.floats[i] = taken + kept;
        }
    }
    if (total > 0.0f) {
        for (size_t i = 0; i < count; ++i) {
            if (is_set(out.floats[i])) {
                out.floats[i] /= total;
            }
        }
    }
    if (children_total > 0.0f) {
        out.floats_fraction /= children_total;
    }
}

} // namespace

void BlendingTransitionEffect::describe(Describer& d) {
    d.member("selfTransitionMode", self_transition_mode);
    d.member("eventMode", event_mode);
    d.member("duration", duration);
    d.member("toGeneratorStartTimeFraction", to_generator_start_time_fraction);
    d.member("flags", flags);
    d.member("endMode", end_mode);
    d.member("blendCurve", blend_curve);
}

void BlendingTransitionEffect::activate(Context& ctx) {
    // The guard means a reactivation part way through a blend resumes it.
    if (time_remaining < k_epsilon) {
        time_remaining = duration;
        time_in_transition = 0.0f;
        m_apply_self_transition = ctx.graph.was_active(inner_generator(to));
    }
    m_default_event_mode = ctx.graph.project().default_event_mode;
    m_first_sync = true;
}

void BlendingTransitionEffect::update(Context&, float timestep) {
    if (record) {
        SyncInfo& s = record->sync;
        s.local_time += timestep;
        const float period = period_of(s);
        if (period > 0.0f && s.local_time > period) {
            s.local_time = std::fmod(s.local_time, period);
        }
    }
    time_remaining -= timestep;
    time_in_transition += timestep;
}

void BlendingTransitionEffect::deactivate(Context&) { time_remaining = 0.0f; }

void BlendingTransitionEffect::children(uint32_t query, std::vector<Child>& out) {
    Child to_child{to};
    Child from_child{from};
    if (query & children_active_only) {
        if (m_apply_self_transition && self_transition_mode == self_reset) {
            to_child.force_reactivate = true;
            m_apply_self_transition = false;
        }
        if (from != to) {
            to_child.sync_to_parent = from_child.sync_to_parent = (flags & flag_sync) != 0;
            const int8_t mode = effective_event_mode();
            from_child.ignore_events = mode == event_mode_ignore_from;
            to_child.ignore_events = mode == event_mode_ignore_to;
        }
    }
    out.push_back(to_child);
    if (from != to) {
        out.push_back(from_child);
    }
}

void BlendingTransitionEffect::apply_self_transition(Context& ctx) {
    int8_t mode = self_transition_mode;
    if (mode == self_by_cycle) {
        const SyncInfo* s = sync_of(ctx, to);
        mode = s && s->cyclic ? self_continue : self_blend;
    }
    if (mode == self_reset) {
        echo(ctx, to, 0.0f);
    } else if (mode == self_blend) {
        echo(ctx, to, duration);
    }
}

void BlendingTransitionEffect::update_sync(Context& ctx) {
    if (!record) {
        return;
    }
    if (m_apply_self_transition) {
        apply_self_transition(ctx);
        m_apply_self_transition = false;
    }
    SyncInfo& own = record->sync;
    const SyncInfo* from_sync = sync_of(ctx, from);
    const SyncInfo* to_sync = sync_of(ctx, to);
    const SyncInfo* basis = &own;
    if (from == to) {
        ctx.graph.copy_sync(*this, from);
    } else if ((flags & flag_sync) && from_sync && to_sync && from_sync->base_frequency != 0.0f &&
               to_sync->base_frequency != 0.0f) {
        const float total = time_in_transition + time_remaining;
        const float w = total != 0.0f ? std::min(time_in_transition / total, 1.0f) : 0.0f;
        combine_sync(own, *from_sync, 1.0f - w, *to_sync, w, m_first_sync);
        if (time_in_transition == 0.0f) {
            float time = own.local_time;
            time_at(*to_sync, sync_position(own, own.local_time), time);
            join(ctx, to, time);
        }
    } else {
        own = SyncInfo{};
        if (time_in_transition == 0.0f && to_generator_start_time_fraction != 0.0f && to_sync &&
            to_sync->base_frequency != 0.0f) {
            join(ctx, to, to_generator_start_time_fraction / to_sync->base_frequency);
        }
        basis = from_sync;
    }
    m_first_sync = false;

    // Each frame, so that a bound duration that changes mid-blend takes effect at once.
    const float to_end_of_duration = duration - time_in_transition;
    const bool has_frequency = basis && basis->base_frequency != 0.0f;
    const float to_end_of_from = has_frequency ? 1.0f / basis->base_frequency - basis->local_time : 0.0f;
    switch (end_mode) {
    case end_mode_until_end_of_from: time_remaining = to_end_of_from; break;
    case end_mode_cap_at_end_of_from:
        time_remaining = has_frequency ? std::min(to_end_of_from, to_end_of_duration) : 0.0f;
        break;
    default: time_remaining = to_end_of_duration; break;
    }
}

void BlendingTransitionEffect::generate(Context& ctx, std::span<const Output* const> kids, Output& output,
                                        float time_offset) {
    if (kids.empty()) {
        return;
    }
    if (kids.size() == 1) {
        output.copy_from(*kids[0]);
        return;
    }
    const Output& to_output = *kids[0];
    const Output& from_output = *kids[1];

    // Ragdoll hand-over belongs to the ragdoll tracks, which Output doesn't carry yet.

    const float total = time_in_transition + time_remaining;
    const float elapsed = std::min(time_in_transition + time_offset, total);
    const float t = total != 0.0f ? std::min(elapsed / total, 1.0f) : 0.0f;
    float w = t;
    switch (blend_curve) {
    case 0: w = 3.0f * t * t - 2.0f * t * t * t; break;
    case 2: w = t * t - t * t * t + t; break;
    case 3: w = 2.0f * t * t - t * t * t; break;
    default: break;
    }
    w = std::clamp(w, 0.0f, 1.0f);

    // Every generator writes these two tracks; a side that didn't counts as the fill a blender would use.
    const Qs& character = ctx.character.world_from_model;
    const auto world_of = [&character](const Output& o) -> const Qs& {
        return o.world_from_model_fraction > 0.0f ? o.world_from_model : character;
    };
    const Qs& from_world = world_of(from_output);
    const Qs& to_world = world_of(to_output);
    const Qs identity;
    const Qs& from_motion = from_output.motion_fraction > 0.0f ? from_output.motion : identity;
    const Qs& to_motion = to_output.motion_fraction > 0.0f ? to_output.motion : identity;

    if (flags & flag_ignore_from_world_from_model) {
        output.world_from_model = to_world;
    } else if (flags & flag_ignore_to_world_from_model) {
        output.world_from_model = from_world;
    } else {
        output.world_from_model = blend_transforms(from_world, 1.0f - w, to_world, w);
    }
    output.world_from_model_fraction = 1.0f;
    output.motion = blend_transforms(from_motion, 1.0f - w, to_motion, w);
    output.motion_fraction = 1.0f;
    blend_pose(ctx.character.skeleton, from_output, 1.0f - w, to_output, w, output);
    blend_floats(from_output, 1.0f - w, to_output, w, output);
}

bool BlendingTransitionEffect::is_done() const {
    return time_in_transition > 0.0f && (from == to || time_remaining <= 0.0f);
}

namespace {

// Null only for a state the file doesn't have.
template <typename T>
void load_shared(Describer& d, const char* name, std::shared_ptr<T>& v) {
    if (!v) {
        v = std::make_shared<T>();
    }
    d.object(name, *v);
}

// An interval with an enter event starts closed, one with only an exit event starts open.
uint8_t initial_interval_bits(const TransitionInfo& t) {
    const auto open = [](const TimeInterval& i) {
        return i.enter_event_id == -1 && (i.exit_event_id != -1 || i.enter_time <= 0.0f);
    };
    uint8_t bits = 0;
    if ((t.flags & TransitionInfo::flag_use_trigger_interval) && open(t.trigger_interval)) {
        bits |= 1;
    }
    if ((t.flags & TransitionInfo::flag_use_initiate_interval) && open(t.initiate_interval)) {
        bits |= 2;
    }
    return bits;
}

void apply_interval_events(const std::vector<TransitionInfo>& list, std::vector<uint8_t>& flags,
                           int32_t event) {
    const auto apply = [event](const TimeInterval& i, uint8_t bit, uint8_t& byte) {
        if (event == i.enter_event_id) {
            byte |= bit;
        }
        if (event == i.exit_event_id) {
            byte &= uint8_t(~bit);
        }
    };
    for (size_t i = 0; i < list.size() && i < flags.size(); ++i) {
        if (list[i].flags & TransitionInfo::flag_use_trigger_interval) {
            apply(list[i].trigger_interval, 1, flags[i]);
        }
        if (list[i].flags & TransitionInfo::flag_use_initiate_interval) {
            apply(list[i].initiate_interval, 2, flags[i]);
        }
    }
}

// The override lives on the clone when there is one. A machine that has none yet would get one in the game;
// here the root keeps the value until the machine activates.
void set_start_override(Context& ctx, const StateMachine* machine, int32_t index) {
    if (Node* live = ctx.graph.clone_of(machine)) {
        static_cast<StateMachine*>(live)->set_next_start_override(index);
    } else {
        ctx.graph.start_overrides[machine] = index;
    }
}

} // namespace

void NotifyEvent::describe(Describer& d) {
    struct StringPayload : Bindable {
        std::string data;
        void describe(Describer& inner) override { inner.member("data", data); }
    };
    d.member("id", event.id);
    StringPayload payload;
    bool has_payload = false;
    d.object("payload", payload, &has_payload);
    if (has_payload) {
        event.payload = std::make_shared<const std::string>(std::move(payload.data));
    }
}

void StateInfo::describe(Describer& d) {
    load_shared(d, "enterNotifyEvents", enter_notify_events);
    load_shared(d, "exitNotifyEvents", exit_notify_events);
    load_shared(d, "transitions", transitions);
    d.node("generator", generator);
    d.member("name", name);
    d.member("stateId", state_id);
    d.member("probability", probability);
    d.member("enable", enable);
}

std::unique_ptr<Node> StateMachine::clone() const {
    auto copy = std::make_unique<StateMachine>(*this);
    copy->m_template = template_of();
    return copy;
}

void StateMachine::describe(Describer& d) {
    d.member("eventToSendWhenStateOrTransitionChanges", event_to_send_when_state_or_transition_changes);
    d.member("startStateId", start_state_id);
    d.member("returnToPreviousStateEventId", return_to_previous_state_event_id);
    d.member("randomTransitionEventId", random_transition_event_id);
    d.member("transitionToNextHigherStateEventId", transition_to_next_higher_state_event_id);
    d.member("transitionToNextLowerStateEventId", transition_to_next_lower_state_event_id);
    d.member("syncVariableIndex", sync_variable_index);
    d.member("wrapAroundStateId", wrap_around_state_id);
    d.member("maxSimultaneousTransitions", max_simultaneous_transitions);
    d.member("startStateMode", start_state_mode);
    d.member("selfTransitionMode", self_transition_mode);
    d.objects("states", states);
    load_shared(d, "wildcardTransitions", wildcard_transitions);
    d.member("currentStateId", current_state_id, true);
    d.member("isActive", is_active, true);
}

void StateMachine::collect_bindables(std::vector<Bindable*>& out) {
    for (StateInfo& s : states) {
        out.push_back(&s);
    }
}

const std::vector<TransitionInfo>& StateMachine::wildcard_list() const {
    static const std::vector<TransitionInfo> none;
    return wildcard_transitions ? wildcard_transitions->transitions : none;
}

const std::vector<TransitionInfo>& StateMachine::state_list(size_t index) const {
    static const std::vector<TransitionInfo> none;
    return index < states.size() && states[index].transitions ? states[index].transitions->transitions : none;
}

uint8_t StateMachine::flags_of(const Reference& r) const {
    const auto& flags = r.from_state_index < 0 ? m_wildcard_flags : m_state_flags;
    return size_t(r.transition_index) < flags.size() ? flags[size_t(r.transition_index)] : uint8_t{0};
}

int32_t StateMachine::state_index(int32_t state_id) const {
    for (size_t i = 0; i < states.size(); ++i) {
        if (states[i].state_id == state_id) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

Node* StateMachine::active_child() const {
    if (!m_transitions.empty()) {
        return m_transitions.back().effect.get();
    }
    return size_t(m_current_index) < states.size() ? states[size_t(m_current_index)].generator : nullptr;
}

void StateMachine::children(uint32_t flags, std::vector<Child>& out) {
    if (flags & children_active_only) {
        out.push_back({active_child(), false, false, true, false});
        return;
    }
    for (const StateInfo& s : states) {
        out.push_back({s.generator});
    }
    for (size_t i = 0; i < states.size(); ++i) {
        for (const TransitionInfo& t : state_list(i)) {
            if (t.transition) {
                out.push_back({t.transition});
            }
        }
    }
    for (const TransitionInfo& t : wildcard_list()) {
        if (t.transition) {
            out.push_back({t.transition});
        }
    }
}

void StateMachine::generate(Context&, std::span<const Output* const> kids, Output& output, float) {
    if (is_active && !kids.empty()) {
        output.copy_from(*kids[0]);
    }
}

void StateMachine::update_sync(Context& ctx) { ctx.graph.forward_sync(*this, active_child()); }

size_t StateMachine::weighted_choice(Context& ctx, std::span<const int32_t> state_indices) const {
    float sum = 0.0f;
    for (int32_t i : state_indices) {
        sum += states[size_t(i)].probability;
    }
    const float u = next_random(ctx);
    float total = 0.0f;
    for (size_t i = 0; i + 1 < state_indices.size(); ++i) {
        total += states[size_t(state_indices[i])].probability / sum;
        if (u <= total) {
            return i;
        }
    }
    return state_indices.size() - 1;
}

int32_t StateMachine::choose_start_state(Context& ctx) {
    int32_t id = start_state_id;
    if (m_next_start_override >= 0 && size_t(m_next_start_override) < states.size()) {
        id = states[size_t(m_next_start_override)].state_id;
        m_next_start_override = -1;
    } else if (start_state_mode == start_random) {
        std::vector<int32_t> enabled;
        for (size_t i = 0; i < states.size(); ++i) {
            if (states[i].enable) {
                enabled.push_back(static_cast<int32_t>(i));
            }
        }
        const int32_t chosen = enabled.empty() ? 0 : enabled[weighted_choice(ctx, enabled)];
        id = states[size_t(chosen)].state_id;
    } else if (start_state_mode == start_sync && sync_variable_index != -1) {
        const int32_t slot = root_slot(ctx.behavior, sync_variable_index);
        const auto& words = ctx.graph.values().words;
        if (slot >= 0 && size_t(slot) < words.size() && state_index(words[size_t(slot)]) >= 0) {
            id = words[size_t(slot)];
        }
    }
    const int32_t index = state_index(id);
    if (index < 0 || !states[size_t(index)].enable) {
        log::warn("Start state selection failed. Choosing an arbitrary state. ({})", name);
        const auto enabled = std::ranges::find_if(states, [](const StateInfo& s) { return s.enable; });
        id = enabled != states.end() ? enabled->state_id : states[0].state_id;
    }
    return id;
}

void StateMachine::write_sync(Context& ctx, int32_t state_id) {
    if (sync_variable_index == -1) {
        return;
    }
    const int32_t slot = root_slot(ctx.behavior, sync_variable_index);
    auto& words = ctx.graph.values().words;
    if (slot >= 0 && size_t(slot) < words.size()) {
        words[size_t(slot)] = state_id;
    }
}

void StateMachine::activate(Context& ctx) {
    // The three vanilla machines with no states are never activated, as far as is known; the game would read
    // past them.
    if (is_active || states.empty()) {
        return;
    }
    if (auto it = ctx.graph.start_overrides.find(template_of()); it != ctx.graph.start_overrides.end()) {
        m_next_start_override = it->second;
        ctx.graph.start_overrides.erase(it);
    }
    const int32_t start_state = choose_start_state(ctx);
    // Return-to-previous true makes the start state its own previous state, so a return can't match straight
    // away.
    set_state(ctx, start_state, true, false);
    write_sync(ctx, start_state);
    is_active = true;
    m_echo_next_update = false;
}

void StateMachine::deactivate(Context& ctx) {
    if (!is_active) {
        return;
    }
    // A pending delayed state change lands here, so the exit event below is the target state's.
    end_all_transitions(ctx);
    m_delayed.clear();
    send_exit(ctx);
    is_active = false;
}

void StateMachine::update(Context& ctx, float) {
    if (!is_active) {
        return;
    }
    m_retired.clear();
    retire_transitions(ctx);
    // Transitions are never polled. A condition is only evaluated when an event arrives for its transition.
    echo_and_delayed(ctx);
    if (m_state_or_transition_changed) {
        ctx.graph.raise(ctx, event_to_send_when_state_or_transition_changes);
        m_state_or_transition_changed = false;
    }
}

void StateMachine::retire_transitions(Context& ctx) {
    for (int32_t i = static_cast<int32_t>(m_transitions.size()) - 1; i >= 0; --i) {
        const BlendingTransitionEffect* instance = m_transitions[size_t(i)].effect.get();
        const auto* live = static_cast<const BlendingTransitionEffect*>(ctx.graph.clone_of(instance));
        bool ended = !live || live->is_done();
        if (!ended && size_t(i) + 1 < m_transitions.size()) {
            const BlendingTransitionEffect& newer = *m_transitions[size_t(i) + 1].effect;
            ended = newer.from != instance && newer.to != instance;
        }
        if (ended) {
            end_transitions(ctx, i);
            return;
        }
    }
}

void StateMachine::echo_and_delayed(Context& ctx) {
    if (m_echo_next_update) {
        const int32_t id = choose_start_state(ctx);
        if (id != current_state_id && self_transition_mode != self_no_transition) {
            go_to_state(ctx, id, false, self_transition_mode == self_force_to_start_state, false);
        }
        m_echo_next_update = false;
    }
    const auto waiting = std::ranges::find_if(m_delayed, [this](const DelayedTransition& d) {
        return (flags_of(d.prospective.transition) & 2) != 0;
    });
    if (waiting != m_delayed.end()) {
        const DelayedTransition chosen = *waiting;
        start(ctx, chosen.prospective, chosen.return_to_previous);
        m_delayed.clear();
    }
}

void StateMachine::handle_event(Context& ctx, const Event& event) {
    if (!is_active) {
        return;
    }
    m_retired.clear();
    apply_interval_events(state_list(size_t(m_current_index)), m_state_flags, event.id);
    apply_interval_events(wildcard_list(), m_wildcard_flags, event.id);

    const bool higher = event.id == transition_to_next_higher_state_event_id;
    if (event.id != -1 && (higher || event.id == transition_to_next_lower_state_event_id)) {
        if (states.size() > 1) {
            std::optional<int32_t> next, wrapped;
            for (const StateInfo& s : states) {
                const int32_t id = s.state_id;
                if ((higher ? id > current_state_id : id < current_state_id) &&
                    (!next || (higher ? id < *next : id > *next))) {
                    next = id;
                }
                if (!wrapped || (higher ? id < *wrapped : id > *wrapped)) {
                    wrapped = id;
                }
            }
            if (next || wrap_around_state_id) {
                go_to_state(ctx, next ? *next : *wrapped, false, true, false);
            }
        }
        return;
    }
    if (!blocked()) {
        Prospects prospects;
        gather(ctx, event.id, false, std::nullopt, prospects);
        process(ctx, std::move(prospects), event.id, std::nullopt);
    }
}

bool StateMachine::blocked() const {
    if (!m_transitions.empty() &&
        (m_transitions.back().transition.info->flags & TransitionInfo::flag_uninterruptible_while_playing)) {
        return true;
    }
    return any_uninterruptible_delayed();
}

bool StateMachine::any_uninterruptible_delayed() const {
    return std::ranges::any_of(m_delayed, [](const DelayedTransition& d) {
        const int32_t flags = d.prospective.transition.info->flags;
        return (flags & TransitionInfo::flag_uninterruptible_while_delayed) != 0;
    });
}

// The tests before the condition have no effects, so their order doesn't matter and the event, which rejects
// nearly everything, goes first. The condition stays last: a random operator in one must only advance the
// generator when reached.
bool StateMachine::passes(Context& ctx, const TransitionInfo& t, const Reference& r, int32_t event,
                          bool any_event, std::optional<int32_t> target) const {
    using F = TransitionInfo::Flag;
    const bool event_matches =
        any_event || t.event_id == event ||
        (event != -1 &&
         ((event == return_to_previous_state_event_id && t.to_state_id == previous_state_id &&
           previous_state_id != current_state_id && !(t.flags & F::flag_disallow_return_to_previous_state)) ||
          (event == random_transition_event_id && !(t.flags & F::flag_disallow_random_transition))));
    if (!event_matches || (t.flags & F::flag_disabled) || (target && t.to_state_id != *target)) {
        return false;
    }
    const int32_t to_index = state_index(t.to_state_id);
    if (to_index < 0 || !states[size_t(to_index)].enable) {
        return false;
    }
    const bool wildcard = r.from_state_index < 0;
    if (wildcard && (!(t.flags & F::flag_is_local_wildcard) ||
                     (t.to_state_id == current_state_id &&
                      !(t.flags & F::flag_allow_self_transition_by_transition_from_any_state)))) {
        return false; // without LOCAL it exists only as a global transition
    }
    const auto& flags = wildcard ? m_wildcard_flags : m_state_flags;
    if ((t.flags & F::flag_use_trigger_interval) && !flags.empty() && !(flags_of(r) & 1)) {
        return false;
    }
    if (t.flags & F::flag_from_nested_state_id_is_valid) {
        const StateMachine* nested = states[size_t(m_current_index)].nested;
        const Node* live = nested ? ctx.graph.clone_of(nested) : nullptr;
        if (!live || static_cast<const StateMachine*>(live)->current_state_id != t.from_nested_state_id) {
            return false;
        }
    }
    return !t.has_condition || (t.flags & F::flag_disable_condition) || t.condition.is_true(ctx);
}

bool StateMachine::ready(const Prospective& p) const {
    const int32_t flags = p.transition.info->flags;
    return (!(flags & TransitionInfo::flag_use_initiate_interval) || (flags_of(p.transition) & 2)) &&
           !(flags & TransitionInfo::flag_abut_at_end_of_from_generator);
}

void StateMachine::gather_list(Context& ctx, bool wildcard, int32_t event, bool any_event,
                               std::optional<int32_t> target, Prospects& out) const {
    const auto& list = wildcard ? wildcard_list() : state_list(size_t(m_current_index));
    for (size_t i = 0; i < list.size(); ++i) {
        const TransitionInfo& t = list[i];
        const Reference r{wildcard ? -1 : m_current_index, static_cast<int32_t>(i), nullptr, &t};
        if (!passes(ctx, t, r, event, any_event, target)) {
            continue;
        }
        const Prospective p{r, r, t.to_state_id};
        if (out.empty()) {
            out.push_back(p);
            continue;
        }
        const int16_t first = out.front().transition.info->priority;
        if (t.priority > first) {
            out.assign(1, p);
        } else if (t.priority == first && !ready(out.front())) {
            if (ready(p)) {
                out.assign(1, p);
            } else {
                out.push_back(p);
            }
        }
    }
}

void StateMachine::gather(Context& ctx, int32_t event, bool any_event, std::optional<int32_t> target,
                          Prospects& out) const {
    if (!any_event && event != -1 && event == random_transition_event_id) {
        Prospects all;
        std::vector<int32_t> targets;
        const auto add_list = [&](bool wildcard) {
            const auto& list = wildcard ? wildcard_list() : state_list(size_t(m_current_index));
            for (size_t i = 0; i < list.size(); ++i) {
                const int32_t from = wildcard ? -1 : m_current_index;
                const Reference r{from, static_cast<int32_t>(i), nullptr, &list[i]};
                if (passes(ctx, list[i], r, event, false, target)) {
                    all.push_back({r, r, list[i].to_state_id});
                    targets.push_back(state_index(list[i].to_state_id));
                }
            }
        };
        add_list(false);
        add_list(true);
        if (!all.empty()) {
            out.push_back(all[weighted_choice(ctx, targets)]);
        }
        return;
    }
    Prospects from_state, from_wildcards;
    gather_list(ctx, false, event, any_event, target, from_state);
    gather_list(ctx, true, event, any_event, target, from_wildcards);
    // "Ready" here looks only at the initiate interval, not at ABUT.
    const auto in_interval = [this](const Prospects& list) {
        if (list.empty()) {
            return false;
        }
        const Reference& r = list.front().transition;
        return !(r.info->flags & TransitionInfo::flag_use_initiate_interval) || (flags_of(r) & 2) != 0;
    };
    const bool state_ready = in_interval(from_state);
    const bool wildcard_ready = in_interval(from_wildcards);
    const auto priority = [](const Prospects& list) { return list.front().transition.info->priority; };
    if (state_ready && wildcard_ready) {
        const bool wildcard = priority(from_wildcards) > priority(from_state);
        out.push_back(wildcard ? from_wildcards.front() : from_state.front());
    } else if (state_ready) {
        out.push_back(from_state.front());
    } else if (wildcard_ready) {
        out.push_back(from_wildcards.front());
    } else if (from_wildcards.empty()) {
        out = std::move(from_state);
    } else if (from_state.empty() || priority(from_wildcards) > priority(from_state)) {
        out = std::move(from_wildcards);
    } else {
        out = std::move(from_state);
    }
}

bool StateMachine::return_to_previous_of(const Prospective& p, int32_t event) const {
    return event != -1 && event == return_to_previous_state_event_id && p.to_state_id == previous_state_id &&
           previous_state_id != current_state_id &&
           !(p.transition.info->flags & TransitionInfo::flag_disallow_return_to_previous_state);
}

// Dead in vanilla, whose validator rejects ABUT; `ready` alone answers for every other transition.
bool StateMachine::ready_to_start(Context& ctx, const Prospective& p) const {
    const TransitionInfo& t = *p.transition.info;
    if ((t.flags & TransitionInfo::flag_use_initiate_interval) && !(flags_of(p.transition) & 2)) {
        return false;
    }
    if (!(t.flags & TransitionInfo::flag_abut_at_end_of_from_generator)) {
        return true;
    }
    const SyncInfo* s = sync_of(ctx, states[size_t(m_current_index)].generator);
    if (!s || s->base_frequency == 0.0f) {
        return true;
    }
    if (s->cyclic) {
        return false;
    }
    const auto* effect = static_cast<const BlendingTransitionEffect*>(p.effect.info->transition);
    return 1.0f / s->base_frequency - s->local_time - (effect ? effect->duration : 0.0f) <= 0.0f;
}

void StateMachine::process(Context& ctx, Prospects entries, int32_t event,
                           std::optional<bool> forced_return) {
    if (entries.empty()) {
        return;
    }
    const auto return_of = [&](const Prospective& p) {
        return forced_return ? *forced_return : return_to_previous_of(p, event);
    };
    if (!is_active) {
        set_state(ctx, entries.front().to_state_id, return_of(entries.front()), false);
        return;
    }
    const int16_t first_priority = entries.front().transition.info->priority;
    if (!m_delayed.empty()) {
        // Unlike the branch below, this doesn't consult the active transition's protection against priority.
        if (m_delayed.front().prospective.transition.info->priority > first_priority) {
            return;
        }
        const auto same = [this](const Prospective& p) {
            return std::ranges::any_of(m_delayed, [&p](const DelayedTransition& d) {
                return d.prospective.transition.info == p.transition.info;
            });
        };
        if (any_uninterruptible_delayed()) {
            return;
        }
        std::erase_if(entries, same);
        if (entries.empty()) {
            return;
        }
    } else if (!m_transitions.empty()) {
        const TransitionInfo& newest = *m_transitions.back().transition.info;
        // Strictly higher: equal priority interrupts.
        const bool shielded = (newest.flags & TransitionInfo::flag_uninterruptible_while_playing) != 0;
        if (shielded || newest.priority > first_priority) {
            return;
        }
    }
    m_delayed.clear();
    for (const Prospective& p : entries) {
        if (ready_to_start(ctx, p)) {
            start(ctx, p, return_of(p));
            return;
        }
    }
    for (const Prospective& p : entries) {
        m_delayed.push_back({p, return_of(p)});
    }
    ctx.event_triggered_transition |= 2;
}

void StateMachine::start(Context& ctx, const Prospective& p, bool return_to_previous) {
    const TransitionInfo& info = *p.transition.info;
    if (info.flags & TransitionInfo::flag_to_nested_state_id_is_valid) {
        // For a global transition the target state is in the machine holding the wildcard, not in this one.
        const StateMachine* holder = p.transition.machine ? p.transition.machine : this;
        const int32_t index = holder->state_index(info.to_state_id);
        const StateMachine* nested = index >= 0 ? holder->states[size_t(index)].nested : nullptr;
        if (nested) {
            set_start_override(ctx, nested, nested->state_index(info.to_nested_state_id));
        }
    }
    write_sync(ctx, p.to_state_id);

    const auto* effect = static_cast<const BlendingTransitionEffect*>(p.effect.info->transition);
    const int32_t to_index = state_index(p.to_state_id);
    if (!effect || to_index < 0) {
        end_all_transitions(ctx);
        set_state(ctx, p.to_state_id, return_to_previous, false);
    } else {
        if (!m_transitions.empty() && std::ssize(m_transitions) >= max_simultaneous_transitions) {
            end_transitions(ctx, 0);
        }
        // The root keys a record by this object and clones it, so one effect in the file can run twice at
        // once.
        auto instance = std::make_shared<BlendingTransitionEffect>(*effect);
        instance->to = states[size_t(to_index)].generator;
        // How interruption works: the new effect blends from the old one, whose own blend carries on
        // underneath.
        instance->from = states[size_t(m_current_index)].generator;
        if (!m_transitions.empty()) {
            instance->from = m_transitions.back().effect.get();
        }
        m_transitions.push_back({std::move(instance), p.transition, p.to_state_id, return_to_previous});
        if (!(info.flags & TransitionInfo::flag_delay_state_change)) {
            set_state(ctx, p.to_state_id, return_to_previous, true);
        }
    }
    m_state_or_transition_changed = true;
    ctx.graph.request_update_active_nodes();
    ctx.event_triggered_transition = 1;
    m_delayed.clear();
}

void StateMachine::end_transitions(Context& ctx, int32_t index) {
    if (index < 0) {
        return;
    }
    size_t last = size_t(index);
    const int32_t target = m_transitions[last].to_state_id;
    while (last + 1 < m_transitions.size() && m_transitions[last + 1].to_state_id == target) {
        ++last;
    }
    if (last + 1 == m_transitions.size()) {
        const ActiveTransition& newest = m_transitions[last];
        if (newest.transition.info->flags & TransitionInfo::flag_delay_state_change) {
            set_state(ctx, newest.to_state_id, newest.return_to_previous, false);
        }
    } else {
        const int32_t target_index = state_index(target);
        Node* generator = target_index >= 0 ? states[size_t(target_index)].generator : nullptr;
        BlendingTransitionEffect* survivor = m_transitions[last + 1].effect.get();
        survivor->from = generator;
        if (Node* live = ctx.graph.clone_of(survivor)) {
            static_cast<BlendingTransitionEffect*>(live)->from = generator;
        }
    }
    for (size_t i = 0; i <= last; ++i) {
        m_retired.push_back(std::move(m_transitions[i].effect));
    }
    m_transitions.erase(m_transitions.begin(), m_transitions.begin() + static_cast<std::ptrdiff_t>(last) + 1);
    m_state_or_transition_changed = true;
    ctx.graph.request_update_active_nodes();
}

void StateMachine::reset_intervals(size_t index) {
    const auto reset = [](const std::vector<TransitionInfo>& list, std::vector<uint8_t>& flags) {
        if (flags.size() < list.size()) {
            flags.resize(list.size());
        }
        for (size_t i = 0; i < list.size(); ++i) {
            flags[i] = initial_interval_bits(list[i]);
        }
    };
    reset(state_list(index), m_state_flags);
    reset(wildcard_list(), m_wildcard_flags);
}

void StateMachine::set_state(Context& ctx, int32_t state_id, bool return_to_previous, bool notify_children) {
    const int32_t index = state_index(state_id);
    if (index < 0) {
        return;
    }
    reset_intervals(size_t(index));
    if (is_active) {
        send_exit(ctx);
        if (notify_children) {
            notify_nested(ctx, false);
        }
    }
    previous_state_id = return_to_previous ? state_id : current_state_id;
    current_state_id = state_id;
    m_current_index = index;
    m_entered = false;
    send_enter(ctx);
    if (notify_children) {
        notify_nested(ctx, true);
    }
}

void StateMachine::send_enter(Context& ctx) {
    if (m_entered) {
        return;
    }
    m_entered = true;
    if (const auto& list = states[size_t(m_current_index)].enter_notify_events) {
        for (const NotifyEvent& e : list->events) {
            ctx.graph.raise(ctx, e.event);
        }
    }
}

void StateMachine::send_exit(Context& ctx) {
    if (!m_entered) {
        return;
    }
    m_entered = false;
    if (const auto& list = states[size_t(m_current_index)].exit_notify_events) {
        for (const NotifyEvent& e : list->events) {
            ctx.graph.raise(ctx, e.event);
        }
    }
}

// With an effect the old subgraph stays alive during the blend, so its nested machines wouldn't otherwise
// send their events at the moment of the change.
void StateMachine::notify_nested(Context& ctx, bool enter) {
    Behavior* saved = ctx.behavior;
    const StateMachine* level = this;
    while (true) {
        const StateInfo& state = level->states[size_t(level->m_current_index)];
        auto* live = state.nested ? static_cast<StateMachine*>(ctx.graph.clone_of(state.nested)) : nullptr;
        if (!live || !live->is_active) {
            break;
        }
        ctx.behavior = state.nested_behavior;
        if (enter) {
            live->send_enter(ctx);
        } else {
            live->send_exit(ctx);
        }
        level = live;
    }
    ctx.behavior = saved;
}

void StateMachine::go_to_state(Context& ctx, int32_t state_id, bool immediate, bool force,
                               bool return_to_previous) {
    if (is_active && !immediate) {
        if (!blocked()) {
            Prospects prospects;
            gather(ctx, -1, true, state_id, prospects);
            if (!prospects.empty()) {
                process(ctx, std::move(prospects), -1, return_to_previous);
                return;
            }
        }
        if (!force) {
            return;
        }
    }
    end_all_transitions(ctx);
    m_delayed.clear();
    m_state_or_transition_changed = true;
    ctx.graph.request_update_active_nodes();
    ctx.event_triggered_transition = 1;
    set_state(ctx, state_id, return_to_previous, false);
    write_sync(ctx, state_id);
}

void StateMachine::start_global(Context& ctx, Prospective p) {
    Prospects from_state, from_wildcards;
    gather_list(ctx, false, -1, true, p.to_state_id, from_state);
    gather_list(ctx, true, -1, true, p.to_state_id, from_wildcards);
    const Prospects* best = nullptr;
    if (!from_state.empty() && !from_wildcards.empty()) {
        const int16_t state_priority = from_state.front().transition.info->priority;
        const bool wildcard = from_wildcards.front().transition.info->priority > state_priority;
        best = wildcard ? &from_wildcards : &from_state;
    } else if (!from_state.empty() || !from_wildcards.empty()) {
        best = from_state.empty() ? &from_wildcards : &from_state;
    }
    if (best && best->front().transition.info->priority > p.transition.info->priority) {
        p.effect = best->front().transition;
    }
    start(ctx, p, false);
}

void StateMachine::fire_global(Context& ctx, const GlobalTransitions& table, int32_t entry,
                               int32_t wildcard) {
    const GlobalTransitions::Entry* child = &table.entries[size_t(entry)];
    const TransitionInfo& info = child->machine->wildcard_list()[size_t(wildcard)];
    set_start_override(ctx, child->machine, child->machine->state_index(info.to_state_id));

    // Climb to the first active machine; every level passed will start in the state leading down.
    while (child->parent >= 0) {
        const GlobalTransitions::Entry& parent = table.entries[size_t(child->parent)];
        auto* live = static_cast<StateMachine*>(ctx.graph.clone_of(parent.machine));
        if (live && live->is_active) {
            const Reference r{-1, wildcard, table.entries[size_t(entry)].machine, &info};
            const Prospective p{r, r, parent.machine->states[size_t(child->parent_state)].state_id};
            Behavior* saved = ctx.behavior;
            ctx.behavior = parent.behavior;
            live->start_global(ctx, p);
            ctx.behavior = saved;
            return;
        }
        set_start_override(ctx, parent.machine, child->parent_state);
        child = &parent;
    }
}

void StateMachine::link(const ExpressionNames& names) {
    // The game's id map holds non-zero ids only, and renumbers a later state of id 0 while building it.
    for (size_t i = 1; i < states.size(); ++i) {
        if (states[i].state_id == 0) {
            states[i].state_id = static_cast<int32_t>(i);
        }
    }
    const auto link_list = [&](std::vector<TransitionInfo>& list) {
        for (TransitionInfo& t : list) {
            const auto fixed_time = [](const TimeInterval& i) {
                return i.enter_event_id == -1 && i.exit_event_id == -1;
            };
            if (t.event_id == -1) {
                log::warn("{}: a transition has no event", name);
            }
            if (((t.flags & TransitionInfo::flag_use_trigger_interval) && fixed_time(t.trigger_interval)) ||
                ((t.flags & TransitionInfo::flag_use_initiate_interval) && fixed_time(t.initiate_interval))) {
                log::warn("{} has a fixed time transition, not supported by Bethesda Game Studios", name);
            }
            if (t.has_condition && t.condition.condition_string.empty()) {
                std::string text = t.condition.expression;
                std::erase_if(text, [](char c) { return c == '\t' || c == '\n'; });
                auto set = std::make_shared<CompiledExpressions>();
                compile_expression(text, names, *set);
                t.condition.compiled = std::move(set);
            }
        }
    };
    for (StateInfo& s : states) {
        if (s.transitions) {
            link_list(s.transitions->transitions);
        }
    }
    if (wildcard_transitions) {
        link_list(wildcard_transitions->transitions);
    }
}

namespace {

// The machines first reached going down from `node`, with the behaviour each lives in.
using FoundMachines = std::vector<std::pair<StateMachine*, Behavior*>>;

void find_machines(Node* node, Behavior* behavior, std::unordered_set<const Node*>& seen,
                   FoundMachines& out) {
    if (!node || !seen.insert(node).second) {
        return;
    }
    if (auto* machine = dynamic_cast<StateMachine*>(node)) {
        out.emplace_back(machine, behavior);
        return;
    }
    Behavior* below = node->behavior_below();
    std::vector<Child> kids;
    node->children(0, kids);
    for (const Child& k : kids) {
        find_machines(k.node, below ? below : behavior, seen, out);
    }
}

// Which machine a state holds when several are reachable isn't in the design. The table has one entry per
// state, so the first found takes it; every machine still gets its own entry, with that state as its parent.
std::shared_ptr<GlobalTransitions> build_global_transitions(const Project& project) {
    auto table = std::make_shared<GlobalTransitions>();
    std::unordered_map<const StateMachine*, int32_t> entries;
    const auto add = [&](StateMachine* machine, Behavior* behavior, int32_t parent, int32_t state) {
        if (entries.emplace(machine, static_cast<int32_t>(table->entries.size())).second) {
            table->entries.push_back({machine, behavior, parent, state});
        }
    };
    {
        Behavior& root = project.root();
        std::unordered_set<const Node*> seen;
        FoundMachines found;
        find_machines(root.root_generator, &root, seen, found);
        for (const auto& [machine, behavior] : found) {
            add(machine, behavior, -1, -1);
        }
    }
    for (size_t i = 0; i < table->entries.size(); ++i) {
        StateMachine* machine = table->entries[i].machine;
        Behavior* behavior = table->entries[i].behavior;
        for (size_t s = 0; s < machine->states.size(); ++s) {
            StateInfo& state = machine->states[s];
            std::unordered_set<const Node*> seen;
            FoundMachines found;
            find_machines(state.generator, behavior, seen, found);
            if (!found.empty()) {
                state.nested = found.front().first;
                state.nested_behavior = found.front().second;
            }
            for (const auto& [nested, nested_behavior] : found) {
                add(nested, nested_behavior, static_cast<int32_t>(i), static_cast<int32_t>(s));
            }
        }
        if (!machine->wildcard_transitions) {
            continue;
        }
        // "A map from event id to wildcard index": one wildcard per event and machine, the earlier one.
        std::unordered_set<int32_t> events;
        const auto& wildcards = machine->wildcard_transitions->transitions;
        for (size_t w = 0; w < wildcards.size(); ++w) {
            const TransitionInfo& t = wildcards[w];
            if (!(t.flags & TransitionInfo::flag_is_global_wildcard) || t.event_id < 0 ||
                size_t(t.event_id) >= behavior->event_to_external.size()) {
                continue;
            }
            const int32_t external = behavior->event_to_external[size_t(t.event_id)];
            if (events.insert(external).second) {
                table->listeners[external].push_back({static_cast<int32_t>(i), static_cast<int32_t>(w)});
            }
        }
    }
    return table;
}

void link_behavior(const Project& project, Behavior& behavior) {
    const ExpressionNames names(project, behavior);
    std::unordered_set<const Node*> seen;
    std::vector<Node*> pending{behavior.root_generator};
    std::vector<Child> kids;
    while (!pending.empty()) {
        Node* n = pending.back();
        pending.pop_back();
        if (!n || !seen.insert(n).second) {
            continue;
        }
        if (auto* machine = dynamic_cast<StateMachine*>(n)) {
            machine->link(names);
        } else if (auto* modifier = dynamic_cast<EvaluateExpressionModifier*>(n)) {
            modifier->link(names);
        }
        kids.clear();
        n->children(children_ignore_references, kids);
        for (const Child& k : kids) {
            pending.push_back(k.node);
        }
    }
}

} // namespace

void link_state_machines(Project& project) {
    for (const auto& behavior : project.behaviors()) {
        link_behavior(project, *behavior);
    }
    project.global_transitions = build_global_transitions(project);
}

// "Firing": reaches machines that are not active. Only runs when no local
// transition started.
void Graph::global_transitions(Context& ctx, int32_t external, const std::string*) {
    const GlobalTransitions* table = m_project->global_transitions.get();
    if (!table) {
        return;
    }
    const auto it = table->listeners.find(external);
    if (it == table->listeners.end()) {
        return;
    }
    Behavior* saved = ctx.behavior;
    for (const GlobalTransitions::Listener& l : it->second) {
        const GlobalTransitions::Entry& entry = table->entries[size_t(l.entry)];
        if (m_index.contains(entry.machine)) {
            continue;
        }
        const TransitionInfo& t = entry.machine->wildcard_transitions->transitions[size_t(l.wildcard)];
        // Neither FLAG_DISABLE_CONDITION, the intervals nor FLAG_DISABLED are consulted on this path.
        ctx.behavior = entry.behavior;
        if (!t.has_condition || t.condition.is_true(ctx)) {
            StateMachine::fire_global(ctx, *table, l.entry, l.wildcard);
        }
    }
    ctx.behavior = saved;
}

void register_state_machine_nodes() {
    register_node<StateMachine>("hkbStateMachine");
    register_node<BlendingTransitionEffect>("hkbBlendingTransitionEffect");
    register_node<EvaluateExpressionModifier>("hkbEvaluateExpressionModifier");
}

} // namespace spud::anim
