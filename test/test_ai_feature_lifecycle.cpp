#include <unity.h>
#include "core/AiFeatureLifecycle.h"
#include "network/handlers/AiRoutePath.h"
#include "../src/core/AiFeatureLifecycle.cpp"

void test_disable_reenable_waits_for_old_worker_then_starts_fresh() {
    TEST_ASSERT_EQUAL(static_cast<int>(AiFeatureAction::REQUEST_STOP),
                      static_cast<int>(nextAiFeatureAction(false, true, true, true)));
    TEST_ASSERT_EQUAL(static_cast<int>(AiFeatureAction::WAIT),
                      static_cast<int>(nextAiFeatureAction(true, true, false, true)));
    TEST_ASSERT_EQUAL(static_cast<int>(AiFeatureAction::RESET_AND_START),
                      static_cast<int>(nextAiFeatureAction(true, true, false, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(AiFeatureAction::RESET),
                      static_cast<int>(nextAiFeatureAction(false, true, false, false)));
}

void test_stopped_runtime_is_reset_even_when_current_intent_disabled() {
    TEST_ASSERT_EQUAL(static_cast<int>(AiFeatureAction::RESET),
                      static_cast<int>(nextAiFeatureAction(false, true, false, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(AiFeatureAction::START),
                      static_cast<int>(nextAiFeatureAction(true, false, false, false)));
}

void test_invalid_cancel_path_releases_every_denied_body() {
    int releases = 0;
    for (int i = 0; i < 100; ++i) {
        TEST_ASSERT_TRUE(rejectInvalidJobPostPath("job-1/not-cancel", [&]() { ++releases; }));
    }
    TEST_ASSERT_EQUAL(100, releases);
    TEST_ASSERT_FALSE(rejectInvalidJobPostPath("job-1/cancel", [&]() { ++releases; }));
    TEST_ASSERT_FALSE(rejectInvalidJobPostPath("job-1/tool-result", [&]() { ++releases; }));
    TEST_ASSERT_EQUAL(100, releases);
}

void test_ai_feature_lifecycle_group() {
    RUN_TEST(test_disable_reenable_waits_for_old_worker_then_starts_fresh);
    RUN_TEST(test_stopped_runtime_is_reset_even_when_current_intent_disabled);
    RUN_TEST(test_invalid_cancel_path_releases_every_denied_body);
}
