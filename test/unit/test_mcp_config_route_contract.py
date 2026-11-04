"""Static route and production-adapter contracts for the MCP configuration bridge."""

import json
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HANDLER_H = ROOT / "include/network/handlers/McpConfigRouteHandler.h"
HANDLER_CPP = ROOT / "src/network/handlers/McpConfigRouteHandler.cpp"
SERVICE_H = ROOT / "include/core/ManagedConfigApplyService.h"
SERVICE_CPP = ROOT / "src/core/ManagedConfigApplyService.cpp"
WEB = (ROOT / "src/network/WebConfigManager.cpp").read_text(encoding="utf-8")


def read(path):
    return path.read_text(encoding="utf-8")


def function_body(source, signature):
    match = re.search(signature, source)
    if not match:
        raise AssertionError(f"function not found: {signature}")
    opening = source.find("{", match.end())
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening:index + 1]
    raise AssertionError(f"unterminated function: {signature}")


class McpConfigRouteContractTests(unittest.TestCase):
    def test_every_section_adapter_exposes_the_required_contract(self):
        header = read(SERVICE_H)
        for method in (
            "section", "schemaVersion", "readRedacted", "validate", "preflight",
            "apply", "readback", "dependencies", "secretFields",
        ):
            self.assertRegex(header, rf"\b{method}\s*\(")
        source = read(SERVICE_CPP)
        for section in (
            "features", "peripherals", "protocol", "ruleScripts", "periphExec",
            "network", "aiKey",
        ):
            self.assertIn(f'"{section}"', source)

    def test_revision_uses_canonical_content_not_time(self):
        revision = read(ROOT / "src/core/ManagedConfigRevision.cpp")
        self.assertIn("canonical", revision)
        self.assertIn("sha256", revision.lower())
        self.assertNotIn("millis()", revision)
        self.assertNotIn("time(nullptr)", revision)

    def test_routes_cover_the_section_5_2_api(self):
        source = read(HANDLER_CPP)
        for route in (
            '"/api/mcp/capabilities"',
            '"/api/mcp/config/snapshot"',
            '"/api/mcp/config/preflight"',
            '"/api/mcp/config/apply"',
            '"/api/mcp/config/rollback"',
            '"/api/mcp/config/operations/"',
            '"/api/mcp/secrets/"',
        ):
            self.assertIn(route, source)
        setup = function_body(source, r"McpConfigRouteHandler::setupRoutes\s*\(")
        self.assertLess(setup.index('"/api/mcp/config/rollback"'), setup.index('"/api/mcp/config/operations/"'))

    def test_all_routes_require_auth_and_developer_mode_and_writes_are_bounded(self):
        source = read(HANDLER_CPP)
        self.assertIn("requireAuth", source)
        self.assertIn("requireDeveloperMode", source)
        self.assertIn("kMaxBodyBytes", read(HANDLER_H))
        self.assertIn("total > kMaxBodyBytes", source)
        self.assertIn("checkLowMemory", source)
        self.assertNotIn("SystemRebooter", source)
        self.assertNotIn("ESP.restart", source)

    def test_mutation_admission_body_cleanup_and_protocol_lifecycle(self):
        source = read(HANDLER_CPP)
        self.assertIn("requireSameOrigin", function_body(source, r"McpConfigRouteHandler::admit\s*\("))
        self.assertIn("using BodyOwner = std::unique_ptr<BodyBuffer>", source)
        self.assertIn("onDisconnect", source)
        self.assertIn("requestModbusStopAsync", read(SERVICE_CPP))
        self.assertNotIn("manager_->stopModbus()", read(SERVICE_CPP))
        for handler in ("setupRoutes", "handleRollback", "handleSecret"):
            body = function_body(source, rf"McpConfigRouteHandler::{handler}\s*\(")
            self.assertIn("setProtocolManager", body)
        self.assertIn('"/api/mcp/config/readback"', source)
        self.assertIn("recoverOperation", source)
        self.assertIn("idempotencyKey", function_body(source, r"McpConfigRouteHandler::handleSecret\s*\("))

    def test_migration_transaction_and_staging_use_existing_facility(self):
        source = read(SERVICE_CPP)
        self.assertIn("ConfigMigration::Storage::slotPath", source)
        self.assertNotIn("periph_exec.mcp.staged.json", source)
        self.assertNotIn("deleteJSONConfigGenerations", source)
        migration = read(ROOT / "include/systems/ConfigMigrationStorage.h")
        self.assertIn("ConfigTransaction::mutex", migration)
        self.assertIn("validateProtocolConfigDocument", read(ROOT / "src/systems/ConfigStorage.cpp"))

    def test_null_service_setup_is_guarded_and_requests_fail_503(self):
        source = read(HANDLER_CPP)
        setup = function_body(source, r"McpConfigRouteHandler::setupRoutes\s*\(")
        self.assertIn("service_ && ctx_->protocolManager", setup)
        admit = function_body(source, r"McpConfigRouteHandler::admit\s*\(")
        self.assertIn("if (!service_)", admit)
        self.assertIn("503", admit)

    def test_successful_apply_and_rollback_use_http_200(self):
        source = read(HANDLER_CPP)
        for handler in ("handleApply", "handleRollback"):
            body = function_body(source, rf"McpConfigRouteHandler::{handler}\s*\(")
            self.assertIn("error.isEmpty() ? 200 : errorStatus(error)", body)

    def test_handler_is_registered_before_static_fallback(self):
        self.assertIn('#include "./network/handlers/McpConfigRouteHandler.h"', WEB)
        setup = function_body(WEB, r"WebConfigManager::setupAllRoutes\s*\(")
        self.assertIn("McpConfigRouteHandler", setup)
        self.assertLess(setup.index("McpConfigRouteHandler"), setup.index("staticHandler->setupRoutes"))

    def test_production_adapters_reuse_managers_and_atomic_storage(self):
        source = read(SERVICE_CPP)
        for token in (
            "prepareConfigurationImport", "commitConfigurationImport",
            "importConfiguration", "importConfigurationFile", "updateAndSave",
            "estimateFromJson", "saveProtocolConfig", "ConfigMigration::validateGraph",
            "restoreMaskedSecrets",
        ):
            self.assertIn(token, source)
        self.assertNotIn("FILE_WRITE", source)
        self.assertNotIn("SystemRebooter", source)
        self.assertNotIn("ESP.restart", source)

    def test_device_matrix_contains_safe_capabilities_and_snapshot_checks(self):
        matrix = json.loads(read(ROOT / "scripts/device-api-test-matrix.json"))
        checks = {entry["name"]: entry for entry in matrix["checks"]}
        for name, method, path in (
            ("mcp-capabilities", "GET", "/api/mcp/capabilities"),
            ("mcp-config-snapshot", "GET", "/api/mcp/config/snapshot?sections=peripherals"),
        ):
            self.assertEqual(method, checks[name]["method"])
            self.assertEqual(path, checks[name]["path"])
            self.assertEqual("GET", checks[name]["method"])

    def test_device_section_adapter_is_writable_and_reuses_production_validation(self):
        source = read(SERVICE_CPP)
        # 可写 device 适配器复用生产 DeviceConfigValidation 候选校验
        body = function_body(source, r"class DeviceAdapter")
        self.assertIn('"device"', body)
        self.assertIn("FileSystem::DEVICE_CONFIG_FILE", body)
        self.assertIn("false", body)  # readOnly=false（网络/AI Key 保持只读）
        self.assertIn("validateDeviceSectionCandidate", body)
        # 生产校验头实现身份保留与允许列表
        validation = read(ROOT / "include/network/DeviceConfigValidation.h")
        self.assertIn("validateDeviceSectionCandidate", validation)
        self.assertIn("MIGRATION_IDENTITY_MUST_BE_PRESERVED", validation)
        self.assertIn("validateDeviceConfigPatch", validation)
        for field in ("deviceName", "location", "description", "enableNTP", "ntpServer1",
                      "ntpServer2", "timezone", "syncInterval", "logLevel", "cacheDuration"):
            self.assertIn(f'"{field}"', validation)
        # 注册并计入默认适配器数
        self.assertIn("registerAdapter(&defaults_->device)", source)
        self.assertIn("adapters_.size() == 8", source)
        # 网络与 AI Key 保持只读
        self.assertIn('StorageSectionAdapter("network", FileSystem::NETWORK_CONFIG_FILE, true', source)
        self.assertIn('StorageSectionAdapter("aiKey", "", true', source)

    def test_mcp_compile_capability_is_independent_from_ai(self):
        flags = read(ROOT / "include/core/FeatureFlags.h")
        self.assertIn("FASTBEE_ENABLE_MCP_CONFIG", flags)
        web = read(ROOT / "src/network/WebConfigManager.cpp")
        self.assertIn("#if FASTBEE_ENABLE_MCP_CONFIG", web)
        profile = read(ROOT / "scripts/web-profile.js")
        self.assertIn("mcpConfigBridge: 'FASTBEE_ENABLE_MCP_CONFIG'", profile)
        matrix = json.loads(read(ROOT / "scripts/device-api-test-matrix.json"))
        checks = {entry["name"]: entry for entry in matrix["checks"]}
        self.assertEqual("/api/mcp/config/snapshot?sections=peripherals", checks["mcp-config-snapshot"]["path"])
        for name, method, path in (
            ("mcp-config-preflight", "POST", "/api/mcp/config/preflight"),
            ("mcp-config-readback", "POST", "/api/mcp/config/readback"),
        ):
            self.assertEqual(method, checks[name]["method"])
            self.assertEqual(path, checks[name]["path"])
            self.assertTrue(checks[name]["skipSoak"])

    def test_capabilities_publish_real_request_and_result_limits(self):
        source = read(SERVICE_CPP)
        capabilities = function_body(source, r"ManagedConfigApplyService::capabilities\s*\(")
        self.assertIn("maxApplyBodyBytes", capabilities)
        self.assertIn("maxSnapshotResultBytes", capabilities)
        self.assertIn("maxOperations", capabilities)
        self.assertIn("kMaxRequestBodyBytes", read(SERVICE_H))

    def test_internal_uncompiled_feature_intent_does_not_block_unrelated_config(self):
        service = read(SERVICE_CPP)
        self.assertRegex(service, r"enabled[^\n]*&&[^\n]*metadata->displayName[^\n]*&&\s*!metadata->compiled")
        migration = read(ROOT / "src/systems/ConfigMigrationValidation.cpp")
        self.assertIn("feature->displayName && !feature->compiled", migration)

    def test_legacy_screen_panel_without_touch_migration_remains_readable(self):
        migration = read(ROOT / "src/systems/ConfigMigrationValidation.cpp")
        self.assertRegex(migration, r'!document\["touchUiMigration"\]\.isNull\(\)\s*&&\s*document\["touchUiMigration"\]\s*!=\s*2')

    def test_digital_input_is_a_valid_event_source_without_sampling_action(self):
        migration = read(ROOT / "src/systems/ConfigMigrationValidation.cpp")
        trigger_source = function_body(migration, r"bool triggerSource\(const String& ref\)const")
        self.assertIn("isGpioDigitalInputType", trigger_source)
        self.assertIn("enabled", trigger_source)

    def test_legacy_null_rule_script_list_is_an_empty_collection(self):
        migration = read(ROOT / "src/systems/ConfigMigrationValidation.cpp")
        self.assertIn('document.size()==1&&document["rules"].isNull()', migration)

    def test_mqtt_test_route_is_bounded_and_uses_production_lifecycle(self):
        handler = read(HANDLER_CPP)
        self.assertIn('"/api/mcp/mqtt/test"', handler)
        self.assertIn("handleMqttTest", handler)
        self.assertIn("handleMqttTestStatus", handler)
        self.assertIn("setMqttTestHooks", handler)
        body = function_body(handler, r"McpConfigRouteHandler::handleMqttTest\s*\(")
        self.assertIn("admit(request, true)", body)
        service = read(SERVICE_CPP)
        mqtt_test = function_body(service, r"ManagedConfigApplyService::mqttTest\s*\(")
        # 屏障门控 + revision 绑定 + 秘密只来自设备本地配置 + 单协议分节限制
        self.assertIn("checkUnknownGate", mqtt_test)
        self.assertIn("checkManualRecoveryGate", mqtt_test)
        self.assertIn("CONFIG_REVISION_CONFLICT", mqtt_test)
        self.assertIn("maskedSecret", mqtt_test)
        self.assertIn("readPlain", mqtt_test)
        self.assertIn("MQTT test accepts only protocol operations", mqtt_test)
        # 生产测试生命周期：暂存/恢复机制 + 受控重建，无自动重连循环
        routes = read(ROOT / "src/network/handlers/MqttRouteHandler.cpp")
        starter = function_body(routes, r"MqttRouteHandler::startMcpConnectionTest\s*\(")
        self.assertIn("validateMqttCandidate", starter)
        self.assertIn("backupMqttConfig", routes)
        self.assertIn("restartMQTTDeferred", starter)
        self.assertIn("compareExchangeMqttConfig", routes)
        self.assertNotIn("while", starter)
        status = function_body(routes, r"MqttRouteHandler::mcpConnectionTestStatus\s*\(")
        self.assertIn("MQTT_TEST_NOT_FOUND", status)
        # 恢复结局按 testId 记录
        self.assertIn("s_mcpTestOutcome = {s_mqttTestId", routes)


if __name__ == "__main__":
    unittest.main()
