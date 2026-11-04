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
bool identifier(JsonVariantConst args, const char* key, bool required, size_t max) {
    if (!text(args, key, required, max)) return false;
    if (args[key].isNull()) return true;
    const char* value = args[key].as<const char*>();
    for (size_t i = 0; value[i] != '\0'; ++i) {
        const char ch = value[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
              ch == '_' || ch == '-')) return false;
    }
    return true;
}
bool validPeripheral(JsonVariantConst peripheral) {
    const char* keys[] = {"id", "name", "type", "enabled", "pinCount", "pins", "activeHigh", "sampleInterval", "params"};
    if (!peripheral.is<JsonObjectConst>() || !keysAre(peripheral, keys, 9) ||
        measureJson(peripheral) > 2048 || !text(peripheral, "name", true, 64) ||
        !identifier(peripheral, "id", false, 40) || !integer(peripheral, "type", true, 1, 60) ||
        !integer(peripheral, "pinCount", false, 1, 8) ||
        !integer(peripheral, "sampleInterval", false, 20, 3600000) ||
        !peripheral["pins"].is<JsonArrayConst>()) return false;
    JsonArrayConst pins = peripheral["pins"];
    if (pins.size() < 1 || pins.size() > 8) return false;
    for (JsonVariantConst pin : pins) {
        if (!pin.is<int>() || pin.as<int>() < 0 || pin.as<int>() > 48) return false;
    }
    if (!peripheral["enabled"].isNull() && !peripheral["enabled"].is<bool>()) return false;
    if (!peripheral["activeHigh"].isNull() && !peripheral["activeHigh"].is<bool>()) return false;
    if (!peripheral["params"].isNull() && !peripheral["params"].is<JsonObjectConst>()) return false;
    if (!peripheral["pinCount"].isNull() && peripheral["pinCount"].as<int>() != static_cast<int>(pins.size())) return false;
    return true;
}
bool containsSecret(JsonVariantConst value) {
    if (!value.is<JsonObjectConst>()) return false;
    for (JsonPairConst field : value.as<JsonObjectConst>()) {
        const String key = field.key().c_str();
        if (key == "password" || key == "mqttSecret" || key == "authCode" || key == "apiKey" ||
            key == "authorization" || key == "authHeader" || containsSecret(field.value())) return true;
    }
    return false;
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
        "search_documents", "extract_configuration_facts",
        "get_device_config_section", "get_device_status", "update_device_profile", "configure_runtime_features",
        "upsert_execution_rule", "remove_execution_rule", "dry_run_execution_rule",
        "upsert_rule_script", "remove_rule_script", "configure_modbus_rtu", "upsert_modbus_device",
        "remove_modbus_device", "upsert_modbus_poll_task", "remove_modbus_poll_task", "configure_mqtt",
        "upsert_mqtt_topic", "remove_mqtt_topic", "preview_configuration_draft", "refresh_device_capabilities",
        "list_workflow_templates", "start_configuration_workflow", "get_workflow_run", "cancel_workflow_run",
        "parse_document", "retry_document_parsing", "get_operation_audit", "submit_feedback"
    };
    return names;
}

bool AiMcpToolBridge::isAllowed(const String& name) {
    for (const auto& candidate : toolNames()) if (candidate == name) return true;
    return false;
}

bool writeJsonSchema(JsonObject output, const char* json) {
    JsonDocument document;
    if (deserializeJson(document, json)) return false;
    return output.set(document.as<JsonObjectConst>());
}

