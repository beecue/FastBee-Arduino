"""Contract checks for the browser MCP connector (Task 3).

The connector runs in the device web page and forwards whitelisted cloud jobs
to same-origin device APIs. These tests pin the security-relevant invariants of
``web-src/modules/ai/ai-mcp-connector.js`` so later edits cannot silently weaken
token storage, job whitelisting, or poll bounds.
"""

from __future__ import annotations

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE_PATH = ROOT / "web-src" / "modules" / "ai" / "ai-mcp-connector.js"
SOURCE = SOURCE_PATH.read_text(encoding="utf-8")

# 与 Task 1 设备契约清单一致的只读分节路由
SECTION_PATHS = {
    "device": "/api/device/config",
    "features": "/api/features/config",
    "peripherals": "/api/peripherals",
    "periphExec": "/api/periph-exec",
    "ruleScripts": "/api/rule-script",
    "protocol": "/api/protocol/config",
    "network": "/api/network/config",
    "screenPanel": "/api/screen-panel/config",
}


class AiMcpConnectorContractTests(unittest.TestCase):
    def test_token_only_lives_in_session_storage(self):
        self.assertIn("sessionStorage", SOURCE)
        self.assertIn("fastbee.mcp.connector.session", SOURCE)
        self.assertNotIn("localStorage", SOURCE)
        self.assertNotIn("document.cookie", SOURCE)

    def test_token_never_enters_url_or_logs(self):
        self.assertNotIn("access_token=", SOURCE)
        self.assertNotIn("?token=", SOURCE)
        self.assertNotIn("&token=", SOURCE)
        self.assertNotIn("console.log", SOURCE)
        self.assertNotIn("console.warn", SOURCE)
        self.assertNotIn("console.error", SOURCE)
        # token 只允许出现在 Authorization 头
        self.assertIn("'Bearer '", SOURCE)

    def test_poll_is_bounded_to_twenty_seconds(self):
        self.assertRegex(SOURCE, r"MAX_POLL_WAIT_MS\s*=\s*20000")
        self.assertIn("Math.min", SOURCE)

    def test_job_whitelist_matches_device_contract(self):
        for job_type in (
            "device.info.read",
            "device.capabilities.read",
            "device.config.section.read",
        ):
            self.assertIn(job_type, SOURCE)
        for section, path in SECTION_PATHS.items():
            self.assertIn(f"'{path}'", SOURCE, f"missing section path for {section}")
        # 云端任务不得携带 URL/任意方法：连接器必须显式拒绝这些键
        for key in ("'url'", "'method'", "'headers'", "'body'", "'path'"):
            self.assertIn(key, SOURCE)
        self.assertIn("INVALID_ARGUMENT", SOURCE)

    def test_device_calls_are_same_origin_and_writes_are_fixed_config_jobs(self):
        self.assertIn("_deviceGet", SOURCE)
        self.assertIn("_devicePost", SOURCE)
        for job_type, path in (
            ("device.config.preflight", "/api/mcp/config/preflight"),
            ("device.config.apply", "/api/mcp/config/apply"),
            ("device.config.readback", "/api/mcp/config/readback"),
            ("device.config.rollback", "/api/mcp/config/rollback"),
        ):
            self.assertIn(job_type, SOURCE)
            self.assertIn(f"'{path}'", SOURCE)
        self.assertIn("OUTCOME_UNKNOWN", SOURCE)
        for forbidden in ("/api/ota", "/api/system/restart", "/api/system/factory-reset", "/api/users"):
            self.assertNotIn(forbidden, SOURCE)

    def test_mqtt_test_job_is_bounded_and_secret_free(self):
        # 有界 MQTT 连接测试任务：固定路由、只携带草案引用与脱敏候选操作
        self.assertIn("'device.mqtt.test'", SOURCE)
        self.assertIn("'/api/mcp/mqtt/test'", SOURCE)
        handler = re.search(r"'device\.mqtt\.test':\s*function\s*\(job\)\s*\{(.*?)\n        \}", SOURCE,
                            re.DOTALL)
        self.assertIsNotNone(handler, "missing device.mqtt.test handler")
        body = handler.group(1)
        self.assertIn("job.draftId", body)
        self.assertIn("job.baseRevision", body)
        self.assertIn("job.operations", body)
        # 秘密绝不随任务载荷传输
        for secret in ("password", "mqttSecret", "authCode"):
            self.assertNotIn(secret, body)
        # 有界轮询：间隔 + 上限 + 尊重任务截止时间，无自动重试循环
        poll = re.search(r"function _pollMqttTest\(.*?\n    \}", SOURCE, re.DOTALL)
        self.assertIsNotNone(poll, "missing _pollMqttTest")
        pollBody = poll.group(0)
        self.assertIn("MQTT_TEST_POLL_INTERVAL_MS", pollBody)
        self.assertIn("MQTT_TEST_MAX_WAIT_MS", pollBody)
        self.assertIn("MQTT_TEST_TIMEOUT", pollBody)
        self.assertIn("deadlineAt", pollBody)
        # 结果按真实结局表达：连接失败不等于成功
        self.assertIn("MQTT_CONNECT_FAILED", pollBody)
        # 启动响应必须有显式 started + 合法 testId
        self.assertIn("data.started === true", SOURCE)
        # 任务形状校验：draftId/baseRevision/operations 必填
        self.assertRegex(SOURCE, r"job\.type === 'device\.mqtt\.test'")

    def test_leave_page_stops_polling_and_reports_state(self):
        self.assertIn("pagehide", SOURCE)
        self.assertIn("visibilitychange", SOURCE)
        self.assertIn("'offline'", SOURCE)
        self.assertIn("AbortController", SOURCE)
        self.assertIn("keepalive", SOURCE)

    def test_bfcache_pageshow_resumes_polling(self):
        self.assertIn("pageshow", SOURCE)
        self.assertIn("persisted", SOURCE)
        self.assertIn("_resume", SOURCE)

    def test_consecutive_failures_are_bounded(self):
        match = re.search(r"MAX_CONSECUTIVE_FAILURES\s*=\s*(\d+)", SOURCE)
        self.assertIsNotNone(match, "missing MAX_CONSECUTIVE_FAILURES")
        limit = int(match.group(1))
        self.assertGreaterEqual(limit, 3)
        self.assertLessEqual(limit, 100)
        self.assertIn("_haltUnreachable", SOURCE)
        self.assertIn("'unreachable'", SOURCE)

    def test_result_payload_is_bounded(self):
        self.assertRegex(SOURCE, r"MAX_RESULT_BYTES\s*=\s*\d+")
        self.assertIn("RESULT_TOO_LARGE", SOURCE)

    def test_session_clear_on_stop_and_revoke(self):
        self.assertIn("sessionStorage.removeItem", SOURCE)
        self.assertIn("/connector/v1/revoke", SOURCE)
        self.assertIn("/connector/v1/token/refresh", SOURCE)
        self.assertIn("/connector/v1/heartbeat", SOURCE)
        self.assertIn("/connector/v1/jobs/poll", SOURCE)
        self.assertIn("/connector/v1/pair", SOURCE)

    def test_refresh_margin_and_result_idempotency_key(self):
        match = re.search(r"TOKEN_REFRESH_MARGIN_MS\s*=\s*(\d+)", SOURCE)
        self.assertIsNotNone(match, "missing TOKEN_REFRESH_MARGIN_MS")
        self.assertGreaterEqual(int(match.group(1)), 30_000)
        self.assertIn("idempotencyKey", SOURCE)
        self.assertIn("leaseExpiresAt", SOURCE)

    def test_pairing_and_capability_refresh_carry_peripheral_catalog(self):
        # 配对与能力刷新都必须读取真实外设类型目录（云端外设工具的 schema 来源）
        self.assertEqual(SOURCE.count("'/api/peripherals/types'"), 2)
        self.assertIn("_buildCapabilities", SOURCE)
        self.assertIn("peripheralCatalog", SOURCE)
        # 有界上限：超限放弃目录而不是截断伪造；目录缺失显式标记
        match = re.search(r"MAX_CAPABILITIES_BYTES\s*=\s*(\d+)", SOURCE)
        self.assertIsNotNone(match, "missing MAX_CAPABILITIES_BYTES")
        self.assertGreaterEqual(int(match.group(1)), 8192)
        self.assertLessEqual(int(match.group(1)), 16 * 1024)
        self.assertIn("peripheralCatalogAvailable", SOURCE)
        self.assertIn("peripheralCatalogOversize", SOURCE)
        # 大小口径必须与云端存储一致：UTF-8 字节数，而非 UTF-16 长度
        self.assertIn("TextEncoder", SOURCE)

    def test_pairing_and_capability_refresh_carry_execution_catalog(self):
        # 配对与能力刷新都必须携带真实执行目录（云端规则工具的 trigger/event/action schema 来源）：
        # 生产只读路由 trigger-types / events/static / action-types
        self.assertIn("'/api/periph-exec/trigger-types'", SOURCE)
        self.assertIn("'/api/periph-exec/events/static'", SOURCE)
        self.assertIn("'/api/periph-exec/action-types'", SOURCE)
        self.assertIn("'/api/periph-exec/controls'", SOURCE)
        self.assertIn("'/api/mcp/config/snapshot'", SOURCE)
        self.assertIn("_fetchExecutionCatalog", SOURCE)
        self.assertIn("executionCatalog", SOURCE)
        # 目录缺失/越界显式标记，云端失败关闭；超限时先放弃执行目录
        self.assertIn("executionCatalogAvailable", SOURCE)
        self.assertIn("executionCatalogOversize", SOURCE)
        # 目录携带 schema/firmware/revision 元数据（云端兼容性门禁与新鲜度绑定来源）；
        # 拉取失败/畸形显式标记 failed（能力刷新据此保留旧注册）
        self.assertIn("schemaVersion", SOURCE)
        self.assertIn("firmwareVersion", SOURCE)
        self.assertIn("configRevision", SOURCE)
        self.assertIn("executionCatalogFailed", SOURCE)

    def test_snapshot_is_section_scoped_and_revision_has_a_bounded_channel(self):
        self.assertIn("'device.config.revision.read'", SOURCE)
        self.assertIn("readGlobalRevision", SOURCE)
        self.assertIn("'/api/mcp/capabilities'", SOURCE)
        snapshot = re.search(r"'device\.config\.snapshot\.read':\s*function\s*\(job\)\s*\{(.*?)\n        \}", SOURCE,
                             re.DOTALL)
        self.assertIsNotNone(snapshot)
        self.assertIn("job.section", snapshot.group(1))
        self.assertIn("sections=", snapshot.group(1))

    def test_execution_catalog_rows_are_strictly_validated(self):
        # 畸形/重复目录行不得被接受：必填字段类型化非空、ID/类型值唯一，整体放弃而不是挑选部分行
        for validator in ("_validTriggerTypes", "_validEvents", "_validActionTypes", "_validControls"):
            self.assertIn(validator, SOURCE)
        self.assertIn("seen[", SOURCE)  # 唯一性检查
        self.assertIn("requiresPeripheralTarget", SOURCE)
        self.assertIn("valueSchema", SOURCE)  # 生产动作值 schema 行校验
        # 能力刷新附带固件版本供云端写回注册
        self.assertIn("'/api/system/info?brief=1'", SOURCE)


if __name__ == "__main__":
    unittest.main()
