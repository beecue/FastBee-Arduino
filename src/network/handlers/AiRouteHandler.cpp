#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_AI || defined(UNIT_TEST)
#include "network/handlers/AiRouteHandler.h"

#include <new>

#include "core/AiConfig.h"
#include "core/AiConversationManager.h"
#include "core/FastBeeFramework.h"
#include "core/RuntimeFeatureManager.h"
#include "network/WebHandlerContext.h"
#include "security/AuthManager.h"

namespace {
struct BodyBuffer {
    String content;
    bool oversized = false;
};
using BodyOwner = std::unique_ptr<BodyBuffer>;

BodyOwner* bodyOf(AsyncWebServerRequest* request) {
    return static_cast<BodyOwner*>(request->_tempObject);
}
void releaseBody(AsyncWebServerRequest* request) {
    delete bodyOf(request);
    request->_tempObject = nullptr;
}
AiSessionIdentity requestOwner(WebHandlerContext* ctx, AsyncWebServerRequest* request) {
    AuthResult auth = ctx->authenticateRequest(request);
    SecurityConfig security = static_cast<AuthManager*>(ctx->authManager)->getSecurityConfig();
    String session = AuthManager::extractSessionIdFromRequest(request, security.cookieName);
    if (session.isEmpty() && request->hasHeader("Authorization")) session = request->header("Authorization");
    return {auth.username + ":" + session};
}
const char* stateName(AiJobState state) {
    switch (state) {
        case AiJobState::QUEUED: return "queued";
        case AiJobState::RUNNING: return "running";
        case AiJobState::AWAITING_TOOL: return "awaiting_tool";
        case AiJobState::COMPLETED: return "completed";
        case AiJobState::FAILED: return "failed";
        default: return "cancelled";
    }
}
void jobJson(const AiJobSnapshot& snapshot, JsonObject output) {
    output["jobId"] = snapshot.jobId;
    output["state"] = stateName(snapshot.state);
    output["text"] = snapshot.text;
    output["error"] = snapshot.error;
    output["proposalId"] = snapshot.proposalId;
    output["iteration"] = snapshot.iteration;
    if (snapshot.state == AiJobState::AWAITING_TOOL) {
        JsonObject call = output["toolCall"].to<JsonObject>();
        call["toolCallId"] = snapshot.toolCallId;
        call["name"] = snapshot.toolName;
        call["arguments"] = serialized(snapshot.toolArgumentsJson);
    }
}
int submitStatus(const String& code) {
    if (code == "AI_JOB_ACTIVE") return 409;
    if (code == "AI_RESOURCE_UNAVAILABLE" || code == "AI_NOT_ACTIVE") return 503;
    return 400;
}
int executionStatus(const String& code) {
    if (code == "AI_PROPOSAL_NOT_FOUND") return 404;
    if (code == "AI_PROPOSAL_EXPIRED") return 410;
    if (code == "AI_PROPOSAL_ALREADY_USED" || code == "AI_CONFIGURATION_CHANGED") return 409;
    if (code == "AI_CONFIRMATION_REQUIRED" || code == "AI_INVALID_TOOL_ARGUMENTS") return 422;
    return 503;
}
}

bool AiRouteHandler::admit(AsyncWebServerRequest* request, bool developer) {
    const bool allowed = ctx_->requireSameOrigin(request) && ctx_->requireAuth(request) &&
                         (!developer || ctx_->requireDeveloperMode(request));
    if (!allowed) releaseBody(request);
    return allowed;
}

void AiRouteHandler::accumulateBody(AsyncWebServerRequest* request, uint8_t* data, size_t len,
                                    size_t index, size_t total) {
    if (index == 0) {
        releaseBody(request);
        request->_tempObject = new (std::nothrow) BodyOwner(new (std::nothrow) BodyBuffer());
        request->onDisconnect([request]() { releaseBody(request); });
        BodyOwner* body = bodyOf(request);
        if (body && *body) {
            (*body)->oversized = total > kMaxBodyBytes;
            if (!(*body)->oversized) (*body)->content.reserve(total);
        }
    }
    BodyOwner* body = bodyOf(request);
    if (!body || !*body || (*body)->oversized || total > kMaxBodyBytes) return;
    if (!(*body)->content.concat(reinterpret_cast<const char*>(data), len)) (*body)->oversized = true;
}

