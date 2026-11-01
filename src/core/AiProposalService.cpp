#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_AI || defined(UNIT_TEST)
#include "core/AiProposalService.h"

#include <cstring>

namespace {
bool validText(JsonVariantConst object, const char* key, size_t max, bool required, String& out) {
    if (object[key].isNull()) return !required;
    if (!object[key].is<JsonString>()) return false;
    const size_t length = object[key].as<JsonString>().size();
    if ((required && length == 0) || length > max) return false;
    out = object[key].as<const char*>();
    return out.length() == length && out.indexOf('\r') < 0 && out.indexOf('\n') < 0;
}
}

AiProposalResult AiProposalService::createFromTool(const AiSessionIdentity& owner,
        const String& toolName, JsonVariantConst arguments, uint64_t configurationVersion,
        uint32_t nowMs) {
    AiProposalResult result;
    std::lock_guard<std::mutex> lock(mutex_);
    const bool gpio = toolName == "create_gpio_output";
    const bool dht = toolName == "create_dht_sensor";
    if ((!gpio && !dht) || owner.value.isEmpty() || !arguments.is<JsonObjectConst>()) {
        result.code = "AI_UNSUPPORTED_TOOL";
        return result;
    }
    for (JsonPairConst field : arguments.as<JsonObjectConst>()) {
        const String key = field.key().c_str();
        if (key != "name" && key != "pin" && key != "id" && !(gpio && key == "activeHigh")) {
            result.code = "AI_INVALID_TOOL_ARGUMENTS";
            return result;
        }
    }
    AiProposal proposal;
    proposal.owner = owner;
    proposal.skill = toolName;
    proposal.configurationVersion = configurationVersion;
    proposal.createdAtMs = nowMs;
    if (!validText(arguments, "name", 64, true, proposal.name) ||
        !validText(arguments, "id", 40, false, proposal.peripheralId) ||
        !arguments["pin"].is<int>()) {
        result.code = "AI_INVALID_TOOL_ARGUMENTS";
        return result;
    }
    const int pin = arguments["pin"].as<int>();
    if (pin < 0 || pin > 48 || (!arguments["activeHigh"].isNull() && !arguments["activeHigh"].is<bool>())) {
        result.code = "AI_INVALID_TOOL_ARGUMENTS";
        return result;
    }
    proposal.pin = static_cast<uint8_t>(pin);
    proposal.activeHigh = !gpio || arguments["activeHigh"] | true;
    if (proposal.peripheralId.isEmpty()) {
        proposal.peripheralId = String(dht ? "ai_dht_" : "ai_gpio_") + String(nowMs) + "_" + String(sequence_ + 1);
    }
    ++sequence_;
    proposal.id = "aip_" + String(nowMs, HEX) + "_" + String(sequence_, HEX) + "_" + String(random(0x7fffffff), HEX);
    if (proposals_.size() >= 8) proposals_.erase(proposals_.begin());
    proposals_.push_back(proposal);
    result.success = true;
    result.proposalId = proposal.id;
    result.createdAtMs = nowMs;
    return result;
}

AiExecutionResult AiProposalService::execute(const AiSessionIdentity& owner, const String& proposalId,
        const String& confirmationText, uint64_t currentConfigurationVersion, uint32_t nowMs) {
    AiExecutionResult result;
    if (confirmationText != "确认执行") {
        result.code = "AI_CONFIRMATION_REQUIRED";
        return result;
    }
    AiProposal selected;
    Executor executor;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        AiProposal* found = nullptr;
        for (auto& proposal : proposals_) {
            if (proposal.id == proposalId && proposal.owner == owner) {
                found = &proposal;
                break;
            }
        }
        if (!found) {
            result.code = "AI_PROPOSAL_NOT_FOUND";
            return result;
        }
        if (found->executed) {
            result.code = "AI_PROPOSAL_ALREADY_USED";
            return result;
        }
        if (static_cast<uint32_t>(nowMs - found->createdAtMs) >= 120000UL) {
            result.code = "AI_PROPOSAL_EXPIRED";
            return result;
        }
        if (found->configurationVersion != currentConfigurationVersion) {
            result.code = "AI_CONFIGURATION_CHANGED";
            return result;
        }
        found->executed = true;
        if (!executor_) {
            result.code = "AI_EXECUTOR_UNAVAILABLE";
            return result;
        }
        selected = *found;
        executor = executor_;
    }
    String error;
    result = executor(selected, error);
    if (!result.code.isEmpty()) return result;
    result.code = result.accepted ? (result.persisted ? "AI_PROPOSAL_EXECUTED" : "AI_SAVE_FAILED")
                                  : (error.isEmpty() ? String("AI_PROPOSAL_REJECTED") : error);
    return result;
}

void AiProposalService::clearOwner(const AiSessionIdentity& owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = proposals_.begin(); it != proposals_.end();) {
        if (it->owner == owner) it = proposals_.erase(it);
        else ++it;
    }
}
#endif
