#include <unity.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <atomic>
#include <fstream>
#include <thread>
#include <vector>

#include "core/ManagedConfigRevision.h"
#include "core/ManagedConfigApplyService.h"
#include "network/ProtocolConfigValidation.h"
#include "network/SameOrigin.h"
#include "../src/core/ManagedConfigRevision.cpp"
#include "../src/core/ManagedConfigApplyService.cpp"

#ifndef TEST_ASSERT_NOT_EQUAL_STRING
#define TEST_ASSERT_NOT_EQUAL_STRING(expected, actual) \
    TEST_ASSERT_TRUE_MESSAGE(std::strcmp((expected), (actual)) != 0, "Strings must differ")
#endif

namespace {

JsonDocument parse(const char* text) {
    JsonDocument document;
    deserializeJson(document, text);
    return document;
}

class FakeAdapter : public FastBee::ManagedConfigSectionAdapter {
public:
    FakeAdapter(const char* name, uint32_t schema = 1, bool readOnly = false)
        : name_(name), schema_(schema), readOnly_(readOnly) { state_.to<JsonObject>(); }

    const char* section() const override { return name_.c_str(); }
    uint32_t schemaVersion() const override { return schema_; }
    bool readOnly() const override { return readOnly_; }

    bool readRedacted(JsonDocument& output, String& error) override {
        output.clear();
        if (!output.set(state_)) {
            error = "READ_FAILED";
            return false;
        }
        for (const char* field : secretFields_) {
            if (!output[field].isNull()) {
                const bool configured = std::strlen(output[field] | "") > 0;
                output[field] = configured ? "********" : "";
                output[String(field) + "Configured"] = configured;
            }
        }
        return true;
    }

    bool validate(JsonVariantConst candidate, String& error) override {
        if (!candidate.is<JsonObjectConst>()) {
            error = "INVALID_ARGUMENT";
            return false;
        }
        for (JsonPairConst field : candidate.as<JsonObjectConst>()) {
            if (String(field.key().c_str()).startsWith("unknown")) {
                error = "INVALID_ARGUMENT";
                return false;
            }
        }
        return true;
    }

    bool preflight(JsonVariantConst candidate, FastBee::ManagedPreflightResult& result,
                   String& error) override {
        ++preflightCalls;
        result = FastBee::ManagedPreflightResult();
        if (candidate["resourceFail"] == true) {
            result.allowed = false;
            result.error = "RESOURCE_EXHAUSTED";
            error = result.error;
            return false;
        }
        result.pendingRestart = candidate["pendingRestart"] == true;
        return true;
    }

    bool preflightGraph(JsonVariantConst candidate, JsonVariantConst graph,
                        FastBee::ManagedPreflightResult& result, String& error) override {
        if (name_ == "protocol" && candidate["modbusRtu"]["enabled"] == true) {
            const bool featureEnabled = graph["features"]["features"]["modbus"]["enabled"] == true;
            const String uartId = candidate["modbusRtu"]["peripheralId"] | "";
            bool uartEnabled = false;
            for (JsonObjectConst peripheral : graph["peripherals"]["peripherals"].as<JsonArrayConst>()) {
                if (peripheral["id"] == uartId && peripheral["type"] == 1 && peripheral["enabled"] == true) {
                    uartEnabled = true;
                }
            }
            if (!featureEnabled || !uartEnabled) {
                result.allowed = false;
                result.error = "MIGRATION_PROTOCOL_UART_MISSING";
                error = result.error;
                return false;
            }
        }
        return preflight(candidate, result, error);
    }

    FastBee::ManagedAdapterApplyResult apply(JsonVariantConst candidate,
                                              String& error) override {
        ++applyCalls;
        lastOrder->push_back(name_);
        if (candidate["applyUnknown"] == true) {
            return {FastBee::ManagedAdapterApplyStatus::Unknown, false, "OUTCOME_UNKNOWN"};
        }
        if (candidate["saveFail"] == true) {
            return {FastBee::ManagedAdapterApplyStatus::SaveFailed, false, "SAVE_FAILED"};
        }
        if (candidate["readbackMismatch"] == true) {
            mismatchNextReadback = true;
        }
        JsonDocument secrets;
        for (const char* field : secretFields_) secrets[field] = state_[field];
        state_.clear();
        state_.set(candidate);
        for (const char* field : secretFields_) {
            if (state_[field] == "********") state_[field] = secrets[field];
        }
        const bool restart = candidate["pendingRestart"] == true;
        if (unknownAfterSuccessfulCalls > 0 && applyCalls > unknownAfterSuccessfulCalls && !unknownOnceConsumed) {
            unknownOnceConsumed = true;
            mismatchNextReadback = true;
            return {FastBee::ManagedAdapterApplyStatus::Unknown, restart, "OUTCOME_UNKNOWN"};
        }
        if (candidate["applyUnknownPersisted"] == true) {
            if (candidate["deferUnknownReadback"] == true) mismatchNextReadback = true;
            return {FastBee::ManagedAdapterApplyStatus::Unknown, restart, "OUTCOME_UNKNOWN"};
        }
        return {restart ? FastBee::ManagedAdapterApplyStatus::PendingRestart
                        : FastBee::ManagedAdapterApplyStatus::Persisted,
                restart, ""};
    }

    bool readback(JsonVariantConst expected, String& error) override {
        ++readbackCalls;
        if (mismatchNextReadback) {
            mismatchNextReadback = false;
            error = "READBACK_MISMATCH";
            return false;
        }
        JsonDocument actual;
        readRedacted(actual, error);
        for (JsonPairConst field : expected.as<JsonObjectConst>()) {
            if (String(field.key().c_str()).endsWith("Configured")) continue;
            if (secretFields_.end() != std::find(secretFields_.begin(), secretFields_.end(),
                                                String(field.key().c_str()))) continue;
            if (actual[field.key()] != field.value()) {
                error = "READBACK_MISMATCH";
                return false;
            }
        }
        return true;
    }

    const char* const* dependencies(size_t& count) const override {
        count = dependencies_.size();
        return dependencies_.empty() ? nullptr : dependencies_.data();
    }

