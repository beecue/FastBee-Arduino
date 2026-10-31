#ifndef FASTBEE_CONFIG_TRANSACTION_H
#define FASTBEE_CONFIG_TRANSACTION_H

#include <mutex>

namespace FastBee {
namespace ConfigTransaction {

inline std::recursive_mutex& mutex() {
    static std::recursive_mutex value;
    return value;
}

} // namespace ConfigTransaction
} // namespace FastBee

#endif // FASTBEE_CONFIG_TRANSACTION_H
