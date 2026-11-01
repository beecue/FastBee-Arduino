#include "core/AiFeatureLifecycle.h"

AiFeatureAction nextAiFeatureAction(bool enabled, bool runtimeExists, bool running, bool workerActive) {
    if (runtimeExists) {
        if (workerActive) {
            if (!enabled) return AiFeatureAction::REQUEST_STOP;
            return running ? AiFeatureAction::NONE : AiFeatureAction::WAIT;
        }
        return enabled ? AiFeatureAction::RESET_AND_START : AiFeatureAction::RESET;
    }
    return enabled ? AiFeatureAction::START : AiFeatureAction::NONE;
}
