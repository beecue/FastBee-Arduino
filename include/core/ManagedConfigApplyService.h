#ifndef MANAGED_CONFIG_APPLY_SERVICE_H
#define MANAGED_CONFIG_APPLY_SERVICE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <mutex>
#include <vector>

#include "core/ManagedConfigRevision.h"

class ProtocolManager;

namespace FastBee {

struct ManagedPreflightResult {
    bool allowed = true;
    bool pendingRestart = false;
    String error;
};

enum class ManagedAdapterApplyStatus : uint8_t {
    Persisted,
    PendingRestart,
    SaveFailed,
    Unknown,
    Rejected,
};

struct ManagedAdapterApplyResult {
    ManagedAdapterApplyStatus status = ManagedAdapterApplyStatus::Rejected;
    bool pendingRestart = false;
    String error;
};

class ManagedConfigSectionAdapter {
public:
    virtual ~ManagedConfigSectionAdapter() = default;
    virtual const char* section() const = 0;
    virtual uint32_t schemaVersion() const = 0;
    virtual bool readRedacted(JsonDocument& output, String& error) = 0;
    virtual bool validate(JsonVariantConst candidate, String& error) = 0;
    virtual bool preflight(JsonVariantConst candidate, ManagedPreflightResult& result,
                           String& error) = 0;
    virtual bool preflightGraph(JsonVariantConst candidate, JsonVariantConst,
                                ManagedPreflightResult& result, String& error) {
        return preflight(candidate, result, error);
    }
    virtual ManagedAdapterApplyResult apply(JsonVariantConst candidate, String& error) = 0;
    virtual bool readback(JsonVariantConst expected, String& error) = 0;
    virtual const char* const* dependencies(size_t& count) const = 0;
    virtual const char* const* secretFields(size_t& count) const = 0;
    virtual bool readOnly() const { return false; }
    virtual bool writeSecret(const String& secretRef, const String& value, String& error) {
        (void)secretRef;
        (void)value;
        (void)error;
        return false;
    }
};

class ManagedConfigApplyService {
public:
    static constexpr size_t kMaxAdapters = 10;
    static constexpr size_t kMaxOperations = 8;
    static constexpr size_t kMaxOperationHistory = 8;
    static constexpr size_t kMaxAggregateBytes = 96 * 1024;
    static constexpr size_t kMaxHistoryBytes = 96 * 1024;

    explicit ManagedConfigApplyService(bool registerDefaults = true);
    ~ManagedConfigApplyService();

    ManagedConfigApplyService(const ManagedConfigApplyService&) = delete;
    ManagedConfigApplyService& operator=(const ManagedConfigApplyService&) = delete;

    bool registerAdapter(ManagedConfigSectionAdapter* adapter);
    bool capabilities(JsonDocument& output, String& error);
    bool snapshot(const std::vector<String>& sections, JsonDocument& output, String& error);
    bool preflight(JsonVariantConst request, JsonDocument& output, String& error);
    String apply(JsonVariantConst request, JsonDocument& output, String& error);
    bool operation(const String& operationId, JsonDocument& output, String& error);
    bool recoverOperation(const String& operationId, JsonDocument& output, String& error);
    String rollback(JsonVariantConst request, JsonDocument& output, String& error);
    bool storeSecret(const String& secretRef, const String& value,
                     const String& idempotencyKey, String& error);

#ifndef UNIT_TEST
    void setProtocolManager(ProtocolManager* manager);
#endif

private:
    struct Candidate {
        ManagedConfigSectionAdapter* adapter = nullptr;
        JsonDocument value;
        String sectionBaseRevision;
        bool deletion = false;
    };

    struct StepRecord {
        String section;
        String state = "PENDING";
        bool accepted = false;
        bool persisted = false;
        bool readback = false;
        bool observed = false;
        bool physicalConfirmed = false;
        bool pendingRestart = false;
        String error;
        JsonDocument previous;
        JsonDocument expected;
    };

    struct OperationRecord {
        String id;
        String idempotencyKey;
        String requestHash;
        String parentOperationId;
        String state;
        String error;
        bool manualRecoveryRequired = false;
        bool readbackRequired = false;
        bool pendingRestart = false;
        bool rollback = false;
        size_t stepCount = 0;
        StepRecord steps[kMaxOperations];
    };

    struct SecretRecord {
        String key;
        String requestHash;
    };

    mutable std::mutex mutex_;
    std::vector<ManagedConfigSectionAdapter*> adapters_;
    std::vector<OperationRecord> operations_;
    std::vector<SecretRecord> secrets_;
    uint32_t nextOperation_ = 1;
    size_t historyBytes_ = 0;
    bool initialized_ = true;
    String unresolvedOperationId_;
    String manualRecoveryOperationId_;
    struct DefaultAdapters;
    DefaultAdapters* defaults_ = nullptr;

    ManagedConfigSectionAdapter* adapter(const String& section) const;
    String currentRevision(String* error = nullptr);
    String currentSectionRevision(ManagedConfigSectionAdapter& adapter, String& error) const;
    bool readValue(ManagedConfigSectionAdapter& adapter, JsonDocument& output,
                   String& error) const;
    bool parseCandidates(JsonVariantConst operations, std::vector<Candidate>& candidates,
                         JsonDocument& output, String& error);
    bool buildCandidate(Candidate& candidate, JsonVariantConst patch, JsonDocument& output,
                        String& error);
    bool checkCandidate(Candidate& candidate, JsonDocument& output, String& error);
    bool buildCandidateGraph(const std::vector<Candidate>& candidates, JsonDocument& graph,
                             String& error);
    bool combinedPreflight(std::vector<Candidate>& candidates, JsonDocument& output,
                           String& error);
    bool checkUnknownGate(JsonDocument& output, String& error) const;
    bool checkManualRecoveryGate(JsonDocument& output, String& error,
                                 const String& allowedOperationId = "") const;
    void orderCandidates(std::vector<Candidate>& candidates, bool reverse) const;
    bool checkRevision(const Candidate& candidate, String& error) const;
    OperationRecord* findIdempotency(const String& key);
    OperationRecord* findOperation(const String& id);
    String newOperationId(const String& key, const String& requestHash);
    void serializeOperation(const OperationRecord& operation, JsonDocument& output) const;
    void clearCandidates(std::vector<Candidate>& candidates);
    void clearDefaults();
#ifndef UNIT_TEST
    void registerDefaultAdapters();
#endif
};

} // namespace FastBee

#endif // MANAGED_CONFIG_APPLY_SERVICE_H