    const char* const* secretFields(size_t& count) const override {
        count = secretFields_.size();
        return secretFields_.empty() ? nullptr : secretFields_.data();
    }

    bool writeSecret(const String& secretRef, const String& value, String& error) override {
        (void)secretRef;
        (void)value;
        (void)error;
        ++secretCalls;
        return true;
    }

    String name_;
    uint32_t schema_;
    bool readOnly_;
    JsonDocument state_;
    std::vector<String> order;
    std::vector<const char*> dependencies_;
    std::vector<String> secretFieldsStorage;
    std::vector<const char*> secretFields_;
    int preflightCalls = 0;
    int applyCalls = 0;
    int readbackCalls = 0;
    int secretCalls = 0;
    int unknownAfterSuccessfulCalls = 0;
    bool unknownOnceConsumed = false;
    bool mismatchNextReadback = false;
    std::vector<String>* lastOrder = nullptr;
};

struct Fixture {
    Fixture() : service(false) {
        for (FakeAdapter* adapter : {&features, &peripherals, &protocol, &rules, &exec, &network, &ai}) {
            adapter->lastOrder = &order;
            service.registerAdapter(adapter);
        }
        features.dependencies_ = {};
        peripherals.dependencies_ = {"features"};
        protocol.dependencies_ = {"peripherals"};
        rules.dependencies_ = {"protocol"};
        exec.dependencies_ = {"ruleScripts"};
        network.state_["ssid"] = "lab";
        network.state_["password"] = "do-not-read";
        network.secretFields_ = {"password"};
        ai.state_["secretConfigured"] = true;
        protocol.state_["password"] = "mqtt-secret";
        protocol.state_["server"] = "mqtt.local";
        protocol.secretFields_ = {"password"};
    }

