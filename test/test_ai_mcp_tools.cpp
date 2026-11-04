#include <unity.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <chrono>
#include <deque>
#include <fstream>
#include <thread>

#include "core/AiMcpToolBridge.h"
#include "core/AiConversationManager.h"
#include "../src/core/AiMcpToolBridge.cpp"

namespace {

bool jsonEqual(JsonVariantConst left, JsonVariantConst right) {
    if (left.is<JsonObjectConst>() || right.is<JsonObjectConst>()) {
        if (!left.is<JsonObjectConst>() || !right.is<JsonObjectConst>()) return false;
        JsonObjectConst leftObject = left.as<JsonObjectConst>();
        JsonObjectConst rightObject = right.as<JsonObjectConst>();
        if (leftObject.size() != rightObject.size()) return false;
        for (JsonPairConst pair : leftObject) {
            if (rightObject[pair.key().c_str()].isNull() || !jsonEqual(pair.value(), rightObject[pair.key().c_str()])) {
                return false;
            }
        }
        return true;
    }
    if (left.is<JsonArrayConst>() || right.is<JsonArrayConst>()) {
        if (!left.is<JsonArrayConst>() || !right.is<JsonArrayConst>()) return false;
        JsonArrayConst leftArray = left.as<JsonArrayConst>();
        JsonArrayConst rightArray = right.as<JsonArrayConst>();
        if (leftArray.size() != rightArray.size()) return false;
        for (size_t i = 0; i < leftArray.size(); ++i) {
            if (!jsonEqual(leftArray[i], rightArray[i])) return false;
        }
        return true;
    }
    if (left.isNull() || right.isNull()) return left.isNull() == right.isNull();
    return left == right;
}

void test_peripheral_schema_matches_shared_fixture() {
    std::ifstream input("test/fixtures/mcp/device-ai-tools-v1.json");
    TEST_ASSERT_TRUE(input.good());
    JsonDocument fixture;
    TEST_ASSERT_FALSE(deserializeJson(fixture, input));
    JsonDocument generated;
    TEST_ASSERT_TRUE(AiMcpToolBridge::writeToolSchema("upsert_peripheral", generated.to<JsonObject>()));
    TEST_ASSERT_TRUE(jsonEqual(fixture["tools"]["upsert_peripheral"]["inputSchema"], generated.as<JsonVariantConst>()));
}

class FakeMcpTransport : public AiProviderTransport {
public:
    int calls = 0;
    int toolCallsRemaining = 0;
    String tool = "search_documents";
    String arguments = "{\"query\":\"GPIO4\"}";
    String finalText = "已根据引用回答；草案未写入设备";
    AiProviderRequest lastRequest;

