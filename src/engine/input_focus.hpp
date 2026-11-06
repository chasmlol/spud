#pragma once

// Who gets keyboard and mouse input: the game UI while a menu or the console is open,
// the game otherwise (the free camera today, player controls later). Every SDL event
// goes through route() before game code looks at it, and game code that polls state
// (SDL_GetKeyboardState, mouse deltas) checks game() first. That is the whole contract,
// so new game controls only need to respect these two calls.

#include "ui/ui.hpp"

namespace spud::engine {

class InputFocus {
public:
    void attach(ui::Ui* ui) { m_ui = ui; }

    // Offers an event to the UI first. True: it was consumed and the game must ignore it.
    bool route(const SDL_Event& e) { return m_ui && m_ui->handle_event(e); }

    // True while the game owns the keyboard and mouse.
    bool game() const { return !m_ui || !m_ui->captures_input(); }

private:
    ui::Ui* m_ui = nullptr;
};

} // namespace spud::engine