bool AiMcpToolBridge::writeToolSchema(const String& name, JsonObject output) {
    if (!isAllowed(name)) return false;
    output.clear();
    output["type"] = "object";
    output["additionalProperties"] = false;
    JsonObject properties = output["properties"].to<JsonObject>();
    auto textProperty = [&](const char* key, int maxLength) {
        properties[key]["type"] = "string";
        if (maxLength > 0) properties[key]["maxLength"] = maxLength;
    };
    auto integerProperty = [&](const char* key, int minimum, int maximum) {
        properties[key]["type"] = "integer";
        properties[key]["minimum"] = minimum;
        properties[key]["maximum"] = maximum;
    };
    auto pageProperties = [&]() {
        textProperty("cursor", 128);
        integerProperty("limit", 1, 100);
    };
    auto draftProperties = [&]() {
        textProperty("draftId", 128);
        integerProperty("expectedVersion", 0, 1000000);
    };
    auto required = [&](std::initializer_list<const char*> fields) {
        JsonArray rows = output["required"].to<JsonArray>();
        for (const char* field : fields) rows.add(field);
    };

    if (name == "list_peripherals" || name == "list_execution_rules" ||
        name == "list_rule_scripts" || name == "get_configuration_history" ||
        name == "list_documents") {
        pageProperties();
    } else if (name == "read_peripheral") {
        textProperty("peripheralId", 128);
        required({"peripheralId"});
    } else if (name == "get_configuration_draft" || name == "validate_configuration_draft") {
        textProperty("draftId", 128);
        required({"draftId"});
    } else if (name == "get_configuration_operation") {
        textProperty("operationId", 128);
        required({"operationId"});
    } else if (name == "upsert_peripheral") {
        draftProperties();
        properties["draftId"]["description"] = "配置草案 UUID";
        properties["expectedVersion"]["description"] = "期望草案版本（乐观锁，取自最近草案响应）";
        JsonObject peripheral = properties["peripheral"].to<JsonObject>();
        peripheral["type"] = "object";
        peripheral["description"] = "外设定义：id/type/name/enabled/pins/params";
        peripheral["additionalProperties"] = false;
        JsonObject values = peripheral["properties"].to<JsonObject>();
        values["id"]["type"] = "string";
        values["id"]["maxLength"] = 40;
        values["id"]["pattern"] = "^[A-Za-z0-9_-]{1,40}$";
        values["name"]["type"] = "string";
        values["name"]["maxLength"] = 64;
        values["type"]["type"] = "integer";
        values["type"]["minimum"] = 1;
        values["type"]["maximum"] = 60;
        values["enabled"]["type"] = "boolean";
        values["pinCount"]["type"] = "integer";
        values["pinCount"]["minimum"] = 1;
        values["pinCount"]["maximum"] = 8;
        values["pins"]["type"] = "array";
        values["pins"]["minItems"] = 1;
        values["pins"]["maxItems"] = 8;
        values["pins"]["items"]["type"] = "integer";
        values["pins"]["items"]["minimum"] = 0;
        values["pins"]["items"]["maximum"] = 48;
        values["activeHigh"]["type"] = "boolean";
        values["sampleInterval"]["type"] = "integer";
        values["sampleInterval"]["minimum"] = 20;
        values["sampleInterval"]["maximum"] = 3600000;
        values["params"]["type"] = "object";
        JsonArray peripheralRequired = peripheral["required"].to<JsonArray>();
        peripheralRequired.add("name");
        peripheralRequired.add("type");
        peripheralRequired.add("pins");
        required({"draftId", "expectedVersion", "peripheral"});
    } else if (name == "remove_peripheral" || name == "set_peripheral_enabled") {
        draftProperties();
        textProperty("peripheralId", 128);
        if (name == "set_peripheral_enabled") {
            properties["enabled"]["type"] = "boolean";
            required({"draftId", "expectedVersion", "peripheralId", "enabled"});
        } else {
            required({"draftId", "expectedVersion", "peripheralId"});
        }
    } else if (name == "search_documents") {
        textProperty("query", 100);
        properties["query"]["minLength"] = 1;
        textProperty("documentId", 128);
        pageProperties();
        properties["limit"]["maximum"] = 50;
        required({"query"});
    } else if (name == "extract_configuration_facts") {
        textProperty("documentId", 128);
        textProperty("kind", 40);
        required({"documentId"});
    } else if (name == "get_document_status") {
        textProperty("documentId", 128);
        required({"documentId"});
    } else if (name == "request_document_upload") {
        textProperty("fileName", 160);
        integerProperty("sizeBytes", 1, 20971520);
        textProperty("sha256", 64);
        properties["sha256"]["pattern"] = "^[0-9a-f]{64}$";
        required({"fileName", "sizeBytes", "sha256"});
    } else if (name == "get_device_config_section") {
        return writeJsonSchema(output, R"json({"type":"object","additionalProperties":false,"properties":{"section":{"type":"string","enum":["device","features","peripherals","periphExec","ruleScripts","protocol","screenPanel","network","ai"]}},"required":["section"]})json");
    } else if (name == "get_device_status" || name == "refresh_device_capabilities" || name == "list_workflow_templates") {
        return writeJsonSchema(output, R"json({"type":"object","additionalProperties":false,"properties":{}})json");
    } else if (name == "update_device_profile") {
        return writeJsonSchema(output, R"json({"type":"object","additionalProperties":false,"properties":{"draftId":{"type":"string"},"expectedVersion":{"type":"integer","minimum":0},"profile":{"type":"object","additionalProperties":false,"properties":{"deviceName":{"type":"string","maxLength":128},"location":{"type":"string","maxLength":128},"description":{"type":"string","maxLength":256},"enableNTP":{"type":"boolean"},"ntpServer1":{"type":"string","maxLength":128},"ntpServer2":{"type":"string","maxLength":128},"timezone":{"type":"string","maxLength":32},"syncInterval":{"type":"integer","minimum":60},"logLevel":{"type":"integer","minimum":0,"maximum":5},"cacheDuration":{"type":"integer","minimum":0}}}},"required":["draftId","expectedVersion","profile"]})json");
    } else if (name == "configure_runtime_features") {
        return writeJsonSchema(output, R"json({"type":"object","additionalProperties":false,"properties":{"draftId":{"type":"string"},"expectedVersion":{"type":"integer","minimum":0},"features":{"type":"object","additionalProperties":{"type":"boolean"}}},"required":["draftId","expectedVersion","features"]})json");
    } else if (name == "upsert_execution_rule") {
        return writeJsonSchema(output, R"json({"type":"object","additionalProperties":false,"properties":{"draftId":{"type":"string"},"expectedVersion":{"type":"integer","minimum":0},"rule":{"type":"object","additionalProperties":false,"properties":{"id":{"type":"string","maxLength":40},"name":{"type":"string","maxLength":48},"enabled":{"type":"boolean","const":false},"execMode":{"type":"integer","minimum":0,"maximum":1},"protocolType":{"type":"integer","enum":[0,1,3,4,5]},"scriptContent":{"type":"string","maxLength":2048},"triggers":{"type":"array","minItems":1,"maxItems":3},"actions":{"type":"array","minItems":1,"maxItems":4}},"required":["name","triggers","actions"]}},"required":["draftId","expectedVersion","rule"]})json");
    } else if (name == "remove_execution_rule") {
        return writeJsonSchema(output, R"json({"type":"object","additionalProperties":false,"properties":{"draftId":{"type":"string"},"expectedVersion":{"type":"integer","minimum":0},"ruleId":{"type":"string","maxLength":40}},"required":["draftId","expectedVersion","ruleId"]})json");
    } else if (name == "dry_run_execution_rule") {
        return writeJsonSchema(output, R"json({"type":"object","additionalProperties":false,"properties":{"draftId":{"type":"string"},"ruleId":{"type":"string","maxLength":40},"inputs":{"type":"object"}},"required":["draftId","ruleId"]})json");
    } else if (name == "upsert_rule_script") {
        return writeJsonSchema(output, R"json({"type":"object","additionalProperties":false,"properties":{"draftId":{"type":"string"},"expectedVersion":{"type":"integer","minimum":0},"script":{"type":"object","additionalProperties":false,"properties":{"id":{"type":"string","maxLength":40},"name":{"type":"string","maxLength":128},"enabled":{"type":"boolean","const":false},"triggerType":{"type":"integer","enum":[0,1]},"protocolType":{"type":"integer","enum":[0,1]},"scriptContent":{"type":"string","maxLength":2048},"sourceTopic":{"type":"string","maxLength":256},"targetTopic":{"type":"string","maxLength":256}},"required":["name","scriptContent"]},"variables":{"type":"array","items":{"type":"string","pattern":"^[A-Za-z0-9_.]{1,64}$"}}},"required":["draftId","expectedVersion","script"]})json");
    } else if (name == "remove_rule_script") {
        return writeJsonSchema(output, R"json({"type":"object","additionalProperties":false,"properties":{"draftId":{"type":"string"},"expectedVersion":{"type":"integer","minimum":0},"scriptId":{"type":"string","maxLength":40}},"required":["draftId","expectedVersion","scriptId"]})json");
    } else if (name == "configure_modbus_rtu") {
        return writeJsonSchema(output, R"json({"type":"object","additionalProperties":false,"properties":{"draftId":{"type":"string"},"expectedVersion":{"type":"integer","minimum":0},"bus":{"type":"object","additionalProperties":false,"properties":{"enabled":{"type":"boolean"},"mode":{"type":"string","enum":["master","slave"]},"peripheralId":{"type":"string"},"dePin":{"type":"integer","minimum":0,"maximum":255},"transferType":{"type":"integer","enum":[0,1]},"baudRate":{"type":"integer","minimum":300,"maximum":1000000}}}},"required":["draftId","expectedVersion","bus"]})json");
    } else if (name == "upsert_modbus_device") {
        return writeJsonSchema(output, R"json({"type":"object","additionalProperties":false,"properties":{"draftId":{"type":"string"},"expectedVersion":{"type":"integer","minimum":0},"device":{"type":"object","additionalProperties":false,"properties":{"name":{"type":"string","maxLength":31},"sensorId":{"type":"string","maxLength":31},"deviceType":{"type":"string","enum":["relay","pwm","pid","motor"]},"slaveAddress":{"type":"integer","minimum":1,"maximum":247},"channelCount":{"type":"integer","minimum":1,"maximum":255},"coilBase":{"type":"integer","minimum":0,"maximum":65535},"enabled":{"type":"boolean"}},"required":["name","deviceType","slaveAddress"]}},"required":["draftId","expectedVersion","device"]})json");
    } else if (name == "remove_modbus_device") {
        return writeJsonSchema(output, R"json({"type":"object","additionalProperties":false,"properties":{"draftId":{"type":"string"},"expectedVersion":{"type":"integer","minimum":0},"slaveAddress":{"type":"integer","minimum":1,"maximum":247}},"required":["draftId","expectedVersion","slaveAddress"]})json");
    } else if (name == "upsert_modbus_poll_task") {
        return writeJsonSchema(output, R"json({"type":"object","additionalProperties":false,"properties":{"draftId":{"type":"string"},"expectedVersion":{"type":"integer","minimum":0},"taskIndex":{"type":"integer","minimum":0},"task":{"type":"object","additionalProperties":false,"properties":{"slaveAddress":{"type":"integer","minimum":1,"maximum":247},"functionCode":{"type":"integer","enum":[1,2,3,4]},"startAddress":{"type":"integer","minimum":0,"maximum":65535},"startAddressText":{"type":"string"},"addressBase":{"type":"string","enum":["zero","one"]},"quantity":{"type":"integer","minimum":1,"maximum":125},"pollInterval":{"type":"integer","minimum":2,"maximum":3600},"enabled":{"type":"boolean"},"name":{"type":"string","maxLength":15},"deadband":{"type":"number","minimum":0,"maximum":100},"sensorId":{"type":"string","maxLength":15},"mappings":{"type":"array","maxItems":8,"items":{"type":"object","additionalProperties":false,"properties":{"regOffset":{"type":"integer","minimum":0,"maximum":124},"dataType":{"type":["integer","string"]},"wordOrder":{"type":"integer","enum":[0,1]},"byteOrder":{"type":"string","const":"bigEndian"},"scaleFactor":{"type":"number"},"decimalPlaces":{"type":"integer","minimum":0,"maximum":6},"sensorId":{"type":"string","maxLength":15},"unit":{"type":"string","maxLength":7}}}}},"required":["slaveAddress","functionCode","quantity","pollInterval"]},"source":{"type":"object","additionalProperties":false,"properties":{"documentId":{"type":"string"},"choices":{"type":"object"}},"required":["documentId"]}},"required":["draftId","expectedVersion","taskIndex","task"]})json");
    } else if (name == "remove_modbus_poll_task") {
        return writeJsonSchema(output, R"json({"type":"object","additionalProperties":false,"properties":{"draftId":{"type":"string"},"expectedVersion":{"type":"integer","minimum":0},"taskIndex":{"type":"integer","minimum":0}},"required":["draftId","expectedVersion","taskIndex"]})json");
    } else if (name == "configure_mqtt") {
        return writeJsonSchema(output, R"json({"type":"object","additionalProperties":false,"properties":{"draftId":{"type":"string"},"expectedVersion":{"type":"integer","minimum":0},"connection":{"type":"object","additionalProperties":false,"properties":{"enabled":{"type":"boolean"},"scheme":{"type":"string","enum":["mqtt","mqtts"]},"server":{"type":"string","maxLength":4096},"port":{"type":"integer","minimum":1,"maximum":65535},"clientId":{"type":"string","maxLength":4096},"username":{"type":"string","maxLength":4096},"topicPrefix":{"type":"string","maxLength":4096},"keepAlive":{"type":"integer","minimum":1,"maximum":65535},"autoReconnect":{"type":"boolean"},"authType":{"type":"integer","enum":[0,1]},"deviceNum":{"type":"string","maxLength":4096},"userId":{"type":"string","maxLength":4096},"willTopic":{"type":"string","maxLength":4096},"willPayload":{"type":"string","maxLength":4096},"willQos":{"type":"integer","minimum":0,"maximum":2},"willRetain":{"type":"boolean"},"secretRef":{"type":"string","enum":["protocol.mqtt.password","protocol.mqtt.mqttSecret","protocol.mqtt.authCode"]}}}},"required":["draftId","expectedVersion","connection"]})json");
    } else if (name == "upsert_mqtt_topic" || name == "remove_mqtt_topic") {
        return writeJsonSchema(output, R"json({"type":"object","additionalProperties":false,"properties":{"draftId":{"type":"string"},"expectedVersion":{"type":"integer","minimum":0},"list":{"type":"string","enum":["publish","subscribe"]},"topic":{"type":"object","additionalProperties":false,"properties":{"topic":{"type":"string","minLength":1,"maxLength":1024},"qos":{"type":"integer","minimum":0,"maximum":2},"retain":{"type":"boolean"},"enabled":{"type":"boolean"},"autoPrefix":{"type":"boolean"},"topicType":{"type":"integer","minimum":0,"maximum":7}},"required":["topic"]}},"required":["draftId","expectedVersion","list","topic"]})json");
    } else if (name == "preview_configuration_draft") {
        return writeJsonSchema(output, R"json({"type":"object","additionalProperties":false,"properties":{"draftId":{"type":"string"}},"required":["draftId"]})json");
    } else if (name == "start_configuration_workflow") {
        return writeJsonSchema(output, R"json({"type":"object","additionalProperties":false,"properties":{"templateId":{"type":"string","enum":["document_to_peripheral","document_to_modbus","mqtt_reporting_setup","configuration_review_apply","device_health_review"]},"input":{"type":"object"}},"required":["templateId"]})json");
    } else if (name == "get_workflow_run" || name == "cancel_workflow_run") {
        return writeJsonSchema(output, R"json({"type":"object","additionalProperties":false,"properties":{"runId":{"type":"string"}},"required":["runId"]})json");
    } else if (name == "parse_document" || name == "retry_document_parsing") {
        return writeJsonSchema(output, R"json({"type":"object","additionalProperties":false,"properties":{"documentId":{"type":"string"}},"required":["documentId"]})json");
    } else if (name == "get_operation_audit") {
        return writeJsonSchema(output, R"json({"type":"object","additionalProperties":false,"properties":{"tool":{"type":"string"},"resultCode":{"type":"string"},"pageNum":{"type":"integer","minimum":1},"pageSize":{"type":"integer","minimum":1,"maximum":100}}})json");
    } else if (name == "submit_feedback") {
        return writeJsonSchema(output, R"json({"type":"object","additionalProperties":false,"properties":{"category":{"type":"string","enum":["problem","unsupported_capability","other"]},"message":{"type":"string","maxLength":2000}},"required":["category","message"]})json");
    }
    return true;
}

