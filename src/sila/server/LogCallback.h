// LogCallback.h — Structured logging callback for dispatch observability (architecture.md §3.4e)
#pragma once

#include <cstdio>
#include <functional>
#include <string_view>

namespace sila2 {

/// Severity of one logged event; see LogCallback.
enum class LogLevel { kInfo, kWarning, kError };

/// A caller-supplied sink for server-side log events: authorization rejections,
/// per-call dispatch (one kInfo line per RPC with the call's FQI), and
/// discovery warnings. `category` is a short tag such as "auth", "dispatch",
/// or "discovery"; `message` is a human-readable line, already formatted.
///
/// @see SilaServerBase::Builder::setLogCallback, which installs it. An empty
///      (default-constructed) LogCallback means no callback was installed,
///      and logEvent() is then a no-op.
using LogCallback = std::function<void(LogLevel level,
                                       std::string_view category,
                                       std::string_view message)>;

/// Invokes cb with the given event if cb is non-empty; otherwise does nothing.
inline void logEvent(const LogCallback& cb, LogLevel level,
                     std::string_view category, std::string_view message) {
    if (cb) cb(level, category, message);
}

/// Built-in LogCallback used when the caller installs none: writes warnings
/// and errors to stderr, plus "connection"-category info lines.
inline LogCallback defaultLogCallback() {
    return [](LogLevel level, std::string_view category, std::string_view message) {
        if (level < LogLevel::kWarning && category != "connection") return;
        const char* tag = (level == LogLevel::kError) ? "ERROR"
                        : (level == LogLevel::kWarning) ? "WARN" : "INFO";
        std::fprintf(stderr, "[sila2:%s] %.*s: %.*s\n", tag,
                     static_cast<int>(category.size()), category.data(),
                     static_cast<int>(message.size()), message.data());
    };
}

}  // namespace sila2
