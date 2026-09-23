#pragma once

// Frozen copy of takeoff::MatchScore and its helpers exactly as they were
// before the NFR-014 speed-up (v1.6.1, src/search.h). core_tests.cpp checks
// that the optimized scorer returns the same score as this one for every
// name/query pair in its corpus. Never edit this to follow search.h - it is
// the "before" side of that comparison.

#include <algorithm>
#include <string>
#include <string_view>

namespace reference_scorer {

inline std::wstring Condense(std::wstring_view value) {
    std::wstring condensed;
    condensed.reserve(value.size());
    for (wchar_t ch : value) {
        if (ch != L' ') condensed.push_back(ch);
    }
    return condensed;
}

inline bool MatchAcronym(std::wstring_view target, std::wstring_view query, bool& exact) {
    exact = false;
    if (query.empty() || target.empty()) return false;
    std::wstring initials;
    initials.push_back(target[0]);
    for (size_t i = 1; i < target.size(); ++i) {
        if (target[i - 1] == L' ' && target[i] != L' ') {
            initials.push_back(target[i]);
        }
    }
    if (query == initials) {
        exact = true;
        return true;
    }
    if (initials.rfind(query, 0) == 0) {
        exact = false;
        return true;
    }
    return false;
}

inline bool MatchTokens(std::wstring_view target, std::wstring_view query) {
    if (query.find(L' ') == std::wstring_view::npos) return false;
    size_t qStart = 0;
    size_t tPos = 0;
    while (qStart < query.size()) {
        size_t qEnd = query.find(L' ', qStart);
        if (qEnd == std::wstring_view::npos) qEnd = query.size();
        std::wstring_view token = query.substr(qStart, qEnd - qStart);
        if (token.empty()) {
            qStart = qEnd + 1;
            continue;
        }
        bool found = false;
        while (tPos < target.size()) {
            if (tPos == 0 || target[tPos - 1] == L' ') {
                if (target.substr(tPos).rfind(token, 0) == 0) {
                    found = true;
                    while (tPos < target.size() && target[tPos] != L' ') ++tPos;
                    if (tPos < target.size() && target[tPos] == L' ') ++tPos;
                    break;
                }
            }
            ++tPos;
        }
        if (!found) return false;
        qStart = qEnd + 1;
    }
    return true;
}

inline int MatchScore(std::wstring_view name, std::wstring_view query) {
    if (query.empty()) return -1;
    if (name == query) return 10000;

    // Condense() heap-allocates; skip it entirely when neither string has a
    // space to collapse - the condensed forms would just equal the
    // originals, which the `name == query` check above already ruled out.
    // This is the common case (most app/file names and queries are a single
    // word), and MatchScore runs once per candidate on every keystroke.
    if (query.find(L' ') != std::wstring_view::npos || name.find(L' ') != std::wstring_view::npos) {
        const std::wstring queryCondensed = Condense(query);
        const std::wstring nameCondensed = Condense(name);
        if (!queryCondensed.empty() && queryCondensed == nameCondensed) {
            return 9500;
        }
    }

    // Check word boundary matches
    int bestWordMatch = -1;
    size_t position = 0;
    while ((position = name.find(query, position)) != std::wstring_view::npos) {
        if (position == 0 || name[position - 1] == L' ') {
            int wordIndex = 0;
            for (size_t k = 0; k < position; ++k) {
                if (name[k] == L' ') ++wordIndex;
            }
            const bool isFullWord = (position + query.size() == name.size() ||
                                     name[position + query.size()] == L' ');
            int s = 0;
            if (isFullWord) {
                s = (position == 0)
                    ? (9200 - static_cast<int>(name.size() - query.size()))
                    : (9000 - wordIndex * 20 - static_cast<int>(name.size()));
            } else {
                s = (position == 0)
                    ? (8500 - static_cast<int>(name.size() - query.size()))
                    : (8000 - wordIndex * 20 - static_cast<int>(name.size()));
            }
            if (s > bestWordMatch) bestWordMatch = s;
        }
        ++position;
    }
    if (bestWordMatch > 0) {
        return bestWordMatch;
    }

    bool exactAcronym = false;
    if (MatchAcronym(name, query, exactAcronym)) {
        return exactAcronym
            ? 7800 - static_cast<int>(name.size())
            : 7400 - static_cast<int>(name.size());
    }

    if (MatchTokens(name, query)) {
        return 7200 - static_cast<int>(name.size());
    }

    const size_t containedAt = name.find(query);
    if (containedAt != std::wstring_view::npos) {
        return 7000 - static_cast<int>(containedAt * 8 + name.size());
    }

    size_t queryIndex = 0;
    size_t previous = std::wstring_view::npos;
    int gaps = 0;
    int consecutive = 0;
    int bestConsecutive = 0;
    int boundaryHits = 0;
    for (size_t i = 0; i < name.size() && queryIndex < query.size(); ++i) {
        if (name[i] != query[queryIndex]) continue;
        if (i == 0 || name[i - 1] == L' ') {
            ++boundaryHits;
        }
        if (previous != std::wstring_view::npos) {
            gaps += static_cast<int>(i - previous - 1);
        }
        consecutive = previous != std::wstring_view::npos && i == previous + 1
            ? consecutive + 1 : 1;
        bestConsecutive = (std::max)(bestConsecutive, consecutive);
        previous = i;
        ++queryIndex;
    }
    return queryIndex == query.size()
        ? 5000 + bestConsecutive * 20 + boundaryHits * 50 - gaps * 5 - static_cast<int>(name.size())
        : -1;
}

} // namespace reference_scorer