    FastBee::ManagedConfigApplyService service;
    std::vector<String> order;
    FakeAdapter features{"features"};
    FakeAdapter peripherals{"peripherals"};
    FakeAdapter protocol{"protocol"};
    FakeAdapter rules{"ruleScripts"};
    FakeAdapter exec{"periphExec"};
    FakeAdapter network{"network", 1, true};
    FakeAdapter ai{"aiKey", 1, true};
};

String revision(Fixture& fixture) {
    JsonDocument snapshot;
    String error;
    if (!fixture.service.snapshot({}, snapshot, error)) return "";
    return snapshot["revision"].as<const char*>();
}

JsonDocument applyBody(Fixture& fixture, const char* operations,
                       const char* key = "key-1", const char* draft = "draft-1") {
    JsonDocument body;
    body["draftId"] = draft;
    body["baseRevision"] = revision(fixture);
    body["operations"].set(parse(operations));
    body["idempotencyKey"] = key;
    body["approvalHash"] = FastBee::ManagedConfigRevision::approvalHash(
        body["baseRevision"].as<const char*>(), body["draftId"].as<const char*>(), body["operations"]);
    return body;
}

void test_revision_canonical_and_content_derived() {
    JsonDocument first = parse("{\"b\":2,\"a\":{\"z\":1,\"y\":true}}");
    JsonDocument reordered = parse("{\"a\":{\"y\":true,\"z\":1},\"b\":2}");
    const String hashA = FastBee::ManagedConfigRevision::sectionHash(first);
    const String hashB = FastBee::ManagedConfigRevision::sectionHash(reordered);
    TEST_ASSERT_EQUAL_STRING(hashA.c_str(), hashB.c_str());
    TEST_ASSERT_EQUAL(64, hashA.length());
    reordered["b"] = 3;
    TEST_ASSERT_NOT_EQUAL_STRING(hashA.c_str(), FastBee::ManagedConfigRevision::sectionHash(reordered).c_str());
    TEST_ASSERT_EQUAL_STRING("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                             FastBee::ManagedConfigRevision::sha256("abc").c_str());

    const String changedHash = FastBee::ManagedConfigRevision::sectionHash(reordered);
    std::vector<std::pair<String, String>> sections{{"protocol", hashB}, {"features", hashA}};
    const String globalA = FastBee::ManagedConfigRevision::globalRevision(sections);
    TEST_ASSERT_EQUAL(64, globalA.length());
    sections[0].second = changedHash;
    const String globalB = FastBee::ManagedConfigRevision::globalRevision(sections);
    TEST_ASSERT_EQUAL(64, globalB.length());
    TEST_ASSERT_NOT_EQUAL_STRING(globalA.c_str(), globalB.c_str());
}

void test_snapshot_is_redacted_and_read_only_sections_cannot_apply() {
    Fixture fixture;
    JsonDocument snapshot;
    String error;
    TEST_ASSERT_TRUE(fixture.service.snapshot({}, snapshot, error));
    TEST_ASSERT_EQUAL_STRING("********", snapshot["sections"]["network"]["value"]["password"]);
    TEST_ASSERT_TRUE(snapshot["sections"]["network"]["value"]["passwordConfigured"]);
    TEST_ASSERT_TRUE(snapshot["sections"]["network"]["readOnly"]);
    TEST_ASSERT_TRUE(snapshot["sections"]["aiKey"]["readOnly"]);
    TEST_ASSERT_EQUAL_STRING("********", snapshot["sections"]["protocol"]["value"]["password"]);
    TEST_ASSERT_EQUAL(64, std::strlen(snapshot["revision"].as<const char*>()));

    JsonDocument second;
    TEST_ASSERT_TRUE(fixture.service.snapshot({}, second, error));
    TEST_ASSERT_EQUAL_STRING(snapshot["revision"], second["revision"]);
    fixture.protocol.state_["server"] = "changed.local";
    TEST_ASSERT_NOT_EQUAL(snapshot["revision"].as<const char*>(), revision(fixture).c_str());

    JsonDocument body = applyBody(fixture, "[{\"section\":\"network\",\"schemaVersion\":1,\"patch\":{\"ssid\":\"new\"}}]");
    JsonDocument result;
    TEST_ASSERT_EQUAL_STRING("CAPABILITY_UNSUPPORTED", fixture.service.apply(body, result, error).c_str());
    TEST_ASSERT_EQUAL(0, fixture.network.applyCalls);
}

void test_unknown_section_field_and_schema_are_rejected_before_side_effects() {
    Fixture fixture;
    String error;
    JsonDocument result;
    struct Case { const char* operations; const char* code; };
    for (const auto& item : std::vector<Case>{
             {"[{\"section\":\"missing\",\"schemaVersion\":1,\"patch\":{}}]", "INVALID_ARGUMENT"},
             {"[{\"section\":\"features\",\"schemaVersion\":99,\"patch\":{}}]", "SCHEMA_VERSION_UNSUPPORTED"},
             {"[{\"section\":\"features\",\"schemaVersion\":1,\"patch\":{\"unknownField\":1}}]", "INVALID_ARGUMENT"},
             {"[{\"section\":\"features\",\"schemaVersion\":1,\"patch\":{},\"unexpected\":true}]", "INVALID_ARGUMENT"}}) {
        JsonDocument body = applyBody(fixture, item.operations);
        TEST_ASSERT_EQUAL_STRING(item.code, fixture.service.apply(body, result, error).c_str());
    }
    TEST_ASSERT_EQUAL(0, fixture.features.applyCalls);
}

void test_preflight_is_side_effect_free_and_reports_resource_failure() {
    Fixture fixture;
    JsonDocument request;
    request["operations"].set(parse("[{\"section\":\"features\",\"schemaVersion\":1,\"patch\":{\"enabled\":true}}]"));
    JsonDocument result;
    String error;
    TEST_ASSERT_TRUE(fixture.service.preflight(request, result, error));
    TEST_ASSERT_TRUE(result["allowed"]);
    TEST_ASSERT_EQUAL(1, fixture.features.preflightCalls);
    TEST_ASSERT_EQUAL(0, fixture.features.applyCalls);
    TEST_ASSERT_EQUAL(0, fixture.features.readbackCalls);

    request["operations"][0]["patch"]["resourceFail"] = true;
    TEST_ASSERT_FALSE(fixture.service.preflight(request, result, error));
    TEST_ASSERT_EQUAL_STRING("PREFLIGHT_FAILED", result["code"]);
    TEST_ASSERT_EQUAL(0, fixture.features.applyCalls);
}

void test_global_and_section_revision_conflicts_block_all_writes() {
    Fixture fixture;
    JsonDocument body = applyBody(fixture, "[{\"section\":\"features\",\"schemaVersion\":1,\"patch\":{\"enabled\":true}}]");
    body["baseRevision"] = "stale";
    body["approvalHash"] = FastBee::ManagedConfigRevision::approvalHash(
        body["baseRevision"].as<const char*>(), body["draftId"].as<const char*>(), body["operations"]);
    JsonDocument result;
    String error;
    TEST_ASSERT_EQUAL_STRING("CONFIG_REVISION_CONFLICT", fixture.service.apply(body, result, error).c_str());

    body = applyBody(fixture, "[{\"section\":\"features\",\"schemaVersion\":1,\"baseRevision\":\"stale-section\",\"patch\":{\"enabled\":true}}]");
    TEST_ASSERT_EQUAL_STRING("CONFIG_REVISION_CONFLICT", fixture.service.apply(body, result, error).c_str());
    TEST_ASSERT_EQUAL(0, fixture.features.applyCalls);
}

void test_approval_hash_is_bound_to_draft_and_operations() {
    Fixture fixture;
    JsonDocument body = applyBody(fixture, "[{\"section\":\"features\",\"schemaVersion\":1,\"patch\":{\"enabled\":true}}]");
    body["approvalHash"] = "00";
    JsonDocument result;
    String error;
    TEST_ASSERT_EQUAL_STRING("CONFIRMATION_REQUIRED", fixture.service.apply(body, result, error).c_str());
    body["operations"][0]["patch"]["enabled"] = false;
    body["approvalHash"] = FastBee::ManagedConfigRevision::approvalHash(
        body["baseRevision"].as<const char*>(), "another-draft", body["operations"]);
    TEST_ASSERT_EQUAL_STRING("CONFIRMATION_REQUIRED", fixture.service.apply(body, result, error).c_str());
    TEST_ASSERT_EQUAL(0, fixture.features.applyCalls);
}

void test_duplicate_and_concurrent_idempotency_return_original_operation() {
    Fixture fixture;
    JsonDocument body = applyBody(fixture, "[{\"section\":\"features\",\"schemaVersion\":1,\"patch\":{\"enabled\":true}}]");
    JsonDocument first;
    String error;
    TEST_ASSERT_TRUE(fixture.service.apply(body, first, error).isEmpty());
    TEST_ASSERT_EQUAL_STRING("SUCCEEDED", first["state"]);
    const String operationId = first["operationId"].as<const char*>();
    JsonDocument duplicate;
    TEST_ASSERT_TRUE(fixture.service.apply(body, duplicate, error).isEmpty());
    TEST_ASSERT_EQUAL_STRING(operationId.c_str(), duplicate["operationId"]);
    TEST_ASSERT_EQUAL(1, fixture.features.applyCalls);

    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::thread workers[2];
    for (int i = 0; i < 2; ++i) {
        workers[i] = std::thread([&]() {
            ++ready;
            while (!start.load()) std::this_thread::yield();
            JsonDocument output;
            String threadError;
            fixture.service.apply(body, output, threadError);
        });
    }
    while (ready.load() < 2) std::this_thread::yield();
    start = true;
    workers[0].join();
    workers[1].join();
    TEST_ASSERT_EQUAL(1, fixture.features.applyCalls);

    body["operations"][0]["patch"]["enabled"] = false;
    body["approvalHash"] = FastBee::ManagedConfigRevision::approvalHash(
        body["baseRevision"].as<const char*>(), body["draftId"].as<const char*>(), body["operations"]);
    TEST_ASSERT_EQUAL_STRING("IDEMPOTENCY_CONFLICT", fixture.service.apply(body, duplicate, error).c_str());
}

void test_multi_section_operations_use_declared_dependency_order() {
    Fixture fixture;
    JsonDocument body = applyBody(fixture,
        "[{\"section\":\"periphExec\",\"schemaVersion\":1,\"patch\":{\"enabled\":false}},"
        "{\"section\":\"features\",\"schemaVersion\":1,\"patch\":{\"enabled\":true}},"
        "{\"section\":\"peripherals\",\"schemaVersion\":1,\"patch\":{\"count\":1}},"
        "{\"section\":\"protocol\",\"schemaVersion\":1,\"patch\":{\"server\":\"new\"}},"
        "{\"section\":\"ruleScripts\",\"schemaVersion\":1,\"patch\":{\"enabled\":false}}]");
    JsonDocument result;
    String error;
    TEST_ASSERT_TRUE(fixture.service.apply(body, result, error).isEmpty());
    const char* expected[] = {"features", "peripherals", "protocol", "ruleScripts", "periphExec"};
    TEST_ASSERT_EQUAL(5, fixture.order.size());
    for (size_t i = 0; i < 5; ++i) TEST_ASSERT_EQUAL_STRING(expected[i], fixture.order[i].c_str());
    TEST_ASSERT_EQUAL(5, result["steps"].size());
}

void test_save_failure_preserves_partial_facts_and_requires_explicit_rollback() {
    Fixture fixture;
    JsonDocument body = applyBody(fixture,
        "[{\"section\":\"features\",\"schemaVersion\":1,\"patch\":{\"enabled\":true}},"
        "{\"section\":\"peripherals\",\"schemaVersion\":1,\"patch\":{\"saveFail\":true}}]");
    JsonDocument result;
    String error;
    TEST_ASSERT_EQUAL_STRING("SAVE_FAILED", fixture.service.apply(body, result, error).c_str());
    TEST_ASSERT_EQUAL_STRING("FAILED", result["state"]);
    TEST_ASSERT_TRUE(result["manualRecoveryRequired"]);
    TEST_ASSERT_TRUE(result["steps"][0]["persisted"]);
    TEST_ASSERT_FALSE(result["steps"][1]["persisted"]);
    TEST_ASSERT_EQUAL(1, fixture.features.applyCalls);

    JsonDocument rollback;
    rollback["operationId"] = result["operationId"];
    rollback["idempotencyKey"] = "rollback-1";
    rollback["approvalHash"] = "bad";
    JsonDocument rollbackResult;
    TEST_ASSERT_EQUAL_STRING("CONFIRMATION_REQUIRED", fixture.service.rollback(rollback, rollbackResult, error).c_str());
    TEST_ASSERT_EQUAL(1, fixture.features.applyCalls);
    rollback["approvalHash"] = FastBee::ManagedConfigRevision::rollbackApprovalHash(
        rollback["operationId"].as<const char*>(), revision(fixture));
    TEST_ASSERT_TRUE(fixture.service.rollback(rollback, rollbackResult, error).isEmpty());
    TEST_ASSERT_EQUAL_STRING("ROLLED_BACK", rollbackResult["state"]);
    TEST_ASSERT_EQUAL(2, fixture.features.applyCalls);
}

void test_unknown_apply_uses_single_readback_and_never_replays() {
    Fixture fixture;
    JsonDocument body = applyBody(fixture,
        "[{\"section\":\"features\",\"schemaVersion\":1,\"patch\":{\"applyUnknown\":true}}]");
    JsonDocument result;
    String error;
    TEST_ASSERT_EQUAL_STRING("OUTCOME_UNKNOWN", fixture.service.apply(body, result, error).c_str());
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", result["state"]);
    TEST_ASSERT_TRUE(result["readbackRequired"]);
    TEST_ASSERT_EQUAL(1, fixture.features.applyCalls);
    TEST_ASSERT_EQUAL(1, fixture.features.readbackCalls);
    JsonDocument duplicate;
    TEST_ASSERT_EQUAL_STRING("OUTCOME_UNKNOWN", fixture.service.apply(body, duplicate, error).c_str());
    TEST_ASSERT_EQUAL(1, fixture.features.applyCalls);
    TEST_ASSERT_EQUAL(1, fixture.features.readbackCalls);
}

void test_readback_mismatch_is_unknown_and_pending_restart_is_not_observed() {
    Fixture fixture;
    JsonDocument result;
    String error;
    JsonDocument mismatch = applyBody(fixture,
        "[{\"section\":\"features\",\"schemaVersion\":1,\"patch\":{\"readbackMismatch\":true}}]", "key-mismatch");
    TEST_ASSERT_EQUAL_STRING("OUTCOME_UNKNOWN", fixture.service.apply(mismatch, result, error).c_str());
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", result["state"]);

    Fixture pendingFixture;
    JsonDocument pending = applyBody(pendingFixture,
        "[{\"section\":\"peripherals\",\"schemaVersion\":1,\"patch\":{\"pendingRestart\":true}}]", "key-pending");
    TEST_ASSERT_TRUE(pendingFixture.service.apply(pending, result, error).isEmpty());
    TEST_ASSERT_TRUE(result["pendingRestart"]);
    TEST_ASSERT_TRUE(result["steps"][0]["pendingRestart"]);
    TEST_ASSERT_FALSE(result["steps"][0]["observed"]);
    TEST_ASSERT_FALSE(result["steps"][0]["physicalConfirmed"]);
}

void test_new_boot_has_no_replayable_operation_or_automatic_rollback() {
    Fixture fixture;
    JsonDocument body = applyBody(fixture,
        "[{\"section\":\"features\",\"schemaVersion\":1,\"patch\":{\"applyUnknown\":true}}]");
    JsonDocument result;
    String error;
    fixture.service.apply(body, result, error);
    const int applyCalls = fixture.features.applyCalls;
    const int readbackCalls = fixture.features.readbackCalls;

    FastBee::ManagedConfigApplyService afterRestart(false);
    afterRestart.registerAdapter(&fixture.features);
    JsonDocument lookup;
    TEST_ASSERT_FALSE(afterRestart.operation(result["operationId"].as<const char*>(), lookup, error));
    TEST_ASSERT_EQUAL(applyCalls, fixture.features.applyCalls);
    TEST_ASSERT_EQUAL(readbackCalls, fixture.features.readbackCalls);
}

void test_protocol_candidate_semantics_are_validated() {
    JsonDocument valid = parse(
        "{\"version\":1,\"mqtt\":{\"enabled\":true,\"scheme\":\"mqtts\",\"port\":8883,"
        "\"keepAlive\":60,\"publishTopics\":[],\"subscribeTopics\":[]},"
        "\"modbusRtu\":{\"enabled\":true,\"mode\":\"master\",\"transferType\":0,"
        "\"master\":{\"tasks\":[{\"slaveAddress\":1,\"functionCode\":3,\"quantity\":2,"
        "\"mappings\":[{\"regOffset\":0,\"dataType\":0,\"sensorId\":\"temperature\"}]}],\"devices\":[]}},"
        "\"coap\":{\"enabled\":false}}");
    String error;
    TEST_ASSERT_TRUE(FastBee::validateProtocolConfigDocument(valid, error));
    JsonDocument invalid = parse("{\"mqtt\":{\"scheme\":\"http\",\"port\":0}}");
    TEST_ASSERT_FALSE(FastBee::validateProtocolConfigDocument(invalid, error));
    invalid = parse("{\"mqtt\":{\"enabled\":\"true\",\"scheme\":\"mqtt\",\"port\":1883}}");
    TEST_ASSERT_FALSE(FastBee::validateProtocolConfigDocument(invalid, error));
    invalid = parse("{\"modbusRtu\":{\"transferType\":2}}");
    TEST_ASSERT_FALSE(FastBee::validateProtocolConfigDocument(invalid, error));
    invalid = parse("{\"modbusRtu\":{\"master\":{\"tasks\":[{\"functionCode\":9}]}}}");
    TEST_ASSERT_FALSE(FastBee::validateProtocolConfigDocument(invalid, error));
    invalid = parse("{\"coap\":true}");
    TEST_ASSERT_FALSE(FastBee::validateProtocolConfigDocument(invalid, error));
}

void test_unknown_gates_writes_until_readonly_recovery_proves_outcome() {
    Fixture fixture;
    JsonDocument body = applyBody(fixture,
        "[{\"section\":\"features\",\"schemaVersion\":1,\"patch\":{\"applyUnknownPersisted\":true,\"deferUnknownReadback\":true}}]",
        "unknown-gate");
    JsonDocument result;
    String error;
    TEST_ASSERT_EQUAL_STRING("OUTCOME_UNKNOWN", fixture.service.apply(body, result, error).c_str());
    const String operationId = result["operationId"].as<const char*>();
    JsonDocument later = applyBody(fixture,
        "[{\"section\":\"peripherals\",\"schemaVersion\":1,\"patch\":{\"enabled\":true}}]",
        "after-unknown");
    TEST_ASSERT_EQUAL_STRING("OUTCOME_UNKNOWN", fixture.service.apply(later, result, error).c_str());
    TEST_ASSERT_EQUAL(0, fixture.peripherals.applyCalls);
    TEST_ASSERT_FALSE(fixture.service.storeSecret("protocol.mqtt.password", "new-secret", "secret-1", error));
    TEST_ASSERT_EQUAL(0, fixture.protocol.secretCalls);

    TEST_ASSERT_TRUE(fixture.service.recoverOperation(operationId, result, error));
    TEST_ASSERT_EQUAL_STRING("SUCCEEDED", result["state"]);
    TEST_ASSERT_TRUE(fixture.service.storeSecret("protocol.mqtt.password", "new-secret", "secret-1", error));
    TEST_ASSERT_EQUAL(1, fixture.protocol.secretCalls);
    TEST_ASSERT_TRUE(fixture.service.storeSecret("protocol.mqtt.password", "new-secret", "secret-1", error));
    TEST_ASSERT_EQUAL(1, fixture.protocol.secretCalls);
    TEST_ASSERT_TRUE(fixture.service.apply(later, result, error).isEmpty());
    TEST_ASSERT_EQUAL(1, fixture.peripherals.applyCalls);
}

void test_unresolved_unknown_remains_blocked_when_readback_does_not_match() {
    Fixture fixture;
    JsonDocument body = applyBody(fixture,
        "[{\"section\":\"features\",\"schemaVersion\":1,\"patch\":{\"applyUnknown\":true}}]",
        "unknown-unresolved");
    JsonDocument result;
    String error;
    fixture.service.apply(body, result, error);
    const String operationId = result["operationId"].as<const char*>();
    TEST_ASSERT_FALSE(fixture.service.recoverOperation(operationId, result, error));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", result["state"]);
    TEST_ASSERT_FALSE(fixture.service.storeSecret("protocol.mqtt.password", "new-secret", "secret-2", error));
    TEST_ASSERT_EQUAL(0, fixture.protocol.secretCalls);
}

void test_cross_section_preflight_uses_candidate_dependency_graph() {
    Fixture fixture;
    const char* operations =
        "[{\"section\":\"periphExec\",\"schemaVersion\":1,\"patch\":{\"enabled\":false}},"
        "{\"section\":\"protocol\",\"schemaVersion\":1,\"patch\":{\"modbusRtu\":{\"enabled\":true,\"mode\":\"master\",\"peripheralId\":\"uart-candidate\"}}},"
        "{\"section\":\"features\",\"schemaVersion\":1,\"patch\":{\"features\":{\"modbus\":{\"enabled\":true}}}},"
        "{\"section\":\"peripherals\",\"schemaVersion\":1,\"patch\":{\"peripherals\":[{\"id\":\"uart-candidate\",\"type\":1,\"enabled\":true}]}}]";
    JsonDocument body = applyBody(fixture, operations, "candidate-graph");
    JsonDocument result;
    String error;
    TEST_ASSERT_TRUE(fixture.service.apply(body, result, error).isEmpty());

    Fixture missing;
    JsonDocument blocked = applyBody(missing,
        "[{\"section\":\"protocol\",\"schemaVersion\":1,\"patch\":{\"modbusRtu\":{\"enabled\":true,\"mode\":\"master\",\"peripheralId\":\"uart-candidate\"}}}]",
        "candidate-graph-missing");
    TEST_ASSERT_EQUAL_STRING("PREFLIGHT_FAILED", missing.service.apply(blocked, result, error).c_str());
    TEST_ASSERT_EQUAL(0, missing.protocol.applyCalls);
}

class ChangingAdapter final : public FakeAdapter {
public:
    ChangingAdapter() : FakeAdapter("features") { state_["value"] = 0; }
    bool readRedacted(JsonDocument& output, String& error) override {
        state_["value"] = state_["value"].as<int>() + 1;
        return FakeAdapter::readRedacted(output, error);
    }
};

void test_snapshot_hashes_and_values_share_one_consistent_read_set() {
    FastBee::ManagedConfigApplyService service(false);
    ChangingAdapter changing;
    service.registerAdapter(&changing);
    JsonDocument snapshot;
    String error;
    TEST_ASSERT_TRUE(service.snapshot({}, snapshot, error));
    std::vector<std::pair<String, String>> hashes;
    hashes.emplace_back("features", snapshot["sections"]["features"]["hash"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING(FastBee::ManagedConfigRevision::globalRevision(hashes).c_str(),
                             snapshot["revision"].as<const char*>());
}

void test_aggregate_candidate_budget_rejects_before_any_write() {
    Fixture fixture;
    JsonDocument body = applyBody(fixture, "[]", "aggregate-budget");
    JsonArray operations = body["operations"].as<JsonArray>();
    for (const char* section : {"features", "peripherals", "protocol", "ruleScripts"}) {
        JsonObject row = operations.add<JsonObject>();
        row["section"] = section;
        row["schemaVersion"] = 1;
        row["patch"]["blob"] = std::string(25000, 'x');
    }
    body["approvalHash"] = FastBee::ManagedConfigRevision::approvalHash(
        body["baseRevision"].as<const char*>(), body["draftId"].as<const char*>(), operations);
    JsonDocument result;
    String error;
    TEST_ASSERT_EQUAL_STRING("QUOTA_EXCEEDED", fixture.service.apply(body, result, error).c_str());
    TEST_ASSERT_EQUAL(0, fixture.features.applyCalls);
    TEST_ASSERT_EQUAL(0, fixture.peripherals.applyCalls);
    TEST_ASSERT_EQUAL(0, fixture.protocol.applyCalls);
    TEST_ASSERT_EQUAL(0, fixture.rules.applyCalls);
}

void test_rollback_propagates_pending_restart() {
    Fixture fixture;
    JsonDocument body = applyBody(fixture,
        "[{\"section\":\"features\",\"schemaVersion\":1,\"patch\":{\"pendingRestart\":true}}]",
        "restart-original");
    JsonDocument result;
    String error;
    TEST_ASSERT_TRUE(fixture.service.apply(body, result, error).isEmpty());
    JsonDocument rollback;
    rollback["operationId"] = result["operationId"];
    rollback["idempotencyKey"] = "restart-rollback";
    rollback["approvalHash"] = FastBee::ManagedConfigRevision::rollbackApprovalHash(
        rollback["operationId"].as<const char*>(), revision(fixture));
    TEST_ASSERT_TRUE(fixture.service.rollback(rollback, result, error).isEmpty());
    TEST_ASSERT_TRUE(result["pendingRestart"]);
    TEST_ASSERT_TRUE(result["steps"][0]["pendingRestart"]);
}

void test_rollback_unknown_recovers_to_rolled_back_without_replay() {
    Fixture fixture;
    fixture.features.unknownAfterSuccessfulCalls = 1;
    JsonDocument body = applyBody(fixture,
        "[{\"section\":\"features\",\"schemaVersion\":1,\"patch\":{\"pendingRestart\":true}}]",
        "rollback-unknown-original");
    JsonDocument result;
    String error;
    TEST_ASSERT_TRUE(fixture.service.apply(body, result, error).isEmpty());
    const String originalId = result["operationId"].as<const char*>();
    JsonDocument rollback;
    rollback["operationId"] = originalId;
    rollback["idempotencyKey"] = "rollback-unknown";
    rollback["approvalHash"] = FastBee::ManagedConfigRevision::rollbackApprovalHash(
        originalId, revision(fixture));
    TEST_ASSERT_EQUAL_STRING("OUTCOME_UNKNOWN", fixture.service.rollback(rollback, result, error).c_str());
    const String rollbackId = result["operationId"].as<const char*>();
    const int applyCalls = fixture.features.applyCalls;
    TEST_ASSERT_FALSE(fixture.service.recoverOperation(rollbackId, result, error));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", result["state"]);
    TEST_ASSERT_TRUE(fixture.service.recoverOperation(rollbackId, result, error));
    TEST_ASSERT_EQUAL_STRING("ROLLED_BACK", result["state"]);
    TEST_ASSERT_EQUAL(applyCalls, fixture.features.applyCalls);
}

void test_recovery_requires_every_apply_step_succeeded() {
    Fixture fixture;
    JsonDocument body = applyBody(fixture,
        "[{\"section\":\"features\",\"schemaVersion\":1,\"patch\":{\"applyUnknownPersisted\":true,\"deferUnknownReadback\":true}},"
        "{\"section\":\"peripherals\",\"schemaVersion\":1,\"patch\":{\"enabled\":true}}]",
        "partial-recovery");
    JsonDocument result;
    String error;
    TEST_ASSERT_EQUAL_STRING("OUTCOME_UNKNOWN", fixture.service.apply(body, result, error).c_str());
    const String operationId = result["operationId"].as<const char*>();
    TEST_ASSERT_TRUE(fixture.service.recoverOperation(operationId, result, error));
    TEST_ASSERT_EQUAL_STRING("FAILED", result["state"]);
    TEST_ASSERT_TRUE(result["manualRecoveryRequired"]);
    TEST_ASSERT_FALSE(result["readbackRequired"]);
    TEST_ASSERT_EQUAL_STRING("PENDING", result["steps"][1]["state"]);
    TEST_ASSERT_EQUAL(0, fixture.peripherals.applyCalls);
}

void test_rollback_recovery_requires_every_step_succeeded() {
    Fixture fixture;
    fixture.peripherals.unknownAfterSuccessfulCalls = 1;
    JsonDocument body = applyBody(fixture,
        "[{\"section\":\"features\",\"schemaVersion\":1,\"patch\":{\"enabled\":true}},"
        "{\"section\":\"peripherals\",\"schemaVersion\":1,\"patch\":{\"enabled\":true}}]",
        "partial-rollback-recovery");
    JsonDocument result;
    String error;
    TEST_ASSERT_TRUE(fixture.service.apply(body, result, error).isEmpty());
    const String originalId = result["operationId"].as<const char*>();
    JsonDocument rollback;
    rollback["operationId"] = originalId;
    rollback["idempotencyKey"] = "partial-rollback";
    rollback["approvalHash"] = FastBee::ManagedConfigRevision::rollbackApprovalHash(
        originalId, revision(fixture));
    TEST_ASSERT_EQUAL_STRING("OUTCOME_UNKNOWN", fixture.service.rollback(rollback, result, error).c_str());
    const String rollbackId = result["operationId"].as<const char*>();
    TEST_ASSERT_FALSE(fixture.service.recoverOperation(rollbackId, result, error));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", result["state"]);
    TEST_ASSERT_TRUE(result["manualRecoveryRequired"]);
    TEST_ASSERT_EQUAL_STRING("PENDING", result["steps"][1]["state"]);
    TEST_ASSERT_EQUAL(1, fixture.features.applyCalls);
    JsonDocument original;
    TEST_ASSERT_TRUE(fixture.service.operation(originalId, original, error));
    TEST_ASSERT_EQUAL_STRING("SUCCEEDED", original["state"]);
}

void test_partial_apply_recovery_allows_confirmed_rollback_of_persisted_steps() {
    Fixture fixture;
    JsonDocument body = applyBody(fixture,
        "[{\"section\":\"features\",\"schemaVersion\":1,\"patch\":{\"applyUnknownPersisted\":true,\"deferUnknownReadback\":true}},"
        "{\"section\":\"peripherals\",\"schemaVersion\":1,\"patch\":{\"enabled\":true}}]",
        "partial-recovery-rollback");
    JsonDocument result;
    String error;
    fixture.service.apply(body, result, error);
    const String operationId = result["operationId"].as<const char*>();
    TEST_ASSERT_TRUE(fixture.service.recoverOperation(operationId, result, error));
    TEST_ASSERT_EQUAL_STRING("FAILED", result["state"]);
    TEST_ASSERT_FALSE(result["readbackRequired"]);
    TEST_ASSERT_TRUE(result["manualRecoveryRequired"]);

    JsonDocument rollback;
    rollback["operationId"] = operationId;
    rollback["idempotencyKey"] = "partial-recovery-rollback-confirm";
    rollback["approvalHash"] = FastBee::ManagedConfigRevision::rollbackApprovalHash(
        operationId, revision(fixture));
    TEST_ASSERT_TRUE(fixture.service.rollback(rollback, result, error).isEmpty());
    TEST_ASSERT_EQUAL_STRING("ROLLED_BACK", result["state"]);
    TEST_ASSERT_EQUAL(2, fixture.features.applyCalls);
    TEST_ASSERT_EQUAL(0, fixture.peripherals.applyCalls);
    JsonDocument original;
    TEST_ASSERT_TRUE(fixture.service.operation(operationId, original, error));
    TEST_ASSERT_EQUAL_STRING("ROLLED_BACK", original["state"]);
}

void test_partial_rollback_recovery_allows_rerollback_of_persisted_steps() {
    Fixture fixture;
    fixture.peripherals.unknownAfterSuccessfulCalls = 1;
    JsonDocument body = applyBody(fixture,
        "[{\"section\":\"features\",\"schemaVersion\":1,\"patch\":{\"enabled\":true}},"
        "{\"section\":\"peripherals\",\"schemaVersion\":1,\"patch\":{\"enabled\":true}}]",
        "partial-rerollback-recovery");
    JsonDocument result;
    String error;
    TEST_ASSERT_TRUE(fixture.service.apply(body, result, error).isEmpty());
    const String originalId = result["operationId"].as<const char*>();
    JsonDocument rollback;
    rollback["operationId"] = originalId;
    rollback["idempotencyKey"] = "partial-rerollback-first";
    rollback["approvalHash"] = FastBee::ManagedConfigRevision::rollbackApprovalHash(
        originalId, revision(fixture));
    TEST_ASSERT_EQUAL_STRING("OUTCOME_UNKNOWN", fixture.service.rollback(rollback, result, error).c_str());
    const String rollbackId = result["operationId"].as<const char*>();
    TEST_ASSERT_FALSE(fixture.service.recoverOperation(rollbackId, result, error));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", result["state"]);
    TEST_ASSERT_TRUE(fixture.service.recoverOperation(rollbackId, result, error));
    TEST_ASSERT_EQUAL_STRING("FAILED", result["state"]);
    TEST_ASSERT_FALSE(result["readbackRequired"]);
    TEST_ASSERT_TRUE(result["manualRecoveryRequired"]);

    JsonDocument second;
    second["operationId"] = rollbackId;
    second["idempotencyKey"] = "partial-rerollback-second";
    second["approvalHash"] = FastBee::ManagedConfigRevision::rollbackApprovalHash(
        rollbackId, revision(fixture));
    TEST_ASSERT_TRUE(fixture.service.rollback(second, result, error).isEmpty());
    TEST_ASSERT_EQUAL_STRING("ROLLED_BACK", result["state"]);
    TEST_ASSERT_EQUAL(1, fixture.features.applyCalls);
    TEST_ASSERT_EQUAL(3, fixture.peripherals.applyCalls);
    JsonDocument original;
    TEST_ASSERT_TRUE(fixture.service.operation(originalId, original, error));
    TEST_ASSERT_EQUAL_STRING("SUCCEEDED", original["state"]);
}

void test_default_factory_protocol_is_valid() {
    std::ifstream input("data/config/protocol.json");
    TEST_ASSERT_TRUE(input.good());
    JsonDocument document;
    const auto error = deserializeJson(document, input);
    TEST_ASSERT_FALSE(error);
    String validationError;
    TEST_ASSERT_TRUE(FastBee::validateProtocolConfigDocument(document, validationError));
}

void test_filtered_snapshot_keeps_complete_global_revision() {
    Fixture fixture;
    JsonDocument all;
    JsonDocument filtered;
    String error;
    TEST_ASSERT_TRUE(fixture.service.snapshot({}, all, error));
    TEST_ASSERT_TRUE(fixture.service.snapshot({"features"}, filtered, error));
    TEST_ASSERT_EQUAL_STRING(all["revision"].as<const char*>(), filtered["revision"].as<const char*>());
    TEST_ASSERT_TRUE(filtered["sections"]["features"].is<JsonObject>());
    TEST_ASSERT_TRUE(filtered["sections"]["peripherals"].isNull());
    TEST_ASSERT_EQUAL(64, std::strlen(filtered["revision"].as<const char*>()));
}

void test_singular_publish_and_legacy_fields_are_validated() {
    String error;
    for (const char* candidate : {
             "{\"mqtt\":{\"publishTopic\":\"\"}}",
             "{\"mqtt\":{\"publishTopic\":\"bad/+\"}}",
             "{\"mqtt\":{\"publishTopic\":null}}",
             "{\"mqtt\":{\"publishTopic\":17}}",
             "{\"mqtt\":{\"publishQos\":3}}",
             "{\"mqtt\":{\"publishRetain\":\"false\"}}",
             "{\"mqtt\":{\"downgraded_original_scheme\":\"http\"}}",
             "{\"mqtt\":{\"downgraded_original_port\":0}}"}) {
        JsonDocument document = parse(candidate);
        TEST_ASSERT_FALSE(FastBee::validateProtocolConfigDocument(document, error));
    }
    JsonDocument valid = parse("{\"mqtt\":{\"publishTopic\":\"/property/post\",\"publishQos\":1,\"publishRetain\":false}}");
    TEST_ASSERT_TRUE(FastBee::validateProtocolConfigDocument(valid, error));
    JsonDocument absentLegacy = parse("{\"mqtt\":{}}");
    TEST_ASSERT_TRUE(FastBee::validateProtocolConfigDocument(absentLegacy, error));
}

void test_same_origin_rejects_cross_origin_mutation() {
    TEST_ASSERT_TRUE(FastBee::isSameOriginRequest("POST", "http://device.local", "device.local"));
    TEST_ASSERT_FALSE(FastBee::isSameOriginRequest("POST", "http://evil.example/device.local", "device.local"));
    TEST_ASSERT_FALSE(FastBee::isSameOriginRequest("POST", "http://device.local.evil.example", "device.local"));
}

void test_nested_secret_values_require_secret_channel() {
    Fixture fixture;
    JsonDocument body = applyBody(fixture,
        "[{\"section\":\"protocol\",\"schemaVersion\":1,\"patch\":{\"mqtt\":{\"password\":\"raw-secret\"}}}]",
        "nested-secret");
    JsonDocument result;
    String error;
    TEST_ASSERT_EQUAL_STRING("SECRET_REQUIRED", fixture.service.apply(body, result, error).c_str());
    TEST_ASSERT_EQUAL(0, fixture.protocol.applyCalls);
}

void test_operation_and_body_bounds_are_explicit() {
    Fixture fixture;
    JsonDocument request;
    JsonArray operations = request["operations"].to<JsonArray>();
    for (size_t i = 0; i <= FastBee::ManagedConfigApplyService::kMaxOperations; ++i) {
        JsonObject row = operations.add<JsonObject>();
        row["section"] = "features";
        row["schemaVersion"] = 1;
        row["patch"]["index"] = i;
    }
    JsonDocument result;
    String error;
    TEST_ASSERT_FALSE(fixture.service.preflight(request, result, error));
    TEST_ASSERT_EQUAL_STRING("QUOTA_EXCEEDED", result["code"]);
}
} // namespace

void test_mcp_config_apply_group() {
    RUN_TEST(test_revision_canonical_and_content_derived);
    RUN_TEST(test_snapshot_is_redacted_and_read_only_sections_cannot_apply);
    RUN_TEST(test_unknown_section_field_and_schema_are_rejected_before_side_effects);
    RUN_TEST(test_preflight_is_side_effect_free_and_reports_resource_failure);
    RUN_TEST(test_global_and_section_revision_conflicts_block_all_writes);
    RUN_TEST(test_approval_hash_is_bound_to_draft_and_operations);
    RUN_TEST(test_duplicate_and_concurrent_idempotency_return_original_operation);
    RUN_TEST(test_multi_section_operations_use_declared_dependency_order);
    RUN_TEST(test_save_failure_preserves_partial_facts_and_requires_explicit_rollback);
    RUN_TEST(test_unknown_apply_uses_single_readback_and_never_replays);
    RUN_TEST(test_readback_mismatch_is_unknown_and_pending_restart_is_not_observed);
    RUN_TEST(test_new_boot_has_no_replayable_operation_or_automatic_rollback);
    RUN_TEST(test_protocol_candidate_semantics_are_validated);
    RUN_TEST(test_unknown_gates_writes_until_readonly_recovery_proves_outcome);
    RUN_TEST(test_unresolved_unknown_remains_blocked_when_readback_does_not_match);
    RUN_TEST(test_cross_section_preflight_uses_candidate_dependency_graph);
    RUN_TEST(test_snapshot_hashes_and_values_share_one_consistent_read_set);
    RUN_TEST(test_aggregate_candidate_budget_rejects_before_any_write);
    RUN_TEST(test_rollback_propagates_pending_restart);
    RUN_TEST(test_rollback_unknown_recovers_to_rolled_back_without_replay);
    RUN_TEST(test_recovery_requires_every_apply_step_succeeded);
    RUN_TEST(test_rollback_recovery_requires_every_step_succeeded);
    RUN_TEST(test_partial_apply_recovery_allows_confirmed_rollback_of_persisted_steps);
    RUN_TEST(test_partial_rollback_recovery_allows_rerollback_of_persisted_steps);
    RUN_TEST(test_default_factory_protocol_is_valid);
    RUN_TEST(test_filtered_snapshot_keeps_complete_global_revision);
    RUN_TEST(test_singular_publish_and_legacy_fields_are_validated);
    RUN_TEST(test_same_origin_rejects_cross_origin_mutation);
    RUN_TEST(test_nested_secret_values_require_secret_channel);
    RUN_TEST(test_operation_and_body_bounds_are_explicit);
}
