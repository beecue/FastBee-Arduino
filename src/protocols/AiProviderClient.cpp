#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_AI || defined(UNIT_TEST)
#include "protocols/AiProviderClient.h"

#include <cstring>
#include <limits>

#ifndef UNIT_TEST
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

class NetworkClientStream : public AiProviderStream {
public:
    explicit NetworkClientStream(NetworkClient* client = nullptr) : client_(client) {}
    int available() override { return client_ ? client_->available() : 0; }
    size_t readBytes(uint8_t* data, size_t length) override {
        return client_ ? client_->readBytes(data, length) : 0;
    }
    bool connected() override { return client_ && client_->connected(); }
private:
    NetworkClient* client_;
};

class NetworkHttpClient : public AiProviderHttpClient {
public:
    explicit NetworkHttpClient(HTTPClient& http) : http_(http) {}
    int post(const String& body) override {
        const int status = http_.POST(reinterpret_cast<uint8_t*>(const_cast<char*>(body.c_str())), body.length());
        stream_ = NetworkClientStream(http_.getStreamPtr());
        return status;
    }
    int responseSize() override { return http_.getSize(); }
    AiProviderStream* responseStream() override { return &stream_; }
    String responseHeader(const char* name) override { return http_.header(name); }
    void end() override { http_.end(); }
private:
    HTTPClient& http_;
    NetworkClientStream stream_;
};
#endif

namespace {
constexpr size_t kMaxResponseBytes = 65536;
constexpr size_t kMaxMessageBytes = 2000;
constexpr size_t kMaxDocumentBytes = 12288;

bool containsTool(const std::vector<String>& tools, const String& name) {
    for (const auto& tool : tools) if (tool == name) return true;
    return false;
}

void addTool(JsonArray tools, const char* name, const char* description, bool activeHigh) {
    JsonObject tool = tools.add<JsonObject>();
    tool["type"] = "function";
    JsonObject function = tool["function"].to<JsonObject>();
    function["name"] = name;
    function["description"] = description;
    JsonObject parameters = function["parameters"].to<JsonObject>();
    parameters["type"] = "object";
    parameters["additionalProperties"] = false;
    JsonObject properties = parameters["properties"].to<JsonObject>();
    properties["name"]["type"] = "string";
    properties["pin"]["type"] = "integer";
    properties["id"]["type"] = "string";
    if (activeHigh) properties["activeHigh"]["type"] = "boolean";
    JsonArray required = parameters["required"].to<JsonArray>();
    required.add("name");
    required.add("pin");
}
}

bool AiProviderClient::terminalAllowed(uint32_t deadlineAtMs, const std::atomic<bool>* cancelRequested) {
    if (cancelRequested && cancelRequested->load()) return false;
    const uint32_t now = static_cast<uint32_t>(millis());
    return !deadlineAtMs || static_cast<int32_t>(deadlineAtMs - now) > 0;
}

bool AiProviderClient::pumpWait(int available, size_t total, int length, uint32_t deadlineAtMs,
                                const std::atomic<bool>* cancelRequested) {
    if (!terminalAllowed(deadlineAtMs, cancelRequested)) return false;
    if (available > 0 || (length >= 0 && total >= static_cast<size_t>(length))) return true;
    delay(1);
    return true;
}

