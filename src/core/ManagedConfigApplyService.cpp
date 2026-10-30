#include "core/ManagedConfigApplyService.h"

#include <algorithm>
#include <new>

#ifndef UNIT_TEST
#include "core/MemoryBudget.h"
#include "core/PeripheralManager.h"
#include "core/PeriphExecManager.h"
#include "core/PeriphExecConfigValidation.h"
#include "core/RuleScriptConfigValidation.h"
#include "core/RuleScriptManager.h"
#include "core/RuleScriptResources.h"
#include "core/RuntimeFeatureManager.h"
#include "network/ProtocolSecret.h"
#include "protocols/ProtocolManager.h"
#include "systems/ConfigMigrationModbus.h"
#include "systems/ConfigMigrationValidation.h"
#include "systems/ConfigStorage.h"
#include "utils/PsramJsonDocument.h"
#include <LittleFS.h>
#endif

namespace FastBee {
namespace {

bool fail(JsonDocument& output, const char* code, String& error, const char* detail = nullptr) {
    output.clear();
    output["success"] = false;
    output["code"] = code;
    error = detail ? detail : code;
    output["error"] = error;
    return false;
}

bool maskedSecret(JsonVariantConst value) {
    const char* text = value.as<const char*>();
    if (!text) return false;
    const size_t length = std::strlen(text);
    if (length < 3) return false;
    for (size_t i = 0; i < length; ++i) if (text[i] != '*') return false;
    return true;
}

bool secretsMaskedRecursive(JsonVariantConst value, const char* const* secrets, size_t count) {
    if (value.is<JsonObjectConst>()) {
        for (JsonPairConst field : value.as<JsonObjectConst>()) {
            for (size_t i = 0; i < count; ++i) {
                if (field.key() == secrets[i] && !maskedSecret(field.value())) return false;
            }
            if (!secretsMaskedRecursive(field.value(), secrets, count)) return false;
        }
    } else if (value.is<JsonArrayConst>()) {
        for (JsonVariantConst child : value.as<JsonArrayConst>()) {
            if (!secretsMaskedRecursive(child, secrets, count)) return false;
        }
    }
    return true;
}

void stripConfiguredRecursive(JsonVariant value, const char* const* secrets, size_t count) {
    if (value.is<JsonObject>()) {
        std::vector<String> remove;
        for (JsonPair field : value.as<JsonObject>()) {
            for (size_t i = 0; i < count; ++i) {
                if (field.key() == (String(secrets[i]) + "Configured").c_str()) {
                    remove.push_back(field.key().c_str());
                }
            }
            stripConfiguredRecursive(field.value(), secrets, count);
        }
        for (const auto& key : remove) value.remove(key.c_str());
    } else if (value.is<JsonArray>()) {
        for (JsonVariant child : value.as<JsonArray>()) stripConfiguredRecursive(child, secrets, count);
    }
}

int sectionRank(const char* section) {
    if (!std::strcmp(section, "features")) return 0;
    if (!std::strcmp(section, "peripherals")) return 1;
    if (!std::strcmp(section, "protocol") || !std::strcmp(section, "modbus")) return 2;
    if (!std::strcmp(section, "ruleScripts")) return 3;
    if (!std::strcmp(section, "periphExec")) return 4;
    return 5;
}

} // namespace

#ifndef UNIT_TEST
namespace {

void redactSecrets(JsonVariant value, const std::vector<const char*>& secrets) {
    if (value.is<JsonObject>()) {
        std::vector<std::pair<String, bool>> configured;
        for (JsonPair field : value.as<JsonObject>()) {
            const String key = field.key().c_str();
            for (const char* secret : secrets) {
                if (key != secret) continue;
                const bool present = std::strlen(field.value() | "") > 0;
                field.value().set(present ? FastBee::ProtocolSecret::MASK : "");
                configured.emplace_back(key + "Configured", present);
            }
            redactSecrets(field.value(), secrets);
        }
        for (const auto& item : configured) value[item.first] = item.second;
    } else if (value.is<JsonArray>()) {
        for (JsonVariant child : value.as<JsonArray>()) redactSecrets(child, secrets);
    }
}

void restoreMaskedSecrets(JsonVariant candidate, JsonVariantConst current,
                          const std::vector<const char*>& secrets) {
    if (candidate.is<JsonObject>()) {
        for (JsonPair field : candidate.as<JsonObject>()) {
            for (const char* secret : secrets) {
                if (field.key() == secret && maskedSecret(field.value())) {
                    field.value().set(current[field.key()]);
                }
            }
            restoreMaskedSecrets(field.value(), current[field.key()], secrets);
        }
    } else if (candidate.is<JsonArray>()) {
        size_t index = 0;
        for (JsonVariant child : candidate.as<JsonArray>()) {
            restoreMaskedSecrets(child, current[index++], secrets);
        }
    }
}

bool jsonSubset(JsonVariantConst expected, JsonVariantConst actual) {
    if (expected.is<JsonObjectConst>()) {
        if (!actual.is<JsonObjectConst>()) return false;
        for (JsonPairConst field : expected.as<JsonObjectConst>()) {
            if (!jsonSubset(field.value(), actual[field.key()])) return false;
        }
        return true;
    }
    if (expected.is<JsonArrayConst>()) {
        if (!actual.is<JsonArrayConst>() || expected.size() != actual.size()) return false;
        size_t index = 0;
        for (JsonVariantConst child : expected.as<JsonArrayConst>()) {
            if (!jsonSubset(child, actual[index++])) return false;
        }
        return true;
    }
    return expected == actual;
}

class StorageSectionAdapter : public ManagedConfigSectionAdapter {
public:
    StorageSectionAdapter(const char* name, const char* path, bool readOnly,
                          std::vector<const char*> dependencies,
                          std::vector<const char*> secrets)
        : name_(name), path_(path), readOnly_(readOnly),
          dependencies_(std::move(dependencies)), secrets_(std::move(secrets)) {}

    const char* section() const override { return name_; }
    uint32_t schemaVersion() const override { return 1; }
    bool readOnly() const override { return readOnly_; }

    bool readRedacted(JsonDocument& output, String& error) override {
        if (!readRaw(output, error)) return false;
        redactSecrets(output.as<JsonVariant>(), secrets_);
        return !output.overflowed();
    }

    bool validate(JsonVariantConst candidate, String& error) override {
        if (!candidate.is<JsonObjectConst>()) {
            error = "CONFIG_OBJECT_REQUIRED";
            return false;
        }
        for (JsonPairConst field : candidate.as<JsonObjectConst>()) {
            if (String(field.key().c_str()).endsWith("Configured")) {
                error = "CONFIG_DERIVED_FIELD";
                return false;
            }
        }
        return validateSection(candidate, error);
    }

    bool preflight(JsonVariantConst candidate, ManagedPreflightResult& result,
                   String& error) override {
        result = ManagedPreflightResult();
        return preflightSection(candidate, result, error);
    }