bool AiMcpToolBridge::validateCall(const String& name, JsonVariantConst arguments, String& error) {
    error = "AI_INVALID_TOOL_ARGUMENTS";
    if (!isAllowed(name)) {
        error = "AI_UNSUPPORTED_TOOL";
        return false;
    }
    if (containsSecret(arguments)) return false;
    auto validPage = [&]() {
        const char* keys[] = {"cursor", "limit"};
        return keysAre(arguments, keys, 2) && text(arguments, "cursor", false, 128) &&
            integer(arguments, "limit", false, 1, 100);
    };
    if (name == "get_device_capabilities" || name == "get_protocol_configuration" ||
        name == "create_config_draft") {
        if (!noArguments(arguments)) return false;
    } else if (name == "list_peripherals" || name == "list_execution_rules" ||
               name == "list_rule_scripts" || name == "get_configuration_history" ||
               name == "list_documents") {
        if (!validPage()) return false;
    } else if (name == "read_peripheral") {
        if (!idOnly(arguments, "peripheralId")) return false;
    } else if (name == "get_configuration_draft" || name == "validate_configuration_draft") {
        if (!idOnly(arguments, "draftId")) return false;
    } else if (name == "get_configuration_operation") {
        if (!idOnly(arguments, "operationId")) return false;
    } else if (name == "upsert_peripheral") {
        const char* keys[] = {"draftId", "expectedVersion", "peripheral"};
        if (!keysAre(arguments, keys, 3) || !text(arguments, "draftId", true) ||
            !integer(arguments, "expectedVersion", true, 0, 1000000) ||
            !validPeripheral(arguments["peripheral"])) return false;
    } else if (name == "remove_peripheral" || name == "set_peripheral_enabled") {
        const char* keys[] = {"draftId", "expectedVersion", "peripheralId", "enabled"};
        if (!keysAre(arguments, keys, name == "set_peripheral_enabled" ? 4 : 3) ||
            !text(arguments, "draftId", true) || !text(arguments, "peripheralId", true) ||
            !integer(arguments, "expectedVersion", true, 0, 1000000) ||
            (name == "set_peripheral_enabled" && !arguments["enabled"].is<bool>())) return false;
    } else if (name == "search_documents") {
        const char* keys[] = {"query", "documentId", "cursor", "limit"};
        if (!keysAre(arguments, keys, 4) || !text(arguments, "query", true, 100) ||
            !text(arguments, "documentId", false) || !text(arguments, "cursor", false, 128) ||
            !integer(arguments, "limit", false, 1, 50)) return false;
    } else if (name == "extract_configuration_facts") {
        const char* keys[] = {"documentId", "kind"};
        if (!keysAre(arguments, keys, 2) || !text(arguments, "documentId", true) ||
            !text(arguments, "kind", false, 40)) return false;
    } else if (name == "get_document_status") {
        if (!idOnly(arguments, "documentId")) return false;
    } else if (name == "request_document_upload") {
        const char* keys[] = {"fileName", "sizeBytes", "sha256"};
        if (!keysAre(arguments, keys, 3) || !text(arguments, "fileName", true, 160) ||
            !integer(arguments, "sizeBytes", true, 1, 20971520) ||
            !text(arguments, "sha256", true, 64)) return false;
        const String digest = arguments["sha256"].as<const char*>();
        for (size_t i = 0; i < digest.length(); ++i) {
            const char ch = digest[i];
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
        }
    } else if (name == "get_device_config_section") {
        if (!idOnly(arguments, "section")) return false;
    } else if (name == "get_device_status" || name == "refresh_device_capabilities" ||
               name == "list_workflow_templates") {
        if (!noArguments(arguments)) return false;
    } else if (name == "update_device_profile" || name == "configure_runtime_features") {
        const char* keys[] = {"draftId", "expectedVersion", name == "update_device_profile" ? "profile" : "features"};
        if (!keysAre(arguments, keys, 3) || !text(arguments, "draftId", true) ||
            !integer(arguments, "expectedVersion", true, 0, 1000000) ||
            !arguments[name == "update_device_profile" ? "profile" : "features"].is<JsonObjectConst>()) return false;
    } else if (name == "upsert_execution_rule") {
        const char* keys[] = {"draftId", "expectedVersion", "rule"};
        if (!keysAre(arguments, keys, 3) || !text(arguments, "draftId", true) ||
            !integer(arguments, "expectedVersion", true, 0, 1000000) ||
            !arguments["rule"].is<JsonObjectConst>() || arguments["rule"]["enabled"] != false) return false;
    } else if (name == "remove_execution_rule") {
        const char* keys[] = {"draftId", "expectedVersion", "ruleId"};
        if (!keysAre(arguments, keys, 3) || !text(arguments, "draftId", true) ||
            !text(arguments, "ruleId", true) || !integer(arguments, "expectedVersion", true, 0, 1000000)) return false;
    } else if (name == "dry_run_execution_rule") {
        const char* keys[] = {"draftId", "ruleId", "inputs"};
        if (!keysAre(arguments, keys, 3) || !text(arguments, "draftId", true) ||
            !text(arguments, "ruleId", true)) return false;
    } else if (name == "upsert_rule_script") {
        const char* keys[] = {"draftId", "expectedVersion", "script", "variables"};
        if (!keysAre(arguments, keys, 4) || !text(arguments, "draftId", true) ||
            !integer(arguments, "expectedVersion", true, 0, 1000000) ||
            !arguments["script"].is<JsonObjectConst>() || arguments["script"]["enabled"] != false ||
            !text(arguments["script"], "name", true, 128) || !text(arguments["script"], "scriptContent", true, 2048)) return false;
    } else if (name == "remove_rule_script") {
        const char* keys[] = {"draftId", "expectedVersion", "scriptId"};
        if (!keysAre(arguments, keys, 3) || !text(arguments, "draftId", true) ||
            !text(arguments, "scriptId", true) || !integer(arguments, "expectedVersion", true, 0, 1000000)) return false;
    } else if (name == "configure_modbus_rtu") {
        const char* keys[] = {"draftId", "expectedVersion", "bus"};
        if (!keysAre(arguments, keys, 3) || !text(arguments, "draftId", true) ||
            !integer(arguments, "expectedVersion", true, 0, 1000000) || !arguments["bus"].is<JsonObjectConst>()) return false;
    } else if (name == "upsert_modbus_device") {
        const char* keys[] = {"draftId", "expectedVersion", "device"};
        if (!keysAre(arguments, keys, 3) || !text(arguments, "draftId", true) ||
            !integer(arguments, "expectedVersion", true, 0, 1000000) ||
            !arguments["device"].is<JsonObjectConst>() || !text(arguments["device"], "name", true, 31) ||
            !integer(arguments["device"], "slaveAddress", true, 1, 247)) return false;
    } else if (name == "remove_modbus_device") {
        const char* keys[] = {"draftId", "expectedVersion", "slaveAddress"};
        if (!keysAre(arguments, keys, 3) || !text(arguments, "draftId", true) ||
            !integer(arguments, "expectedVersion", true, 0, 1000000) ||
            !integer(arguments, "slaveAddress", true, 1, 247)) return false;
    } else if (name == "upsert_modbus_poll_task") {
        const char* keys[] = {"draftId", "expectedVersion", "taskIndex", "task", "source"};
        if (!keysAre(arguments, keys, 5) || !text(arguments, "draftId", true) ||
            !integer(arguments, "expectedVersion", true, 0, 1000000) ||
            !integer(arguments, "taskIndex", true, 0, 1000000) || !arguments["task"].is<JsonObjectConst>() ||
            !integer(arguments["task"], "slaveAddress", true, 1, 247) ||
            !integer(arguments["task"], "functionCode", true, 1, 4) ||
            !integer(arguments["task"], "quantity", true, 1, 125) ||
            !integer(arguments["task"], "pollInterval", true, 2, 3600)) return false;
    } else if (name == "remove_modbus_poll_task") {
        const char* keys[] = {"draftId", "expectedVersion", "taskIndex"};
        if (!keysAre(arguments, keys, 3) || !text(arguments, "draftId", true) ||
            !integer(arguments, "expectedVersion", true, 0, 1000000) ||
            !integer(arguments, "taskIndex", true, 0, 1000000)) return false;
    } else if (name == "configure_mqtt") {
        const char* keys[] = {"draftId", "expectedVersion", "connection"};
        if (!keysAre(arguments, keys, 3) || !text(arguments, "draftId", true) ||
            !integer(arguments, "expectedVersion", true, 0, 1000000) ||
            !arguments["connection"].is<JsonObjectConst>()) return false;
    } else if (name == "upsert_mqtt_topic" || name == "remove_mqtt_topic") {
        const char* keys[] = {"draftId", "expectedVersion", "list", "topic"};
        if (!keysAre(arguments, keys, 4) || !text(arguments, "draftId", true) ||
            !integer(arguments, "expectedVersion", true, 0, 1000000) ||
            !text(arguments, "list", true, 16) || !arguments["topic"].is<JsonObjectConst>()) return false;
    } else if (name == "preview_configuration_draft") {
        if (!idOnly(arguments, "draftId")) return false;
    } else if (name == "start_configuration_workflow") {
        const char* keys[] = {"templateId", "input"};
        if (!keysAre(arguments, keys, 2) || !text(arguments, "templateId", true, 64) ||
            (!arguments["input"].isNull() && !arguments["input"].is<JsonObjectConst>())) return false;
    } else if (name == "get_workflow_run" || name == "cancel_workflow_run") {
        if (!idOnly(arguments, "runId")) return false;
    } else if (name == "parse_document" || name == "retry_document_parsing") {
        if (!idOnly(arguments, "documentId")) return false;
    } else if (name == "get_operation_audit") {
        const char* keys[] = {"tool", "resultCode", "pageNum", "pageSize"};
        if (!keysAre(arguments, keys, 4) || !integer(arguments, "pageNum", false, 1, 1000000) ||
            !integer(arguments, "pageSize", false, 1, 100)) return false;
    } else if (name == "submit_feedback") {
        const char* keys[] = {"category", "message"};
        if (!keysAre(arguments, keys, 2) || !text(arguments, "category", true, 32) ||
            !text(arguments, "message", true, 2000)) return false;
    }
    error = "";
    return true;
}
#endif
