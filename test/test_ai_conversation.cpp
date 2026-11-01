#include <unity.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <chrono>
#include <deque>
#include <thread>

#include "core/AiConversationManager.h"
#include "../src/core/AiConversationManager.cpp"

namespace {

class FakeTransport : public AiProviderTransport {
public:
    std::deque<AiProviderResult> results;
    std::deque<String> texts;
    std::deque<String> tools;
    int calls = 0;
    AiProviderRequest lastRequest;

    AiProviderResult complete(const AiProviderRequest& request, AiProviderReply& reply) override {
        ++calls;
        lastRequest = request;
        if (!texts.empty()) {
            reply.text = texts.front();
            texts.pop_front();
        }
        if (!tools.empty()) {
            reply.toolName = tools.front();
            tools.pop_front();
            reply.toolCallId = "call-local";
            deserializeJson(reply.toolArguments, "{\"name\":\"relay\",\"pin\":4}");
        }
        if (!results.empty()) {
            auto result = results.front();
            results.pop_front();
            return result;
        }
        return AiProviderResult::success();
    }
};

bool waitFor(AiConversationManager& manager, const AiSessionIdentity& owner,
             const String& jobId, AiJobState state, AiJobSnapshot& out) {
    for (int i = 0; i < 200; ++i) {
        if (manager.getJob(owner, jobId, out) && out.state == state) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

void test_local_chat_without_mcp_and_history_bound() {
    FakeTransport transport;
    transport.texts = {"已回答"};
    AiProposalService proposals;
    AiConversationManager manager(transport, proposals);
    String error;
    TEST_ASSERT_TRUE(manager.start(error));
    String jobId;
    auto submitted = manager.submit({"a"}, "你好", "", jobId);
    TEST_ASSERT_TRUE(submitted.success);
    AiJobSnapshot snapshot;
    TEST_ASSERT_TRUE(waitFor(manager, {"a"}, jobId, AiJobState::COMPLETED, snapshot));
    TEST_ASSERT_EQUAL_STRING("已回答", snapshot.text.c_str());
    TEST_ASSERT_FALSE(manager.getJob({"b"}, jobId, snapshot));
    TEST_ASSERT_EQUAL(1, transport.calls);
    TEST_ASSERT_TRUE(transport.lastRequest.documentContext.isEmpty());
    TEST_ASSERT_TRUE(transport.lastRequest.history.size() <= 4);
    manager.stop();
    TEST_ASSERT_FALSE(manager.isRunning());
}

void test_local_tool_becomes_trusted_proposal_not_direct_write() {
    FakeTransport transport;
    transport.tools = {"create_gpio_output"};
    AiProposalService proposals;
    int writes = 0;
    proposals.setExecutor([&](const AiProposal&, String&) {
        ++writes;
        AiExecutionResult result;
        result.accepted = result.persisted = result.readback = true;
        return result;
    });
    AiConversationManager manager(transport, proposals);
    String error;
    TEST_ASSERT_TRUE(manager.start(error));
    String jobId;
    TEST_ASSERT_TRUE(manager.submit({"a"}, "GPIO4 继电器", "", jobId).success);
    AiJobSnapshot snapshot;
    TEST_ASSERT_TRUE(waitFor(manager, {"a"}, jobId, AiJobState::COMPLETED, snapshot));
    TEST_ASSERT_FALSE(snapshot.proposalId.isEmpty());
    TEST_ASSERT_EQUAL(0, writes);
    auto execution = manager.executeProposal({"a"}, snapshot.proposalId, "确认执行");
    TEST_ASSERT_TRUE(execution.accepted);
    TEST_ASSERT_EQUAL(1, writes);
    manager.stop();
}

void test_single_active_job_limits_cancel_and_cleanup() {
    FakeTransport transport;
    AiProposalService proposals;
    AiConversationManager manager(transport, proposals);
    manager.setResourceProbe([]() { return true; });
    String error;
    TEST_ASSERT_TRUE(manager.start(error));
    String first;
    TEST_ASSERT_FALSE(manager.submit({"a"}, String(std::string(2001, 'x')), "", first).success);
    TEST_ASSERT_FALSE(manager.submit({"a"}, "ok", String(std::string(12289, 'x')), first).success);
    TEST_ASSERT_TRUE(manager.submit({"a"}, "cancel me", "", first).success);
    TEST_ASSERT_TRUE(manager.cancel({"a"}, first));
    AiJobSnapshot snapshot;
    TEST_ASSERT_TRUE(waitFor(manager, {"a"}, first, AiJobState::CANCELLED, snapshot));
    manager.clearSession({"a"});
    TEST_ASSERT_FALSE(manager.getJob({"a"}, first, snapshot));
    for (int i = 0; i < 20; ++i) {
        String id;
        auto result = manager.submit({"a"}, "question", "", id);
        TEST_ASSERT_TRUE(result.success);
        TEST_ASSERT_TRUE(waitFor(manager, {"a"}, id, AiJobState::COMPLETED, snapshot));
    }
    manager.stop();
    TEST_ASSERT_FALSE(manager.start(error)); // stopped managers require a fresh lifecycle/configuration
}

void test_provider_timeout_and_resource_denial_do_not_pause_other_tasks() {
    FakeTransport transport;
    transport.results = {AiProviderResult::failure(AiProviderError::TIMEOUT)};
    AiProposalService proposals;
    AiConversationManager manager(transport, proposals);
    String error;
    TEST_ASSERT_TRUE(manager.start(error));
    String jobId;
    TEST_ASSERT_TRUE(manager.submit({"a"}, "question", "", jobId).success);
    AiJobSnapshot snapshot;
    TEST_ASSERT_TRUE(waitFor(manager, {"a"}, jobId, AiJobState::FAILED, snapshot));
    TEST_ASSERT_EQUAL_STRING("AI_TIMEOUT", snapshot.error.c_str());
    manager.stop();

    AiConversationManager denied(transport, proposals);
    denied.setResourceProbe([]() { return false; });
    TEST_ASSERT_TRUE(denied.start(error));
    auto result = denied.submit({"a"}, "question", "", jobId);
    TEST_ASSERT_FALSE(result.success);
    TEST_ASSERT_EQUAL_STRING("AI_RESOURCE_UNAVAILABLE", result.code.c_str());
    TEST_ASSERT_EQUAL(1, transport.calls);
    denied.stop();
}

} // namespace

void test_ai_conversation_group() {
    RUN_TEST(test_local_chat_without_mcp_and_history_bound);
    RUN_TEST(test_local_tool_becomes_trusted_proposal_not_direct_write);
    RUN_TEST(test_single_active_job_limits_cancel_and_cleanup);
    RUN_TEST(test_provider_timeout_and_resource_denial_do_not_pause_other_tasks);
}
