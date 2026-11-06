#include "game/inventory.hpp"

#include <algorithm>

namespace spud::game {

void Inventory::add(uint32_t form, int32_t count, const ItemExtra& extra) {
    if (count <= 0) {
        return;
    }
    if (m_listener) {
        m_listener->inventory_changed(m_owner, form, count);
    }
    for (Stack& s : m_stacks) {
        if (s.form == form && s.extra == extra) {
            s.count += count;
            return;
        }
    }
    m_stacks.push_back({form, count, extra});
}

int32_t Inventory::remove_at(size_t index, int32_t count) {
    Stack& s = m_stacks[index];
    const int32_t taken = std::min(s.count, count);
    s.count -= taken;
    if (m_listener && taken > 0) {
        m_listener->inventory_changed(m_owner, s.form, -taken);
    }
    if (s.count <= 0) {
        m_stacks.erase(m_stacks.begin() + std::ptrdiff_t(index));
    }
    return taken;
}

int32_t Inventory::remove(uint32_t form, int32_t count) {
    int32_t removed = 0;
    for (int pass = 0; pass < 3 && removed < count; ++pass) {
        for (size_t i = m_stacks.size(); i-- > 0 && removed < count;) {
            const Stack& s = m_stacks[i];
            const int rank = s.worn != Worn::none ? 2 : s.extra.owner ? 1 : 0;
            if (s.form == form && rank == pass) {
                removed += remove_at(i, count - removed);
            }
        }
    }
    return removed;
}

int32_t Inventory::count(uint32_t form) const {
    int32_t n = 0;
    for (const Stack& s : m_stacks) {
        if (s.form == form) {
            n += s.count;
        }
    }
    return n;
}

float Inventory::weight(const Items& items) const {
    float total = 0;
    for (const Stack& s : m_stacks) {
        if (const Item* i = items.find(s.form)) {
            total += i->weight * float(s.count);
        }
    }
    return total;
}

} // namespace spud::game
