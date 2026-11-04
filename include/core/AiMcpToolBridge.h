#ifndef AI_MCP_TOOL_BRIDGE_H
#define AI_MCP_TOOL_BRIDGE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>

class AiMcpToolBridge {
public:
    static constexpr uint8_t kMaxIterations = 8;
    static constexpr uint32_t kDeadlineMs = 120000;
    static constexpr size_t kMaxToolResultBytes = 16384;
    static const std::vector<String>& toolNames();
    static bool isAllowed(const String& name);
    static bool writeToolSchema(const String& name, JsonObject output);
    static bool validateCall(const String& name, JsonVariantConst arguments, String& error);
};

#endif
