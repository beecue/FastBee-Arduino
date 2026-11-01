#ifndef PERIPHERAL_CHANGE_EXECUTOR_H
#define PERIPHERAL_CHANGE_EXECUTOR_H

#include <Arduino.h>

class FBNetworkManager;
struct PeripheralConfig;

class PeripheralChangeExecutor {
public:
    static bool add(const PeripheralConfig& config, FBNetworkManager* network,
                    uint32_t acknowledgements, String& error);
};

#endif