bool AiRouteHandler::parseBody(AsyncWebServerRequest* request, JsonDocument& output) {
    BodyOwner* body = bodyOf(request);
    if (!body || !*body || (*body)->oversized || (*body)->content.isEmpty()) {
        const bool oversized = body && *body && (*body)->oversized;
        releaseBody(request);
        ctx_->sendError(request, oversized ? 413 : 400, oversized ? "AI_BODY_TOO_LARGE" : "AI_BODY_REQUIRED");
        return false;
    }
    const auto error = deserializeJson(output, (*body)->content.c_str(), (*body)->content.length());
    releaseBody(request);
    if (error || output.overflowed() || !output.is<JsonObject>()) {
        ctx_->sendBadRequest(request, "AI_INVALID_JSON");
        return false;
    }
    return true;
}

void AiRouteHandler::handleConfigGet(AsyncWebServerRequest* request) {
    if (!admit(request)) return;
    AiConfig config;
    String error;
    AiConfigStore store;
    if (!store.load(config, error)) {
        ctx_->sendError(request, 500, "AI_CONFIG_LOAD_FAILED");
        return;
    }
    JsonDocument data;
    auto& features = FastBee::RuntimeFeatureManager::instance();
    AiConfigStore::writePublicJson(config, data.to<JsonObject>(), features.isUserEnabled("aiAssistant"),
        features.shouldEnable("aiAssistant"), features.runtimeDisableReason("aiAssistant"));
    ctx_->sendSuccess(request, data);
}

void AiRouteHandler::handleConfigPut(AsyncWebServerRequest* request) {
    if (!admit(request, true)) return;
    JsonDocument input;
    if (!parseBody(request, input)) return;
    for (JsonPairConst field : input.as<JsonObjectConst>()) {
        const String key = field.key().c_str();
        if (key != "baseUrl" && key != "model" && key != "apiKey" && key != "clearApiKey") {
            ctx_->sendBadRequest(request, "AI_UNKNOWN_CONFIG_FIELD");
            return;
        }
    }
    AiConfigStore store;
    AiConfig config;
    String error;
    if (!store.load(config, error)) {
        ctx_->sendError(request, 500, "AI_CONFIG_LOAD_FAILED");
        return;
    }
    if (!input["baseUrl"].isNull()) {
        if (!input["baseUrl"].is<JsonString>()) { ctx_->sendBadRequest(request, "AI_INVALID_URL"); return; }
        config.baseUrl = input["baseUrl"].as<const char*>();
    }
    if (!input["model"].isNull()) {
        if (!input["model"].is<JsonString>()) { ctx_->sendBadRequest(request, "AI_INVALID_PROVIDER_CONFIG"); return; }
        config.model = input["model"].as<const char*>();
    }
    if (input["clearApiKey"] == true) config.apiKey = "";
    else if (!input["apiKey"].isNull()) {
        if (!input["apiKey"].is<JsonString>()) { ctx_->sendBadRequest(request, "AI_INVALID_PROVIDER_CONFIG"); return; }
        const String key = input["apiKey"].as<const char*>();
        if (!key.isEmpty()) config.apiKey = key;
    }
    input.clear();
    if (!store.saveProvider(config, error)) {
        ctx_->sendError(request, error == "AI_CONFIG_SAVE_FAILED" ? 500 : 400, error);
        return;
    }
    config.apiKey = "";
    JsonDocument data;
    AiConfigStore::writePublicJson(config, data.to<JsonObject>(), true, false, "configuration_saved");
    ctx_->sendSuccess(request, data);
}

void AiRouteHandler::handleMcpConfigPut(AsyncWebServerRequest* request) {
    if (!admit(request, true)) return;
    JsonDocument input;
    if (!parseBody(request, input)) return;
    if (input.size() != 1 || !input["mcpBaseUrl"].is<JsonString>()) {
        ctx_->sendBadRequest(request, "AI_INVALID_MCP_CONFIG");
        return;
    }
    const String url = input["mcpBaseUrl"].as<const char*>();
    AiConfigStore store;
    String error;
    if (!store.saveMcpBaseUrl(url, error)) {
        ctx_->sendError(request, error == "AI_CONFIG_SAVE_FAILED" ? 500 : 400, error);
        return;
    }
    JsonDocument data;
    data["mcpBaseUrl"] = url;
    data["saved"] = true;
    ctx_->sendSuccess(request, data);
}

