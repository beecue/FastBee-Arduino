#include <unity.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <atomic>
#include <thread>
#include <vector>

#include "core/ManagedConfigRevision.h"
#include "core/ManagedConfigApplyService.h"
#include "../src/core/ManagedConfigRevision.cpp"
#include "../src/core/ManagedConfigApplyService.cpp"

namespace {

JsonDocument parse(const char* text) {
    JsonDocument document;
    deserializeJson(document, text);
    return document;
}

class FakeAdapter final : public FastBee::ManagedConfigSectionAdapter {
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
    TEST_ASSERT_NOT_EQUAL(hashA.c_str(), FastBee::ManagedConfigRevision::sectionHash(reordered).c_str());

    std::vector<std::pair<String, String>> sections{{"protocol", "b"}, {"features", "a"}};
    const String globalA = FastBee::ManagedConfigRevision::globalRevision(sections);
    sections[0].second = "changed";
    TEST_ASSERT_NOT_EQUAL(globalA.c_str(), FastBee::ManagedConfigRevision::globalRevision(sections).c_str());
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

    JsonDocument pending = applyBody(fixture,
        "[{\"section\":\"peripherals\",\"schemaVersion\":1,\"patch\":{\"pendingRestart\":true}}]", "key-pending");
    TEST_ASSERT_TRUE(fixture.service.apply(pending, result, error).isEmpty());
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
    RUN_TEST(test_nested_secret_values_require_secret_channel);
    RUN_TEST(test_operation_and_body_bounds_are_explicit);
}
