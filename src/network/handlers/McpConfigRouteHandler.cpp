#include "network/handlers/McpConfigRouteHandler.h"

#include <cstring>
#include <new>

#include "network/WebHandlerContext.h"
#include "network/handlers/HandlerUtils.h"
#include "network/SameOrigin.h"
#include "protocols/ProtocolManager.h"
#include "utils/PsramJsonDocument.h"

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

int errorStatus(const String& code) {
    if (code == "CONFIG_REVISION_CONFLICT" || code == "IDEMPOTENCY_CONFLICT" ||
        code == "PREFLIGHT_FAILED" || code == "CONFIRMATION_REQUIRED" ||
        code == "OPERATION_IN_PROGRESS") return 409;
    if (code == "OPERATION_NOT_FOUND") return 404;
    if (code == "QUOTA_EXCEEDED") return 413;
    if (code == "DEVICE_CHANNEL_UNAVAILABLE") return 503;
    if (code == "OUTCOME_UNKNOWN") return 200;
    return 400;
}

} // namespace

McpConfigRouteHandler::McpConfigRouteHandler(WebHandlerContext* ctx) : ctx_(ctx) {
    service_.reset(new (std::nothrow) FastBee::ManagedConfigApplyService());
}

void McpConfigRouteHandler::setupRoutes(AsyncWebServer* server) {
#ifndef UNIT_TEST
    if (ctx_->protocolManager) service_->setProtocolManager(ctx_->protocolManager);
#endif
    server->on(AsyncURIMatcher::exact("/api/mcp/capabilities"), HTTP_GET,
               [this](AsyncWebServerRequest* request) { handleCapabilities(request); });
    server->on(AsyncURIMatcher::exact("/api/mcp/config/snapshot"), HTTP_GET,
               [this](AsyncWebServerRequest* request) { handleSnapshot(request); });
    server->on(AsyncURIMatcher::exact("/api/mcp/config/preflight"), HTTP_POST,
               [this](AsyncWebServerRequest* request) { handlePreflight(request); }, nullptr,
               [this](AsyncWebServerRequest* request, uint8_t* data, size_t len,
                      size_t index, size_t total) {
                   accumulateBody(request, data, len, index, total);
               });
    server->on(AsyncURIMatcher::exact("/api/mcp/config/apply"), HTTP_POST,
               [this](AsyncWebServerRequest* request) { handleApply(request); }, nullptr,
               [this](AsyncWebServerRequest* request, uint8_t* data, size_t len,
                      size_t index, size_t total) {
                   accumulateBody(request, data, len, index, total);
               });
    server->on(AsyncURIMatcher::exact("/api/mcp/config/rollback"), HTTP_POST,
               [this](AsyncWebServerRequest* request) { handleRollback(request); }, nullptr,
               [this](AsyncWebServerRequest* request, uint8_t* data, size_t len,
                      size_t index, size_t total) {
                   accumulateBody(request, data, len, index, total);
               });
    server->on(AsyncURIMatcher::exact("/api/mcp/config/readback"), HTTP_POST,
               [this](AsyncWebServerRequest* request) { handleRecovery(request); }, nullptr,
               [this](AsyncWebServerRequest* request, uint8_t* data, size_t len,
                      size_t index, size_t total) {
                   accumulateBody(request, data, len, index, total);
               });
    server->on(AsyncURIMatcher::prefix("/api/mcp/config/operations/"), HTTP_GET,
               [this](AsyncWebServerRequest* request) { handleOperation(request); });
    server->on(AsyncURIMatcher::prefix("/api/mcp/secrets/"), HTTP_POST,
               [this](AsyncWebServerRequest* request) { handleSecret(request); }, nullptr,
               [this](AsyncWebServerRequest* request, uint8_t* data, size_t len,
                      size_t index, size_t total) {
                   accumulateBody(request, data, len, index, total);
               });
}

