#include <unity.h>
#include <Arduino.h>
#include <ArduinoJson.h>

#include "core/AiProposalService.h"
#include "../src/core/AiProposalService.cpp"

namespace {

JsonDocument arguments(const char* json) {
    JsonDocument doc;
    deserializeJson(doc, json);
    return doc;
}

void test_gpio_is_rejected_and_dht_proposal_requires_confirmation() {
    AiProposalService service;
    int executions = 0;
    service.setExecutor([&](const AiProposal&, String&) {
        ++executions;
        AiExecutionResult result;
        result.accepted = true;
        result.persisted = true;
        result.readback = true;
        result.observed = false;
        result.physicalConfirmed = false;
        return result;
    });
    auto gpio = service.createFromTool({"session-a"}, "create_gpio_output",
        arguments("{\"name\":\"继电器\",\"pin\":4,\"activeHigh\":true}").as<JsonVariantConst>(), 7, 1000);
    TEST_ASSERT_FALSE(gpio.success);
    TEST_ASSERT_EQUAL_STRING("AI_UNSUPPORTED_TOOL", gpio.code.c_str());
    TEST_ASSERT_EQUAL(0, executions);

    auto created = service.createFromTool({"session-a"}, "create_dht_sensor",
        arguments("{\"name\":\"温湿度\",\"pin\":5}").as<JsonVariantConst>(), 7, 1000);
    TEST_ASSERT_TRUE(created.success);
    TEST_ASSERT_EQUAL(0, executions);
    TEST_ASSERT_TRUE(created.proposalId.length() > 12);
    auto denied = service.execute({"session-b"}, created.proposalId, "确认执行", 7, 1001);
    TEST_ASSERT_EQUAL_STRING("AI_PROPOSAL_NOT_FOUND", denied.code.c_str());
    auto badText = service.execute({"session-a"}, created.proposalId, "yes", 7, 1001);
    TEST_ASSERT_EQUAL_STRING("AI_CONFIRMATION_REQUIRED", badText.code.c_str());
    auto stale = service.execute({"session-a"}, created.proposalId, "确认执行", 8, 1001);
    TEST_ASSERT_EQUAL_STRING("AI_CONFIGURATION_CHANGED", stale.code.c_str());
    auto result = service.execute({"session-a"}, created.proposalId, "确认执行", 7, 1001);
    TEST_ASSERT_TRUE(result.accepted);
    TEST_ASSERT_TRUE(result.persisted);
    TEST_ASSERT_TRUE(result.readback);
    TEST_ASSERT_FALSE(result.observed);
    TEST_ASSERT_FALSE(result.physicalConfirmed);
    TEST_ASSERT_EQUAL(1, executions);
    auto replay = service.execute({"session-a"}, created.proposalId, "确认执行", 7, 1002);
    TEST_ASSERT_EQUAL_STRING("AI_PROPOSAL_ALREADY_USED", replay.code.c_str());
    TEST_ASSERT_EQUAL(1, executions);
}

void test_invalid_arguments_and_injected_runtime_fields_fail_closed() {
    AiProposalService service;
    const char* invalid[] = {
        "{\"pin\":4}", "{\"name\":\"x\"}", "{\"name\":\"x\",\"pin\":4,\"enabled\":true}",
        "{\"name\":\"x\",\"pin\":4,\"acknowledgements\":255}",
        "{\"name\":\"x\",\"pin\":4,\"initialState\":1}"
    };
    for (const char* json : invalid) {
        auto created = service.createFromTool({"s"}, "create_dht_sensor",
            arguments(json).as<JsonVariantConst>(), 1, 0);
        TEST_ASSERT_FALSE(created.success);
        TEST_ASSERT_EQUAL_STRING("AI_INVALID_TOOL_ARGUMENTS", created.code.c_str());
    }
    auto unknown = service.createFromTool({"s"}, "delete_all", arguments("{}").as<JsonVariantConst>(), 1, 0);
    TEST_ASSERT_FALSE(unknown.success);
}

void test_expiry_wraparound_and_execution_failure_states() {
    AiProposalService service;
    service.setExecutor([](const AiProposal&, String&) {
        AiExecutionResult result;
        result.accepted = true;
        result.persisted = true;
        result.readback = false;
        result.code = "HARDWARE_INIT_FAILED";
        return result;
    });
    auto created = service.createFromTool({"s"}, "create_dht_sensor",
        arguments("{\"name\":\"dht\",\"pin\":5}").as<JsonVariantConst>(), 1, UINT32_MAX - 5000);
    TEST_ASSERT_TRUE(created.success);
    auto expired = service.execute({"s"}, created.proposalId, "确认执行", 1, created.createdAtMs + 120000);
    TEST_ASSERT_EQUAL_STRING("AI_PROPOSAL_EXPIRED", expired.code.c_str());
    auto second = service.createFromTool({"s"}, "create_dht_sensor",
        arguments("{\"name\":\"dht\",\"pin\":5}").as<JsonVariantConst>(), 1, 100);
    auto result = service.execute({"s"}, second.proposalId, "确认执行", 1, 101);
    TEST_ASSERT_TRUE(result.persisted);
    TEST_ASSERT_FALSE(result.readback);
    TEST_ASSERT_FALSE(result.physicalConfirmed);
}

} // namespace

void test_ai_proposal_group() {
    RUN_TEST(test_gpio_is_rejected_and_dht_proposal_requires_confirmation);
    RUN_TEST(test_invalid_arguments_and_injected_runtime_fields_fail_closed);
    RUN_TEST(test_expiry_wraparound_and_execution_failure_states);
}
