#pragma once

#include <cstddef>

// Settings page geometry that depends on how many rows a card has, kept free
// of window state so it can be unit-tested. The Search & Features card grows
// with every feature toggle (NFR-018), and everything below it in the "All"
// view moves with it - these helpers are the one place that arithmetic lives.
namespace leanlauncher {
namespace settings_layout {

inline constexpr float kRowHeight = 47.0f;
// Single-category views: every card starts right under a 16px header.
inline constexpr float kCategoryCardTop = 36.0f;
// "All" view: the SEARCH & FEATURES header and card.
inline constexpr float kAllSearchHeaderTop = 421.0f;
inline constexpr float kAllSearchCardTop = 441.0f;
// Spacing shared by every card: header to its card, card to the next header.
inline constexpr float kHeaderToCard = 20.0f;
inline constexpr float kCardToNextHeader = 18.0f;
inline constexpr float kBottomPadding = 16.0f;

constexpr float CategoryRowTop(int rank) {
    return kCategoryCardTop + rank * kRowHeight;
}

constexpr float AllSearchRowTop(int rank) {
    return kAllSearchCardTop + rank * kRowHeight;
}

constexpr float AllObsidianHeaderTop(int searchRowCount) {
    return kAllSearchCardTop + searchRowCount * kRowHeight + kCardToNextHeader;
}

constexpr float AllObsidianCardTop(int searchRowCount) {
    return AllObsidianHeaderTop(searchRowCount) + kHeaderToCard;
}

constexpr float SearchCategoryContentBottom(int searchRowCount) {
    return kCategoryCardTop + searchRowCount * kRowHeight + kBottomPadding;
}

// Obsidian is the last card in the "All" view.
constexpr float AllContentBottom(int searchRowCount, int obsidianRowCount) {
    return AllObsidianCardTop(searchRowCount) + obsidianRowCount * kRowHeight + kBottomPadding;
}

// Screen position of `row` in an ordered row list, or -1.
template <std::size_t N>
constexpr int RowRank(const int (&rows)[N], int row) {
    for (std::size_t i = 0; i < N; ++i) {
        if (rows[i] == row) return static_cast<int>(i);
    }
    return -1;
}

} // namespace settings_layout
} // namespace leanlauncher