void AiRouteHandler::handleJobSubmit(AsyncWebServerRequest* request) {
    if (!admit(request)) return;
    JsonDocument input;
    if (!parseBody(request, input)) return;
    for (JsonPairConst field : input.as<JsonObjectConst>()) {
        const String key = field.key().c_str();
        if (key != "message" && key != "clientSessionId" && key != "documentContext" && key != "mcpEnabled") {
            ctx_->sendBadRequest(request, "AI_UNKNOWN_JOB_FIELD");
            return;
        }
    }
    if (!input["message"].is<JsonString>() || input["message"].as<JsonString>().size() > 2000 ||
        (!input["clientSessionId"].isNull() && (!input["clientSessionId"].is<JsonString>() ||
         input["clientSessionId"].as<JsonString>().size() > 128)) ||
        (!input["documentContext"].isNull() && (!input["documentContext"].is<JsonString>() ||
         input["documentContext"].as<JsonString>().size() > 12288)) ||
        (!input["mcpEnabled"].isNull() && !input["mcpEnabled"].is<bool>())) {
        ctx_->sendBadRequest(request, "AI_INVALID_REQUEST");
        return;
    }
    AiConversationManager* manager = FastBeeFramework::getInstance()->getAiConversationManager();
    if (!manager) { ctx_->sendError(request, 503, "AI_NOT_ACTIVE"); return; }
    String jobId;
    auto result = manager->submit(requestOwner(ctx_, request), input["message"].as<const char*>(),
                                  input["documentContext"] | "", jobId, input["mcpEnabled"] | false);
    if (!result.success) { ctx_->sendError(request, submitStatus(result.code), result.code); return; }
    JsonDocument response;
    response["success"] = true;
    response["data"]["jobId"] = jobId;
    ctx_->sendJsonResponse(request, 202, response);
}

void AiRouteHandler::handleJobGet(AsyncWebServerRequest* request) {
    if (!admit(request)) return;
    const String prefix = "/api/ai/jobs/";
    const String jobId = request->url().substring(prefix.length());
    if (jobId.isEmpty() || jobId.length() > 96 || jobId.indexOf('/') >= 0) {
        ctx_->sendBadRequest(request, "AI_INVALID_JOB_ID"); return;
    }
    AiConversationManager* manager = FastBeeFramework::getInstance()->getAiConversationManager();
    AiJobSnapshot snapshot;
    if (!manager || !manager->getJob(requestOwner(ctx_, request), jobId, snapshot)) {
        ctx_->sendNotFound(request); return;
    }
    JsonDocument data;
    jobJson(snapshot, data.to<JsonObject>());
    ctx_->sendSuccess(request, data);
}

void AiRouteHandler::handleJobCancel(AsyncWebServerRequest* request) {
    const String path = request->url().substring(String("/api/ai/jobs/").length());
    if (path.endsWith("/tool-result")) {
        handleToolResult(request);
        return;
    }
    if (!admit(request)) return;
    if (!path.endsWith("/cancel")) { ctx_->sendBadRequest(request, "AI_INVALID_JOB_PATH"); return; }
    const String jobId = path.substring(0, path.length() - 7);
    AiConversationManager* manager = FastBeeFramework::getInstance()->getAiConversationManager();
    if (!manager || !manager->cancel(requestOwner(ctx_, request), jobId)) {
        ctx_->sendError(request, 409, "AI_JOB_NOT_CANCELLABLE"); return;
    }
    JsonDocument data;
    data["cancelRequested"] = true;
    data["stopped"] = false;
    ctx_->sendSuccess(request, data);
}

void AiRouteHandler::handleToolResult(AsyncWebServerRequest* request) {
    if (!admit(request)) return;
    JsonDocument input;
    if (!parseBody(request, input)) return;
    for (JsonPairConst field : input.as<JsonObjectConst>()) {
        const String key = field.key().c_str();
        if (key != "toolCallId" && key != "result" && key != "error") {
            ctx_->sendBadRequest(request, "AI_UNKNOWN_TOOL_RESULT_FIELD");
            return;
        }
    }
    if (!input["toolCallId"].is<JsonString>() || input["toolCallId"].as<JsonString>().size() > 128 ||
        (!input["result"].isNull() && !input["result"].is<JsonString>()) ||
        (!input["error"].isNull() && !input["error"].is<JsonString>()) ||
        input["result"].as<JsonString>().size() > 16384 || input["error"].as<JsonString>().size() > 128) {
        ctx_->sendBadRequest(request, "AI_INVALID_TOOL_RESULT");
        return;
    }
    const String result = input["result"] | "";
    const String error = input["error"] | "";
    if (result.isEmpty() == error.isEmpty()) {
        ctx_->sendBadRequest(request, "AI_INVALID_TOOL_RESULT");
        return;
    }
    String path = request->url().substring(String("/api/ai/jobs/").length());
    const String jobId = path.substring(0, path.length() - 12);
    AiConversationManager* manager = FastBeeFramework::getInstance()->getAiConversationManager();
    if (!manager || !manager->provideToolResult(requestOwner(ctx_, request), jobId,
            input["toolCallId"].as<const char*>(), result, error)) {
        ctx_->sendError(request, 409, "AI_STALE_TOOL_RESULT");
        return;
    }
    JsonDocument data;
    data["accepted"] = true;
    ctx_->sendSuccess(request, data);
}

