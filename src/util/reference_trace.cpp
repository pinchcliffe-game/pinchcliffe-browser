#include "util/reference_trace.h"

#include <array>
#include <iostream>

#include <cstdlib>
#include <functional>
#include <iomanip>
#include <sstream>
#include <thread>
#include <type_traits>

namespace fgp {
namespace {

std::uint16_t cp1252CodePoint(unsigned char character) {
    static constexpr std::array<std::uint16_t,32> extended{
        0x20AC,0x0081,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,
        0x02C6,0x2030,0x0160,0x2039,0x0152,0x008D,0x017D,0x008F,
        0x0090,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,
        0x02DC,0x2122,0x0161,0x203A,0x0153,0x009D,0x017E,0x0178,
    };
    if (character>=0x80U && character<0xA0U)
        return extended[character-0x80U];
    return character;
}

void appendUnicodeEscape(std::ostringstream &result,std::uint16_t codePoint) {
    const char previousFill=result.fill();
    result << "\\u" << std::hex << std::setw(4) << std::setfill('0')
           << codePoint << std::dec;
    result.fill(previousFill);
}

std::string jsonString(std::string_view text) {
    std::ostringstream result;
    result << '"';
    for (const unsigned char character : text) {
        switch (character) {
        case '"': result << "\\\""; break;
        case '\\': result << "\\\\"; break;
        case '\b': result << "\\b"; break;
        case '\f': result << "\\f"; break;
        case '\n': result << "\\n"; break;
        case '\r': result << "\\r"; break;
        case '\t': result << "\\t"; break;
        default:
            if (character < 0x20U) {
                appendUnicodeEscape(result,character);
            } else if (character >= 0x80U) {
                // Game/package strings are Windows-1252 byte strings. Raw
                // bytes such as Norwegian å (0xE5) are not valid UTF-8 and
                // made the JSONL reader fail mid-capture. Escaping their
                // Unicode values keeps the trace ASCII, valid JSON/UTF-8, and
                // preserves the decoded text used by comparisons.
                appendUnicodeEscape(result,cp1252CodePoint(character));
            } else {
                result << static_cast<char>(character);
            }
        }
    }
    result << '"';
    return result.str();
}

std::string jsonValue(const TraceField::Value &value) {
    return std::visit(
        [](const auto &item) -> std::string {
            using Type = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<Type, std::string>) {
                return jsonString(item);
            } else if constexpr (std::is_same_v<Type, bool>) {
                return item ? "true" : "false";
            } else if constexpr (std::is_same_v<Type, double>) {
                std::ostringstream result;
                result << std::setprecision(12) << item;
                return result.str();
            } else {
                return std::to_string(item);
            }
        },
        value);
}

} // namespace

TraceField::TraceField(std::string_view fieldName, std::string_view fieldValue)
    : name(fieldName), value(std::string(fieldValue)) {}
TraceField::TraceField(std::string_view fieldName, const std::string &fieldValue)
    : name(fieldName), value(fieldValue) {}
TraceField::TraceField(std::string_view fieldName, const char *fieldValue)
    : name(fieldName), value(std::string(fieldValue ? fieldValue : "")) {}
TraceField::TraceField(std::string_view fieldName, int fieldValue)
    : name(fieldName), value(static_cast<std::int64_t>(fieldValue)) {}
TraceField::TraceField(std::string_view fieldName, std::int64_t fieldValue)
    : name(fieldName), value(fieldValue) {}
TraceField::TraceField(std::string_view fieldName, std::uint32_t fieldValue)
    : name(fieldName), value(static_cast<std::uint64_t>(fieldValue)) {}
TraceField::TraceField(std::string_view fieldName, std::uint64_t fieldValue)
    : name(fieldName), value(fieldValue) {}
TraceField::TraceField(std::string_view fieldName, double fieldValue)
    : name(fieldName), value(fieldValue) {}
TraceField::TraceField(std::string_view fieldName, bool fieldValue)
    : name(fieldName), value(fieldValue) {}

ReferenceTrace::ReferenceTrace() : started_(std::chrono::steady_clock::now()) {
    const char *path = std::getenv("FGP_TRACE_PATH");
    if (!path || !*path) {
        // Tracing off is the normal case and stays silent. But a near-miss
        // variable name is worth complaining about: the tracer is a deliberate
        // no-op without FGP_TRACE_PATH, so a misspelled name produces an empty
        // capture that looks exactly like a game that emitted nothing. Both of
        // us lost time to that from one wrong name in a handoff note.
        static constexpr std::array<const char *, 4> nearMisses{
            "FGP_REFERENCE_TRACE", "FGP_TRACE", "FGP_TRACE_FILE",
            "FGP_TRACEPATH"};
        for (const char *candidate : nearMisses) {
            if (const char *value = std::getenv(candidate)) {
                if (*value) {
                    std::cerr << "warning: " << candidate << " is set but "
                                 "unused; the reference tracer reads "
                                 "FGP_TRACE_PATH, so no trace will be written\n";
                }
            }
        }
        return;
    }
    if (const char *runId = std::getenv("FGP_TRACE_RUN_ID")) {
        runId_ = runId;
    }
    if (runId_.empty()) {
        runId_ = "native-local";
    }
    if (const char *source = std::getenv("FGP_TRACE_SOURCE")) {
        source_ = source;
    }
    if (source_ != "native" && source_ != "original") {
        source_ = "native";
    }
    stream_.open(path, std::ios::out | std::ios::trunc);
    if (stream_) {
        emit("process_start", {{"component", "FGPNativePOC"}});
    }
}

void ReferenceTrace::emit(std::string_view event,
                          std::initializer_list<TraceField> fields) {
    if (!stream_.is_open()) {
        return;
    }
    std::lock_guard lock(mutex_);
    const double elapsed = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - started_)
                               .count();
    stream_ << "{\"schema_version\":1,\"run_id\":" << jsonString(runId_)
            << ",\"sequence\":" << sequence_++
            << ",\"t_ms\":" << std::fixed << std::setprecision(3) << elapsed
            << ",\"source\":" << jsonString(source_)
            << ",\"thread_id\":"
            << std::hash<std::thread::id>{}(std::this_thread::get_id())
            << ",\"event\":" << jsonString(event) << ",\"data\":{";
    bool first = true;
    for (const auto &field : fields) {
        if (!first) {
            stream_ << ',';
        }
        first = false;
        stream_ << jsonString(field.name) << ':' << jsonValue(field.value);
    }
    stream_ << "}}\n";
    stream_.flush();
}

void ReferenceTrace::finish(int returnCode) {
    if (!finished_) {
        emit("process_stop", {{"return_code", returnCode}});
        finished_ = true;
    }
}

} // namespace fgp