AiHttpReadStatus AiProviderClient::readResponse(AiProviderStream& stream, int contentLength,
        uint32_t deadlineAtMs, const std::atomic<bool>* cancelRequested, String& response, bool chunked) {
    response.clear();
    auto finish = [&](AiHttpReadStatus status) {
        return terminalAllowed(deadlineAtMs, cancelRequested) ? status : AiHttpReadStatus::TIMEOUT;
    };
    enum class ChunkState : uint8_t { SIZE, DATA, DATA_END, TRAILER, DONE };
    ChunkState chunkState = ChunkState::SIZE;
    String chunkLine;
    size_t chunkRemaining = 0;
    size_t trailerBytes = 0;
    constexpr size_t maxTrailerLineBytes = 1024;
    constexpr size_t maxTrailerBytes = 4096;
    bool carriage = false;
    auto consume = [&](uint8_t value) -> bool {
        switch (chunkState) {
            case ChunkState::SIZE:
                if (value == '\r') {
                    if (carriage) return false;
                    carriage = true;
                    return true;
                }
                if (value == '\n') {
                    if (!carriage) return false;
                    carriage = false;
                    const int extension = chunkLine.indexOf(';');
                    const String sizeText = extension >= 0 ? chunkLine.substring(0, extension) : chunkLine;
                    if (sizeText.isEmpty()) return false;
                    size_t parsed = 0;
                    for (size_t i = 0; i < sizeText.length(); ++i) {
                        const char digitText = sizeText[i];
                        unsigned digit;
                        if (digitText >= '0' && digitText <= '9') digit = digitText - '0';
                        else if (digitText >= 'a' && digitText <= 'f') digit = digitText - 'a' + 10;
                        else if (digitText >= 'A' && digitText <= 'F') digit = digitText - 'A' + 10;
                        else return false;
                        if (parsed > (std::numeric_limits<size_t>::max() - digit) / 16) return false;
                        parsed = parsed * 16 + digit;
                    }
                    chunkRemaining = parsed;
                    chunkLine = "";
                    chunkState = chunkRemaining == 0 ? ChunkState::TRAILER : ChunkState::DATA;
                    return true;
                }
                if (carriage || chunkLine.length() >= 24) return false;
                chunkLine.concat(static_cast<char>(value));
                return true;
            case ChunkState::DATA:
                if (chunkRemaining == 0) return false;
                response.concat(static_cast<char>(value));
                if (--chunkRemaining == 0) chunkState = ChunkState::DATA_END;
                return true;
            case ChunkState::DATA_END:
                if (value == '\r') {
                    if (carriage) return false;
                    carriage = true;
                    return true;
                }
                if (value == '\n') {
                    if (!carriage) return false;
                    carriage = false;
                    chunkState = ChunkState::SIZE;
                    return true;
                }
                return false;
            case ChunkState::TRAILER:
                if (++trailerBytes > maxTrailerBytes) return false;
                if (value == '\r') {
                    if (carriage) return false;
                    carriage = true;
                    return true;
                }
                if (value == '\n') {
                    if (!carriage) return false;
                    carriage = false;
                    if (chunkLine.isEmpty()) {
                        chunkState = ChunkState::DONE;
                        return true;
                    }
                    const int colon = chunkLine.indexOf(':');
                    if (colon <= 0) return false;
                    for (int i = 0; i < colon; ++i) {
                        const char c = chunkLine[i];
                        const bool token = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                            (c >= 'A' && c <= 'Z') || std::strchr("!#$%&'*+-.^_`|~", c);
                        if (!token) return false;
                    }
                    for (size_t i = colon + 1; i < chunkLine.length(); ++i) {
                        const uint8_t c = static_cast<uint8_t>(chunkLine[i]);
                        if ((c < 0x20 && c != '\t') || c == 0x7f) return false;
                    }
                    chunkLine = "";
                    return true;
                }
                if (carriage || chunkLine.length() >= maxTrailerLineBytes) return false;
                chunkLine.concat(static_cast<char>(value));
                return true;
            default:
                return true;
        }
    };
    uint8_t buffer[512];
    while (response.length() < kMaxResponseBytes + 1) {
        const int available = stream.available();
        if (chunked && chunkState == ChunkState::DONE) return finish(AiHttpReadStatus::COMPLETE);
        if (available <= 0 && contentLength < 0 && !stream.connected()) {
            return finish(chunked && chunkState != ChunkState::DONE ? AiHttpReadStatus::IO_ERROR : AiHttpReadStatus::COMPLETE);
        }
        if (!pumpWait(available, response.length(), contentLength, deadlineAtMs, cancelRequested)) {
            return AiHttpReadStatus::TIMEOUT;
        }
        if (!chunked && contentLength >= 0 && response.length() >= static_cast<size_t>(contentLength)) {
            return finish(AiHttpReadStatus::COMPLETE);
        }
        if (available > 0) {
            const size_t wanted = available > static_cast<int>(sizeof(buffer)) ? sizeof(buffer) : available;
            const size_t got = stream.readBytes(buffer, wanted);
            if (!terminalAllowed(deadlineAtMs, cancelRequested)) return AiHttpReadStatus::TIMEOUT;
            if (!got) return finish(stream.connected() ? AiHttpReadStatus::IO_ERROR : AiHttpReadStatus::COMPLETE);
            if (chunked) {
                for (size_t i = 0; i < got; ++i) {
                    if (!consume(buffer[i])) return finish(AiHttpReadStatus::IO_ERROR);
                }
            } else {
                response.concat(reinterpret_cast<const char*>(buffer), got);
            }
        }
    }
    return finish(AiHttpReadStatus::TOO_LARGE);
}

