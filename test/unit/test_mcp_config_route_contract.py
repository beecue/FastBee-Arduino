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
            ("mcp-config-snapshot", "GET", "/api/mcp/config/snapshot"),
        ):
            self.assertEqual(method, checks[name]["method"])
            self.assertEqual(path, checks[name]["path"])
            self.assertEqual("GET", checks[name]["method"])


if __name__ == "__main__":
    unittest.main()
