#include "core/ManagedConfigRevision.h"

#include <algorithm>
#include <cstring>

namespace FastBee {
namespace {

struct StringWriter {
    std::string& output;
    size_t write(uint8_t value) {
        output.push_back(static_cast<char>(value));
        return 1;
    }
    size_t write(const uint8_t* values, size_t size) {
        output.append(reinterpret_cast<const char*>(values), size);
        return size;
    }
};

class Sha256 {
public:
    Sha256() {
        state_[0] = 0x6a09e667U;
        state_[1] = 0xbb67ae85U;
        state_[2] = 0x3c6ef372U;
        state_[3] = 0xa54ff53aU;
        state_[4] = 0x510e527fU;
        state_[5] = 0x9b05688cU;
        state_[6] = 0x1f83d9abU;
        state_[7] = 0x5be0cd19U;
    }

    void update(const uint8_t* data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            block_[blockSize_++] = data[i];
            if (blockSize_ == 64) {
                transform(block_);
                bitLength_ += 512;
                blockSize_ = 0;
            }
        }
    }

    void finish(uint8_t output[32]) {
        uint64_t totalBits = bitLength_ + blockSize_ * 8;
        block_[blockSize_++] = 0x80;
        if (blockSize_ > 56) {
            while (blockSize_ < 64) block_[blockSize_++] = 0;
            transform(block_);
            blockSize_ = 0;
        }
        while (blockSize_ < 56) block_[blockSize_++] = 0;
        for (uint8_t i = 0; i < 8; ++i) {
            block_[63 - i] = static_cast<uint8_t>(totalBits >> (i * 8));
        }
        transform(block_);
        for (uint8_t i = 0; i < 8; ++i) {
            output[i * 4] = static_cast<uint8_t>(state_[i] >> 24);
            output[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 16);
            output[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 8);
            output[i * 4 + 3] = static_cast<uint8_t>(state_[i]);
        }
    }

private:
    static uint32_t rotate(uint32_t value, uint8_t bits) {
        return (value >> bits) | (value << (32 - bits));
    }

