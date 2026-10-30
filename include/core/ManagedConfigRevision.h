#ifndef MANAGED_CONFIG_REVISION_H
#define MANAGED_CONFIG_REVISION_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <string>
#include <utility>
#include <vector>

namespace FastBee {

class ManagedConfigRevision {
public:
    static bool canonicalJson(JsonVariantConst value, std::string& output,
                              uint8_t depth = 0);
    static String sha256(const std::string& content);
    static String sectionHash(JsonVariantConst redactedSection);
    static String globalRevision(
        const std::vector<std::pair<String, String>>& sectionHashes);
    static String draftHash(JsonVariantConst operations);
    static String approvalHash(const String& baseRevision,
                               const String& draftId,
                               JsonVariantConst operations);
    static String rollbackApprovalHash(const String& operationId,
                                       const String& currentRevision);
};

} // namespace FastBee

#endif // MANAGED_CONFIG_REVISION_H
