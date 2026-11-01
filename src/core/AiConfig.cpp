#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_AI || defined(UNIT_TEST)
#include "core/AiConfig.h"

#include <cstring>

#ifndef UNIT_TEST
#include "systems/ConfigStorage.h"
#endif

namespace {
constexpr const char* kAiConfigPath = "/config/ai.json";

bool validHttpsUrl(const String& value, bool provider, String& error) {
    if (value.isEmpty() || value.length() > 256 || !value.startsWith("https://") ||
        value.indexOf('@') >= 0 || value.indexOf('?') >= 0 || value.indexOf('#') >= 0 ||
        value.indexOf(' ') >= 0 || value.indexOf('\r') >= 0 || value.indexOf('\n') >= 0) {
        error = "AI_INVALID_URL";
        return false;
    }
    const String rest = value.substring(8);
    const int slash = rest.indexOf('/');
    const String host = slash < 0 ? rest : rest.substring(0, slash);
    const String path = slash < 0 ? String() : rest.substring(slash);
    if (host.isEmpty() || host.startsWith(".") || host.endsWith(".") ||
        host.indexOf("..") >= 0 || host.indexOf('\\') >= 0) {
        error = "AI_INVALID_URL";
        return false;
    }
    if (provider && !path.isEmpty() && path != "/" && path != "/v1") {
        error = "AI_INVALID_PROVIDER_PATH";
        return false;
    }
    return true;
}
}

bool AiConfigStore::validateProvider(const AiConfig& config, String& error) {
    error = "";
    if (!validHttpsUrl(config.baseUrl, true, error)) return false;
    if (config.model.isEmpty() || config.model.length() > 64 || config.apiKey.length() > 256 ||
        config.model.indexOf('\r') >= 0 || config.model.indexOf('\n') >= 0) {
        error = "AI_INVALID_PROVIDER_CONFIG";
        return false;
    }
    for (size_t i = 0; i < config.apiKey.length(); ++i) {
        const uint8_t ch = static_cast<uint8_t>(config.apiKey[i]);
        if (ch < 0x20 || ch == 0x7f) {
            error = "AI_INVALID_PROVIDER_CONFIG";
            return false;
        }
    }
    return true;
}

bool AiConfigStore::validateMcpBaseUrl(const String& baseUrl, String& error) {
    error = "";
    if (baseUrl.isEmpty()) return true;
    return validHttpsUrl(baseUrl, false, error);
}

void AiConfigStore::writePublicJson(const AiConfig& config, JsonObject output,
                                    bool enabled, bool active, const String& runtimeReason) {
    output["enabled"] = enabled;
    output["baseUrl"] = config.baseUrl;
    output["model"] = config.model;
    output["apiKeyConfigured"] = !config.apiKey.isEmpty();
    output["mcpBaseUrl"] = config.mcpBaseUrl;
    output["active"] = active;
    output["runtimeReason"] = runtimeReason;
}

bool AiConfigStore::load(AiConfig& out, String& error) {
#ifdef UNIT_TEST
    (void)out;
    error = "AI_STORAGE_UNAVAILABLE";
    return false;
#else
    AiConfig loaded;
    JsonDocument doc;
    if (ConfigStorage::getInstance().loadJSONConfig(kAiConfigPath, doc)) {
        loaded.baseUrl = doc["baseUrl"] | loaded.baseUrl;
        loaded.apiKey = doc["apiKey"] | "";
        loaded.model = doc["model"] | loaded.model;
        loaded.mcpBaseUrl = doc["mcpBaseUrl"] | "";
    }
    String validation;
    if (!validateProvider(loaded, validation) || !validateMcpBaseUrl(loaded.mcpBaseUrl, validation)) {
        error = validation;
        return false;
    }
    out = loaded;
    error = "";
    return true;
#endif
}

bool AiConfigStore::saveProvider(const AiConfig& next, String& error) {
    if (!validateProvider(next, error)) return false;
#ifdef UNIT_TEST
    error = "AI_STORAGE_UNAVAILABLE";
    return false;
#else
    AiConfig current;
    JsonDocument existing;
    ConfigStorage::getInstance().loadJSONConfig(kAiConfigPath, existing);
    JsonDocument doc;
    doc["version"] = 1;
    doc["baseUrl"] = next.baseUrl;
    doc["apiKey"] = next.apiKey;
    doc["model"] = next.model;
    doc["mcpBaseUrl"] = next.mcpBaseUrl.isEmpty() ? String(existing["mcpBaseUrl"] | "") : next.mcpBaseUrl;
    if (!ConfigStorage::getInstance().saveJSONConfig(kAiConfigPath, doc)) {
        error = "AI_CONFIG_SAVE_FAILED";
        return false;
    }
    error = "";
    return true;
#endif
}

bool AiConfigStore::saveMcpBaseUrl(const String& baseUrl, String& error) {
    if (!validateMcpBaseUrl(baseUrl, error)) return false;
#ifdef UNIT_TEST
    error = "AI_STORAGE_UNAVAILABLE";
    return false;
#else
    JsonDocument existing;
    ConfigStorage::getInstance().loadJSONConfig(kAiConfigPath, existing);
    AiConfig current;
    current.baseUrl = existing["baseUrl"] | current.baseUrl;
    current.apiKey = existing["apiKey"] | "";
    current.model = existing["model"] | current.model;
    current.mcpBaseUrl = baseUrl;
    if (!validateProvider(current, error)) return false;
    JsonDocument doc;
    doc["version"] = 1;
    doc["baseUrl"] = current.baseUrl;
    doc["apiKey"] = current.apiKey;
    doc["model"] = current.model;
    doc["mcpBaseUrl"] = current.mcpBaseUrl;
    if (!ConfigStorage::getInstance().saveJSONConfig(kAiConfigPath, doc)) {
        error = "AI_CONFIG_SAVE_FAILED";
        return false;
    }
    error = "";
    return true;
#endif
}
#endif
