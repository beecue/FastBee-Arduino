#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_AI || defined(UNIT_TEST)
#include "core/AiMcpToolBridge.h"

#include <cstring>

namespace {
bool text(JsonVariantConst args, const char* key, bool required, size_t max = 128) {
    if (args[key].isNull()) return !required;
    if (!args[key].is<JsonString>()) return false;
    const size_t length = args[key].as<JsonString>().size();
    return length > 0 && length <= max && std::strlen(args[key].as<const char*>()) == length;
}
bool integer(JsonVariantConst args, const char* key, bool required, int min, int max) {
    if (args[key].isNull()) return !required;
    if (!args[key].is<int>()) return false;
    const int value = args[key].as<int>();
    return value >= min && value <= max;
}
bool keysAre(JsonVariantConst args, const char* const* allowed, size_t count) {
    if (!args.is<JsonObjectConst>()) return false;
    for (JsonPairConst field : args.as<JsonObjectConst>()) {
        bool found = false;
        for (size_t i = 0; i < count; ++i) {
            if (std::strcmp(field.key().c_str(), allowed[i]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}
bool noArguments(JsonVariantConst args) {
    return args.is<JsonObjectConst>() && args.as<JsonObjectConst>().size() == 0;
}
bool idOnly(JsonVariantConst args, const char* id) {
    const char* keys[] = {id};
    return keysAre(args, keys, 1) && text(args, id, true);
}
}

const std::vector<String>& AiMcpToolBridge::toolNames() {
    static const std::vector<String> names = {
        "get_device_capabilities", "list_peripherals", "read_peripheral",
        "list_execution_rules", "list_rule_scripts", "get_protocol_configuration",
        "get_configuration_history", "create_config_draft", "get_configuration_draft",
        "upsert_peripheral", "remove_peripheral", "set_peripheral_enabled",
        "validate_configuration_draft", "get_configuration_operation",
        "request_document_upload", "list_documents", "get_document_status",
        "search_documents", "extract_configuration_facts"
    };
    return names;
}

bool AiMcpToolBridge::isAllowed(const String& name) {
    for (const auto& candidate : toolNames()) if (candidate == name) return true;
    return false;
}

bool AiMcpToolBridge::validateCall(const String& name, JsonVariantConst arguments, String& error) {
    error = "AI_INVALID_TOOL_ARGUMENTS";
    if (!isAllowed(name)) {
        error = "AI_UNSUPPORTED_TOOL";
        return false;
    }
    if (name == "get_device_capabilities" || name == "list_peripherals" ||
        name == "list_execution_rules" || name == "list_rule_scripts" ||
        name == "get_protocol_configuration" || name == "get_configuration_history") {
        if (!noArguments(arguments)) return false;
    } else if (name == "read_peripheral") {
        if (!idOnly(arguments, "peripheralId")) return false;
    } else if (name == "create_config_draft") {
        if (!idOnly(arguments, "deviceId")) return false;
    } else if (name == "get_configuration_draft" || name == "validate_configuration_draft") {
        const char* keys[] = {"draftId", "expectedVersion"};
        if (!keysAre(arguments, keys, 2) || !text(arguments, "draftId", true) ||
            !integer(arguments, "expectedVersion", false, 1, 1000000)) return false;
    } else if (name == "get_configuration_operation") {
        if (!idOnly(arguments, "operationId")) return false;
    } else if (name == "upsert_peripheral") {
        const char* keys[] = {"draftId", "expectedVersion", "peripheral"};
        if (!keysAre(arguments, keys, 3) || !text(arguments, "draftId", true) ||
            !integer(arguments, "expectedVersion", true, 1, 1000000) ||
            !arguments["peripheral"].is<JsonObjectConst>()) return false;
    } else if (name == "remove_peripheral" || name == "set_peripheral_enabled") {
        const char* keys[] = {"draftId", "expectedVersion", "peripheralId", "enabled"};
        if (!keysAre(arguments, keys, name == "set_peripheral_enabled" ? 4 : 3) ||
            !text(arguments, "draftId", true) || !text(arguments, "peripheralId", true) ||
            !integer(arguments, "expectedVersion", true, 1, 1000000) ||
            (name == "set_peripheral_enabled" && !arguments["enabled"].is<bool>())) return false;
    } else if (name == "search_documents") {
        const char* keys[] = {"query", "documentId", "page", "pageSize"};
        if (!keysAre(arguments, keys, 4) || !text(arguments, "query", true, 256) ||
            !text(arguments, "documentId", false) || !integer(arguments, "page", false, 1, 10000) ||
            !integer(arguments, "pageSize", false, 1, 20)) return false;
    } else if (name == "extract_configuration_facts") {
        const char* keys[] = {"documentId", "kind", "page", "pageSize"};
        if (!keysAre(arguments, keys, 4) || !text(arguments, "documentId", true) ||
            !text(arguments, "kind", false, 40) || !integer(arguments, "page", false, 1, 10000) ||
            !integer(arguments, "pageSize", false, 1, 50)) return false;
    } else if (name == "list_documents") {
        const char* keys[] = {"page", "pageSize"};
        if (!keysAre(arguments, keys, 2) || !integer(arguments, "page", false, 1, 10000) ||
            !integer(arguments, "pageSize", false, 1, 50)) return false;
    } else if (name == "get_document_status") {
        if (!idOnly(arguments, "documentId")) return false;
    } else if (name == "request_document_upload") {
        const char* keys[] = {"fileName", "size", "contentType", "sha256"};
        if (!keysAre(arguments, keys, 4) || !text(arguments, "fileName", true, 160) ||
            !integer(arguments, "size", true, 1, 20971520) ||
            !text(arguments, "contentType", true, 80) || !text(arguments, "sha256", true, 64)) return false;
        const String digest = arguments["sha256"].as<const char*>();
        for (size_t i = 0; i < digest.length(); ++i) {
            const char ch = digest[i];
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) return false;
        }
    }
    error = "";
    return true;
}
#endif
