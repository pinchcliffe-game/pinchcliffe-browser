#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace fgp {

namespace detail {

// Integer types the TraceField constructors below match exactly.
template <typename T>
inline constexpr bool traceFieldExactlyOverloaded =
    std::is_same_v<T, bool> || std::is_same_v<T, int> ||
    std::is_same_v<T, std::int64_t> || std::is_same_v<T, std::uint32_t> ||
    std::is_same_v<T, std::uint64_t>;

// Character types already promote to the int overload; leave them alone so a
// char argument keeps serialising as the number it does today.
template <typename T>
inline constexpr bool traceFieldIsCharacter =
    std::is_same_v<T, char> || std::is_same_v<T, signed char> ||
    std::is_same_v<T, unsigned char> || std::is_same_v<T, wchar_t> ||
    std::is_same_v<T, char8_t> || std::is_same_v<T, char16_t> ||
    std::is_same_v<T, char32_t>;

template <typename T>
inline constexpr bool traceFieldNeedsWidening =
    std::is_integral_v<T> && !traceFieldExactlyOverloaded<T> &&
    !traceFieldIsCharacter<T>;

template <typename T>
using TraceFieldWidened =
    std::conditional_t<std::is_signed_v<T>, std::int64_t, std::uint64_t>;

} // namespace detail

struct TraceField {
    using Value = std::variant<std::string, std::int64_t, std::uint64_t,
                               double, bool>;

    TraceField(std::string_view name, std::string_view value);
    TraceField(std::string_view name, const std::string &value);
    TraceField(std::string_view name, const char *value);
    TraceField(std::string_view name, int value);
    TraceField(std::string_view name, std::int64_t value);
    TraceField(std::string_view name, std::uint32_t value);
    TraceField(std::string_view name, std::uint64_t value);
    TraceField(std::string_view name, double value);
    TraceField(std::string_view name, bool value);

    // The overloads above name fixed-width types, so an integer argument whose
    // type is none of them converts to every one of them equally badly and the
    // call is ambiguous. Which types fall in that gap is platform-specific: on
    // Darwin size_t is `unsigned long` while uint64_t is `unsigned long long`,
    // so `emit("x", {{"n", clips.size()}})` fails to compile there while
    // building cleanly on Windows and Linux, where size_t and uint64_t are the
    // same type. MSVC has the same gap at `long` and `unsigned long`. Widening
    // whatever the fixed-width overloads miss closes the gap once, instead of
    // asking every call site to remember a cast the way commit 71e033c had to.
    template <typename T,
              std::enable_if_t<detail::traceFieldNeedsWidening<T>, int> = 0>
    TraceField(std::string_view name, T value)
        : TraceField(name,
                     static_cast<detail::TraceFieldWidened<T>>(value)) {}

    std::string name;
    Value value;
};

namespace detail {

template <typename... Ts>
inline constexpr bool traceFieldAcceptsAll =
    (std::is_constructible_v<TraceField, std::string_view, Ts> && ...);

} // namespace detail

// Guard for the ambiguity described above. It has to bite on every platform,
// not only the one whose size_t happens to fall in the gap, so assert against
// size_t itself *and* against every standard integer type: whichever type a
// platform maps size_t onto is then covered under its own name. Without the
// widening constructor this assertion fails for `long` and `unsigned long` on
// MSVC and for `size_t`, `long` and `unsigned long` on Darwin -- so a plain
// Windows build now catches the mistake that previously only a Mac could.
static_assert(
    detail::traceFieldAcceptsAll<
        std::size_t, std::ptrdiff_t, short, unsigned short, int, unsigned int,
        long, unsigned long, long long, unsigned long long, std::int8_t,
        std::uint8_t, std::int16_t, std::uint16_t, std::int32_t, std::uint32_t,
        std::int64_t, std::uint64_t>,
    "TraceField must accept every standard integer type unambiguously, "
    "including size_t. Adding or removing a numeric overload can make a "
    "size_t emit ambiguous on platforms where size_t is not one of the "
    "fixed-width types (Darwin), which no Windows or Linux build would catch.");

// Opt-in newline-delimited JSON tracing for original/native A/B captures.
// With no FGP_TRACE_PATH environment variable, emit() is a cheap no-op.
class ReferenceTrace {
public:
    ReferenceTrace();
    ReferenceTrace(const ReferenceTrace &) = delete;
    ReferenceTrace &operator=(const ReferenceTrace &) = delete;

    bool enabled() const { return stream_.is_open(); }
    void emit(std::string_view event,
              std::initializer_list<TraceField> fields = {});
    void finish(int returnCode);

private:
    std::ofstream stream_;
    std::string runId_;
    std::string source_{"native"};
    std::chrono::steady_clock::time_point started_;
    std::uint64_t sequence_{};
    bool finished_{};
    std::mutex mutex_;
};

} // namespace fgp
