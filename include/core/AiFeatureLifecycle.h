#ifndef AI_FEATURE_LIFECYCLE_H
#define AI_FEATURE_LIFECYCLE_H

#include <stdint.h>

enum class AiFeatureAction : uint8_t {
    NONE,
    START,
    REQUEST_STOP,
    WAIT,
    RESET,
    RESET_AND_START
};

AiFeatureAction nextAiFeatureAction(bool enabled, bool runtimeExists, bool running, bool workerActive);

#endif
