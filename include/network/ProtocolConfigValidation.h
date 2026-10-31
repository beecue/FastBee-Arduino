#ifndef PROTOCOL_CONFIG_VALIDATION_H
#define PROTOCOL_CONFIG_VALIDATION_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstring>
#include "protocols/ModbusRegisterMapping.h"
#include "protocols/MqttTopicPath.h"
#include "systems/ConfigMigrationModbus.h"

namespace FastBee {

inline bool protocolError(String& error, const char* code) {
    error = code;
    return false;
}

inline bool protocolBoolean(JsonObjectConst object, const char* key) {
    return !object.containsKey(key) || object[key].is<bool>();
}

inline bool protocolText(JsonVariantConst value, size_t maxBytes = 4096, bool allowEmpty = true) {
    if (value.isNull()) return true;
    const auto text = value.as<JsonString>();
    return text && (allowEmpty || text.size() > 0) && text.size() <= maxBytes &&
           text.size() == std::strlen(text.c_str());
}

inline bool validOptionalMqttTopic(JsonVariantConst value, bool publish, bool allowEmpty) {
    if (value.isNull()) return true;
    const String topic = value.as<const char*>();
    if (!protocolText(value, UINT16_MAX, allowEmpty)) return false;
    if (allowEmpty && topic.isEmpty()) return true;
    return validMqttTopicText(topic, publish);
}

inline bool protocolMqttTopics(JsonObjectConst mqtt, const char* key, String& error) {
    if (!mqtt.containsKey(key)) return true;
    const auto topics = mqtt[key].as<JsonArrayConst>();
    if (topics.isNull() || topics.size() > 32) return protocolError(error, "MQTT_TOPICS_INVALID");
    for (JsonVariantConst entry : topics) {
        if (!entry.is<JsonObjectConst>()) return protocolError(error, "MQTT_TOPICS_INVALID");
        const auto topic = entry.as<JsonObjectConst>();
        if (!validOptionalMqttTopic(topic["topic"], std::strcmp(key, "publishTopics") == 0, true) ||
            (topic.containsKey("qos") && (!topic["qos"].is<uint8_t>() || topic["qos"].as<uint8_t>() > 2)) ||
            (topic.containsKey("topicType") && (!topic["topicType"].is<uint8_t>() || topic["topicType"].as<uint8_t>() > 7)) ||
            !protocolBoolean(topic, "retain") || !protocolBoolean(topic, "enabled") ||
            !protocolBoolean(topic, "autoPrefix")) return protocolError(error, "MQTT_TOPICS_INVALID");
        for (JsonPairConst field : topic) {
            const String name = field.key().c_str();
            if (name != "topic" && name != "qos" && name != "retain" && name != "enabled" &&
                name != "autoPrefix" && name != "topicType") return protocolError(error, "CONFIG_UNKNOWN_FIELD");
        }
    }
    return true;
}

inline bool validateMqttCandidate(JsonVariantConst value, String& error) {
    if (value.isNull()) return true;
    if (!value.is<JsonObjectConst>()) return protocolError(error, "MQTT_SCHEMA_INVALID");
    const auto mqtt = value.as<JsonObjectConst>();
    static const char* const fields[] = {
        "enabled","scheme","server","port","clientId","username","password","topicPrefix",
        "otaBaseUrl","subscribeTopic","keepAlive","autoReconnect","authType","deviceNum",
        "productId","userId","mqttSecret","authCode","ntpServer","willTopic","willPayload",
        "willQos","willRetain","longitude","latitude","iccid","cardPlatformId","summary",
        "publishTopics","subscribeTopics","publishTopic","publishQos","publishRetain",
        "connectionTestOnly","connectionTestId","disabled_by_memory","disable_mqtt_by_memory",
        "downgraded_by_memory","downgraded_original_scheme","downgraded_original_port",
    };
    for (JsonPairConst field : mqtt) {
        bool known = false;
        for (const char* name : fields) known = known || field.key() == name;
        if (!known) return protocolError(error, "CONFIG_UNKNOWN_FIELD");
    }
    if (mqtt.containsKey("scheme")) {
        const char* scheme = mqtt["scheme"].as<const char*>();
        if (!scheme || (std::strcmp(scheme, "mqtt") && std::strcmp(scheme, "mqtts")))
            return protocolError(error, "MQTT_SCHEME_INVALID");
    }
    if (mqtt.containsKey("port") && (!mqtt["port"].is<uint16_t>() || mqtt["port"].as<uint16_t>() == 0))
        return protocolError(error, "MQTT_PORT_INVALID");
    if (mqtt.containsKey("authType") && (!mqtt["authType"].is<uint8_t>() || mqtt["authType"].as<uint8_t>() > 1))
        return protocolError(error, "MQTT_AUTH_TYPE_INVALID");
    if (mqtt.containsKey("keepAlive") && (!mqtt["keepAlive"].is<uint16_t>() || mqtt["keepAlive"].as<uint16_t>() == 0))
        return protocolError(error, "MQTT_KEEP_ALIVE_INVALID");
    if (mqtt.containsKey("willQos") && (!mqtt["willQos"].is<uint8_t>() || mqtt["willQos"].as<uint8_t>() > 2))
        return protocolError(error, "MQTT_WILL_QOS_INVALID");
    for (const char* key : {"enabled","autoReconnect","willRetain","retain","connectionTestOnly",
                            "disabled_by_memory","disable_mqtt_by_memory","downgraded_by_memory"}) {
        if (!protocolBoolean(mqtt, key)) return protocolError(error, "MQTT_TYPE_INVALID");
    }
    for (const char* key : {"server","clientId","username","password","topicPrefix","otaBaseUrl",
                            "subscribeTopic","deviceNum","productId","userId","mqttSecret","authCode",
                            "ntpServer","willTopic","willPayload","iccid","summary","connectionTestId"}) {
        if (!protocolText(mqtt[key])) return protocolError(error, "MQTT_TYPE_INVALID");
    }
    if (mqtt.containsKey("publishTopic") &&
        (mqtt["publishTopic"].isNull() || !validOptionalMqttTopic(mqtt["publishTopic"], true, false)))
        return protocolError(error, "MQTT_PUBLISH_TOPIC_INVALID");
    if (!validOptionalMqttTopic(mqtt["subscribeTopic"], false, true) ||
        !validOptionalMqttTopic(mqtt["willTopic"], true, true) ||
        !validOptionalMqttTopic(mqtt["topicPrefix"], true, true))
        return protocolError(error, "MQTT_TOPIC_INVALID");
    if (mqtt.containsKey("downgraded_original_scheme")) {
        const char* scheme = mqtt["downgraded_original_scheme"].as<const char*>();
        if (!scheme || (std::strcmp(scheme, "mqtt") && std::strcmp(scheme, "mqtts")))
            return protocolError(error, "MQTT_SCHEME_INVALID");
    }
    if (mqtt.containsKey("downgraded_original_port") &&
        (!mqtt["downgraded_original_port"].is<uint16_t>() || mqtt["downgraded_original_port"].as<uint16_t>() == 0))
        return protocolError(error, "MQTT_PORT_INVALID");
    if ((mqtt["connectionTestOnly"] | false) && String(mqtt["connectionTestId"] | "").isEmpty())
        return protocolError(error, "MQTT_TEST_ID_REQUIRED");
    if (mqtt.containsKey("cardPlatformId") && !mqtt["cardPlatformId"].is<int32_t>())
        return protocolError(error, "MQTT_TYPE_INVALID");
    if ((mqtt.containsKey("longitude") && !mqtt["longitude"].is<double>()) ||
        (mqtt.containsKey("latitude") && !mqtt["latitude"].is<double>()) ||
        (mqtt.containsKey("publishQos") && (!mqtt["publishQos"].is<uint8_t>() || mqtt["publishQos"].as<uint8_t>() > 2)) ||
        (mqtt.containsKey("publishRetain") && !mqtt["publishRetain"].is<bool>()))
        return protocolError(error, "MQTT_TYPE_INVALID");
    return protocolMqttTopics(mqtt, "publishTopics", error) &&
           protocolMqttTopics(mqtt, "subscribeTopics", error);
}

inline bool validateModbusCandidate(JsonVariantConst value, String& error) {
    if (value.isNull()) return true;
    if (!value.is<JsonObjectConst>()) return protocolError(error, "MODBUS_SCHEMA_INVALID");
    const auto rtu = value.as<JsonObjectConst>();
    if (!protocolBoolean(rtu, "enabled")) return protocolError(error, "MODBUS_TYPE_INVALID");
    if (rtu.containsKey("mode")) {
        const char* mode = rtu["mode"].as<const char*>();
        if (!mode || (std::strcmp(mode, "master") && std::strcmp(mode, "slave")))
            return protocolError(error, "MODBUS_MODE_INVALID");
    }
    if (rtu.containsKey("transferType") &&
        (!rtu["transferType"].is<uint8_t>() || rtu["transferType"].as<uint8_t>() > 1))
        return protocolError(error, "MODBUS_TRANSFER_TYPE_INVALID");
    if (!protocolText(rtu["peripheralId"], 80)) return protocolError(error, "MODBUS_TYPE_INVALID");
    for (const char* key : {"dePin","txPin","rxPin"}) {
        if (rtu.containsKey(key) && !rtu[key].is<int32_t>()) return protocolError(error, "MODBUS_TYPE_INVALID");
    }
    for (const char* key : {"slaveAddress","baudRate","responseTimeout","interFrameDelay"}) {
        if (rtu.containsKey(key) && !rtu[key].is<uint32_t>()) return protocolError(error, "MODBUS_TYPE_INVALID");
    }
    ConfigMigration::ModbusReviewGraph graph;
    if (!ConfigMigration::readModbusReviewGraph(rtu, graph)) return protocolError(error, "MODBUS_SCHEMA_INVALID");
    if (rtu.containsKey("master") && !rtu["master"].is<JsonObjectConst>())
        return protocolError(error, "MODBUS_SCHEMA_INVALID");
    const auto tasks = rtu["master"]["tasks"].as<JsonArrayConst>();
    if (tasks.size() > Protocols::MODBUS_MAX_POLL_TASKS) return protocolError(error, "MODBUS_CAPACITY_INVALID");
    for (JsonVariantConst entry : tasks) {
        if (!entry.is<JsonObjectConst>()) return protocolError(error, "MODBUS_TASK_INVALID");
        const auto task = entry.as<JsonObjectConst>();
        if ((task.containsKey("slaveAddress") && (!task["slaveAddress"].is<uint8_t>() ||
             task["slaveAddress"].as<uint8_t>() == 0 || task["slaveAddress"].as<uint8_t>() > 247)) ||
            (task.containsKey("functionCode") && (!task["functionCode"].is<uint8_t>() ||
             task["functionCode"].as<uint8_t>() < 1 || task["functionCode"].as<uint8_t>() > 4)) ||
            (task.containsKey("pollInterval") && !task["pollInterval"].is<uint16_t>()) ||
            (task.containsKey("startAddress") && !task["startAddress"].is<uint16_t>()) ||
            (task.containsKey("quantity") && (!task["quantity"].is<uint16_t>() ||
             task["quantity"].as<uint16_t>() == 0 || task["quantity"].as<uint16_t>() > Protocols::MODBUS_MAX_REGISTERS_PER_READ)) ||
            !protocolBoolean(task, "enabled") || !protocolText(task["name"], 96) ||
            !protocolText(task["label"], 96) || !validModbusTaskMappings(task))
            return protocolError(error, "MODBUS_TASK_INVALID");
    }
    const auto devices = rtu["master"]["devices"].as<JsonArrayConst>();
    if (devices.size() > Protocols::MODBUS_MAX_SUB_DEVICES) return protocolError(error, "MODBUS_CAPACITY_INVALID");
    for (JsonVariantConst entry : devices) {
        if (!entry.is<JsonObjectConst>()) return protocolError(error, "MODBUS_DEVICE_INVALID");
        const auto device = entry.as<JsonObjectConst>();
        if (!protocolText(device["name"], 96) || !protocolText(device["sensorId"], 80) ||
            !protocolText(device["deviceType"], 16, false) || !protocolBoolean(device, "enabled") ||
            (device.containsKey("slaveAddress") && (!device["slaveAddress"].is<uint8_t>() ||
             device["slaveAddress"].as<uint8_t>() == 0 || device["slaveAddress"].as<uint8_t>() > 247)) ||
            (device.containsKey("channelCount") && (!device["channelCount"].is<uint8_t>() ||
             device["channelCount"].as<uint8_t>() == 0)) ||
            (device.containsKey("controlProtocol") && (!device["controlProtocol"].is<uint8_t>() ||
             device["controlProtocol"].as<uint8_t>() > 1)) ||
            (device.containsKey("pwmResolution") && (!device["pwmResolution"].is<uint8_t>() ||
             device["pwmResolution"].as<uint8_t>() == 0 || device["pwmResolution"].as<uint8_t>() > 16)))
            return protocolError(error, "MODBUS_DEVICE_INVALID");
    }
    return true;
}

inline bool validateProtocolConfigDocument(JsonVariantConst document, String& error) {
    error = "";
    if (!document.is<JsonObjectConst>()) return protocolError(error, "PROTOCOL_SCHEMA_INVALID");
    const auto root = document.as<JsonObjectConst>();
    for (JsonPairConst field : root) {
        const String key = field.key().c_str();
        if (key != "version" && key != "mqtt" && key != "modbusRtu" && key != "coap" &&
            key != "tcp" && key != "http" && key != "remote") return protocolError(error, "CONFIG_UNKNOWN_FIELD");
        if (key == "version" && !field.value().is<int32_t>()) return protocolError(error, "PROTOCOL_VERSION_INVALID");
        if ((key == "coap" || key == "tcp" || key == "http" || key == "remote") &&
            !field.value().is<JsonObjectConst>()) return protocolError(error, "PROTOCOL_SECTION_INVALID");
    }
    return validateMqttCandidate(root["mqtt"], error) && validateModbusCandidate(root["modbusRtu"], error);
}

} // namespace FastBee

#endif // PROTOCOL_CONFIG_VALIDATION_H
