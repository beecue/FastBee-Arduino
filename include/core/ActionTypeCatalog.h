#ifndef ACTION_TYPE_CATALOG_H
#define ACTION_TYPE_CATALOG_H

#include <ArduinoJson.h>
#include <cstring>
#include "core/PeripheralActionContract.h"
#include "core/PeripheralTypes.h"
#include "core/ActionValueContract.h"
#include "core/SystemConstants.h"

// 生产动作类型目录：供 /api/periph-exec/action-types 与 MCP 配对能力载荷使用。
// 动作语义（是否需要外设目标、普通模式允许的目标类型）全部由
// PeripheralActionContract 的生产谓词实时计算，与规则 preflight/保存路径同源，
// 不复制人工表格。扩展器/无源（ioSource/passive）变体不在普通模式目录内，
// 由设备生产预检裁定。
namespace FastBee {
namespace ActionTypeCatalog {

// 目录 schema 版本：字段语义变化时递增；云端按它做兼容性门禁
constexpr uint8_t SCHEMA_VERSION = 1;

// ExecActionType 的编译期名称（运行时无反射；test_action_type_catalog_contract
// 解析 PeripheralExecution.h 源码核对该表，禁止漂移）
inline const char* actionTypeName(uint8_t value) {
    switch (static_cast<ExecActionType>(value)) {
        case ExecActionType::ACTION_HIGH: return "ACTION_HIGH";
        case ExecActionType::ACTION_LOW: return "ACTION_LOW";
        case ExecActionType::ACTION_BLINK: return "ACTION_BLINK";
        case ExecActionType::ACTION_BREATHE: return "ACTION_BREATHE";
        case ExecActionType::ACTION_SET_PWM: return "ACTION_SET_PWM";
        case ExecActionType::ACTION_SET_DAC: return "ACTION_SET_DAC";
        case ExecActionType::ACTION_SYS_RESTART: return "ACTION_SYS_RESTART";
        case ExecActionType::ACTION_SYS_FACTORY_RESET: return "ACTION_SYS_FACTORY_RESET";
        case ExecActionType::ACTION_SYS_NTP_SYNC: return "ACTION_SYS_NTP_SYNC";
        case ExecActionType::ACTION_SYS_OTA: return "ACTION_SYS_OTA";
        case ExecActionType::ACTION_CALL_PERIPHERAL: return "ACTION_CALL_PERIPHERAL";
        case ExecActionType::ACTION_NEOPIXEL_EFFECT: return "ACTION_NEOPIXEL_EFFECT";
        case ExecActionType::ACTION_STEPPER_CONTROL: return "ACTION_STEPPER_CONTROL";
        case ExecActionType::ACTION_HIGH_INVERTED: return "ACTION_HIGH_INVERTED";
        case ExecActionType::ACTION_LOW_INVERTED: return "ACTION_LOW_INVERTED";
        case ExecActionType::ACTION_SCRIPT: return "ACTION_SCRIPT";
        case ExecActionType::ACTION_MODBUS_COIL_WRITE: return "ACTION_MODBUS_COIL_WRITE";
        case ExecActionType::ACTION_MODBUS_REG_WRITE: return "ACTION_MODBUS_REG_WRITE";
        case ExecActionType::ACTION_MODBUS_POLL: return "ACTION_MODBUS_POLL";
        case ExecActionType::ACTION_SENSOR_READ: return "ACTION_SENSOR_READ";
        case ExecActionType::ACTION_RESERVED_20: return "ACTION_RESERVED_20";
        case ExecActionType::ACTION_TRIGGER_EVENT: return "ACTION_TRIGGER_EVENT";
        case ExecActionType::ACTION_ENABLE_EXEC_RULE: return "ACTION_ENABLE_EXEC_RULE";
        case ExecActionType::ACTION_DISABLE_EXEC_RULE: return "ACTION_DISABLE_EXEC_RULE";
        case ExecActionType::ACTION_DISPLAY_NUMBER: return "ACTION_DISPLAY_NUMBER";
        case ExecActionType::ACTION_DISPLAY_TEXT: return "ACTION_DISPLAY_TEXT";
        case ExecActionType::ACTION_DISPLAY_CLEAR: return "ACTION_DISPLAY_CLEAR";
        case ExecActionType::ACTION_OLED_DISPLAY: return "ACTION_OLED_DISPLAY";
        case ExecActionType::ACTION_RF_SEND: return "ACTION_RF_SEND";
        case ExecActionType::ACTION_UART_SEND: return "ACTION_UART_SEND";
        case ExecActionType::ACTION_GPIO_TOGGLE: return "ACTION_GPIO_TOGGLE";
        case ExecActionType::ACTION_FEATURE_ENABLE: return "ACTION_FEATURE_ENABLE";
        case ExecActionType::ACTION_FEATURE_DISABLE: return "ACTION_FEATURE_DISABLE";
        case ExecActionType::ACTION_REMOTE_CHANNEL: return "ACTION_REMOTE_CHANNEL";
        case ExecActionType::ACTION_REMOTE_READ: return "ACTION_REMOTE_READ";
        case ExecActionType::ACTION_RTC_CONTROL: return "ACTION_RTC_CONTROL";
        case ExecActionType::ACTION_RTC_READ: return "ACTION_RTC_READ";
        case ExecActionType::ACTION_RTC_SET: return "ACTION_RTC_SET";
        case ExecActionType::ACTION_SD_STATUS: return "ACTION_SD_STATUS";
        case ExecActionType::ACTION_THERMOSTAT: return "ACTION_THERMOSTAT";
        case ExecActionType::ACTION_SD_RECORD: return "ACTION_SD_RECORD";
    }
    return "UNKNOWN";
}

// 动作值范围：ExecActionType 当前定义 0..40（含保留/退役号；是否允许进入
// AI 配置由云端安全策略决定，目录只描述生产语义）
constexpr uint8_t ACTION_TYPE_MAX = static_cast<uint8_t>(ExecActionType::ACTION_SD_RECORD);

// PeripheralType 的枚举值清单（与 PeripheralTypes.h 一致；
// test_action_type_catalog_contract 解析源码核对，禁止漂移）
constexpr uint8_t PERIPHERAL_TYPE_VALUES[] = {
    0, 1, 2, 3, 4, 5, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 26, 27, 31, 32,
    36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 60
};
constexpr size_t PERIPHERAL_TYPE_COUNT = sizeof(PERIPHERAL_TYPE_VALUES) / sizeof(PERIPHERAL_TYPE_VALUES[0]);

// 写入动作类型目录。targetTypes 为普通模式（ioSource=0、非 passive）下
// 生产谓词放行的外设类型；ACTION_CALL_PERIPHERAL 的目标匹配是命令相关的，
// 目录不展开为类型清单（省略 targetTypes 键）。
inline void writeActionTypeCatalog(JsonObject out) {
    out["schemaVersion"] = SCHEMA_VERSION;
    out["firmwareVersion"] = SystemInfo::VERSION;
    JsonArray actions = out["actions"].to<JsonArray>();
    for (uint8_t value = 0; value <= ACTION_TYPE_MAX; ++value) {
        JsonObject item = actions.add<JsonObject>();
        item["value"] = value;
        item["name"] = actionTypeName(value);
        const bool required = PeripheralActionContract::requiresPeripheralTarget(value);
        item["requiresPeripheralTarget"] = required;
        // 生产动作值 schema（与规则 preflight 的逐动作校验同一语义来源）
        const ActionValueSchema schema = describeActionValueSchema(value);
        JsonObject valueSchema = item["valueSchema"].to<JsonObject>();
        valueSchema["kind"] = schema.kind;
        if (std::strcmp(schema.kind, "integer") == 0) {
            valueSchema["min"] = schema.minimum;
            valueSchema["max"] = schema.maximum;
        }
        if (schema.allowedValues != nullptr) {
            valueSchema["values"] = schema.allowedValues;
        }
        valueSchema["relaxedByReceivedValue"] = schema.relaxedByReceivedValue;
        valueSchema["receivedValueAllowed"] = schema.receivedValueAllowed;
        if (!required || value == static_cast<uint8_t>(ExecActionType::ACTION_CALL_PERIPHERAL)) {
            continue;
        }
        JsonArray types = item["targetTypes"].to<JsonArray>();
        for (size_t i = 0; i < PERIPHERAL_TYPE_COUNT; ++i) {
            PeripheralConfig target;  // 默认零值 = 普通模式
            target.type = static_cast<PeripheralType>(PERIPHERAL_TYPE_VALUES[i]);
            if (value == static_cast<uint8_t>(ExecActionType::ACTION_THERMOSTAT)) {
                // 与 scripts/generate-action-contracts.py 一致：枚举声明的可行目标，
                // 而不是默认零值（pinCount=0/初始 LOW 不支持任何温控器）
                target.pinCount = 1;
                target.params.gpio.invertLogic = false;
                target.params.gpio.initialState = GPIOState::STATE_HIGH;
            }
            if (PeripheralActionContract::matchesPeripheralTarget(value, target)) {
                types.add(PERIPHERAL_TYPE_VALUES[i]);
            }
        }
    }
}

} // namespace ActionTypeCatalog
} // namespace FastBee

#endif // ACTION_TYPE_CATALOG_H
