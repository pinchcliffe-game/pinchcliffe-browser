#pragma once

// The game's shared no-repeat selector, recovered from 0x0040a150.
//
// It is not a shuffled bag. The state is a call counter and a used-bits
// mask: while the counter is below the option count, the selection IS the
// counter, so the first cycle always runs in table order 0, 1, 2, ...
// Afterwards the selection is drawn randomly from the options whose bits
// are not yet set, and the mask resets once every bit is set — each later
// cycle still exhausts all options before any repeat, but in random order.
//
// The yard opening depends on the ordered first cycle: chain 0 is the short
// four-stage variant, and playing chains 1 or 2 on the first visit fronts
// the entire story sequence at once.

#include <cstddef>
#include <cstdint>

namespace fgp {

struct NoRepeatState {
    std::uint32_t callCount{0};
    std::uint32_t usedMask{0};
};

// randomBelow(n) must return a value in [0, n), like the game's rand() % n.
template <typename RandomBelow>
std::size_t nextNoRepeatOption(NoRepeatState &state,
                               std::size_t optionCount,
                               RandomBelow randomBelow) {
    const std::uint32_t fullMask =
        (1U << static_cast<std::uint32_t>(optionCount)) - 1U;
    if (state.usedMask == fullMask) {
        state.usedMask = 0;
    }

    std::size_t selection = 0;
    if (state.callCount < optionCount) {
        selection = state.callCount;
    } else {
        std::size_t unusedCount = 0;
        for (std::size_t option = 0; option < optionCount; ++option) {
            if ((state.usedMask & (1U << option)) == 0U) {
                ++unusedCount;
            }
        }
        std::size_t skip =
            static_cast<std::size_t>(randomBelow(static_cast<int>(unusedCount)));
        for (std::size_t option = 0; option < optionCount; ++option) {
            if ((state.usedMask & (1U << option)) != 0U) {
                continue;
            }
            if (skip == 0) {
                selection = option;
                break;
            }
            --skip;
        }
    }

    state.usedMask |= 1U << static_cast<std::uint32_t>(selection);
    ++state.callCount;
    return selection;
}

} // namespace fgp
