from __future__ import annotations

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
ROUTE = ROOT / "src/network/handlers/AiRouteHandler.cpp"
HEADER = ROOT / "include/network/handlers/AiRouteHandler.h"


class AiWebContractTests(unittest.TestCase):
    def test_routes_and_security_exist(self):
        self.assertTrue(ROUTE.exists(), "AI route implementation is missing")
        self.assertTrue(HEADER.exists(), "AI route header is missing")
        source = (ROUTE.read_text(encoding="utf-8") + HEADER.read_text(encoding="utf-8")
              + (ROOT / "src/core/AiConfig.cpp").read_text(encoding="utf-8"))
        for path in (
            "/api/ai/config", "/api/ai/mcp-config", "/api/ai/jobs",
            "/api/ai/jobs/", "/api/ai/proposals/", "/api/ai/session",
        ):
            self.assertIn(path, source)
        self.assertIn("requireAuth", source)
        self.assertIn("requireDeveloperMode", source)
        self.assertIn("requireSameOrigin", source)
        self.assertIn("confirmationText", source)
        self.assertIn("apiKeyConfigured", source)
        self.assertNotIn("setInsecure", source)

    def test_route_is_bounded_and_never_enables_feature(self):
        source = (ROUTE.read_text(encoding="utf-8") + HEADER.read_text(encoding="utf-8")
              + (ROOT / "src/core/AiConfig.cpp").read_text(encoding="utf-8"))
        self.assertRegex(source, r"kMaxBodyBytes\s*=\s*(?:16384|32768)")
        self.assertIn("12288", source)
        self.assertIn("2000", source)
        self.assertNotIn("setUserEnabled", source)
        self.assertNotIn("TaskManager::pause", source)
        self.assertNotRegex(source, r"output\[.apiKey.\]\s*=")

    def test_only_f16r8_compiles_ai(self):
        ini = (ROOT / "platformio.ini").read_text(encoding="utf-8")
        f16 = ini.split("[env:esp32s3-F16R8]", 1)[1].split("[env:", 1)[0]
        self.assertIn("-DFASTBEE_ENABLE_AI=1", f16)
        for name in ("esp32-F4R0", "esp32-F8R0", "esp32-F8R8", "esp32c3-F4R0",
                     "esp32c6-F4R0", "esp32c6-F8R0", "esp32s3-F8R0", "esp32s3-F8R8"):
            section = ini.split(f"[env:{name}]", 1)[1].split("[env:", 1)[0]
            self.assertNotIn("-DFASTBEE_ENABLE_AI=1", section, name)

    def test_default_feature_intent_is_off(self):
        for relative in ("data/config/features.json", "test/fixtures/factory-config/features.json"):
            text = (ROOT / relative).read_text(encoding="utf-8")
            self.assertRegex(text, r'"aiAssistant"\s*:\s*\{\s*"enabled"\s*:\s*false\s*\}')

    def test_worker_and_session_resources_are_bounded(self):
        source = (ROOT / "src/core/AiConversationManager.cpp").read_text(encoding="utf-8")
        proposals = (ROOT / "include/core/AiProposalService.h").read_text(encoding="utf-8")
        self.assertIn("xTaskCreate", source)
        self.assertIn("8192", source)
        self.assertIn("histories_.size() >= 8", source)
        self.assertIn("mutex", proposals)

    def test_ai_sources_are_compile_gated(self):
        for relative in (
            "src/core/AiConfig.cpp", "src/core/AiConversationManager.cpp",
            "src/core/AiProposalService.cpp", "src/core/AiMcpToolBridge.cpp",
            "src/core/PeripheralChangeExecutor.cpp",
            "src/protocols/AiProviderClient.cpp", "src/network/handlers/AiRouteHandler.cpp",
        ):
            source = (ROOT / relative).read_text(encoding="utf-8")
            self.assertIn("#if FASTBEE_ENABLE_AI || defined(UNIT_TEST)", source, relative)
            self.assertTrue(source.rstrip().endswith("#endif"), relative)

    def test_bearer_and_cookie_sessions_have_distinct_owner_binding(self):
        source = (ROOT / "src/network/handlers/AiRouteHandler.cpp").read_text(encoding="utf-8")
        self.assertIn('hasHeader("Authorization")', source)
        self.assertIn('request->header("Authorization")', source)

    def test_repeated_denied_requests_release_accumulated_body(self):
        source = (ROOT / "src/network/handlers/AiRouteHandler.cpp").read_text(encoding="utf-8")
        admit = source.split("bool AiRouteHandler::admit", 1)[1].split("void AiRouteHandler::accumulateBody", 1)[0]
        for _ in range(100):
            self.assertIn("releaseBody(request);", admit)
        self.assertIn("return allowed;", admit)


if __name__ == "__main__":
    unittest.main()
