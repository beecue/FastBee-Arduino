#include <unity.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <chrono>
#include <deque>
#include <thread>

#include "core/AiMcpToolBridge.h"
#include "core/AiConversationManager.h"
#include "../src/core/AiMcpToolBridge.cpp"

namespace {

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
    RUN_TEST(test_catalog_is_pruned_and_arguments_are_strict);
    RUN_TEST(test_single_awaiting_tool_roundtrip_and_stale_result_rejection);
    RUN_TEST(test_max_eight_iterations_and_unknown_extra_fields_fail_closed);
    RUN_TEST(test_cloud_timeout_and_cancellation_never_retry_or_apply);
}
