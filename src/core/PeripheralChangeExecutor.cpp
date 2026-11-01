#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_AI || defined(UNIT_TEST)
#include "core/PeripheralChangeExecutor.h"

#include <new>

#include "core/ChipConfig.h"
#include "core/AsyncExecTypes.h"
#include "core/PeripheralManager.h"
#include "core/PeripheralResourcePlanner.h"
#include "core/PeripheralPreflightRouteSupport.h"
#include "network/NetworkManager.h"
#include "network/NetworkResourceConfig.h"

bool PeripheralChangeExecutor::add(const PeripheralConfig& config, FBNetworkManager* network,
                                   uint32_t acknowledgements, String& error) {
    PeripheralManager& manager = PeripheralManager::getInstance();
    RecursiveMutexGuard lock(manager.getMutex(), pdMS_TO_TICKS(500));
    if (!lock.isLocked()) {
        error = "PREFLIGHT_BUSY";
        return false;
    }
    if (FastBee::compiledBoardProfileSupported()) {
        const auto& profile = FastBee::getCompiledBoardProfile();
        std::unique_ptr<FastBee::PreflightResult> result(new (std::nothrow) FastBee::PreflightResult());
        if (!result) {
            error = "AI_RESOURCE_UNAVAILABLE";
            return false;
        }
        FastBee::PreflightContext context;
        context.acknowledgements = acknowledgements;
        if (network) {
            context.ethernet = FastBee::ethernetResourceConfig(network->getConfig());
            context.cellular = FastBee::cellularResourceConfig(network->getConfig());
        }
        const std::vector<PeripheralConfig> existing = manager.getAllPeripherals();
        FastBee::preflightPeripheralChange(profile, existing.data(), existing.size(), config,
            FastBee::PreflightOperation::ADD, "", context, *result);
        if (!result->allowed || result->requiresConfirmation) {
            error = "PREFLIGHT_FAILED";
            return false;
        }
    }
    return manager.addPeripheralAndSave(config, error);
}
#endif
