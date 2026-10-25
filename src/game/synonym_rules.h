#pragma once

// Pure Synonym rules recovered from FGP.exe. Keeping the arithmetic and
// recency ordering independent of SDL makes the executable contracts directly
// testable, including the original's counter-intuitive level-bonus formula.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <vector>

#include "game/question_recency.h"

namespace fgp {

struct SynonymRankCandidate {
    std::size_t entryIndex{};
    int difficulty{};
    int wordLength{};
    std::uint32_t lastSeen{};
    std::uint32_t stableId{};
};

inline std::uint32_t synonymMinuteStamp(std::time_t unixSeconds) {
    return questionMinuteStamp(unixSeconds);
}

inline bool synonymEntryEligible(int difficulty,
                                 int wordLength,
                                 const int *minimumLength,
                                 const int *maximumLength) {
    return difficulty >= 0 && difficulty < 3 &&
           wordLength >= minimumLength[difficulty] &&
           wordLength <= maximumLength[difficulty];
}

inline int synonymWordScore(int basePoints,
                            int wordLength,
                            int hintsUsed) {
    // level.ini caps hints at four and 0x00483BC0 rejects another Hint once
    // that cap is reached. The clamp only keeps a corrupt caller from making
    // C++ shift by an invalid count; it cannot change reachable game scores.
    const int divisor = 1 << std::clamp(hintsUsed, 0, 30);
    return basePoints * wordLength / divisor;
}

inline std::size_t synonymProgressFrame(std::size_t frameCount,
                                        double remainingFraction) {
    if (frameCount == 0U) {
        return 0U;
    }
    const double fraction = std::clamp(remainingFraction, 0.0, 1.0);
    const auto rounded = static_cast<std::size_t>(std::floor(
        static_cast<double>(frameCount - 1U) * fraction + 0.5));
    // 0x004824A0 clamps the original player's one-based frame to at least 1,
    // then seeks it through 0x00403770. Convert that result to our zero-based
    // decoded frame index.
    return std::clamp<std::size_t>(rounded, 1U, frameCount) - 1U;
}

inline std::size_t synonymLaunchedLetterCount(int elapsedMilliseconds,
                                              std::size_t wordLength) {
    if (wordLength==0U) return 0U;
    // The signed reciprocal sequence at 0x00484118-0x00484135 is division by
    // 133, then adds one and clamps to the answer length.
    const int released=std::max(0,elapsedMilliseconds)/133+1;
    return std::min(wordLength,static_cast<std::size_t>(released));
}

struct SynonymTransitionPosition {
    double x{};
    double y{};
    bool visible{};
};

inline SynonymTransitionPosition synonymTransitionPosition(
    double startX,double startY,double elapsedSeconds) {
    // Placed/Pass-converted balls take the 0x004842BA branch: x advances at
    // 400 px/s (0x0049F25C). Once x reaches the target rectangle's right edge
    // 543, y advances at the same speed until it reaches 361 and is removed.
    constexpr double speed=400.0;
    constexpr double rightEdge=543.0;
    constexpr double bottomEdge=361.0;
    const double elapsed=std::max(0.0,elapsedSeconds);
    const double edgeTime=std::max(0.0,(rightEdge-startX)/speed);
    const double x=startX+speed*elapsed;
    const double y=startY+speed*std::max(0.0,elapsed-edgeTime);
    return {x,y,y<bottomEdge};
}

inline int synonymLevelBonus(int elapsedMilliseconds,
                             int totalHintsUsed,
                             int secondsPoints) {
    const int elapsedSeconds = std::max(0, elapsedMilliseconds) / 1000;
    return (2 * elapsedSeconds / (std::max(0, totalHintsUsed) + 1)) *
           secondsPoints;
}

// 0x004825B0 first sorts the eligible pointer pool by its persisted timestamp,
// then applies the shared 0x0047EC00 jitter and sorts again. randomBelow(n)
// mirrors 0x0040C530 and returns [0,n), so the raw jitter is [-100,99].
template <typename RandomBelow>
std::vector<std::size_t> orderSynonymCandidates(
    std::vector<SynonymRankCandidate> candidates,
    RandomBelow randomBelow) {
    if (candidates.empty()) {
        return {};
    }

    // Loader comparator 0x00482F00 establishes difficulty, word length, then
    // lexical word order. stableId is the source-order stand-in for the last
    // comparison because the dictionary itself is already lexical.
    std::sort(candidates.begin(), candidates.end(), [](const auto &left,
                                                        const auto &right) {
        return std::tie(left.difficulty, left.wordLength, left.stableId) <
               std::tie(right.difficulty, right.wordLength, right.stableId);
    });
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const auto &left, const auto &right) {
                         return left.lastSeen < right.lastSeen;
                     });
    const float spread = 10.0F + 0.25F * static_cast<float>(
        candidates.back().lastSeen - candidates.front().lastSeen);
    struct Weighted {
        std::size_t entryIndex{};
        std::uint32_t stableId{};
        float weight{};
    };
    std::vector<Weighted> weighted;
    weighted.reserve(candidates.size());
    for (const auto &candidate : candidates) {
        weighted.push_back({
            candidate.entryIndex,
            candidate.stableId,
            static_cast<float>(candidate.lastSeen) +
                static_cast<float>(randomBelow(200) - 100) * 0.01F * spread,
        });
    }
    std::sort(weighted.begin(), weighted.end(), [](const auto &left,
                                                    const auto &right) {
        return std::tie(left.weight, left.stableId) <
               std::tie(right.weight, right.stableId);
    });
    std::vector<std::size_t> result;
    result.reserve(weighted.size());
    for (const auto &candidate : weighted) {
        result.push_back(candidate.entryIndex);
    }
    return result;
}

inline std::string_view synonymDisplayPrompt(std::string_view synonyms) {
    return synonyms.substr(0, synonyms.find(':'));
}

} // namespace fgp
