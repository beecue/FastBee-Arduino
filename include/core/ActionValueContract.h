#pragma once
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "core/PeripheralExecution.h"  // ExecActionType（动作值 schema 描述）

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

// 生产动作值 schema 描述：与 PeriphExecRouteHandler 规则 preflight 的逐动作
// 校验同一语义来源（区间/枚举字面量修改时必须同步，宿主机契约测试逐项核对边界）。
// 供 /api/periph-exec/action-types 目录携带，云端规则工具据此做早期值形态校验。
struct ActionValueSchema {
    const char* kind;          // none|any|integer|nonempty-text|empty-only|enum|json-object|json-poll
    long minimum;              // integer kind 的下界
    long maximum;              // integer kind 的上界
    const char* allowedValues; // enum kind：逗号分隔字面量
    bool relaxedByReceivedValue; // useReceivedValue=true 时跳过值形态校验（与生产 preflight 一致）
    bool receivedValueAllowed;   // false 表示生产校验禁止 useReceivedValue（RTC/SD/远程只读等）
};

inline ActionValueSchema describeActionValueSchema(uint8_t actionType) {
    switch (static_cast<ExecActionType>(actionType)) {
        // 电平类动作不使用 actionValue
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
            return {"none", 0, 0, nullptr, false, true};
        // 生产 preflight：闪烁/呼吸 50-60000ms；PWM 0-65535；DAC 0-255；
        // useReceivedValue 时跳过
        case ExecActionType::ACTION_BLINK:
        case ExecActionType::ACTION_BREATHE:
            return {"integer", 50, 60000, nullptr, true, true};
        case ExecActionType::ACTION_SET_PWM:
            return {"integer", 0, 65535, nullptr, true, true};
        case ExecActionType::ACTION_SET_DAC:
            return {"integer", 0, 255, nullptr, true, true};
        // 命令序列脚本必须有内容
        case ExecActionType::ACTION_SCRIPT:
            return {"nonempty-text", 0, 0, nullptr, false, true};
        // Modbus 轮询：JSON 且含 poll 或 ctrl 数组
        case ExecActionType::ACTION_MODBUS_POLL:
            return {"json-poll", 0, 0, nullptr, false, true};
        // 传感器读取/温控/SD 记录：JSON 对象
        case ExecActionType::ACTION_SENSOR_READ:
        case ExecActionType::ACTION_THERMOSTAT:
        case ExecActionType::ACTION_SD_RECORD:
            return {"json-object", 0, 0, nullptr, false, true};
        // 功能动作只允许 webServer/mdns
        case ExecActionType::ACTION_FEATURE_ENABLE:
        case ExecActionType::ACTION_FEATURE_DISABLE:
            return {"enum", 0, 0, "webServer,mdns", false, true};
        // 受限远程读：目标必填、值为空、禁止接收值替换
        case ExecActionType::ACTION_REMOTE_READ:
            return {"empty-only", 0, 0, nullptr, false, false};
        // RTC：生产 validateRtcAction 的字面量契约
        case ExecActionType::ACTION_RTC_CONTROL:
            return {"enum", 0, 0, "start,stop", false, false};
        case ExecActionType::ACTION_RTC_READ:
            return {"empty-only", 0, 0, nullptr, false, false};
        case ExecActionType::ACTION_RTC_SET:
            return {"enum", 0, 0, "system-local", false, false};
        // SD 状态：只读，值为空、禁止接收值替换
        case ExecActionType::ACTION_SD_STATUS:
            return {"empty-only", 0, 0, nullptr, false, false};
        // 其余（指令/灯效/电机/显示/RF/UART/Modbus 写/事件数据）：有界任意文本，
        // 逐命令形态由生产执行器/驱动校验
        default:
            return {"any", 0, 0, nullptr, false, true};
    }
}
} // namespace FastBee
