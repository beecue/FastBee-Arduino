#ifndef AI_ROUTE_HANDLER_H
#define AI_ROUTE_HANDLER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

class WebHandlerContext;

class AiRouteHandler {
public:
    explicit AiRouteHandler(WebHandlerContext* ctx) : ctx_(ctx) {}
    void setupRoutes(AsyncWebServer* server);

private:
    WebHandlerContext* ctx_;
    bool admit(AsyncWebServerRequest* request, bool developer = false);
    void accumulateBody(AsyncWebServerRequest* request, uint8_t* data, size_t len,
                        size_t index, size_t total);
    bool parseBody(AsyncWebServerRequest* request, JsonDocument& output);
    void handleConfigGet(AsyncWebServerRequest* request);
    void handleConfigPut(AsyncWebServerRequest* request);
    void handleMcpConfigPut(AsyncWebServerRequest* request);
    void handleJobSubmit(AsyncWebServerRequest* request);
    void handleJobGet(AsyncWebServerRequest* request);
    void handleJobCancel(AsyncWebServerRequest* request);
    void handleProposalExecute(AsyncWebServerRequest* request);
    void handleSessionClear(AsyncWebServerRequest* request);
    static constexpr size_t kMaxBodyBytes = 16384;
};

#endif
