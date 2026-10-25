#pragma once

// Pure Game Trivia rules recovered from FGP.exe. Keeping them independent of
// SDL makes the ordering, timestamp and score contracts directly testable.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <vector>

#include "game/question_recency.h"

#include "util/no_repeat.h"

namespace fgp {

struct TriviaRankCandidate {
    std::size_t questionIndex{};
    int scene{};
    int difficulty{};
    std::uint32_t lastSeen{};
    std::uint32_t stableId{};
};

struct TriviaAnswerRankCandidate {
    std::size_t answerIndex{};
    std::uint32_t averageLastSeen{};
    std::uint32_t stableId{};
};

inline std::uint32_t triviaMinuteStamp(std::time_t unixSeconds) {
    return questionMinuteStamp(unixSeconds);
}

inline int triviaCorrectScore(int basePoints,
                              int secondsPoints,
                              double remainingSeconds) {
    const int wholeSeconds = std::max(
        0, static_cast<int>(std::floor(remainingSeconds)));
    return basePoints + secondsPoints * wholeSeconds;
}

// 0x0047EA80 computes mean matching-question timestamps in 0x0047EB90,
// qsorts those weights, applies the jitter at 0x0047EB03, then qsorts again.
// This is recency weighting, not a uniform shuffle.
template <typename RandomBelow>
std::vector<std::size_t> orderTriviaAnswers(
    std::vector<TriviaAnswerRankCandidate> candidates,
    RandomBelow randomBelow) {
    if (candidates.empty()) return {};
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const auto &left, const auto &right) {
                         return left.averageLastSeen < right.averageLastSeen;
                     });
    const float spread = 10.0F + 0.25F * static_cast<float>(
        candidates.back().averageLastSeen - candidates.front().averageLastSeen);
    struct Weighted {
        std::size_t answerIndex{};
        std::uint32_t stableId{};
        float weight{};
    };
    std::vector<Weighted> weighted;
    weighted.reserve(candidates.size());
    for (const auto &candidate : candidates) {
        weighted.push_back({candidate.answerIndex, candidate.stableId,
            static_cast<float>(candidate.averageLastSeen) +
                static_cast<float>(randomBelow(200)-100) * 0.01F * spread});
    }
    std::sort(weighted.begin(), weighted.end(), [](const auto &left,
                                                   const auto &right) {
        return std::tie(left.weight, left.stableId) <
               std::tie(right.weight, right.stableId);
    });
    std::vector<std::size_t> result;
    result.reserve(weighted.size());
    for (const auto &candidate:weighted) result.push_back(candidate.answerIndex);
    return result;
}

// 0x0047FC6F-0x0047FC9C calls the shared selector at 0x0040A150 with
// (globalQuestionIndex + 4), a persistent used-mask, and four choices. The
// counter is therefore already past the selector's ordered first cycle even on
// question zero: the first transition is random, while the raw four choices
// are exhausted before any repeats. Wrong/timeout remap raw choice 3 to choice
// 0 after it has been recorded in the mask, giving visible 50/25/25 odds.
template <typename RandomBelow>
std::size_t triviaTransitionChoice(NoRepeatState &state,
                                   bool correct,
                                   RandomBelow randomBelow) {
    state.callCount = std::max<std::uint32_t>(state.callCount, 4U);
    const std::size_t raw = nextNoRepeatOption(state, 4U, randomBelow);
    return !correct && raw == 3U ? 0U : raw;
}

// randomBelow(n) must mirror 0x0040C530 and return [0,n). The jitter call at
// 0x0047EC01-0x0047EC05 passes (-100,100) to 0x0040C550; that helper computes
// rand()%200 - 100, so the exact integer range is [-100,99], not inclusive 100.
template <typename RandomBelow>
std::vector<std::size_t> orderTriviaCandidates(
    std::vector<TriviaRankCandidate> candidates,
    RandomBelow randomBelow) {
    if (candidates.empty()) {
        return {};
    }

    // 0x0047F330 establishes this stable structural order before the round
    // subset is weighted.
    std::sort(candidates.begin(), candidates.end(), [](const auto &left,
                                                        const auto &right) {
        return std::tie(left.scene, left.difficulty, left.lastSeen,
                        left.stableId) <
               std::tie(right.scene, right.difficulty, right.lastSeen,
                        right.stableId);
    });
    // 0x0047EC93 stores lastSeen as the initial float weight, then the first
    // qsort at 0x0047ECCF orders the candidate pointers by that weight. The
    // random draws at 0x0047ED03 are therefore consumed in recency order, not
    // in the preceding scene/id order.
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const auto &left, const auto &right) {
                         return left.lastSeen < right.lastSeen;
                     });
    const float spread = 10.0F + 0.25F * static_cast<float>(
        candidates.back().lastSeen - candidates.front().lastSeen);
    struct Weighted {
        std::size_t questionIndex{};
        std::uint32_t stableId{};
        float weight{};
    };
    std::vector<Weighted> weighted;
    weighted.reserve(candidates.size());
    for (const auto &candidate : candidates) {
        weighted.push_back({
            candidate.questionIndex,
            candidate.stableId,
            static_cast<float>(candidate.lastSeen) +
                static_cast<float>(randomBelow(200)-100) * 0.01F * spread,
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
        result.push_back(candidate.questionIndex);
    }
    return result;
}

} // namespace fgp
