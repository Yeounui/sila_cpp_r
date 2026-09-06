// LogCallback.h — Structured logging callback for dispatch observability (architecture.md §3.4e)
#pragma once

#include <cstdio>
#include <functional>
#include <string_view>

namespace sila2 {

enum class LogLevel { kInfo, kWarning, kError };

using LogCallback = std::function<void(LogLevel level,
                                       std::string_view category,
                                       std::string_view message)>;

inline void logEvent(const LogCallback& cb, LogLevel level,
                     std::string_view category, std::string_view message) {
    if (cb) cb(level, category, message);
}

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
