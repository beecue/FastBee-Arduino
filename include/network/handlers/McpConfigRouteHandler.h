#ifndef MCP_CONFIG_ROUTE_HANDLER_H
#define MCP_CONFIG_ROUTE_HANDLER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <memory>

#include "core/ManagedConfigApplyService.h"

class WebHandlerContext;

class McpConfigRouteHandler {
public:
    explicit McpConfigRouteHandler(WebHandlerContext* ctx);
    void setupRoutes(AsyncWebServer* server);

private:
    WebHandlerContext* ctx_;
    std::unique_ptr<FastBee::ManagedConfigApplyService> service_;

    bool admit(AsyncWebServerRequest* request, bool heavy = false);
    void accumulateBody(AsyncWebServerRequest* request, uint8_t* data,
                        size_t len, size_t index, size_t total);
    bool parseBody(AsyncWebServerRequest* request, JsonDocument& document);
    void handleCapabilities(AsyncWebServerRequest* request);
    void handleSnapshot(AsyncWebServerRequest* request);
    void handlePreflight(AsyncWebServerRequest* request);
    void handleApply(AsyncWebServerRequest* request);
    void handleOperation(AsyncWebServerRequest* request);
    void handleRecovery(AsyncWebServerRequest* request);
    void handleRollback(AsyncWebServerRequest* request);
    void handleSecret(AsyncWebServerRequest* request);
    void sendDocument(AsyncWebServerRequest* request, JsonDocument& document, int status = 200);

    static constexpr size_t kMaxBodyBytes = 32768;
};

#endif // MCP_CONFIG_ROUTE_HANDLER_H