    void transform(const uint8_t block[64]) {
        static const uint32_t constants[64] = {
            0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
            0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
            0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
            0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
            0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
            0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
            0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
            0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U,
        };
        uint32_t words[64];
        for (uint8_t i = 0; i < 16; ++i) {
            words[i] = (uint32_t(block[i * 4]) << 24) |
                       (uint32_t(block[i * 4 + 1]) << 16) |
                       (uint32_t(block[i * 4 + 2]) << 8) | block[i * 4 + 3];
        }
        for (uint8_t i = 16; i < 64; ++i) {
            const uint32_t s0 = rotate(words[i - 15], 7) ^ rotate(words[i - 15], 18) ^ (words[i - 15] >> 3);
            const uint32_t s1 = rotate(words[i - 2], 17) ^ rotate(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (uint8_t i = 0; i < 64; ++i) {
            const uint32_t s1 = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
            const uint32_t choice = (e & f) ^ (~e & g);
            const uint32_t t1 = h + s1 + choice + constants[i] + words[i];
            const uint32_t s0 = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
            const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = s0 + majority;
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    uint32_t state_[8];
    uint8_t block_[64];
    size_t blockSize_ = 0;
    uint64_t bitLength_ = 0;
};

bool canonicalString(const char* value, std::string& output) {
    if (!value) return false;
    JsonDocument document;
    document.set(value);
    if (document.overflowed()) return false;
    StringWriter writer{output};
    return serializeJson(document, writer) > 0;
}

bool canonicalApproval(const char* domain, JsonVariantConst first,
                       JsonVariantConst second, JsonVariantConst operations,
                       std::string& output) {
    output = domain;
    output.push_back('\n');
    if (!ManagedConfigRevision::canonicalJson(first, output)) return false;
    output.push_back('\n');
    if (!ManagedConfigRevision::canonicalJson(second, output)) return false;
    output.push_back('\n');
    return ManagedConfigRevision::canonicalJson(operations, output);
}

} // namespace

bool ManagedConfigRevision::canonicalJson(JsonVariantConst value, std::string& output,
                                          uint8_t depth) {
    if (depth > 16) return false;
    if (value.is<JsonObjectConst>()) {
        std::vector<std::string> keys;
        for (JsonPairConst field : value.as<JsonObjectConst>()) {
            const JsonString key = field.key();
            if (!key.c_str() || key.size() != std::strlen(key.c_str())) return false;
            keys.emplace_back(key.c_str(), key.size());
        }
        std::sort(keys.begin(), keys.end());
        if (std::adjacent_find(keys.begin(), keys.end()) != keys.end()) return false;
        output.push_back('{');
        bool first = true;
        for (const auto& key : keys) {
            if (!first) output.push_back(',');
            first = false;
            JsonDocument name;
            name.set(key);
            if (name.overflowed()) return false;
            StringWriter writer{output};
            if (serializeJson(name, writer) != key.size() + 2) return false;
            output.push_back(':');
            if (!canonicalJson(value[key.c_str()], output, depth + 1)) return false;
        }
        output.push_back('}');
        return true;
    }
    if (value.is<JsonArrayConst>()) {
        output.push_back('[');
        bool first = true;
        for (JsonVariantConst child : value.as<JsonArrayConst>()) {
            if (!first) output.push_back(',');
            first = false;
            if (!canonicalJson(child, output, depth + 1)) return false;
        }
        output.push_back(']');
        return true;
    }
    StringWriter writer{output};
    const size_t written = serializeJson(value, writer);
    return written > 0 || value.isNull();
}

String ManagedConfigRevision::sha256(const std::string& content) {
    Sha256 hash;
    hash.update(reinterpret_cast<const uint8_t*>(content.data()), content.size());
    uint8_t digest[32];
    hash.finish(digest);
    static const char hex[] = "0123456789abcdef";
    std::string output(64, '0');
    for (size_t i = 0; i < sizeof(digest); ++i) {
        output[i * 2] = hex[digest[i] >> 4];
        output[i * 2 + 1] = hex[digest[i] & 15];
    }
    return String(output.c_str());
}

String ManagedConfigRevision::sectionHash(JsonVariantConst redactedSection) {
    std::string canonical;
    return canonicalJson(redactedSection, canonical) ? sha256(canonical) : String();
}

String ManagedConfigRevision::globalRevision(
    const std::vector<std::pair<String, String>>& sectionHashes) {
    std::vector<std::pair<String, String>> sorted = sectionHashes;
    std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    std::string canonical = "fastbee-managed-config-v1\n";
    for (const auto& item : sorted) {
        if (item.first.isEmpty() || item.second.length() != 64) return "";
        if (!canonicalString(item.first.c_str(), canonical)) return "";
        canonical.push_back('=');
        canonical += item.second.c_str();
        canonical.push_back('\n');
    }
    return sha256(canonical);
}

String ManagedConfigRevision::draftHash(JsonVariantConst operations) {
    std::string canonical;
    return canonicalJson(operations, canonical) ? sha256(canonical) : String();
}

String ManagedConfigRevision::approvalHash(const String& baseRevision,
                                           const String& draftId,
                                           JsonVariantConst operations) {
    JsonDocument first;
    JsonDocument second;
    first.set(baseRevision.c_str());
    second.set(draftId.c_str());
    std::string canonical;
    return canonicalApproval("fastbee-mcp-approval-v1", first, second,
                             operations, canonical) ? sha256(canonical) : String();
}

String ManagedConfigRevision::rollbackApprovalHash(const String& operationId,
                                                   const String& currentRevision) {
    JsonDocument first;
    JsonDocument second;
    first.set(operationId.c_str());
    second.set(currentRevision.c_str());
    std::string canonical;
    return canonicalApproval("fastbee-mcp-rollback-v1", first,
                             second, JsonVariantConst(), canonical)
        ? sha256(canonical) : String();
}

} // namespace FastBee
