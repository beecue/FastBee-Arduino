#include <unity.h>
#include <Arduino.h>
#include <ArduinoJson.h>

#include "core/AiConfig.h"
#include "../src/core/AiConfig.cpp"

namespace {

void test_provider_url_and_model_validation() {
    AiConfig config;
    String error;
    config.baseUrl = "https://api.deepseek.com";
    config.model = "deepseek-chat";
    config.apiKey = "secret-key";
    TEST_ASSERT_TRUE(AiConfigStore::validateProvider(config, error));
    config.baseUrl = "https://api.deepseek.com/v1";
    TEST_ASSERT_TRUE(AiConfigStore::validateProvider(config, error));

    const char* invalid[] = {
        "http://api.deepseek.com", "https://user:pass@api.deepseek.com",
        "https://api.deepseek.com/v1/chat/completions?key=secret",
        "https://api.deepseek.com/v1#fragment", "https://api.deepseek.com/other"
    };
    for (const char* url : invalid) {
        config.baseUrl = url;
        TEST_ASSERT_FALSE(AiConfigStore::validateProvider(config, error));
        TEST_ASSERT_TRUE(error.indexOf("secret-key") < 0);
    }
    config.baseUrl = "https://api.deepseek.com";
    config.model = "";
    TEST_ASSERT_FALSE(AiConfigStore::validateProvider(config, error));
}

void test_mcp_is_optional_and_provider_key_is_never_serialized() {
    AiConfig config;
    String error;
    TEST_ASSERT_TRUE(AiConfigStore::validateMcpBaseUrl("", error));
    TEST_ASSERT_TRUE(AiConfigStore::validateMcpBaseUrl("https://mcp.example.test", error));
    TEST_ASSERT_TRUE(AiConfigStore::validateMcpBaseUrl("http://127.0.0.1:8090", error));
    TEST_ASSERT_TRUE(AiConfigStore::validateMcpBaseUrl("http://localhost:8090", error));
    TEST_ASSERT_TRUE(AiConfigStore::validateMcpBaseUrl("http://localhost:000080", error));
    TEST_ASSERT_TRUE(AiConfigStore::validateMcpBaseUrl("http://localhost:8090//", error));
    TEST_ASSERT_FALSE(AiConfigStore::validateMcpBaseUrl("http://mcp.example.test", error));
    TEST_ASSERT_FALSE(AiConfigStore::validateMcpBaseUrl("http://192.168.5.24:8090", error));
    TEST_ASSERT_FALSE(AiConfigStore::validateMcpBaseUrl("http://127.0.0.1.evil.test:8090", error));
    TEST_ASSERT_FALSE(AiConfigStore::validateMcpBaseUrl("https://u:p@mcp.example.test", error));
    config.apiKey = "never-return-this";
    JsonDocument doc;
    AiConfigStore::writePublicJson(config, doc.to<JsonObject>(), true, false, "user_disabled");
    String json;
    serializeJson(doc, json);
    TEST_ASSERT_TRUE(doc["apiKeyConfigured"].as<bool>());
    TEST_ASSERT_TRUE(doc["enabled"].as<bool>());
    TEST_ASSERT_FALSE(doc["active"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("user_disabled", doc["runtimeReason"].as<const char*>());
    TEST_ASSERT_TRUE(json.indexOf("never-return-this") < 0);
    TEST_ASSERT_TRUE(json.indexOf("apiKey\"") < 0);
}

void test_provider_and_mcp_limits_are_bounded() {
    AiConfig config;
    String error;
    config.baseUrl = "https://api.deepseek.com";
    config.model = "deepseek-chat";
    config.apiKey = String(std::string(257, 'x'));
    TEST_ASSERT_FALSE(AiConfigStore::validateProvider(config, error));
    TEST_ASSERT_FALSE(AiConfigStore::validateMcpBaseUrl(String("https://") + String(std::string(300, 'a')), error));
}

} // namespace

void test_ai_config_group() {
    RUN_TEST(test_provider_url_and_model_validation);
    RUN_TEST(test_mcp_is_optional_and_provider_key_is_never_serialized);
    RUN_TEST(test_provider_and_mcp_limits_are_bounded);
}