String AiProviderClient::safeError(AiProviderError error) {
    switch (error) {
        case AiProviderError::TIMEOUT: return "AI_TIMEOUT";
        case AiProviderError::UNAUTHORIZED: return "AI_PROVIDER_AUTH_FAILED";
        case AiProviderError::RATE_LIMITED: return "AI_PROVIDER_RATE_LIMITED";
        case AiProviderError::RESPONSE_TOO_LARGE: return "AI_RESPONSE_TOO_LARGE";
        case AiProviderError::UNSUPPORTED_TOOL: return "AI_UNSUPPORTED_TOOL";
        case AiProviderError::INVALID_ARGUMENTS: return "AI_INVALID_TOOL_ARGUMENTS";
        case AiProviderError::INVALID_RESPONSE: return "AI_INVALID_RESPONSE";
        default: return "AI_PROVIDER_UNAVAILABLE";
    }
}

String AiProviderClient::completionUrl(const AiConfig& config) {
    String base = config.baseUrl;
    while (base.endsWith("/")) base = base.substring(0, base.length() - 1);
    if (!base.endsWith("/v1")) base += "/v1";
    return base + "/chat/completions";
}

bool AiProviderClient::buildRequest(const AiProviderRequest& request, JsonDocument& output, String& error) {
    output.clear();
    if (request.userMessage.isEmpty() || request.userMessage.length() > kMaxMessageBytes ||
        request.userMessage.length() != std::strlen(request.userMessage.c_str()) ||
        request.documentContext.length() > kMaxDocumentBytes ||
        request.documentContext.length() != std::strlen(request.documentContext.c_str()) ||
        request.toolResult.length() > 16384) {
        error = "AI_INVALID_REQUEST";
        return false;
    }
    output["model"] = request.config.model;
    output["max_tokens"] = 800;
    output["tool_choice"] = "auto";
    output["stream"] = false;
    JsonArray messages = output["messages"].to<JsonArray>();
    messages.add<JsonObject>()["role"] = "system";
    messages[0]["content"] = request.systemPrompt;
    const size_t historyStart = request.history.size() > 8 ? request.history.size() - 8 : 0;
    for (size_t i = historyStart; i < request.history.size(); ++i) {
        JsonObject message = messages.add<JsonObject>();
        message["role"] = request.history[i].first;
        message["content"] = request.history[i].second;
    }
    JsonObject current = messages.add<JsonObject>();
    current["role"] = "user";
    current["content"] = request.documentContext.isEmpty()
        ? request.userMessage
        : request.userMessage + "\n\n<untrusted_document_context>\n" + request.documentContext +
          "\n</untrusted_document_context>";
    if (!request.toolCallId.isEmpty()) {
        if (request.toolName.isEmpty() || request.toolArgumentsJson.isEmpty() || request.toolResult.isEmpty()) {
            error = "AI_INVALID_TOOL_RESULT";
            return false;
        }
        JsonObject assistant = messages.add<JsonObject>();
        assistant["role"] = "assistant";
        assistant["content"] = nullptr;
        JsonObject call = assistant["tool_calls"].to<JsonArray>().add<JsonObject>();
        call["id"] = request.toolCallId;
        call["type"] = "function";
        call["function"]["name"] = request.toolName;
        call["function"]["arguments"] = request.toolArgumentsJson;
        JsonObject result = messages.add<JsonObject>();
        result["role"] = "tool";
        result["tool_call_id"] = request.toolCallId;
        result["content"] = request.toolResult;
    }
    JsonArray tools = output["tools"].to<JsonArray>();
    if (request.allowedTools.empty() || containsTool(request.allowedTools, "create_dht_sensor")) {
        addTool(tools, "create_dht_sensor", "Create a DHT sensor proposal", false);
    }
    for (const auto& name : request.allowedTools) {
        if (name == "create_gpio_output" || name == "create_dht_sensor") continue;
        JsonObject tool = tools.add<JsonObject>();
        tool["type"] = "function";
        tool["function"]["name"] = name;
        tool["function"]["description"] = "Call the named read-only or draft MCP tool through the browser";
        tool["function"]["parameters"]["type"] = "object";
    }
    error = "";
    return !output.overflowed();
}

