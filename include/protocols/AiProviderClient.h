#ifndef AI_PROVIDER_CLIENT_H
#define AI_PROVIDER_CLIENT_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <atomic>
#include <utility>
#include <vector>

#include "core/AiConfig.h"
#include "protocols/AiProviderStream.h"

enum class AiProviderError : uint8_t {
    NONE,
    TIMEOUT,
    TRANSPORT,
    UNAUTHORIZED,
    RATE_LIMITED,
    RESPONSE_TOO_LARGE,
    INVALID_RESPONSE,
    UNSUPPORTED_TOOL,
    INVALID_ARGUMENTS
};

struct AiProviderRequest {
    AiConfig config;
    String systemPrompt;
    String userMessage;
    String documentContext;
    std::vector<std::pair<String, String>> history;
    String toolCallId;
    String toolName;
    String toolArgumentsJson;
    String toolResult;
    std::vector<String> allowedTools;
    uint32_t deadlineAtMs = 0;
    const std::atomic<bool>* cancelRequested = nullptr;
};

struct AiProviderReply {
    String text;
    String toolName;
    String toolCallId;
    JsonDocument toolArguments;
};

struct AiProviderResult {
    AiProviderError error = AiProviderError::NONE;
    String detail;
    bool ok() const { return error == AiProviderError::NONE; }
    static AiProviderResult success() { return {}; }
    static AiProviderResult failure(AiProviderError value) { return {value, {}}; }
};

class AiProviderTransport {
public:
    virtual ~AiProviderTransport() = default;
    virtual AiProviderResult complete(const AiProviderRequest& request, AiProviderReply& reply) = 0;
};

class AiProviderClient : public AiProviderTransport {
public:
    AiProviderResult complete(const AiProviderRequest& request, AiProviderReply& reply) override;
    static bool buildRequest(const AiProviderRequest& request, JsonDocument& output, String& error);
    static bool parseResponse(const String& body, const std::vector<String>& allowedTools,
                              AiProviderReply& reply, String& error);
    static String completionUrl(const AiConfig& config);
    static String safeError(AiProviderError error);
    static bool terminalAllowed(uint32_t deadlineAtMs, const std::atomic<bool>* cancelRequested);
    static bool pumpWait(int available, size_t total, int length, uint32_t deadlineAtMs,
                         const std::atomic<bool>* cancelRequested);
    static AiHttpReadStatus readResponse(AiProviderStream& stream, int contentLength,
                                         uint32_t deadlineAtMs, const std::atomic<bool>* cancelRequested,
                                         String& response, bool chunked = false);
};

#endif
