#ifndef AI_CONVERSATION_MANAGER_H
#define AI_CONVERSATION_MANAGER_H

#include <Arduino.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

#include "core/AiProposalService.h"
#include "protocols/AiProviderClient.h"

enum class AiJobState : uint8_t {
    QUEUED,
    RUNNING,
    AWAITING_TOOL,
    COMPLETED,
    FAILED,
    CANCELLED
};

struct AiJobSnapshot {
    String jobId;
    AiJobState state = AiJobState::QUEUED;
    String text;
    String error;
    String proposalId;
    String toolCallId;
    String toolName;
    String toolArgumentsJson;
    uint8_t iteration = 0;
};

struct AiSubmitResult {
    bool success = false;
    String code;
};

class AiConversationManager {
public:
    AiConversationManager(AiProviderTransport& transport, AiProposalService& proposals);
    ~AiConversationManager();
    bool start(String& error);
    void requestStop();
    void stop();
    bool isRunning() const { return running_; }
    bool isWorkerActive() const { return workerActive_.load(); }
    AiSubmitResult submit(const AiSessionIdentity& owner, const String& message,
                          const String& documentContext, String& jobId, bool mcpEnabled = false);
    bool provideToolResult(const AiSessionIdentity& owner, const String& jobId,
                           const String& toolCallId, const String& result, const String& error);
    bool getJob(const AiSessionIdentity& owner, const String& jobId, AiJobSnapshot& out) const;
    bool cancel(const AiSessionIdentity& owner, const String& jobId);
    AiExecutionResult executeProposal(const AiSessionIdentity& owner, const String& proposalId,
                                      const String& confirmationText);
    void clearSession(const AiSessionIdentity& owner);
    void setResourceProbe(std::function<bool()> probe) { resourceProbe_ = probe; }
    void setConfigurationVersionProvider(std::function<uint64_t()> provider) {
        configurationVersion_ = provider;
    }

private:
    struct Job {
        AiJobSnapshot snapshot;
        AiSessionIdentity owner;
        String message;
        String documentContext;
        uint32_t startedAtMs = 0;
        std::atomic<bool> cancelRequested{false};
        bool mcpEnabled = false;
        bool toolResultReady = false;
        String toolResult;
        String toolResultError;
    };

    void workerLoop();
    void workerMain();
#ifndef UNIT_TEST
    static void workerEntry(void* argument);
    TaskHandle_t workerHandle_ = nullptr;
#endif
    void runJob(std::shared_ptr<Job> job);
    void finishHistory(const Job& job, const String& assistantText);
    std::vector<std::pair<String, String>> historyFor(const AiSessionIdentity& owner) const;

    AiProviderTransport& transport_;
    AiProposalService& proposals_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::thread worker_;
    std::atomic<bool> workerActive_{false};
    bool running_ = false;
    bool everStarted_ = false;
    bool hasJob_ = false;
    std::shared_ptr<Job> current_;
    uint32_t sequence_ = 0;
    std::map<String, std::deque<std::pair<String, String>>> histories_;
    std::function<bool()> resourceProbe_;
    std::function<uint64_t()> configurationVersion_;
    std::function<bool(AiConfig&)> configProvider_;
};

#endif