bool AiProviderClient::parseResponse(const String& body, const std::vector<String>& allowedTools,
                                     AiProviderReply& reply, String& error) {
    reply = AiProviderReply();
    if (body.length() > kMaxResponseBytes) {
        error = "AI_RESPONSE_TOO_LARGE";
        return false;
    }
    if (body.length() != std::strlen(body.c_str())) {
        error = "AI_INVALID_RESPONSE";
        return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body) || doc.overflowed() || !doc.is<JsonObject>() ||
        !doc["choices"].is<JsonArray>() || doc["choices"].size() != 1) {
        error = "AI_INVALID_RESPONSE";
        return false;
    }
    JsonObjectConst message = doc["choices"][0]["message"];
    if (message.isNull()) {
        error = "AI_INVALID_RESPONSE";
        return false;
    }
    const bool hasTools = !message["tool_calls"].isNull();
    const bool hasContent = message["content"].is<JsonString>();
    if (hasTools == hasContent) {
        error = "AI_INVALID_RESPONSE";
        return false;
    }
    if (hasContent) {
        const String text = message["content"].as<const char*>();
        if (text.isEmpty() || text.length() > 16384) {
            error = "AI_INVALID_RESPONSE";
            return false;
        }
        reply.text = text;
        error = "";
        return true;
    }
    JsonArrayConst calls = message["tool_calls"];
    if (calls.size() != 1) {
        error = "AI_MULTIPLE_TOOL_CALLS";
        return false;
    }
    JsonObjectConst call = calls[0];
    for (JsonPairConst field : call) {
        const String key = field.key().c_str();
        if (key != "id" && key != "type" && key != "function") {
            error = "AI_INVALID_TOOL_CALL";
            return false;
        }
    }
    if (!call["id"].is<JsonString>() || call["id"].as<JsonString>().size() == 0 ||
        call["id"].as<JsonString>().size() > 128 || call["type"] != "function" ||
        !call["function"].is<JsonObjectConst>()) {
        error = "AI_INVALID_TOOL_CALL";
        return false;
    }
    JsonObjectConst function = call["function"];
    for (JsonPairConst field : function) {
        const String key = field.key().c_str();
        if (key != "name" && key != "arguments") {
            error = "AI_INVALID_TOOL_CALL";
            return false;
        }
    }
    if (!function["name"].is<JsonString>() || !function["arguments"].is<JsonString>()) {
        error = "AI_INVALID_TOOL_CALL";
        return false;
    }
    const String name = function["name"].as<const char*>();
    if (!containsTool(allowedTools, name)) {
        error = "AI_UNSUPPORTED_TOOL";
        return false;
    }
    if (deserializeJson(reply.toolArguments, function["arguments"].as<const char*>()) ||
        !reply.toolArguments.is<JsonObject>() || reply.toolArguments.overflowed()) {
        error = "AI_INVALID_TOOL_ARGUMENTS";
        return false;
    }
    reply.toolName = name;
    reply.toolCallId = call["id"].as<const char*>();
    error = "";
    return true;
}