    ManagedAdapterApplyResult apply(JsonVariantConst candidate, String& error) override {
        return applySection(candidate, error);
    }

    bool readback(JsonVariantConst expected, String& error) override {
        JsonDocument actual;
        JsonDocument wanted;
        if (!readRedacted(actual, error) || !wanted.set(expected)) {
            error = "READBACK_FAILED";
            return false;
        }
        redactSecrets(wanted.as<JsonVariant>(), secrets_);
        if (!jsonSubset(wanted, actual)) {
            error = "READBACK_MISMATCH";
            return false;
        }
        return true;
    }

    const char* const* dependencies(size_t& count) const override {
        count = dependencies_.size();
        return dependencies_.empty() ? nullptr : dependencies_.data();
    }

    const char* const* secretFields(size_t& count) const override {
        count = secrets_.size();
        return secrets_.empty() ? nullptr : secrets_.data();
    }

protected:
    virtual bool validateSection(JsonVariantConst, String&) { return true; }
    virtual bool preflightSection(JsonVariantConst, ManagedPreflightResult&, String&) {
        return true;
    }

    virtual bool readRaw(JsonDocument& output, String& error) {
        output.clear();
        if (!path_.isEmpty() &&
            (LittleFS.exists(path_.c_str()) || LittleFS.exists((path_ + ".bak").c_str()))) {
            if (!ConfigStorage::getInstance().loadJSONConfig(path_, output) ||
                !output.is<JsonObject>()) {
                error = "CONFIG_READ_FAILED";
                return false;
            }
        } else {
            setDefault(output);
        }
        return !output.overflowed();
    }

    virtual ManagedAdapterApplyResult applySection(JsonVariantConst candidate, String& error) {
        JsonDocument document;
        if (!document.set(candidate) ||
            !ConfigStorage::getInstance().saveImportedJSONConfig(path_, document)) {
            return {ManagedAdapterApplyStatus::SaveFailed, false, "SAVE_FAILED"};
        }
        return {ManagedAdapterApplyStatus::Persisted, false, ""};
    }

    virtual void setDefault(JsonDocument& output) { output.to<JsonObject>(); }

    const char* name_;
    String path_;
    bool readOnly_;
    std::vector<const char*> dependencies_;
    std::vector<const char*> secrets_;
};

class FeatureAdapter final : public StorageSectionAdapter {
public:
    FeatureAdapter() : StorageSectionAdapter("features", "/config/features.json", false,
                                             {}, {}) {}
protected:
    void setDefault(JsonDocument& output) override {
        output["version"] = 1;
        output["features"].to<JsonObject>();
    }

    bool validateSection(JsonVariantConst candidate, String& error) override {
        if (candidate["version"] != 1 || !candidate["features"].is<JsonObjectConst>()) {
            error = "MIGRATION_FEATURE_SCHEMA";
            return false;
        }
        auto& manager = RuntimeFeatureManager::instance();
        for (JsonPairConst row : candidate["features"].as<JsonObjectConst>()) {
            const RuntimeFeatureManager::FeatureMeta* metadata = nullptr;
            for (size_t i = 0; i < manager.featureCount(); ++i) {
                if (!std::strcmp(manager.getFeatureAt(i)->key, row.key().c_str())) {
                    metadata = manager.getFeatureAt(i);
                    break;
                }
            }
            if (!metadata || row.value().size() != 1 || !row.value()["enabled"].is<bool>()) {
                error = "MIGRATION_FEATURE_UNKNOWN";
                return false;
            }
            if (row.value()["enabled"].as<bool>() && !metadata->compiled) {
                error = "MIGRATION_FEATURE_UNAVAILABLE";
                return false;
            }
        }
        return true;
    }

    bool preflightSection(JsonVariantConst candidate, ManagedPreflightResult& result,
                          String& error) override {
        JsonDocument requested;
        if (!requested.set(candidate)) {
            error = "CONFIG_MEMORY_PRESSURE";
            result.allowed = false;
            return false;
        }
        auto& manager = RuntimeFeatureManager::instance();
        uint32_t flash = 0;
        uint32_t ram = 0;
        manager.estimateFromJson(requested, flash, ram);
        const uint32_t current = manager.estimateRamB();
        const uint32_t increase = ram > current ? ram - current : 0;
        if (FastBee::MemoryBudget::dram8Free() <
            increase + FastBee::MemoryBudget::GUARD8_SEVERE_DRAM_FREE) {
            error = "CONFIG_MEMORY_PRESSURE";
            result.allowed = false;
            result.error = error;
            return false;
        }
        result.pendingRestart = true;
        return true;
    }

    ManagedAdapterApplyResult applySection(JsonVariantConst candidate, String& error) override {
        JsonDocument document;
        if (!document.set(candidate)) return {ManagedAdapterApplyStatus::SaveFailed, true, "SAVE_FAILED"};
        auto& manager = RuntimeFeatureManager::instance();
        RuntimeFeatureManager::UserConfigSnapshot previous;
        const auto saved = manager.updateAndSave(document, &previous);
        if (saved != RuntimeFeatureManager::ConfigUpdateResult::SAVED) {
            error = saved == RuntimeFeatureManager::ConfigUpdateResult::NO_VALID_ENTRIES
                ? "INVALID_ARGUMENT" : "SAVE_FAILED";
            return {ManagedAdapterApplyStatus::SaveFailed, true, error};
        }
        return {ManagedAdapterApplyStatus::PendingRestart, true, ""};
    }
};

class PeripheralAdapter final : public StorageSectionAdapter {
public:
    PeripheralAdapter() : StorageSectionAdapter("peripherals", PERIPHERAL_CONFIG_FILE, false,
                                                 {"features"}, {}) {}
protected:
    void setDefault(JsonDocument& output) override { output["peripherals"].to<JsonArray>(); }

    bool validateSection(JsonVariantConst candidate, String& error) override {
        if (candidate.size() != 1 || !candidate["peripherals"].is<JsonArrayConst>() ||
            measureJson(candidate) > 24 * 1024) {
            error = "CONFIG_IMPORT_SCHEMA_OR_SIZE";
            return false;
        }
        return true;
    }

    bool preflightSection(JsonVariantConst candidate, ManagedPreflightResult& result,
                          String& error) override {
        JsonDocument document;
        std::vector<PeripheralConfig> graph;
        std::unique_ptr<PreflightResult> review(new (std::nothrow) PreflightResult);
        if (!document.set(candidate) || !review) {
            error = "CONFIG_MEMORY_PRESSURE";
            result.allowed = false;
            return false;
        }
        PreflightContext context;
        auto& manager = PeripheralManager::getInstance();
        if (!manager.prepareConfigurationImport(document, graph, context, *review, error)) {
            result.allowed = false;
            result.error = error;
            return false;
        }
        reviewVersion_ = manager.getConfigurationReviewVersion();
        result.pendingRestart = true;
        return true;
    }

