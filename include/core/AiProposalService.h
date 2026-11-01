#ifndef AI_PROPOSAL_SERVICE_H
#define AI_PROPOSAL_SERVICE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <functional>
#include <mutex>
#include <vector>

struct AiSessionIdentity {
    String value;
    bool operator==(const AiSessionIdentity& other) const { return value == other.value; }
};

struct AiProposal {
    String id;
    AiSessionIdentity owner;
    String skill;
    String peripheralId;
    String name;
    uint8_t pin = 255;
    bool activeHigh = true;
    uint64_t configurationVersion = 0;
    uint32_t createdAtMs = 0;
    bool executed = false;
};

struct AiProposalResult {
    bool success = false;
    String code;
    String proposalId;
    uint32_t createdAtMs = 0;
};

struct AiExecutionResult {
    bool accepted = false;
    bool persisted = false;
    bool readback = false;
    bool observed = false;
    bool physicalConfirmed = false;
    String code;
};

class AiProposalService {
public:
    using Executor = std::function<AiExecutionResult(const AiProposal&, String&)>;
    void setExecutor(Executor executor) { executor_ = executor; }
    AiProposalResult createFromTool(const AiSessionIdentity& owner, const String& toolName,
                                    JsonVariantConst arguments, uint64_t configurationVersion,
                                    uint32_t nowMs);
    AiExecutionResult execute(const AiSessionIdentity& owner, const String& proposalId,
                              const String& confirmationText, uint64_t currentConfigurationVersion,
                              uint32_t nowMs);
    void clearOwner(const AiSessionIdentity& owner);

private:
    mutable std::mutex mutex_;
    std::vector<AiProposal> proposals_;
    Executor executor_;
    uint32_t sequence_ = 0;
};

#endif
