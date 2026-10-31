#pragma once
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ArduinoJson.h>
#include "core/PeripheralExecution.h"  // ExecActionType（动作值 schema 描述）
#include "core/ResourceProfile.h"

namespace FastBee {
// Same decimal conversion as the existing action preflight, with explicit
// overflow and embedded-NUL rejection. Output changes only after success.
// This pure helper neither allocates nor authorizes a hardware operation.
inline bool parseStrictActionLong(const char* text, size_t length, long& value) {
    if (!text || !length || std::strlen(text) != length) return false;
    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (errno == ERANGE || end == text || end != text + length) return false;
    value = parsed;
    return true;
}

inline bool parseDacActionValue(const char* text, size_t length, uint8_t& value) {
    long parsed = 0;
    if (!parseStrictActionLong(text, length, parsed) || parsed < 0 || parsed > 255)
        return false;
    value = static_cast<uint8_t>(parsed);
    return true;
}

// 生产动作值 schema 描述与共享校验器：规则 preflight（PeriphExecRouteHandler）
// 与动作目录生成（ActionTypeCatalog）消费同一份实现，不允许平行常量漂移。
// 宿主机契约测试逐项核对边界；云端 Java 校验器消费目录中的 schema 数据。
struct ActionValueSchema {
    const char* kind;          // none|any|integer|nonempty-text|empty-only|enum|json-object|json-poll|modbus-coil|modbus-reg|uart-command
    long minimum;              // integer kind 的下界
    long maximum;              // integer kind 的上界
    const char* allowedValues; // enum kind：逗号分隔字面量；json-poll kind：ctrl 条目的 a 字段枚举
    bool relaxedByReceivedValue; // useReceivedValue=true 时跳过值形态校验（与生产 preflight 一致）
    bool receivedValueAllowed;   // false 表示生产校验禁止 useReceivedValue（RTC/SD/远程只读等）
    const char* errorCode;     // 生产 preflight 沿用的错误码（保持既有客户端兼容）
};

inline bool stringEqualsIgnoreCaseAscii(const char* text, size_t length, const char* literal) {
    const size_t literalLength = std::strlen(literal);
    if (length != literalLength) return false;
    for (size_t i = 0; i < length; ++i) {
        char a = text[i];
        char b = literal[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

inline bool parseStrictActionUnsigned(const char* text, size_t length, long& value) {
    // 有界前缀十进制解析（用于 "addr:value" 的段），拒绝空段/非数字/溢出/内嵌 NUL
    if (!text || !length) return false;
    long parsed = 0;
    for (size_t i = 0; i < length; ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') return false;
        if (parsed > 6553) return false;  // 6553*10+9 > 65535 上界前的粗拦截
        parsed = parsed * 10 + (c - '0');
    }
    if (parsed > 65535) return false;
    value = parsed;
    return true;
}

// 生产共享校验器：规则 preflight、目录语义与云端早期校验的唯一实现来源。
// 返回 nullptr 表示通过，否则返回 descriptor 的 errorCode。
inline const char* validateActionValueBySchema(const ActionValueSchema& schema, const char* value,
                                               bool useReceivedValue) {
    if (useReceivedValue && !schema.receivedValueAllowed) return schema.errorCode;
    if (useReceivedValue && schema.relaxedByReceivedValue) return nullptr;
    const size_t length = value ? std::strlen(value) : 0;
    if (length > ResourceProfile::MAX_ACTION_VALUE_LEN) return schema.errorCode;
    switch (schema.kind[0]) {
        case 'n': {
            if (std::strcmp(schema.kind, "none") == 0) return nullptr;
            if (std::strcmp(schema.kind, "nonempty-text") == 0) return length > 0 ? nullptr : schema.errorCode;
            return schema.errorCode;
        }
        case 'a': return nullptr;  // any：有界自由文本（仅事件附加数据等）
        case 'e': {
            if (std::strcmp(schema.kind, "empty-only") == 0) return length == 0 ? nullptr : schema.errorCode;
            // enum：逗号分隔字面量的精确成员匹配
            const char* list = schema.allowedValues;
            while (list && *list) {
                const char* comma = std::strchr(list, ',');
                const size_t entryLength = comma ? static_cast<size_t>(comma - list) : std::strlen(list);
                if (entryLength == length && std::strncmp(list, value, length) == 0) return nullptr;
                if (!comma) break;
                list = comma + 1;
            }
            return schema.errorCode;
        }
        case 'i': {
            long parsed = 0;
            if (!parseStrictActionLong(value, length, parsed) || parsed < schema.minimum
                    || parsed > schema.maximum) {
                return schema.errorCode;
            }
            return nullptr;
        }
        case 'j': {
            JsonDocument doc;
            if (!length || deserializeJson(doc, value, length) || !doc.is<JsonObject>()) {
                return schema.errorCode;
            }
            if (std::strcmp(schema.kind, "json-object") == 0) return nullptr;
            // json-poll：必须含 poll（非负整数数组）或 ctrl（{d:非负整数, c?，a∈枚举, v/p?}）数组
            bool valid = false;
            if (doc["poll"].is<JsonArrayConst>()) {
                valid = true;
                for (JsonVariantConst item : doc["poll"].as<JsonArrayConst>()) {
                    if (!item.is<int>() || item.as<int>() < 0) return schema.errorCode;
                }
            }
            if (doc["ctrl"].is<JsonArrayConst>()) {
                valid = true;
                for (JsonObjectConst item : doc["ctrl"].as<JsonArrayConst>()) {
                    if (!item["d"].is<int>() || item["d"].as<int>() < 0) return schema.errorCode;
                    if (!item["c"].isNull() && !item["c"].is<int>()) return schema.errorCode;
                    if (!item["v"].isNull() && !item["v"].is<int>()) return schema.errorCode;
                    if (!item["p"].isNull() && !item["p"].is<const char*>()) return schema.errorCode;
                    if (!item["a"].isNull()) {
                        const char* action = item["a"].as<const char*>();
                        bool known = false;
                        const char* list = schema.allowedValues;
                        while (action && list && *list) {
                            const char* comma = std::strchr(list, ',');
                            const size_t entryLength = comma ? static_cast<size_t>(comma - list)
                                                             : std::strlen(list);
                            if (std::strlen(action) == entryLength
                                    && std::strncmp(list, action, entryLength) == 0) {
                                known = true;
                                break;
                            }
                            if (!comma) break;
                            list = comma + 1;
                        }
                        if (!known) return schema.errorCode;
                    }
                }
            }
            return valid ? nullptr : schema.errorCode;
        }
        case 'm': {
            // modbus-coil：[通道:]状态（1/0/true/false/on/off，大小写不敏感）；
            // modbus-reg：十进制 或 地址:十进制
            const char* state = value;
            size_t stateLength = length;
            const char* separator = value ? static_cast<const char*>(std::memchr(value, ':', length)) : nullptr;
            if (separator) {
                long ignored = 0;
                if (!parseStrictActionUnsigned(value, static_cast<size_t>(separator - value), ignored)) {
                    return schema.errorCode;
                }
                state = separator + 1;
                stateLength = length - static_cast<size_t>(separator - value) - 1;
            }
            if (std::strcmp(schema.kind, "modbus-reg") == 0) {
                long ignored = 0;
                return parseStrictActionUnsigned(state, stateLength, ignored) ? nullptr : schema.errorCode;
            }
            if (stringEqualsIgnoreCaseAscii(state, stateLength, "1")
                    || stringEqualsIgnoreCaseAscii(state, stateLength, "0")
                    || stringEqualsIgnoreCaseAscii(state, stateLength, "true")
                    || stringEqualsIgnoreCaseAscii(state, stateLength, "false")
                    || stringEqualsIgnoreCaseAscii(state, stateLength, "on")
                    || stringEqualsIgnoreCaseAscii(state, stateLength, "off")) {
                return nullptr;
            }
            return schema.errorCode;
        }
        case 'u': {
            // uart-command：纯文本或受限 JSON 命令（periphId/action∈send,text,write/value/text/message）
            if (!length) return schema.errorCode;
            if (value[0] != '{') return nullptr;
            JsonDocument doc;
            if (deserializeJson(doc, value, length) || !doc.is<JsonObject>()) return schema.errorCode;
            for (JsonPairConst field : doc.as<JsonObjectConst>()) {
                const char* key = field.key().c_str();
                if (std::strcmp(key, "periphId") && std::strcmp(key, "action") && std::strcmp(key, "value")
                        && std::strcmp(key, "text") && std::strcmp(key, "message")) {
                    return schema.errorCode;
                }
            }
            if (!doc["action"].isNull()) {
                const char* command = doc["action"].as<const char*>();
                if (!command || (!stringEqualsIgnoreCaseAscii(command, std::strlen(command), "send")
                        && !stringEqualsIgnoreCaseAscii(command, std::strlen(command), "text")
                        && !stringEqualsIgnoreCaseAscii(command, std::strlen(command), "write"))) {
                    return schema.errorCode;
                }
            }
            return nullptr;
        }
        default:
            return schema.errorCode;
    }
}

inline ActionValueSchema describeActionValueSchema(uint8_t actionType) {
    switch (static_cast<ExecActionType>(actionType)) {
        // 电平类/系统/规则控制动作不使用 actionValue
        case ExecActionType::ACTION_HIGH:
        case ExecActionType::ACTION_LOW:
        case ExecActionType::ACTION_HIGH_INVERTED:
        case ExecActionType::ACTION_LOW_INVERTED:
        case ExecActionType::ACTION_GPIO_TOGGLE:
        case ExecActionType::ACTION_ENABLE_EXEC_RULE:
        case ExecActionType::ACTION_DISABLE_EXEC_RULE:
        case ExecActionType::ACTION_SYS_RESTART:
        case ExecActionType::ACTION_SYS_FACTORY_RESET:
        case ExecActionType::ACTION_SYS_NTP_SYNC:
        case ExecActionType::ACTION_SYS_OTA:
        case ExecActionType::ACTION_RESERVED_20:
        case ExecActionType::ACTION_REMOTE_CHANNEL:
        case ExecActionType::ACTION_DISPLAY_CLEAR:
            return {"none", 0, 0, nullptr, false, true, "INVALID_ACTION_VALUE"};
        // 生产 preflight：闪烁/呼吸 50-60000ms；PWM 0-65535；DAC 0-255；useReceivedValue 时跳过
        case ExecActionType::ACTION_BLINK:
        case ExecActionType::ACTION_BREATHE:
            return {"integer", 50, 60000, nullptr, true, true, "INVALID_EFFECT_INTERVAL"};
        case ExecActionType::ACTION_SET_PWM:
            return {"integer", 0, 65535, nullptr, true, true, "INVALID_PWM_VALUE"};
        case ExecActionType::ACTION_SET_DAC:
            return {"integer", 0, 255, nullptr, true, true, "INVALID_DAC_VALUE"};
        // 命令序列脚本/通用外设指令必须有内容
        case ExecActionType::ACTION_SCRIPT:
            return {"nonempty-text", 0, 0, nullptr, false, true, "SCRIPT_ACTION_EMPTY"};
        case ExecActionType::ACTION_CALL_PERIPHERAL:
        case ExecActionType::ACTION_NEOPIXEL_EFFECT:
        case ExecActionType::ACTION_STEPPER_CONTROL:
        case ExecActionType::ACTION_DISPLAY_NUMBER:
        case ExecActionType::ACTION_DISPLAY_TEXT:
        case ExecActionType::ACTION_OLED_DISPLAY:
        case ExecActionType::ACTION_RF_SEND:
            return {"nonempty-text", 0, 0, nullptr, false, true, "INVALID_ACTION_VALUE"};
        // UART 发送：纯文本或受限 JSON 命令
        case ExecActionType::ACTION_UART_SEND:
            return {"uart-command", 0, 0, nullptr, false, true, "INVALID_UART_PAYLOAD"};
        // Modbus 写：线圈/寄存器的生产形态
        case ExecActionType::ACTION_MODBUS_COIL_WRITE:
            return {"modbus-coil", 0, 0, nullptr, true, true, "INVALID_MODBUS_VALUE"};
        case ExecActionType::ACTION_MODBUS_REG_WRITE:
            return {"modbus-reg", 0, 0, nullptr, true, true, "INVALID_MODBUS_VALUE"};
        // Modbus 轮询：JSON 且含 poll 或 ctrl 数组（ctrl.a 枚举与生产执行器一致）
        case ExecActionType::ACTION_MODBUS_POLL:
            return {"json-poll", 0, 0, "on,off,trigger,pwm,pid", false, true, "INVALID_MODBUS_POLL"};
        // 传感器读取/温控/SD 记录：JSON 对象
        case ExecActionType::ACTION_SENSOR_READ:
            return {"json-object", 0, 0, nullptr, false, true, "INVALID_SENSOR_CONFIG"};
        case ExecActionType::ACTION_THERMOSTAT:
        case ExecActionType::ACTION_SD_RECORD:
            return {"json-object", 0, 0, nullptr, false, true, "INVALID_ACTION_VALUE"};
        // 功能动作只允许 webServer/mdns
        case ExecActionType::ACTION_FEATURE_ENABLE:
        case ExecActionType::ACTION_FEATURE_DISABLE:
            return {"enum", 0, 0, "webServer,mdns", false, true, "FEATURE_NOT_ALLOWED"};
        // 受限远程读：目标必填、值为空、禁止接收值替换
        case ExecActionType::ACTION_REMOTE_READ:
            return {"empty-only", 0, 0, nullptr, false, false, "INVALID_REMOTE_READ"};
        // RTC：生产 validateRtcAction 的字面量契约
        case ExecActionType::ACTION_RTC_CONTROL:
            return {"enum", 0, 0, "start,stop", false, false, "DS1302_ACTION_INVALID"};
        case ExecActionType::ACTION_RTC_READ:
            return {"empty-only", 0, 0, nullptr, false, false, "DS1302_READ_ACTION_INVALID"};
        case ExecActionType::ACTION_RTC_SET:
            return {"enum", 0, 0, "system-local", false, false, "DS1302_SET_ACTION_INVALID"};
        // SD 状态：只读，值为空、禁止接收值替换
        case ExecActionType::ACTION_SD_STATUS:
            return {"empty-only", 0, 0, nullptr, false, false, "SD_STATUS_ACTION_INVALID"};
        // 触发设备事件：值为可选的事件附加数据（有界自由文本）
        case ExecActionType::ACTION_TRIGGER_EVENT:
            return {"any", 0, 0, nullptr, false, true, "INVALID_ACTION_VALUE"};
    }
    return {"none", 0, 0, nullptr, false, true, "INVALID_ACTION_VALUE"};
}

// 生产 preflight 入口：规则动作值的唯一共享校验（目录生成与路由校验同源）。
inline const char* validateRuleActionValue(uint8_t actionType, const char* value, bool useReceivedValue) {
    return validateActionValueBySchema(describeActionValueSchema(actionType), value, useReceivedValue);
}
} // namespace FastBee
