#ifndef AI_ROUTE_PATH_H
#define AI_ROUTE_PATH_H

#include <Arduino.h>

enum class AiJobPostPath : uint8_t {
    CANCEL,
    TOOL_RESULT,
    INVALID
};

inline AiJobPostPath classifyAiJobPostPath(const String& path) {
    if (path.endsWith("/tool-result")) return AiJobPostPath::TOOL_RESULT;
    if (path.endsWith("/cancel")) return AiJobPostPath::CANCEL;
    return AiJobPostPath::INVALID;
}

template <typename Release>
bool rejectInvalidJobPostPath(const String& path, Release release) {
    if (classifyAiJobPostPath(path) != AiJobPostPath::INVALID) return false;
    release();
    return true;
}

#endif