    ManagedAdapterApplyResult applySection(JsonVariantConst candidate, String& error) override {
        JsonDocument document;
        if (!document.set(candidate) || !reviewVersion_ ||
            !PeripheralManager::getInstance().commitConfigurationImport(document, reviewVersion_, error)) {
            if (error.isEmpty()) error = "SAVE_FAILED";
            return {ManagedAdapterApplyStatus::SaveFailed, true, error};
        }
        return {ManagedAdapterApplyStatus::PendingRestart, true, ""};
    }

private:
    uint64_t reviewVersion_ = 0;
};

class ProtocolAdapter final : public StorageSectionAdapter {
public:
    ProtocolAdapter() : StorageSectionAdapter("protocol", FileSystem::PROTOCOL_CONFIG_FILE, false,
                                               {"peripherals"},
                                               {"password", "mqttSecret", "authCode", "apiKey", "authHeader"}) {}
    void setManager(ProtocolManager* manager) { manager_ = manager; }

    bool writeSecret(const String& secretRef, const String& value, String& error) override {
        const char* field = nullptr;
        if (secretRef == "protocol.mqtt.password") field = "password";
        else if (secretRef == "protocol.mqtt.mqttSecret") field = "mqttSecret";
        else if (secretRef == "protocol.mqtt.authCode") field = "authCode";
        else {
            error = "CAPABILITY_UNSUPPORTED";
            return false;
        }
        JsonDocument document;
        if (!readRaw(document, error)) return false;
        document["mqtt"][field] = value;
        if (!ConfigStorage::getInstance().saveProtocolConfig(document)) {
            error = "SAVE_FAILED";
            return false;
        }
        if (manager_) manager_->requestMqttRestartAsync();
        return true;
    }

protected:
    bool validateSection(JsonVariantConst candidate, String& error) override {
        for (JsonPairConst row : candidate.as<JsonObjectConst>()) {
            const String key = row.key().c_str();
            if (key != "version" && key != "mqtt" && key != "modbusRtu" &&
                key != "coap" && key != "tcp" && key != "http" && key != "remote") {
                error = "CONFIG_UNKNOWN_FIELD";
                return false;
            }
        }
        static const char* const mqttFields[] = {
            "enabled","scheme","server","port","clientId","username","password","topicPrefix",
            "otaBaseUrl","subscribeTopic","keepAlive","autoReconnect","authType","deviceNum",
            "productId","userId","mqttSecret","authCode","ntpServer","willTopic","willPayload",
            "willQos","willRetain","longitude","latitude","iccid","cardPlatformId","summary",
            "publishTopics","subscribeTopics","disabled_by_memory","disable_mqtt_by_memory",
            "downgraded_by_memory","downgraded_original_scheme","downgraded_original_port",
        };
        for (JsonPairConst row : candidate["mqtt"].as<JsonObjectConst>()) {
            bool known = false;
            for (const char* field : mqttFields) known = known || row.key() == field;
            if (!known) {
                error = "CONFIG_UNKNOWN_FIELD";
                return false;
            }
        }
        if (!candidate["modbusRtu"].isNull()) {
            ConfigMigration::ModbusReviewGraph graph;
            if (!ConfigMigration::readModbusReviewGraph(candidate["modbusRtu"], graph)) {
                error = "MIGRATION_MODBUS_BASELINE_SCHEMA";
                return false;
            }
        }
        return true;
    }

    bool preflightSection(JsonVariantConst candidate, ManagedPreflightResult& result,
                          String& error) override {
        const auto rtu = candidate["modbusRtu"].as<JsonObjectConst>();
        if (!(rtu["enabled"] | false) || String(rtu["mode"] | "slave") != "master") return true;
        auto& features = RuntimeFeatureManager::instance();
        const RuntimeFeatureManager::FeatureMeta* modbus = nullptr;
        for (size_t i = 0; i < features.featureCount(); ++i) {
            if (!std::strcmp(features.getFeatureAt(i)->key, "modbus")) {
                modbus = features.getFeatureAt(i);
                break;
            }
        }
        const String peripheralId = rtu["peripheralId"] | "";
        const PeripheralConfig* uart = peripheralId.isEmpty() ? nullptr
            : PeripheralManager::getInstance().getPeripheral(peripheralId);
        if (!modbus || !modbus->compiled || !features.isUserEnabled("modbus") ||
            !uart || uart->type != PeripheralType::UART || !uart->enabled) {
            error = "MIGRATION_PROTOCOL_UART_MISSING";
            result.allowed = false;
            result.error = error;
            return false;
        }
        return true;
    }

    ManagedAdapterApplyResult applySection(JsonVariantConst candidate, String& error) override {
        JsonDocument document;
        JsonDocument current;
        if (!document.set(candidate) || !readRaw(current, error)) {
            error = error.isEmpty() ? String("SAVE_FAILED") : error;
            return {ManagedAdapterApplyStatus::SaveFailed, false, error};
        }
        restoreMaskedSecrets(document.as<JsonVariant>(), current, secrets_);
        if (document.overflowed() ||
            !ConfigStorage::getInstance().saveProtocolConfig(document)) {
            error = "SAVE_FAILED";
            return {ManagedAdapterApplyStatus::SaveFailed, false, error};
        }
        if (manager_) {
            manager_->requestMqttRestartAsync();
#if FASTBEE_ENABLE_MODBUS
            if (document["modbusRtu"]["enabled"] | false) manager_->restartModbusDeferred();
            else manager_->stopModbus();
#endif
        }
        return {ManagedAdapterApplyStatus::Persisted, false, ""};
    }

private:
    ProtocolManager* manager_ = nullptr;
};

class RuleScriptAdapter final : public StorageSectionAdapter {
public:
    RuleScriptAdapter() : StorageSectionAdapter("ruleScripts", RULE_SCRIPT_CONFIG_FILE, false,
                                                 {"protocol"}, {}) {}
protected:
    void setDefault(JsonDocument& output) override { output["rules"].to<JsonArray>(); }

    bool validateSection(JsonVariantConst candidate, String& error) override {
        if (measureJson(candidate) > RuleScriptImportMaxBytes) {
            error = "CONFIG_IMPORT_SCHEMA_OR_SIZE";
            return false;
        }
        RuleScriptMap<String, RuleScript> parsed;
        return parseRuleScriptImport<RuleScript, String>(candidate, parsed, error);
    }

    bool preflightSection(JsonVariantConst candidate, ManagedPreflightResult& result,
                          String& error) override {
        if (!ruleScriptCandidateMemory<RuleScriptMap<String, RuleScript>>(candidate, true)) {
            error = "MIGRATION_RULE_SCRIPT_MEMORY";
            result.allowed = false;
            result.error = error;
            return false;
        }
        return true;
    }