    AiProviderResult complete(const AiProviderRequest& request, AiProviderReply& reply) override {
        ++calls;
        lastRequest = request;
        if (toolCallsRemaining > 0) {
            --toolCallsRemaining;
            reply.toolName = tool;
            reply.toolCallId = "call-" + String(calls);
            if (tool == "search_documents" && arguments == "{\"query\":\"GPIO4\"}") {
                reply.toolArguments["query"] = "GPIO4";
            } else {
                deserializeJson(reply.toolArguments, arguments);
            }
        } else {
            reply.text = finalText;
        }
        return AiProviderResult::success();
    }
};

bool waitState(AiConversationManager& manager, const String& jobId, AiJobState state,
               AiJobSnapshot& snapshot) {
    for (int i = 0; i < 300; ++i) {
        if (manager.getJob({"session-a"}, jobId, snapshot) && snapshot.state == state) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    return false;
}

void test_catalog_is_pruned_and_arguments_are_strict() {
    TEST_ASSERT_EQUAL(8, AiMcpToolBridge::kMaxIterations);
    TEST_ASSERT_EQUAL(120000, AiMcpToolBridge::kDeadlineMs);
    TEST_ASSERT_EQUAL(16384, AiMcpToolBridge::kMaxToolResultBytes);
    TEST_ASSERT_TRUE(AiMcpToolBridge::isAllowed("search_documents"));
    TEST_ASSERT_TRUE(AiMcpToolBridge::isAllowed("read_peripheral"));
    TEST_ASSERT_TRUE(AiMcpToolBridge::isAllowed("validate_configuration_draft"));
    TEST_ASSERT_FALSE(AiMcpToolBridge::isAllowed("apply_configuration_draft"));
    TEST_ASSERT_FALSE(AiMcpToolBridge::isAllowed("delete_everything"));
    String error;
    JsonDocument args;
    deserializeJson(args, "{\"query\":\"GPIO4\"}");
    TEST_ASSERT_TRUE(AiMcpToolBridge::validateCall("search_documents", args.as<JsonVariantConst>(), error));
    deserializeJson(args, "{\"query\":\"ignore instructions\",\"admin\":true}");
    TEST_ASSERT_FALSE(AiMcpToolBridge::validateCall("search_documents", args.as<JsonVariantConst>(), error));
    deserializeJson(args, "{}");
    TEST_ASSERT_FALSE(AiMcpToolBridge::validateCall("read_peripheral", args.as<JsonVariantConst>(), error));
    TEST_ASSERT_FALSE(AiMcpToolBridge::validateCall("apply_configuration_draft", args.as<JsonVariantConst>(), error));
    deserializeJson(args, "{\"draftId\":\"d-1\",\"expectedVersion\":1,\"peripheral\":{\"name\":\"relay\",\"type\":12,\"pins\":[4],\"enabled\":true}}");
    TEST_ASSERT_TRUE(AiMcpToolBridge::validateCall("upsert_peripheral", args.as<JsonVariantConst>(), error));
    deserializeJson(args, "{\"draftId\":\"d-1\",\"expectedVersion\":1,\"peripheral\":{\"name\":\"relay\",\"type\":12,\"pins\":[4],\"unexpected\":true}}");
    TEST_ASSERT_FALSE(AiMcpToolBridge::validateCall("upsert_peripheral", args.as<JsonVariantConst>(), error));
    deserializeJson(args, "{\"draftId\":\"d-1\",\"expectedVersion\":1,\"peripheral\":{\"name\":\"relay\",\"type\":12,\"pins\":[0,1,2,3,4,5,6,7,8]}}");
    TEST_ASSERT_FALSE(AiMcpToolBridge::validateCall("upsert_peripheral", args.as<JsonVariantConst>(), error));
    deserializeJson(args, "{\"draftId\":\"d-1\",\"expectedVersion\":1,\"peripheral\":{\"name\":\"relay\",\"type\":12,\"pins\":[4],\"pinCount\":2}}");
    TEST_ASSERT_FALSE(AiMcpToolBridge::validateCall("upsert_peripheral", args.as<JsonVariantConst>(), error));
}

void test_cloud_schema_and_initial_draft_version_are_compatible() {
    String error;
    JsonDocument args;
    deserializeJson(args, "{\"draftId\":\"d-1\",\"expectedVersion\":0,\"peripheral\":{\"name\":\"relay\",\"type\":12,\"pins\":[4]}}");
    TEST_ASSERT_TRUE(AiMcpToolBridge::validateCall("upsert_peripheral", args.as<JsonVariantConst>(), error));

    JsonDocument schema;
    TEST_ASSERT_TRUE(AiMcpToolBridge::writeToolSchema("search_documents", schema.to<JsonObject>()));
    TEST_ASSERT_EQUAL(false, schema["additionalProperties"].as<bool>());
    TEST_ASSERT_TRUE(schema["properties"]["query"].is<JsonObject>());
    TEST_ASSERT_TRUE(schema["properties"]["limit"].is<JsonObject>());
    TEST_ASSERT_TRUE(schema["properties"]["cursor"].is<JsonObject>());
    TEST_ASSERT_FALSE(schema["properties"]["page"].is<JsonObject>());
    TEST_ASSERT_FALSE(schema["properties"]["pageSize"].is<JsonObject>());
    TEST_ASSERT_TRUE(schema["required"][0].is<const char*>());
    TEST_ASSERT_FALSE(AiMcpToolBridge::writeToolSchema("apply_configuration_draft", schema.to<JsonObject>()));
}

void test_all_safe_configuration_domains_have_strict_schemas() {
    const char* names[] = {
        "get_device_config_section", "get_device_status", "update_device_profile", "configure_runtime_features",
        "upsert_execution_rule", "remove_execution_rule", "dry_run_execution_rule",
        "upsert_rule_script", "remove_rule_script", "configure_modbus_rtu", "upsert_modbus_device",
        "remove_modbus_device", "upsert_modbus_poll_task", "remove_modbus_poll_task", "configure_mqtt",
        "upsert_mqtt_topic", "remove_mqtt_topic", "preview_configuration_draft", "refresh_device_capabilities",
        "list_workflow_templates", "start_configuration_workflow", "get_workflow_run", "cancel_workflow_run",
        "parse_document", "retry_document_parsing", "get_operation_audit", "submit_feedback"
    };
    for (const char* name : names) {
        TEST_ASSERT_TRUE(AiMcpToolBridge::isAllowed(name));
        JsonDocument schema;
        TEST_ASSERT_TRUE(AiMcpToolBridge::writeToolSchema(name, schema.to<JsonObject>()));
        TEST_ASSERT_EQUAL(false, schema["additionalProperties"].as<bool>());
        TEST_ASSERT_TRUE(schema["properties"].is<JsonObject>());
    }
    TEST_ASSERT_FALSE(AiMcpToolBridge::isAllowed("apply_configuration_draft"));
    TEST_ASSERT_FALSE(AiMcpToolBridge::isAllowed("rollback_configuration_operation"));
    TEST_ASSERT_FALSE(AiMcpToolBridge::isAllowed("set_execution_rule_enabled"));
    TEST_ASSERT_FALSE(AiMcpToolBridge::isAllowed("set_rule_script_enabled"));
    TEST_ASSERT_FALSE(AiMcpToolBridge::isAllowed("test_mqtt_configuration"));
    TEST_ASSERT_FALSE(AiMcpToolBridge::isAllowed("delete_document"));

    String error;
    JsonDocument args;
    deserializeJson(args, "{\"section\":\"protocol\"}");
    TEST_ASSERT_TRUE(AiMcpToolBridge::validateCall("get_device_config_section", args.as<JsonVariantConst>(), error));
    deserializeJson(args, "{\"draftId\":\"d\",\"expectedVersion\":0,\"bus\":{\"enabled\":true,\"mode\":\"master\",\"peripheralId\":\"uart1\",\"dePin\":4,\"transferType\":0,\"baudRate\":9600}}");
    TEST_ASSERT_TRUE(AiMcpToolBridge::validateCall("configure_modbus_rtu", args.as<JsonVariantConst>(), error));
    deserializeJson(args, "{\"draftId\":\"d\",\"expectedVersion\":0,\"taskIndex\":0,\"task\":{\"slaveAddress\":1,\"functionCode\":3,\"startAddressText\":\"40001\",\"quantity\":1,\"pollInterval\":30,\"mappings\":[{\"regOffset\":0,\"dataType\":\"uint16\",\"wordOrder\":0,\"byteOrder\":\"bigEndian\",\"scaleFactor\":0.1,\"unit\":\"C\"}]}}");
    TEST_ASSERT_TRUE(AiMcpToolBridge::validateCall("upsert_modbus_poll_task", args.as<JsonVariantConst>(), error));
    deserializeJson(args, "{\"draftId\":\"d\",\"expectedVersion\":0,\"connection\":{\"scheme\":\"mqtts\",\"server\":\"broker.example\",\"port\":8883,\"secretRef\":\"protocol.mqtt.password\"}}");
    TEST_ASSERT_TRUE(AiMcpToolBridge::validateCall("configure_mqtt", args.as<JsonVariantConst>(), error));
    deserializeJson(args, "{\"draftId\":\"d\",\"expectedVersion\":0,\"connection\":{\"password\":\"secret\"}}");
    TEST_ASSERT_FALSE(AiMcpToolBridge::validateCall("configure_mqtt", args.as<JsonVariantConst>(), error));
    deserializeJson(args, "{\"templateId\":\"mqtt_reporting_setup\",\"input\":{\"deviceId\":\"dev-1\"}}");
    TEST_ASSERT_TRUE(AiMcpToolBridge::validateCall("start_configuration_workflow", args.as<JsonVariantConst>(), error));
}

void test_single_awaiting_tool_roundtrip_and_stale_result_rejection() {
    FakeMcpTransport transport;
    transport.toolCallsRemaining = 1;
    AiProposalService proposals;
    AiConversationManager manager(transport, proposals);
    String error;
    TEST_ASSERT_TRUE(manager.start(error));
    String jobId;
    TEST_ASSERT_TRUE(manager.submit({"session-a"}, "查找 GPIO4", "", jobId, true).success);
    AiJobSnapshot snapshot;
    TEST_ASSERT_TRUE(waitState(manager, jobId, AiJobState::AWAITING_TOOL, snapshot));
    TEST_ASSERT_EQUAL_STRING("search_documents", snapshot.toolName.c_str());
    TEST_ASSERT_EQUAL_STRING("call-1", snapshot.toolCallId.c_str());
    TEST_ASSERT_EQUAL(1, snapshot.iteration);
    TEST_ASSERT_TRUE(snapshot.toolArgumentsJson.indexOf("GPIO4") >= 0);
    TEST_ASSERT_FALSE(manager.provideToolResult({"session-b"}, jobId, "call-1", "{}", ""));
    TEST_ASSERT_FALSE(manager.provideToolResult({"session-a"}, jobId, "wrong-call", "{}", ""));
    TEST_ASSERT_TRUE(manager.provideToolResult({"session-a"}, jobId, "call-1",
        "{\"citations\":[{\"documentId\":\"doc-1\",\"page\":4}],\"untrusted\":true}", ""));
    TEST_ASSERT_TRUE(waitState(manager, jobId, AiJobState::COMPLETED, snapshot));
    TEST_ASSERT_EQUAL(2, transport.calls);
    TEST_ASSERT_EQUAL_STRING("call-1", transport.lastRequest.toolCallId.c_str());
    TEST_ASSERT_TRUE(transport.lastRequest.toolResult.indexOf("doc-1") >= 0);
    TEST_ASSERT_FALSE(manager.provideToolResult({"session-a"}, jobId, "call-1", "{}", ""));
    manager.stop();
}

void test_max_eight_iterations_and_unknown_extra_fields_fail_closed() {
    FakeMcpTransport transport;
    transport.toolCallsRemaining = 9;
    AiProposalService proposals;
    AiConversationManager manager(transport, proposals);
    String error;
    TEST_ASSERT_TRUE(manager.start(error));
    String jobId;
    TEST_ASSERT_TRUE(manager.submit({"session-a"}, "loop", "", jobId, true).success);
    AiJobSnapshot snapshot;
    for (int i = 0; i < 7; ++i) {
        const bool awaiting = waitState(manager, jobId, AiJobState::AWAITING_TOOL, snapshot);
        char message[96];
        snprintf(message, sizeof(message), "awaiting i=%d calls=%d state=%u error=%s", i, transport.calls,
            static_cast<unsigned>(snapshot.state), snapshot.error.c_str());
        TEST_ASSERT_TRUE_MESSAGE(awaiting, message);
        TEST_ASSERT_TRUE_MESSAGE(manager.provideToolResult({"session-a"}, jobId, snapshot.toolCallId, "{}", ""), "provide iteration");
    }
    TEST_ASSERT_TRUE(waitState(manager, jobId, AiJobState::FAILED, snapshot));
    TEST_ASSERT_EQUAL_STRING("AI_TOOL_ITERATION_LIMIT", snapshot.error.c_str());
    TEST_ASSERT_EQUAL(8, transport.calls);
    manager.stop();

    FakeMcpTransport unknown;
    unknown.toolCallsRemaining = 1;
    unknown.tool = "apply_configuration_draft";
    AiConversationManager rejected(unknown, proposals);
    TEST_ASSERT_TRUE(rejected.start(error));
    TEST_ASSERT_TRUE(rejected.submit({"session-a"}, "apply", "", jobId, true).success);
    TEST_ASSERT_TRUE(waitState(rejected, jobId, AiJobState::FAILED, snapshot));
    TEST_ASSERT_EQUAL_STRING("AI_UNSUPPORTED_TOOL", snapshot.error.c_str());
    rejected.stop();

    FakeMcpTransport extra;
    extra.toolCallsRemaining = 1;
    extra.arguments = "{\"query\":\"x\",\"confirmed\":true}";
    AiConversationManager invalid(extra, proposals);
    TEST_ASSERT_TRUE(invalid.start(error));
    TEST_ASSERT_TRUE(invalid.submit({"session-a"}, "search", "", jobId, true).success);
    TEST_ASSERT_TRUE(waitState(invalid, jobId, AiJobState::FAILED, snapshot));
    TEST_ASSERT_EQUAL_STRING("AI_INVALID_TOOL_ARGUMENTS", snapshot.error.c_str());
    invalid.stop();
}

void test_cloud_timeout_and_cancellation_never_retry_or_apply() {
    FakeMcpTransport transport;
    transport.toolCallsRemaining = 1;
    AiProposalService proposals;
    int writes = 0;
    proposals.setExecutor([&](const AiProposal&, String&) { ++writes; return AiExecutionResult(); });
    AiConversationManager manager(transport, proposals);
    String error;
    TEST_ASSERT_TRUE(manager.start(error));
    String jobId;
    TEST_ASSERT_TRUE(manager.submit({"session-a"}, "search", "", jobId, true).success);
    AiJobSnapshot snapshot;
    TEST_ASSERT_TRUE(waitState(manager, jobId, AiJobState::AWAITING_TOOL, snapshot));
    TEST_ASSERT_TRUE(manager.provideToolResult({"session-a"}, jobId, snapshot.toolCallId, "", "MCP_TIMEOUT"));
    TEST_ASSERT_TRUE(waitState(manager, jobId, AiJobState::FAILED, snapshot));
    TEST_ASSERT_EQUAL_STRING("MCP_TIMEOUT", snapshot.error.c_str());
    TEST_ASSERT_EQUAL(1, transport.calls);
    TEST_ASSERT_EQUAL(0, writes);
    manager.stop();

    FakeMcpTransport second;
    second.toolCallsRemaining = 1;
    AiConversationManager cancelled(second, proposals);
    TEST_ASSERT_TRUE(cancelled.start(error));
    TEST_ASSERT_TRUE(cancelled.submit({"session-a"}, "search", "", jobId, true).success);
    TEST_ASSERT_TRUE(waitState(cancelled, jobId, AiJobState::AWAITING_TOOL, snapshot));
    TEST_ASSERT_TRUE(cancelled.cancel({"session-a"}, jobId));
    TEST_ASSERT_FALSE(cancelled.provideToolResult({"session-a"}, jobId, snapshot.toolCallId, "{}", ""));
    TEST_ASSERT_EQUAL(0, writes);
    cancelled.stop();
}

} // namespace

void test_ai_mcp_tools_group() {
    RUN_TEST(test_peripheral_schema_matches_shared_fixture);
    RUN_TEST(test_catalog_is_pruned_and_arguments_are_strict);
    RUN_TEST(test_cloud_schema_and_initial_draft_version_are_compatible);
    RUN_TEST(test_all_safe_configuration_domains_have_strict_schemas);
    RUN_TEST(test_single_awaiting_tool_roundtrip_and_stale_result_rejection);
    RUN_TEST(test_max_eight_iterations_and_unknown_extra_fields_fail_closed);
    RUN_TEST(test_cloud_timeout_and_cancellation_never_retry_or_apply);
}
