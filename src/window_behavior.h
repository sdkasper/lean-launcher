#pragma once

#include <algorithm>
#include <string>

#include "search.h"
#include "settings.h"

// Pure decisions behind the window-behaviour settings (issue #6), kept out of
// launcher.h so they can be unit-tested.
namespace leanlauncher {
namespace window_behavior {

// Losing focus hides a visible launcher unless "stay open" is on or a modal
// dialog of ours has focus.
inline bool ShouldHideOnDeactivate(const takeoff::Settings& settings, bool visible, bool modalOpen) {
    return visible && !modalOpen && !settings.keepOpenOnFocusLoss;
}

// Puts the remembered query back with everything selected, so typing starts a
// new search and Enter or the arrow keys still work on the old one.
inline void RestoreQuery(takeoff::SearchInput& input, const std::wstring& last) {
    input.Clear();
    if (last.empty()) return;
    input.text = last.substr(0, takeoff::SearchInput::kLimit);
    input.SelectAll();
}

// Horizontal scroll for a single-line text field: the smallest change to
// `scroll` that keeps the caret (x offset into the text) inside the visible
// `available` width, never negative. Shared by the search box and the Settings
// edit field.
inline float ScrollToCaret(float scroll, float caret, float available) {
    return (std::max)(0.0f, (std::max)(caret - available, (std::min)(scroll, caret)));
}

}  // namespace window_behavior
}  // namespace leanlauncher