    ManagedAdapterApplyResult applySection(JsonVariantConst candidate, String& error) override {
        JsonDocument document;
        if (!document.set(candidate) ||
            !RuleScriptManager::getInstance().importConfiguration(document, error)) {
            if (error.isEmpty()) error = "SAVE_FAILED";
            return {ManagedAdapterApplyStatus::SaveFailed, false, error};
        }
        return {ManagedAdapterApplyStatus::Persisted, false, ""};
    }
};

class PeriphExecAdapter final : public StorageSectionAdapter {
public:
    PeriphExecAdapter() : StorageSectionAdapter("periphExec", PERIPH_EXEC_CONFIG_FILE, false,
                                                 {"ruleScripts"}, {}) {}
protected:
    void setDefault(JsonDocument& output) override { output["rules"].to<JsonArray>(); }

    bool validateSection(JsonVariantConst candidate, String& error) override {
        if (!candidate["rules"].is<JsonArrayConst>() ||
            measureJson(candidate) > PeriphExecConfig::kMaxBytes) {
            error = "CONFIG_IMPORT_SCHEMA_OR_SIZE";
            return false;
        }
        return true;
    }

    ManagedAdapterApplyResult applySection(JsonVariantConst candidate, String& error) override {
        const String staged = "/config/periph_exec.mcp.staged.json";
        JsonDocument document;
        auto& storage = ConfigStorage::getInstance();
        if (!document.set(candidate) || !storage.saveImportedJSONConfig(staged, document)) {
            error = "SAVE_FAILED";
            return {ManagedAdapterApplyStatus::SaveFailed, true, error};
        }
        auto& manager = PeriphExecManager::getInstance();
        if (!manager.importConfigurationFile(staged.c_str(), error)) {
            storage.deleteJSONConfigGenerations(staged);
            if (error.isEmpty()) error = "SAVE_FAILED";
            return {ManagedAdapterApplyStatus::SaveFailed, true, error};
        }
        storage.deleteJSONConfigGenerations(staged);
        return {ManagedAdapterApplyStatus::PendingRestart, true, ""};
    }
};

class NetworkAdapter final : public StorageSectionAdapter {
public:
    NetworkAdapter() : StorageSectionAdapter("network", FileSystem::NETWORK_CONFIG_FILE, true, {},
                       {"password", "staPassword", "wifiPassword", "apPassword", "enterprisePassword", "pin"}) {}
};

class AiKeyAdapter final : public StorageSectionAdapter {
public:
    AiKeyAdapter() : StorageSectionAdapter("aiKey", "", true, {}, {"apiKey"}) {}
protected:
    void setDefault(JsonDocument& output) override {
        output["available"] = false;
        output["secretConfigured"] = false;
    }
};

} // namespace

struct ManagedConfigApplyService::DefaultAdapters {
    FeatureAdapter features;
    PeripheralAdapter peripherals;
    ProtocolAdapter protocol;
    RuleScriptAdapter ruleScripts;
    PeriphExecAdapter periphExec;
    NetworkAdapter network;
    AiKeyAdapter aiKey;
};

void ManagedConfigApplyService::registerDefaultAdapters() {
    registerAdapter(&defaults_->features);
    registerAdapter(&defaults_->peripherals);
    registerAdapter(&defaults_->protocol);
    registerAdapter(&defaults_->ruleScripts);
    registerAdapter(&defaults_->periphExec);
    registerAdapter(&defaults_->network);
    registerAdapter(&defaults_->aiKey);
}
#else
struct ManagedConfigApplyService::DefaultAdapters {};
#endif

ManagedConfigApplyService::ManagedConfigApplyService(bool registerDefaults) {
    adapters_.reserve(kMaxAdapters);
#ifndef UNIT_TEST
    if (registerDefaults) {
        defaults_ = new (std::nothrow) DefaultAdapters();
        if (defaults_) registerDefaultAdapters();
    }
#else
    (void)registerDefaults;
#endif
}

ManagedConfigApplyService::~ManagedConfigApplyService() {
    clearDefaults();
}

void ManagedConfigApplyService::clearDefaults() {
#ifndef UNIT_TEST
    delete defaults_;
    defaults_ = nullptr;
#endif
}

bool ManagedConfigApplyService::registerAdapter(ManagedConfigSectionAdapter* adapterValue) {
    if (!adapterValue || adapters_.size() >= kMaxAdapters || adapter(adapterValue->section())) {
        return false;
    }
    adapters_.push_back(adapterValue);
    return adapters_.back() == adapterValue;
}

ManagedConfigSectionAdapter* ManagedConfigApplyService::adapter(const String& section) const {
    for (auto* candidate : adapters_) {
        if (section == candidate->section()) return candidate;
    }
    return nullptr;
}

bool ManagedConfigApplyService::readValue(ManagedConfigSectionAdapter& adapterValue,
                                          JsonDocument& output, String& error) const {
    output.clear();
    if (!adapterValue.readRedacted(output, error) || output.overflowed() ||
        !output.is<JsonObject>()) {
        output.clear();
        if (error.isEmpty()) error = "CONFIG_READ_FAILED";
        return false;
    }
    return true;
}

String ManagedConfigApplyService::currentSectionRevision(
    ManagedConfigSectionAdapter& adapterValue, String& error) const {
    JsonDocument value;
    if (!readValue(adapterValue, value, error)) return "";
    return ManagedConfigRevision::sectionHash(value);
}

String ManagedConfigApplyService::currentRevision(String* error) {
    std::vector<std::pair<String, String>> hashes;
    hashes.reserve(adapters_.size());
    for (auto* adapterValue : adapters_) {
        String readError;
        const String hash = currentSectionRevision(*adapterValue, readError);
        if (hash.length() != 64) {
            if (error) *error = readError.isEmpty() ? String("CONFIG_READ_FAILED") : readError;
            return "";
        }
        hashes.emplace_back(adapterValue->section(), hash);
    }
    return ManagedConfigRevision::globalRevision(hashes);
}

bool ManagedConfigApplyService::capabilities(JsonDocument& output, String& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    output.clear();
    const String revision = currentRevision(&error);
    if (revision.isEmpty()) return fail(output, "DEVICE_CHANNEL_UNAVAILABLE", error,
                                        error.isEmpty() ? "CONFIG_READ_FAILED" : error.c_str());
    output["success"] = true;
    output["schemaVersion"] = 1;
    output["compiled"] = true;
    output["runtimeAvailable"] = true;
    output["revision"] = revision;
    JsonObject sections = output["sections"].to<JsonObject>();
    for (auto* adapterValue : adapters_) {
        String readError;
        const String hash = currentSectionRevision(*adapterValue, readError);
        if (hash.length() != 64) return fail(output, "DEVICE_CHANNEL_UNAVAILABLE", error, readError.c_str());
        JsonObject row = sections[adapterValue->section()].to<JsonObject>();
        row["schemaVersion"] = adapterValue->schemaVersion();
        row["readOnly"] = adapterValue->readOnly();
        row["hash"] = hash;
        row["revision"] = hash;
        size_t count = 0;
        const char* const* dependencies = adapterValue->dependencies(count);
        JsonArray dependencyRows = row["dependencies"].to<JsonArray>();
        for (size_t i = 0; i < count; ++i) dependencyRows.add(dependencies[i]);
        const char* const* secrets = adapterValue->secretFields(count);
        JsonArray secretRows = row["secretFields"].to<JsonArray>();
        for (size_t i = 0; i < count; ++i) secretRows.add(secrets[i]);
    }
    return !output.overflowed() || fail(output, "QUOTA_EXCEEDED", error, "CAPACITY");
}

bool ManagedConfigApplyService::snapshot(const std::vector<String>& selected,
                                         JsonDocument& output, String& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    output.clear();
    const String revision = currentRevision(&error);
    if (revision.isEmpty()) return fail(output, "DEVICE_CHANNEL_UNAVAILABLE", error,
                                        error.isEmpty() ? "CONFIG_READ_FAILED" : error.c_str());
    output["success"] = true;
    output["revision"] = revision;
    JsonObject sections = output["sections"].to<JsonObject>();
    for (auto* adapterValue : adapters_) {
        if (!selected.empty() && std::find(selected.begin(), selected.end(),
                                          String(adapterValue->section())) == selected.end()) continue;
        JsonDocument value;
        if (!readValue(*adapterValue, value, error)) {
            return fail(output, "DEVICE_CHANNEL_UNAVAILABLE", error, error.c_str());
        }
        JsonObject row = sections[adapterValue->section()].to<JsonObject>();
        row["schemaVersion"] = adapterValue->schemaVersion();
        row["readOnly"] = adapterValue->readOnly();
        row["value"].set(value);
        row["hash"] = ManagedConfigRevision::sectionHash(value);
        row["revision"] = row["hash"];
        size_t count = 0;
        const char* const* dependencies = adapterValue->dependencies(count);
        JsonArray dependencyRows = row["dependencies"].to<JsonArray>();
        for (size_t i = 0; i < count; ++i) dependencyRows.add(dependencies[i]);
        const char* const* secrets = adapterValue->secretFields(count);
        JsonArray secretRows = row["secretFields"].to<JsonArray>();
        for (size_t i = 0; i < count; ++i) secretRows.add(secrets[i]);
    }
    for (const auto& requested : selected) {
        if (sections[requested].isNull()) return fail(output, "INVALID_ARGUMENT", error,
                                                       "Unknown configuration section");
    }
    return !output.overflowed() || fail(output, "QUOTA_EXCEEDED", error, "CAPACITY");
}

bool ManagedConfigApplyService::buildCandidate(Candidate& candidate, JsonVariantConst patch,
                                               JsonDocument& output, String& error) {
    if (!readValue(*candidate.adapter, candidate.value, error)) {
        return fail(output, "DEVICE_CHANNEL_UNAVAILABLE", error, error.c_str());
    }
    size_t secretCount = 0;
    const char* const* secrets = candidate.adapter->secretFields(secretCount);
    stripConfiguredRecursive(candidate.value.as<JsonVariant>(), secrets, secretCount);
    if (!secretsMaskedRecursive(patch, secrets, secretCount)) {
        return fail(output, "SECRET_REQUIRED", error,
                    "Secret values must use the one-time secret channel");
    }
    for (JsonPairConst field : patch.as<JsonObjectConst>()) {
        candidate.value[field.key()] = field.value();
    }
    if (candidate.value.overflowed()) return fail(output, "QUOTA_EXCEEDED", error, "CAPACITY");
    return true;
}

bool ManagedConfigApplyService::checkCandidate(Candidate& candidate, JsonDocument& output,
                                               String& error) {
    if (!candidate.adapter->validate(candidate.value, error)) {
        return fail(output, "INVALID_ARGUMENT", error, error.isEmpty() ? "INVALID_ARGUMENT" : error.c_str());
    }
    ManagedPreflightResult result;
    if (!candidate.adapter->preflight(candidate.value, result, error) || !result.allowed) {
        output.clear();
        output["success"] = false;
        output["allowed"] = false;
        output["code"] = "PREFLIGHT_FAILED";
        output["error"] = result.error.isEmpty() ? error : result.error;
        error = output["error"].as<const char*>();
        return false;
    }
    return true;
}

bool ManagedConfigApplyService::parseCandidates(JsonVariantConst operations,
                                                std::vector<Candidate>& candidates,
                                                JsonDocument& output, String& error) {
    if (!operations.is<JsonArrayConst>() || operations.size() == 0) {
        return fail(output, "INVALID_ARGUMENT", error, "operations must be a non-empty array");
    }
    if (operations.size() > kMaxOperations) {
        return fail(output, "QUOTA_EXCEEDED", error, "Too many configuration operations");
    }
    candidates.clear();
    candidates.reserve(operations.size());
    for (JsonObjectConst row : operations.as<JsonArrayConst>()) {
        for (JsonPairConst field : row) {
            const String key = field.key().c_str();
            if (key != "section" && key != "schemaVersion" && key != "patch" &&
                key != "baseRevision" && key != "delete") {
                return fail(output, "INVALID_ARGUMENT", error, "Unknown operation field");
            }
        }
        const char* section = row["section"].as<const char*>();
        if (!section || row["section"].as<JsonString>().size() != std::strlen(section)) {
            return fail(output, "INVALID_ARGUMENT", error, "Invalid section");
        }
        ManagedConfigSectionAdapter* adapterValue = adapter(section);
        if (!adapterValue) return fail(output, "INVALID_ARGUMENT", error, "Unknown section");
        if (adapterValue->readOnly()) {
            return fail(output, "CAPABILITY_UNSUPPORTED", error, "Section is read-only");
        }
        if (!row["schemaVersion"].is<uint32_t>() ||
            row["schemaVersion"].as<uint32_t>() != adapterValue->schemaVersion()) {
            return fail(output, "SCHEMA_VERSION_UNSUPPORTED", error, "Unsupported schema version");
        }
        if (!row["patch"].is<JsonObjectConst>()) {
            return fail(output, "INVALID_ARGUMENT", error, "patch must be an object");
        }
        for (const auto& existing : candidates) {
            if (!std::strcmp(existing.adapter->section(), section)) {
                return fail(output, "INVALID_ARGUMENT", error, "Duplicate section");
            }
        }
        Candidate candidate;
        candidate.adapter = adapterValue;
        candidate.sectionBaseRevision = row["baseRevision"].as<const char*>() ?: "";
        candidate.deletion = row["delete"] | false;
        if (!buildCandidate(candidate, row["patch"], output, error)) {
            clearCandidates(candidates);
            return false;
        }
        candidates.push_back(std::move(candidate));
        if (candidates.back().adapter != adapterValue) {
            return fail(output, "QUOTA_EXCEEDED", error, "CAPACITY");
        }
    }
    orderCandidates(candidates, false);
    for (auto& candidate : candidates) {
        if (candidate.deletion) {
            orderCandidates(candidates, true);
            break;
        }
    }
    return true;
}

void ManagedConfigApplyService::orderCandidates(std::vector<Candidate>& candidates,
                                                bool reverse) const {
    std::stable_sort(candidates.begin(), candidates.end(), [reverse](const Candidate& left,
                                                                     const Candidate& right) {
        const int leftRank = sectionRank(left.adapter->section());
        const int rightRank = sectionRank(right.adapter->section());
        return reverse ? leftRank > rightRank : leftRank < rightRank;
    });
}

bool ManagedConfigApplyService::combinedPreflight(std::vector<Candidate>& candidates,
                                                  JsonDocument& output, String& error) {
    for (auto& candidate : candidates) {
        if (!checkCandidate(candidate, output, error)) return false;
    }
#ifndef UNIT_TEST
    ConfigMigration::Selection selected[4]{};
    size_t selectedCount = 0;
    for (const auto& candidate : candidates) {
        const char* name = candidate.adapter->section();
        int file = -1;
        if (!std::strcmp(name, "features")) file = 1;
        else if (!std::strcmp(name, "peripherals")) file = 2;
        else if (!std::strcmp(name, "periphExec")) file = 3;
        else if (!std::strcmp(name, "ruleScripts")) file = 4;
        if (file < 0) continue;
        const size_t bytes = measureJson(candidate.value);
        if (!bytes || bytes > ConfigMigration::MaxFileBytes) {
            return fail(output, "QUOTA_EXCEEDED", error, "MIGRATION_CONFIG_SIZE");
        }
        selected[selectedCount++] = {static_cast<uint32_t>(file), static_cast<uint32_t>(bytes)};
    }
    if (selectedCount) {
        ConfigMigration::Guard guard;
        if (!guard) return fail(output, "OPERATION_IN_PROGRESS", error, "MIGRATION_SLOT_OCCUPIED");
        ConfigMigration::Storage storage;
        const String session = ManagedConfigRevision::sha256(
            std::string(currentRevision().c_str()) + std::to_string(nextOperation_)).substring(0, 32);
        if (session.length() != 32 || !storage.begin(session.c_str(), selected, selectedCount, error)) {
            return fail(output, "OPERATION_IN_PROGRESS", error,
                        error.isEmpty() ? "MIGRATION_SLOT_OCCUPIED" : error.c_str());
        }
        bool staged = true;
        size_t index = 0;
        for (const auto& candidate : candidates) {
            const char* name = candidate.adapter->section();
            if (std::strcmp(name, "features") && std::strcmp(name, "peripherals") &&
                std::strcmp(name, "periphExec") && std::strcmp(name, "ruleScripts")) continue;
            String content;
            const size_t expected = measureJson(candidate.value);
            content.reserve(expected);
            if (serializeJson(candidate.value, content) != expected ||
                !storage.append(session.c_str(), selected[index].fileId, 0,
                                reinterpret_cast<const uint8_t*>(content.c_str()),
                                content.length(), error)) {
                staged = false;
                break;
            }
            ++index;
        }
        ConfigMigration::Manifest manifest;
        JsonDocument report;
        bool valid = staged && storage.inspect(session.c_str(), manifest, error);
        if (valid) {
            ConfigMigration::ReviewContext review;
            review.report = &report;
            valid = ConfigMigration::validateGraph(manifest, &review, error);
        }
        String discardError;
        const bool discarded = storage.discard(session.c_str(), discardError);
        if (!valid || !discarded) {
            output.clear();
            output["success"] = false;
            output["allowed"] = false;
            output["code"] = discarded ? "PREFLIGHT_FAILED" : "DEVICE_CHANNEL_UNAVAILABLE";
            output["error"] = discarded ? error : discardError;
            if (!report.isNull()) output["report"].set(report);
            error = output["error"].as<const char*>();
            return false;
        }
        if (!report.isNull()) output["report"].set(report);
    }
#endif
    output["allowed"] = true;
    output["operations"] = candidates.size();
    output["activation"] = "managed";
    output["persisted"] = false;
    return true;
}

bool ManagedConfigApplyService::preflight(JsonVariantConst request, JsonDocument& output,
                                          String& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    output.clear();
    std::vector<Candidate> candidates;
    if (!parseCandidates(request["operations"], candidates, output, error)) return false;
    if (!request["baseRevision"].isNull()) {
        const String expected = request["baseRevision"].as<const char*>() ?: "";
        String revisionError;
        if (expected != currentRevision(&revisionError)) {
            clearCandidates(candidates);
            return fail(output, "CONFIG_REVISION_CONFLICT", error,
                        revisionError.isEmpty() ? "Configuration changed" : revisionError.c_str());
        }
    }
    for (const auto& candidate : candidates) {
        if (!checkRevision(candidate, error)) {
            clearCandidates(candidates);
            return fail(output, "CONFIG_REVISION_CONFLICT", error, error.c_str());
        }
    }
    const bool allowed = combinedPreflight(candidates, output, error);
    if (allowed) output["success"] = true;
    clearCandidates(candidates);
    return allowed;
}

bool ManagedConfigApplyService::checkRevision(const Candidate& candidate, String& error) const {
    if (candidate.sectionBaseRevision.isEmpty()) return true;
    const String current = currentSectionRevision(*candidate.adapter, error);
    return current == candidate.sectionBaseRevision;
}

ManagedConfigApplyService::OperationRecord* ManagedConfigApplyService::findIdempotency(
    const String& key) {
    for (auto& item : operations_) if (item.idempotencyKey == key) return &item;
    return nullptr;
}

ManagedConfigApplyService::OperationRecord* ManagedConfigApplyService::findOperation(
    const String& id) {
    for (auto& item : operations_) if (item.id == id) return &item;
    return nullptr;
}

String ManagedConfigApplyService::newOperationId(const String& key, const String& requestHash) {
    std::string identity = key.c_str();
    identity.push_back('\n');
    identity += requestHash.c_str();
    identity.push_back('\n');
    identity += std::to_string(nextOperation_++);
    const String hash = ManagedConfigRevision::sha256(identity);
    return hash.isEmpty() ? String() : String("mcp-") + hash.substring(0, 24);
}

void ManagedConfigApplyService::serializeOperation(const OperationRecord& record,
                                                   JsonDocument& output) const {
    output.clear();
    output["success"] = record.state != "FAILED" && record.state != "UNKNOWN";
    output["operationId"] = record.id;
    output["state"] = record.state;
    output["manualRecoveryRequired"] = record.manualRecoveryRequired;
    output["readbackRequired"] = record.readbackRequired;
    output["pendingRestart"] = record.pendingRestart;
    if (!record.error.isEmpty()) {
        output["code"] = record.error;
        output["error"] = record.error;
    }
    JsonArray steps = output["steps"].to<JsonArray>();
    for (size_t i = 0; i < record.stepCount; ++i) {
        const StepRecord& source = record.steps[i];
        JsonObject row = steps.add<JsonObject>();
        row["section"] = source.section;
        row["state"] = source.state;
        row["accepted"] = source.accepted;
        row["persisted"] = source.persisted;
        row["readback"] = source.readback;
        row["observed"] = source.observed;
        row["physicalConfirmed"] = source.physicalConfirmed;
        row["pendingRestart"] = source.pendingRestart;
        if (!source.error.isEmpty()) row["error"] = source.error;
    }
}

String ManagedConfigApplyService::apply(JsonVariantConst request, JsonDocument& output,
                                        String& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    output.clear();
    std::vector<Candidate> candidates;
    if (!parseCandidates(request["operations"], candidates, output, error)) return output["code"].as<const char*>();
    const String baseRevision = request["baseRevision"].as<const char*>() ?: "";
    const String draftId = request["draftId"].as<const char*>() ?: "";
    const String key = request["idempotencyKey"].as<const char*>() ?: "";
    const String approval = request["approvalHash"].as<const char*>() ?: "";
    if (baseRevision.isEmpty() || draftId.isEmpty() || draftId.length() > 64 ||
        key.isEmpty() || key.length() > 80 || approval.isEmpty()) {
        clearCandidates(candidates);
        fail(output, "INVALID_ARGUMENT", error, "Invalid apply envelope");
        return output["code"].as<const char*>();
    }
    std::string requestMaterial = baseRevision.c_str();
    requestMaterial.push_back('\n');
    requestMaterial += draftId.c_str();
    requestMaterial.push_back('\n');
    std::string canonicalOperations;
    if (!ManagedConfigRevision::canonicalJson(request["operations"], canonicalOperations)) {
        clearCandidates(candidates);
        fail(output, "INVALID_ARGUMENT", error, "Invalid operations");
        return output["code"].as<const char*>();
    }
    requestMaterial += canonicalOperations;
    const String requestHash = ManagedConfigRevision::sha256(requestMaterial);
    if (OperationRecord* existing = findIdempotency(key)) {
        serializeOperation(*existing, output);
        clearCandidates(candidates);
        if (existing->requestHash != requestHash) {
            fail(output, "IDEMPOTENCY_CONFLICT", error, "Idempotency key reused with another draft");
            return output["code"].as<const char*>();
        }
        error = existing->error;
        return error;
    }
    const String expectedApproval = ManagedConfigRevision::approvalHash(
        baseRevision, draftId, request["operations"]);
    if (expectedApproval.isEmpty() || approval != expectedApproval) {
        clearCandidates(candidates);
        fail(output, "CONFIRMATION_REQUIRED", error, "Approval does not match draft and revision");
        return output["code"].as<const char*>();
    }
    String revisionError;
    if (baseRevision != currentRevision(&revisionError)) {
        clearCandidates(candidates);
        fail(output, "CONFIG_REVISION_CONFLICT", error,
             revisionError.isEmpty() ? "Configuration changed" : revisionError.c_str());
        return output["code"].as<const char*>();
    }
    for (const auto& candidate : candidates) {
        if (!checkRevision(candidate, error)) {
            clearCandidates(candidates);
            fail(output, "CONFIG_REVISION_CONFLICT", error, error.c_str());
            return output["code"].as<const char*>();
        }
    }
    JsonDocument preflightResult;
    if (!combinedPreflight(candidates, preflightResult, error)) {
        const String code = preflightResult["code"].as<const char*>();
        output.clear();
        output.set(preflightResult);
        clearCandidates(candidates);
        return code;
    }
    if (operations_.size() >= kMaxOperationHistory) {
        clearCandidates(candidates);
        fail(output, "QUOTA_EXCEEDED", error, "Operation history is full");
        return output["code"].as<const char*>();
    }

    OperationRecord record;
    record.id = newOperationId(key, requestHash);
    record.idempotencyKey = key;
    record.requestHash = requestHash;
    record.state = "APPLYING";
    record.stepCount = candidates.size();
    if (record.id.isEmpty()) {
        clearCandidates(candidates);
        fail(output, "QUOTA_EXCEEDED", error, "Operation ID allocation failed");
        return output["code"].as<const char*>();
    }
    operations_.push_back(std::move(record));
    OperationRecord* stored = &operations_.back();
    String resultCode;
    for (size_t i = 0; i < candidates.size(); ++i) {
        StepRecord& step = stored->steps[i];
        step.section = candidates[i].adapter->section();
        if (!readValue(*candidates[i].adapter, step.previous, error)) {
            step.state = "UNKNOWN";
            step.error = "READBACK_REQUIRED";
            stored->state = "UNKNOWN";
            stored->readbackRequired = true;
            stored->manualRecoveryRequired = i > 0;
            stored->error = "OUTCOME_UNKNOWN";
            resultCode = stored->error;
            break;
        }
        step.state = "APPLYING";
        step.accepted = true;
        ManagedAdapterApplyResult applied = candidates[i].adapter->apply(candidates[i].value, error);
        step.pendingRestart = applied.pendingRestart;
        stored->pendingRestart = stored->pendingRestart || applied.pendingRestart;
        if (applied.status == ManagedAdapterApplyStatus::Rejected) {
            step.accepted = false;
            step.state = "FAILED";
            step.error = applied.error.isEmpty() ? String("APPLY_REJECTED") : applied.error;
            stored->state = "FAILED";
            stored->error = step.error;
            resultCode = stored->error;
            break;
        }
        if (applied.status == ManagedAdapterApplyStatus::SaveFailed) {
            step.state = "FAILED";
            step.error = applied.error.isEmpty() ? String("SAVE_FAILED") : applied.error;
            stored->state = "FAILED";
            stored->error = step.error;
            resultCode = stored->error;
            for (size_t prior = 0; prior < i; ++prior) {
                stored->manualRecoveryRequired = stored->manualRecoveryRequired || stored->steps[prior].persisted;
            }
            break;
        }
        if (applied.status == ManagedAdapterApplyStatus::Unknown) {
            step.state = "UNKNOWN";
            step.error = "OUTCOME_UNKNOWN";
            stored->readbackRequired = true;
            String readbackError;
            if (candidates[i].adapter->readback(candidates[i].value, readbackError)) {
                step.state = "SUCCEEDED";
                step.persisted = true;
                step.readback = true;
                step.error = "";
                stored->readbackRequired = false;
                continue;
            }
            stored->state = "UNKNOWN";
            stored->manualRecoveryRequired = true;
            stored->error = "OUTCOME_UNKNOWN";
            resultCode = stored->error;
            break;
        }
        step.persisted = true;
        String readbackError;
        if (!candidates[i].adapter->readback(candidates[i].value, readbackError)) {
            step.state = "UNKNOWN";
            step.error = "READBACK_MISMATCH";
            stored->state = "UNKNOWN";
            stored->readbackRequired = true;
            stored->manualRecoveryRequired = true;
            stored->error = "OUTCOME_UNKNOWN";
            resultCode = stored->error;
            break;
        }
        step.readback = true;
        step.state = "SUCCEEDED";
    }
    if (resultCode.isEmpty()) {
        stored->state = "SUCCEEDED";
        stored->error = "";
    } else if (stored->state == "UNKNOWN") {
        stored->error = "OUTCOME_UNKNOWN";
    }
    serializeOperation(*stored, output);
    clearCandidates(candidates);
    error = resultCode;
    return resultCode;
}

bool ManagedConfigApplyService::operation(const String& operationId, JsonDocument& output,
                                          String& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    OperationRecord* record = findOperation(operationId);
    if (!record) return fail(output, "OPERATION_NOT_FOUND", error, "Operation is unavailable in this boot");
    serializeOperation(*record, output);
    return true;
}

String ManagedConfigApplyService::rollback(JsonVariantConst request, JsonDocument& output,
                                           String& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    output.clear();
    const String id = request["operationId"].as<const char*>() ?: "";
    const String key = request["idempotencyKey"].as<const char*>() ?: "";
    const String approval = request["approvalHash"].as<const char*>() ?: "";
    if (id.isEmpty() || key.isEmpty() || key.length() > 80 || approval.isEmpty()) {
        fail(output, "INVALID_ARGUMENT", error, "Invalid rollback envelope");
        return output["code"].as<const char*>();
    }
    const String requestHash = ManagedConfigRevision::sha256(std::string("rollback\n") + id.c_str() + "\n" + key.c_str());
    if (OperationRecord* existing = findIdempotency(key)) {
        if (existing->requestHash != requestHash) {
            fail(output, "IDEMPOTENCY_CONFLICT", error, "Idempotency key reused");
            return output["code"].as<const char*>();
        }
        serializeOperation(*existing, output);
        error = existing->error;
        return error;
    }
    OperationRecord* original = findOperation(id);
    if (!original || original->rollback) {
        fail(output, "OPERATION_NOT_FOUND", error, "Operation is unavailable in this boot");
        return output["code"].as<const char*>();
    }
    if (original->state == "UNKNOWN" || original->state == "APPLYING" || original->state == "ROLLING_BACK") {
        serializeOperation(*original, output);
        output["code"] = "OUTCOME_UNKNOWN";
        error = "OUTCOME_UNKNOWN";
        return error;
    }
    String revisionError;
    const String current = currentRevision(&revisionError);
    const String expected = ManagedConfigRevision::rollbackApprovalHash(id, current);
    if (expected.isEmpty() || approval != expected) {
        fail(output, "CONFIRMATION_REQUIRED", error, "Rollback approval does not match current revision");
        return output["code"].as<const char*>();
    }
    if (operations_.size() >= kMaxOperationHistory) {
        fail(output, "QUOTA_EXCEEDED", error, "Operation history is full");
        return output["code"].as<const char*>();
    }
    for (size_t i = original->stepCount; i-- > 0;) {
        StepRecord& prior = original->steps[i];
        if (!prior.persisted) continue;
        ManagedConfigSectionAdapter* adapterValue = adapter(prior.section);
        if (!adapterValue) {
            fail(output, "CAPABILITY_UNSUPPORTED", error, "Rollback adapter unavailable");
            return output["code"].as<const char*>();
        }
        size_t secretCount = 0;
        const char* const* secrets = adapterValue->secretFields(secretCount);
        stripConfiguredRecursive(prior.previous.as<JsonVariant>(), secrets, secretCount);
        ManagedPreflightResult review;
        if (!adapterValue->preflight(prior.previous, review, error) || !review.allowed) {
            fail(output, "PREFLIGHT_FAILED", error, error.isEmpty() ? "Rollback preflight failed" : error.c_str());
            return output["code"].as<const char*>();
        }
    }
    OperationRecord rollbackRecord;
    rollbackRecord.id = newOperationId(key, requestHash);
    rollbackRecord.idempotencyKey = key;
    rollbackRecord.requestHash = requestHash;
    rollbackRecord.state = "ROLLING_BACK";
    rollbackRecord.rollback = true;
    operations_.push_back(std::move(rollbackRecord));
    OperationRecord* rollback = &operations_.back();
    original = findOperation(id);
    String resultCode;
    for (size_t i = original->stepCount; i-- > 0;) {
        StepRecord& prior = original->steps[i];
        if (!prior.persisted) continue;
        StepRecord& step = rollback->steps[rollback->stepCount++];
        step.section = prior.section;
        step.accepted = true;
        step.state = "APPLYING";
        ManagedConfigSectionAdapter* adapterValue = adapter(prior.section);
        ManagedAdapterApplyResult applied = adapterValue->apply(prior.previous, error);
        if (applied.status == ManagedAdapterApplyStatus::SaveFailed ||
            applied.status == ManagedAdapterApplyStatus::Rejected ||
            applied.status == ManagedAdapterApplyStatus::Unknown) {
            step.state = applied.status == ManagedAdapterApplyStatus::Unknown ? "UNKNOWN" : "FAILED";
            step.error = applied.error;
            rollback->state = applied.status == ManagedAdapterApplyStatus::Unknown ? "UNKNOWN" : "FAILED";
            rollback->manualRecoveryRequired = true;
            rollback->readbackRequired = applied.status == ManagedAdapterApplyStatus::Unknown;
            rollback->error = applied.status == ManagedAdapterApplyStatus::Unknown ? "OUTCOME_UNKNOWN" : step.error;
            resultCode = rollback->error;
            break;
        }
        step.persisted = true;
        if (!adapterValue->readback(prior.previous, error)) {
            step.state = "UNKNOWN";
            rollback->state = "UNKNOWN";
            rollback->readbackRequired = true;
            rollback->manualRecoveryRequired = true;
            rollback->error = "OUTCOME_UNKNOWN";
            resultCode = rollback->error;
            break;
        }
        step.readback = true;
        step.state = "SUCCEEDED";
    }
    if (resultCode.isEmpty()) {
        rollback->state = "ROLLED_BACK";
        original->state = "ROLLED_BACK";
        original->manualRecoveryRequired = false;
    }
    serializeOperation(*rollback, output);
    error = resultCode;
    return resultCode;
}

bool ManagedConfigApplyService::storeSecret(const String& secretRef, const String& value,
                                            String& error) {
    if (secretRef.length() > 96 || value.length() > 1024 || secretRef.isEmpty()) {
        error = "INVALID_ARGUMENT";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const int separator = secretRef.indexOf('.');
    if (separator <= 0) {
        error = "INVALID_ARGUMENT";
        return false;
    }
    ManagedConfigSectionAdapter* adapterValue = adapter(secretRef.substring(0, separator));
    if (!adapterValue || !adapterValue->writeSecret(secretRef, value, error)) {
        if (error.isEmpty()) error = "CAPABILITY_UNSUPPORTED";
        return false;
    }
    return true;
}

void ManagedConfigApplyService::clearCandidates(std::vector<Candidate>& candidates) {
    for (auto& candidate : candidates) candidate.value.clear();
    candidates.clear();
}

#ifndef UNIT_TEST
void ManagedConfigApplyService::setProtocolManager(ProtocolManager* manager) {
    if (defaults_) defaults_->protocol.setManager(manager);
}
#endif

} // namespace FastBee
