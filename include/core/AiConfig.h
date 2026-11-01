#ifndef AI_CONFIG_H
#define AI_CONFIG_H

#include <Arduino.h>
#include <ArduinoJson.h>

struct AiConfig {
    String baseUrl = "https://api.deepseek.com";
    String apiKey;
    String model = "deepseek-chat";
    String mcpBaseUrl;
};

class AiConfigStore {
public:
    bool load(AiConfig& out, String& error);
    bool saveProvider(const AiConfig& next, String& error);
    bool saveMcpBaseUrl(const String& baseUrl, String& error);
    static bool validateProvider(const AiConfig& config, String& error);
    static bool validateMcpBaseUrl(const String& baseUrl, String& error);
    static void writePublicJson(const AiConfig& config, JsonObject output,
                                bool enabled, bool active, const String& runtimeReason);
};

#endif
