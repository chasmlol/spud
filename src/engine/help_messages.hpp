#pragma once

// the hints Message.ShowAsHelpMessage puts on the HUD, shown every so often until the player does
// what they ask or they run out., 1.8.

#include <algorithm>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace spud::engine {

class HelpMessages {
public:
    std::function<void(const std::string& text)> post; // empty text: hide

    void show(const std::string& event, const std::string& text, float duration, float interval, int max_times) {
        if (std::ranges::find(m_entries, event, &Entry::event) != m_entries.end() || done(event)) {
            return;
        }
        m_entries.push_back({event, text, duration > 0.0f ? duration : std::numeric_limits<float>::max(), interval,
                             max_times > 0 ? max_times : -1, 0.0f});
        display(int(m_entries.size()) - 1);
    }

    void reset(const std::string& event) { std::erase(m_done, event); }

    // The user event `event` came in.
    void input(const std::string& event) {
        const auto it = std::ranges::find(m_entries, event, &Entry::event);
        if (it == m_entries.end()) {
            return;
        }
        m_done.push_back(event);
        if (int(it - m_entries.begin()) == m_current) {
            post("");
            m_current = -1;
        }
    }

    void update(float dt) {
        for (int i = 0; i < int(m_entries.size());) {
            Entry& e = m_entries[size_t(i)];
            e.timer += dt;
            if (done(e.event)) {
                m_entries.erase(m_entries.begin() + i);
                m_current -= i < m_current ? 1 : 0;
                continue;
            }
            if (i == m_current) {
                if (e.timer >= e.duration) {
                    post("");
                    finish_if_spent(e);
                    m_current = -1;
                }
            } else if (e.timer >= e.interval) {
                display(i);
            }
            ++i;
        }
    }

private:
    struct Entry {
        std::string event, text;
        float duration, interval;
        int count; // shows left, below 0 for no limit
        float timer;
    };

    bool done(const std::string& event) const { return std::ranges::find(m_done, event) != m_done.end(); }
    void finish_if_spent(const Entry& e) {
        if (e.count == 0) {
            m_done.push_back(e.event);
        }
    }
    void display(int i) {
        Entry& e = m_entries[size_t(i)];
        post(e.text);
        --e.count;
        e.timer = 0.0f;
        if (m_current >= 0 && m_current != i) {
            finish_if_spent(m_entries[size_t(m_current)]);
        }
        m_current = i;
    }

    std::vector<Entry> m_entries;
    std::vector<std::string> m_done;
    int m_current = -1;
};

} // namespace spud::engine