bool McpConfigRouteHandler::admit(AsyncWebServerRequest* request, bool heavy) {
    if (!ctx_->requireSameOrigin(request)) return false;
    if (!ctx_->requireAuth(request) || !ctx_->requireDeveloperMode(request)) return false;
    if (!service_) {
        ctx_->sendError(request, 503, "MCP configuration service unavailable");
        return false;
    }
    if (heavy && (HandlerUtils::rejectHeavyRequestOnPressure(
                      request, "MCP configuration", MemoryGuardLevel::SEVERE, 8) ||
                  HandlerUtils::checkLowMemory(request, 16384))) return false;
    return true;
}

void McpConfigRouteHandler::accumulateBody(AsyncWebServerRequest* request, uint8_t* data,
                                           size_t len, size_t index, size_t total) {
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
    if ((*body)->content.concat(reinterpret_cast<const char*>(data), len) == false) {
        (*body)->oversized = true;
    }
}

bool McpConfigRouteHandler::parseBody(AsyncWebServerRequest* request, JsonDocument& document) {
    BodyOwner* body = bodyOf(request);
    if (!body || !*body || (*body)->oversized || (*body)->content.isEmpty()) {
        const bool oversized = body && *body && (*body)->oversized;
        releaseBody(request);
        ctx_->sendError(request, oversized ? 413 : 400,
                        oversized ? "JSON body too large" : "Missing JSON body");
        return false;
    }
    const DeserializationError parseError = deserializeJson(document, (*body)->content.c_str(),
                                                            (*body)->content.length());
    releaseBody(request);
    if (parseError || document.overflowed() || !document.is<JsonObject>()) {
        ctx_->sendBadRequest(request, "Invalid or incomplete JSON body");
        return false;
    }
    return true;
}

void McpConfigRouteHandler::sendDocument(AsyncWebServerRequest* request,
                                         JsonDocument& document, int status) {
    if (document.overflowed()) {
        HandlerUtils::sendJsonError(request, 503, "JSON document allocation failed", 5);
        return;
    }
    AsyncResponseStream* response = request->beginResponseStream("application/json");
    response->setCode(status);
    response->addHeader("Connection", "close");
    serializeJson(document, *response);
    request->send(response);
}

void McpConfigRouteHandler::handleCapabilities(AsyncWebServerRequest* request) {
    if (!admit(request)) return;
    if (!service_) {
        ctx_->sendError(request, 503, "MCP configuration service unavailable");
        return;
    }
    auto output = FastBee::makeJsonDocument();
    String error;
    service_->capabilities(output, error);
    sendDocument(request, output, output["success"] | false ? 200 : 503);
}

void McpConfigRouteHandler::handleSnapshot(AsyncWebServerRequest* request) {
    if (!admit(request, true)) return;
    std::vector<String> sections;
    const String selected = ctx_->getParamValue(request, "sections", "");
    int start = 0;
    while (start < selected.length()) {
        int end = selected.indexOf(',', start);
        if (end < 0) end = selected.length();
        String section = selected.substring(start, end);
        section.trim();
        if (section.isEmpty() || section.length() > 24 || sections.size() >= 10) {
            ctx_->sendBadRequest(request, "Invalid sections selection");
            return;
        }
        sections.push_back(section);
        start = end + 1;
    }
    auto output = FastBee::makeJsonDocument();
    String error;
    service_->snapshot(sections, output, error);
    output["snapshotAtMs"] = millis();
    sendDocument(request, output, output["success"] | false ? 200 : errorStatus(error));
}

void McpConfigRouteHandler::handlePreflight(AsyncWebServerRequest* request) {
    if (!admit(request, true)) return;
    auto input = FastBee::makeJsonDocument();
    if (!parseBody(request, input)) return;
    auto output = FastBee::makeJsonDocument();
    String error;
    service_->preflight(input, output, error);
    sendDocument(request, output, output["success"] | false ? 200 : errorStatus(output["code"].as<const char*>()));
}