AiProviderResult AiProviderClient::completePost(AiProviderHttpClient& http, const String& body,
        const AiProviderRequest& request, AiProviderReply& reply, uint32_t deadlineAtMs) {
    auto remaining = [&]() -> uint32_t {
        if (!terminalAllowed(deadlineAtMs, request.cancelRequested)) return 0;
        const int32_t left = static_cast<int32_t>(deadlineAtMs - millis());
        return left > 0 ? static_cast<uint32_t>(left) : 0;
    };
    const int status = http.post(body);
    AiProviderResult result;
    if (!remaining()) {
        result = AiProviderResult::failure(AiProviderError::TIMEOUT);
    } else if (status == 401 || status == 403) {
        result = AiProviderResult::failure(AiProviderError::UNAUTHORIZED);
    } else if (status == 429) {
        result = AiProviderResult::failure(AiProviderError::RATE_LIMITED);
    } else if (status < 200 || status >= 300) {
        result = AiProviderResult::failure(status < 0 ? AiProviderError::TIMEOUT : AiProviderError::TRANSPORT);
    } else {
        const int length = http.responseSize();
        if (length > static_cast<int>(kMaxResponseBytes)) {
            result = AiProviderResult::failure(AiProviderError::RESPONSE_TOO_LARGE);
        } else {
            String response;
            response.reserve(length > 0 ? length : 4096);
            AiProviderStream* stream = http.responseStream();
            const AiHttpReadStatus readStatus = readResponse(*stream, length, deadlineAtMs,
                request.cancelRequested, response, http.responseHeader("Transfer-Encoding").equalsIgnoreCase("chunked"));
            if (readStatus == AiHttpReadStatus::TIMEOUT || readStatus == AiHttpReadStatus::IO_ERROR) {
                result = AiProviderResult::failure(AiProviderError::TIMEOUT);
            } else if (readStatus == AiHttpReadStatus::TOO_LARGE) {
                result = AiProviderResult::failure(AiProviderError::RESPONSE_TOO_LARGE);
            } else {
                String error;
                if (!parseResponse(response, request.allowedTools.empty()
                        ? std::vector<String>{"create_dht_sensor"}
                        : request.allowedTools, reply, error)) {
                    result = AiProviderResult::failure(error == "AI_RESPONSE_TOO_LARGE"
                        ? AiProviderError::RESPONSE_TOO_LARGE
                        : error == "AI_UNSUPPORTED_TOOL" ? AiProviderError::UNSUPPORTED_TOOL
                        : AiProviderError::INVALID_RESPONSE);
                }
            }
        }
    }
    http.end();
    return result;
}

AiProviderResult AiProviderClient::complete(const AiProviderRequest& request, AiProviderReply& reply) {
#ifdef UNIT_TEST
    (void)request;
    (void)reply;
    return AiProviderResult::failure(AiProviderError::TRANSPORT);
#else
    const uint32_t deadlineAtMs = request.deadlineAtMs ? request.deadlineAtMs : millis() + 20000UL;
    auto remaining = [&]() -> uint32_t {
        if (!terminalAllowed(deadlineAtMs, request.cancelRequested)) return 0;
        const int32_t left = static_cast<int32_t>(deadlineAtMs - millis());
        return left > 0 ? static_cast<uint32_t>(left) : 0;
    };
    if (!remaining()) return AiProviderResult::failure(AiProviderError::TIMEOUT);
    String validation;
    if (!AiConfigStore::validateProvider(request.config, validation) || request.config.apiKey.isEmpty() ||
        time(nullptr) < 1577836800) {
        return AiProviderResult::failure(AiProviderError::TRANSPORT);
    }
    JsonDocument body;
    String error;
    if (!buildRequest(request, body, error)) return AiProviderResult::failure(AiProviderError::INVALID_ARGUMENTS);
    String serialized;
    serializeJson(body, serialized);
    WiFiClientSecure secure;
    secure.setCACertBundle(x509_crt_bundle_start,
                           static_cast<size_t>(x509_crt_bundle_end - x509_crt_bundle_start));
    secure.setHandshakeTimeout(remaining() > 5000 ? 5 : 1);
    HTTPClient http;
    if (!http.begin(static_cast<NetworkClient&>(secure), completionUrl(request.config))) {
        return AiProviderResult::failure(AiProviderError::TRANSPORT);
    }
    http.setConnectTimeout(remaining() > 5000 ? 5000 : remaining());
    http.setTimeout(remaining() > 20000 ? 20000 : remaining());
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.setReuse(false);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept", "application/json");
    http.addHeader("Authorization", "Bearer " + request.config.apiKey);
    if (!remaining()) {
        http.end();
        return AiProviderResult::failure(AiProviderError::TIMEOUT);
    }
    NetworkHttpClient exchange(http);
    return completePost(exchange, serialized, request, reply, deadlineAtMs);
#endif
}
#endif
