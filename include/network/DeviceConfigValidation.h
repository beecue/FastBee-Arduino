#pragma once
#include <ArduinoJson.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace FastBee {
inline bool deviceConfigInteger(JsonVariantConst value, long long low, long long high) {
    long long number = 0;
    if (value.is<const char*>()) {
        const char* text = value.as<const char*>();
        if (!text || !*text) return false;
        for (const char* p = text; *p; ++p) if (*p < '0' || *p > '9') return false;
        errno = 0;
        char* end = nullptr;
        number = strtoll(text, &end, 10);
        if (errno == ERANGE || !end || *end) return false;
    } else if (value.is<long long>() && !value.is<bool>()) {
        number = value.as<long long>();
    } else return false;
    return number >= low && number <= high;
}

inline bool deviceConfigBoolean(JsonVariantConst value) {
    if (value.is<bool>()) return true;
    if (!value.is<const char*>()) return false;
    const char* text = value.as<const char*>();
    return !strcmp(text, "true") || !strcmp(text, "false") || !strcmp(text, "1") || !strcmp(text, "0");
}

inline const char* validateDeviceConfigPatch(JsonObjectConst obj) {
    if (obj.isNull() || obj.size() == 0) return "Configuration patch must be a nonempty object";
    bool security = false, device = false;
    for (JsonPairConst pair : obj) {
        const char* key = pair.key().c_str();
        JsonVariantConst value = pair.value();
        long long low = 0, high = 0;
        size_t maxLength = 0;
        bool isSecurity = false, boolean = false;
        if (!strcmp(key, "productNumber")) high = 2147483647;
        else if (!strcmp(key, "syncInterval")) { low = 60; high = 86400; }
        else if (!strcmp(key, "cacheDuration")) high = 31536000;
        else if (!strcmp(key, "maxLoginAttempts")) { low = 1; high = 10; isSecurity = true; }
        else if (!strcmp(key, "loginLockoutTime")) { low = 60000; high = 3600000; isSecurity = true; }
        else if (!strcmp(key, "minPasswordLength")) { low = 4; high = 32; isSecurity = true; }
        else if (!strcmp(key, "requireStrongPasswords")) { boolean = true; isSecurity = true; }
        else if (!strcmp(key, "enableNTP")) boolean = true;
        else if (!strcmp(key, "deviceId")) maxLength = 32;
        else if (!strcmp(key, "deviceName")) maxLength = 128;
        else if (!strcmp(key, "userId")) maxLength = 64;
        else if (!strcmp(key, "description")) maxLength = 1024;
        else if (!strcmp(key, "ntpServer1") || !strcmp(key, "ntpServer2")) maxLength = 255;
        else if (!strcmp(key, "timezone")) maxLength = 63;
        else if (!strcmp(key, "logLevel")) {
            const char* level = value.as<const char*>();
            if (!level || (strcmp(level, "DEBUG") && strcmp(level, "INFO") && strcmp(level, "WARNING") && strcmp(level, "ERROR"))) return "Invalid log level";
        } else return "Unsupported configuration field";
        if (high && !deviceConfigInteger(value, low, high)) return "Configuration integer out of range";
        if (boolean && !deviceConfigBoolean(value)) return "Invalid configuration boolean";
        if (maxLength && (!value.is<const char*>() || strlen(value.as<const char*>()) > maxLength)) return "Invalid configuration text length";
        security |= isSecurity;
        device |= !isSecurity;
    }
    // Each tab commits one file. Reject cross-file partial commits explicitly.
    return security && device ? "Save security policy separately from device settings" : nullptr;
}

/**
 * MCP 受控写入的 device 分节完整候选校验（与 ConfigMigrationValidation 的设备契约一致）：
 * 身份、产品、账号绑定、开发者模式与任何未知键必须与当前值完全一致；允许列表字段
 * （deviceName/location/description/enableNTP/ntpServer1/ntpServer2/timezone/syncInterval/
 * logLevel/cacheDuration）的差异部分走 validateDeviceConfigPatch 的生产边界
 * （location 按迁移契约单独校验 ≤128 字符）。网络配置与 AI Key 不属于本分节。
 */
inline const char* validateDeviceSectionCandidate(JsonObjectConst candidate, JsonObjectConst current) {
    if (candidate.isNull() || current.isNull()) return "MIGRATION_DEVICE_SCHEMA";
    JsonDocument patch;
    for (JsonPairConst entry : candidate) {
        const char* key = entry.key().c_str();
        if (entry.key().size() != std::strlen(key)) return "MIGRATION_DEVICE_SCHEMA";
        bool allowed = false;
        for (const char* name : {"deviceName", "location", "description", "enableNTP", "ntpServer1",
                                 "ntpServer2", "timezone", "syncInterval", "logLevel", "cacheDuration"}) {
            if (!std::strcmp(key, name)) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            if (!current.containsKey(key) || current[key] != entry.value()) {
                return "MIGRATION_IDENTITY_MUST_BE_PRESERVED";
            }
            continue;
        }
        if (!std::strcmp(key, "location")) {
            const char* text = entry.value().as<const char*>();
            if (!text || std::strlen(text) > 128) return "MIGRATION_DEVICE_SCHEMA";
            continue;
        }
        patch[entry.key()] = entry.value();
    }
    if (patch.overflowed()) return "MIGRATION_MEMORY_PRESSURE";
    if (patch.size() == 0) return nullptr;
    return validateDeviceConfigPatch(patch.as<JsonObjectConst>());
}
} // namespace FastBee