void McpConfigRouteHandler::handleApply(AsyncWebServerRequest* request) {
    if (!admit(request, true)) return;
    auto input = FastBee::makeJsonDocument();
    if (!parseBody(request, input)) return;
#ifndef UNIT_TEST
    if (ctx_->protocolManager) service_->setProtocolManager(ctx_->protocolManager);
#endif
    auto output = FastBee::makeJsonDocument();
    String error;
    service_->apply(input, output, error);
    sendDocument(request, output, error.isEmpty() ? 200 : errorStatus(error));
}

void McpConfigRouteHandler::handleOperation(AsyncWebServerRequest* request) {
    if (!admit(request)) return;
    const String prefix = "/api/mcp/config/operations/";
    const String path = request->url();
    if (!path.startsWith(prefix)) {
        ctx_->sendBadRequest(request, "Invalid operation path");
        return;
    }
    const String id = path.substring(prefix.length());
    if (id.isEmpty() || id.length() > 40 || id.indexOf('/') >= 0) {
        ctx_->sendBadRequest(request, "Invalid operation ID");
        return;
    }
    auto output = FastBee::makeJsonDocument();
    String error;
    const bool found = service_->operation(id, output, error);
    sendDocument(request, output, found ? 200 : 404);
}

void McpConfigRouteHandler::handleRecovery(AsyncWebServerRequest* request) {
    if (!admit(request, true)) return;
    auto input = FastBee::makeJsonDocument();
    if (!parseBody(request, input)) return;
    const String operationId = input["operationId"].as<const char*>();
    if (operationId.isEmpty()) {
        ctx_->sendBadRequest(request, "operationId is required");
        return;
    }
    auto output = FastBee::makeJsonDocument();
    String error;
    service_->recoverOperation(operationId, output, error);
    sendDocument(request, output, error == "OPERATION_NOT_FOUND" ? 404 : 200);
}

void McpConfigRouteHandler::handleRollback(AsyncWebServerRequest* request) {
    if (!admit(request, true)) return;
    auto input = FastBee::makeJsonDocument();
    if (!parseBody(request, input)) return;
#ifndef UNIT_TEST
    if (ctx_->protocolManager) service_->setProtocolManager(ctx_->protocolManager);
#endif
    auto output = FastBee::makeJsonDocument();
    String error;
    service_->rollback(input, output, error);
    sendDocument(request, output, error.isEmpty() ? 200 : errorStatus(error));
}

void McpConfigRouteHandler::handleSecret(AsyncWebServerRequest* request) {
    if (!admit(request, true)) return;
    const String prefix = "/api/mcp/secrets/";
    const String path = request->url();
    const String secretRef = path.startsWith(prefix) ? path.substring(prefix.length()) : String();
    if (secretRef.isEmpty() || secretRef.length() > 96 || secretRef.indexOf('/') >= 0) {
        releaseBody(request);
        ctx_->sendBadRequest(request, "Invalid secret reference");
        return;
    }
    auto input = FastBee::makeJsonDocument();
    if (!parseBody(request, input)) return;
    if (input.size() != 2 || !input["value"].is<JsonString>() ||
        input["value"].as<JsonString>().size() > 1024 ||
        input["value"].as<JsonString>().size() != std::strlen(input["value"].as<const char*>()) ||
        !input["idempotencyKey"].is<JsonString>() || input["idempotencyKey"].as<JsonString>().size() == 0 ||
        input["idempotencyKey"].as<JsonString>().size() > 80) {
        ctx_->sendBadRequest(request, "Invalid one-time secret value or idempotency key");
        return;
    }
    String value = input["value"].as<const char*>();
    const String idempotencyKey = input["idempotencyKey"].as<const char*>();
    String error;
#ifndef UNIT_TEST
    if (ctx_->protocolManager) service_->setProtocolManager(ctx_->protocolManager);
#endif
    const bool stored = service_->storeSecret(secretRef, value, idempotencyKey, error);
    value = "";
    input.clear();
    auto output = FastBee::makeJsonDocument();
    output["success"] = stored;
    if (!stored) {
        output["code"] = error;
        output["error"] = error;
    } else {
        output["data"]["consumed"] = true;
    }
    sendDocument(request, output, stored ? 200 : errorStatus(error));
}
