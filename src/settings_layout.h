#pragma once

#include <cstddef>

// Settings page geometry, kept free of window state so it can be unit-tested.
// A tab is an ordered list of cards; each card has a header and a list of row
// IDs. Everything - where a card starts, where a row sits, how tall the tab
// is - derives from that list, so moving a row between cards or adding a
// card needs no hand-edited offsets.
//
// A "card" is any type with `const int* rows` and `int count` members.
namespace leanlauncher {
namespace settings_layout {

inline constexpr float kRowHeight = 47.0f;
// The first card's header sits 16px from the top; its card 20px below that.
inline constexpr float kFirstHeaderTop = 16.0f;
// Spacing shared by every card: header to its card, card to the next header.
inline constexpr float kHeaderToCard = 20.0f;
inline constexpr float kCardToNextHeader = 18.0f;
inline constexpr float kBottomPadding = 16.0f;

// Where a row sits in a tab: which card and which position inside it.
struct RowSlot {
    int card = -1;
    int rank = -1;
    constexpr bool Found() const { return card >= 0; }
};

template <class Card>
constexpr float CardHeaderTop(const Card* cards, std::size_t count, std::size_t index) {
    float top = kFirstHeaderTop;
    for (std::size_t i = 0; i < index && i < count; ++i) {
        top += kHeaderToCard + cards[i].count * kRowHeight + kCardToNextHeader;
    }
    return top;
}

template <class Card>
constexpr float CardTop(const Card* cards, std::size_t count, std::size_t index) {
    return CardHeaderTop(cards, count, index) + kHeaderToCard;
}

// Scrollable height of a tab: the last card's bottom plus padding.
template <class Card>
constexpr float ContentBottom(const Card* cards, std::size_t count) {
    if (count == 0) return 0.0f;
    return CardTop(cards, count, count - 1) + cards[count - 1].count * kRowHeight + kBottomPadding;
}

template <class Card>
constexpr RowSlot FindRow(const Card* cards, std::size_t count, int row) {
    for (std::size_t c = 0; c < count; ++c) {
        for (int r = 0; r < cards[c].count; ++r) {
            if (cards[c].rows[r] == row) return RowSlot{static_cast<int>(c), r};
        }
    }
    return RowSlot{};
}

// Top of `row`, or 0 if it is not in this tab.
template <class Card>
constexpr float RowTop(const Card* cards, std::size_t count, int row) {
    const RowSlot slot = FindRow(cards, count, row);
    if (!slot.Found()) return 0.0f;
    return CardTop(cards, count, static_cast<std::size_t>(slot.card)) + slot.rank * kRowHeight;
}

} // namespace settings_layout
} // namespace leanlauncher