void AiRouteHandler::handleProposalExecute(AsyncWebServerRequest* request) {
    if (!admit(request, true)) return;
    JsonDocument input;
    if (!parseBody(request, input)) return;
    if (input.size() != 1 || input["confirmationText"] != "确认执行") {
        ctx_->sendError(request, 422, "AI_CONFIRMATION_REQUIRED"); return;
    }
    const String prefix = "/api/ai/proposals/";
    String path = request->url().substring(prefix.length());
    if (!path.endsWith("/execute")) { ctx_->sendBadRequest(request, "AI_INVALID_PROPOSAL_PATH"); return; }
    const String proposalId = path.substring(0, path.length() - 8);
    AiConversationManager* manager = FastBeeFramework::getInstance()->getAiConversationManager();
    if (!manager) { ctx_->sendError(request, 503, "AI_NOT_ACTIVE"); return; }
    auto result = manager->executeProposal(requestOwner(ctx_, request), proposalId,
                                           input["confirmationText"].as<const char*>());
    JsonDocument data;
    data["accepted"] = result.accepted;
    data["persisted"] = result.persisted;
    data["readback"] = result.readback;
    data["observed"] = result.observed;
    data["physicalConfirmed"] = result.physicalConfirmed;
    data["code"] = result.code;
    const bool ok = result.accepted && result.persisted;
    data["success"] = ok;
    if (!ok) { ctx_->sendError(request, executionStatus(result.code), result.code); return; }
    ctx_->sendSuccess(request, data);
}

void AiRouteHandler::handleSessionClear(AsyncWebServerRequest* request) {
    if (!admit(request)) return;
    AiConversationManager* manager = FastBeeFramework::getInstance()->getAiConversationManager();
    if (manager) manager->clearSession(requestOwner(ctx_, request));
    ctx_->sendSuccess(request, "AI session cleared");
}

void AiRouteHandler::setupRoutes(AsyncWebServer* server) {
    server->on(AsyncURIMatcher::exact("/api/ai/config"), HTTP_GET,
        [this](AsyncWebServerRequest* request) { handleConfigGet(request); });
    server->on(AsyncURIMatcher::exact("/api/ai/config"), HTTP_PUT,
        [this](AsyncWebServerRequest* request) { handleConfigPut(request); }, nullptr,
        [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            accumulateBody(request, data, len, index, total);
        });
    server->on(AsyncURIMatcher::exact("/api/ai/mcp-config"), HTTP_PUT,
        [this](AsyncWebServerRequest* request) { handleMcpConfigPut(request); }, nullptr,
        [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            accumulateBody(request, data, len, index, total);
        });
    server->on(AsyncURIMatcher::exact("/api/ai/jobs"), HTTP_POST,
        [this](AsyncWebServerRequest* request) { handleJobSubmit(request); }, nullptr,
        [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            accumulateBody(request, data, len, index, total);
        });
    server->on(AsyncURIMatcher::prefix("/api/ai/jobs/"), HTTP_GET,
        [this](AsyncWebServerRequest* request) { handleJobGet(request); });
    server->on(AsyncURIMatcher::prefix("/api/ai/jobs/"), HTTP_POST,
        [this](AsyncWebServerRequest* request) { handleJobCancel(request); }, nullptr,
        [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            accumulateBody(request, data, len, index, total);
        });
    server->on(AsyncURIMatcher::prefix("/api/ai/proposals/"), HTTP_POST,
        [this](AsyncWebServerRequest* request) { handleProposalExecute(request); }, nullptr,
        [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            accumulateBody(request, data, len, index, total);
        });
    server->on(AsyncURIMatcher::exact("/api/ai/session"), HTTP_DELETE,
        [this](AsyncWebServerRequest* request) { handleSessionClear(request); });
}
#endif
