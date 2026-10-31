"""Contract checks for the production action type catalog (Task 7 fix).

``include/core/ActionTypeCatalog.h`` drives ``/api/periph-exec/action-types``
and the MCP pairing execution catalog. These tests compile the production
header on the host (no hardware) and pin:

- 目录动作与 ExecActionType 枚举一一对应（名称/值，含保留/退役号）；
- requiresPeripheralTarget 与 matchesPeripheralTarget 的生产谓词结果
  与生成文档 docs/generated-action-target-contracts.md 的普通模式表一致；
- 目录携带 schemaVersion/firmwareVersion（云端新鲜度/兼容性绑定来源）；
- 路由只读、需认证、不走任意输入。

目录是设备动作语义的唯一权威来源；云端不再维护静态镜像。
"""
from pathlib import Path
import json
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HEADER = (ROOT / 'include/core/ActionTypeCatalog.h').read_text(encoding='utf-8')
ROUTE_CPP = (ROOT / 'src/network/handlers/PeriphExecRouteHandler.cpp').read_text(encoding='utf-8')
ROUTE_H = (ROOT / 'include/network/handlers/PeriphExecRouteHandler.h').read_text(encoding='utf-8')

PRODUCTION_CONFIG_PATHS = (
    'docs/device-test-configs/com15-esp32s3-f16r8/config/periph_exec.json',
    'docs/device-test-configs/com8-com9-esp32s3-touch/config/periph_exec.json',
    'docs/device-test-configs/com4-esp32c3-f4r0/config/periph_exec.json',
    'docs/c6-rgb-led-f4r0/periph_exec.json',
    'docs/generic-i2c-peripherals/periph_exec.json',
    'docs/waveshare-esp32s3-relay6ch/periph_exec.json',
)
PRODUCTION_ACTIONS = [
    (path, rule['id'], action)
    for path in PRODUCTION_CONFIG_PATHS
    for rule in json.loads((ROOT / path).read_text(encoding='utf-8'))['rules']
    for action in rule['actions']
]


def production_action_checks():
    checks = []
    for index, (_path, _rule_id, action) in enumerate(PRODUCTION_ACTIONS):
        value = action.get('actionValue', '')
        assert isinstance(value, str)
        literal = json.dumps(value, ensure_ascii=False)
        received = 'true' if action.get('useReceivedValue', False) else 'false'
        checks.append(f'    if (FastBee::validateRuleActionValue({action["actionType"]}, '
                      f'{literal}, {received})) return {100 + index};')
    return '\n'.join(checks)


HARNESS = r'''
#include <cstdio>
#include "core/ActionTypeCatalog.h"
int main() {
    JsonDocument doc;
    FastBee::ActionTypeCatalog::writeActionTypeCatalog(doc.to<JsonObject>());
    if (doc.overflowed()) return 2;
    std::printf("S %u %s\n", unsigned(doc["schemaVersion"].as<unsigned>()),
                doc["firmwareVersion"].as<const char*>());
    for (uint8_t value : FastBee::ActionTypeCatalog::PERIPHERAL_TYPE_VALUES)
        std::printf("T %u\n", unsigned(value));
    for (JsonObject item : doc["actions"].as<JsonArray>()) {
        std::printf("A %u %s %u", unsigned(item["value"].as<unsigned>()),
                    item["name"].as<const char*>(),
                    unsigned(item["requiresPeripheralTarget"].as<bool>()));
        if (item["targetTypes"].is<JsonArray>()) {
            JsonArray types = item["targetTypes"].as<JsonArray>();
            for (JsonVariant type : types)
                std::printf(" %u", unsigned(type.as<unsigned>()));
        }
        std::printf("\n");
        JsonObject schema = item["valueSchema"].as<JsonObject>();
        std::printf("V %u %s", unsigned(item["value"].as<unsigned>()),
                    schema["kind"].as<const char*>());
        if (schema["min"].is<long>()) std::printf(" %ld", schema["min"].as<long>());
        if (schema["max"].is<long>()) std::printf(" %ld", schema["max"].as<long>());
        if (schema["values"].is<const char*>()) std::printf(" %s", schema["values"].as<const char*>());
        std::printf(" %u %u\n", unsigned(schema["relaxedByReceivedValue"].as<bool>()),
                    unsigned(schema["receivedValueAllowed"].as<bool>()));
    }
    // 边界一致性：DAC 生产解析器与描述符区间必须一致
    uint8_t dac = 0;
    if (!FastBee::parseDacActionValue("255", 3, dac) || dac != 255) return 3;
    if (FastBee::parseDacActionValue("256", 3, dac)) return 4;
    long ignored = 0;
    if (!FastBee::parseStrictActionLong("60000", 5, ignored)) return 5;
    // 共享校验器的生产边界：PWM/DAC/闪烁、Modbus 写、UART 命令、poll/ctrl 条目
    if (FastBee::validateRuleActionValue(4, "65535", false)) return 6;
    if (!FastBee::validateRuleActionValue(4, "65536", false)) return 7;
    if (!FastBee::validateRuleActionValue(2, "49", false)) return 8;
    if (FastBee::validateRuleActionValue(16, "2:on", false)) return 9;
    if (!FastBee::validateRuleActionValue(16, "2:maybe", false)) return 10;
    if (FastBee::validateRuleActionValue(17, "100:42", false)) return 11;
    if (!FastBee::validateRuleActionValue(17, "addr:xx", false)) return 12;
    if (FastBee::validateRuleActionValue(18, "{\"ctrl\":[{\"d\":0,\"a\":\"pwm\",\"v\":10}]}", false)) return 13;
    if (!FastBee::validateRuleActionValue(18, "{\"ctrl\":[{\"a\":\"pwm\"}]}", false)) return 14;
    if (FastBee::validateRuleActionValue(29, "{\"action\":\"send\",\"value\":\"hi\"}", false)) return 15;
    if (!FastBee::validateRuleActionValue(29, "{\"action\":\"exec\"}", false)) return 16;
    if (FastBee::validateRuleActionValue(24, "12:30", false)) return 17;
    if (!FastBee::validateRuleActionValue(24, "", false)) return 18;
    if (!FastBee::validateRuleActionValue(34, "", true)) return 19;  // 禁止接收值替换
    // 与 ADMIN crossRuntimeModbusAndControlBoundariesMatchProduction 完全相同的边界输入
    if (FastBee::validateRuleActionValue(17, "0", false)) return 20;
    if (FastBee::validateRuleActionValue(17, "65535", false)) return 21;
    if (FastBee::validateRuleActionValue(17, "65535:65535", false)) return 22;
    if (!FastBee::validateRuleActionValue(17, "65536", false)) return 23;
    if (!FastBee::validateRuleActionValue(17, "99999", false)) return 24;
    if (!FastBee::validateRuleActionValue(17, "65536:0", false)) return 25;
    if (!FastBee::validateRuleActionValue(17, "0:65536", false)) return 26;
    if (!FastBee::validateRuleActionValue(17, "100000", false)) return 27;
    if (FastBee::validateRuleActionValue(18,
            "{\"ctrl\":[{\"d\":0,\"c\":1,\"a\":\"pwm\",\"v\":10,\"p\":\"param\"}]}", false)) return 28;
    if (!FastBee::validateRuleActionValue(18,
            "{\"ctrl\":[{\"d\":0,\"c\":\"1\",\"a\":\"pwm\"}]}", false)) return 29;
    if (!FastBee::validateRuleActionValue(18,
            "{\"ctrl\":[{\"d\":0,\"a\":\"pwm\",\"v\":\"10\"}]}", false)) return 30;
    if (!FastBee::validateRuleActionValue(18,
            "{\"ctrl\":[{\"d\":0,\"a\":\"pwm\",\"p\":10}]}", false)) return 31;
    // @PRODUCTION_CONFIG_CHECKS@
    return 0;
}
'''


def parse_enum_values(path: Path, enum: str) -> dict:
    text = path.read_text(encoding='utf-8')
    body = re.search(r'enum class ' + enum + r'\s*(?::\s*\w+)?\s*\{(.*?)\};', text, re.S)
    assert body, f'enum {enum} not found in {path}'
    # 先剥离行注释再按逗号分段（注释可能位于逗号之后）
    cleaned = re.sub(r'//[^\n]*', '', body.group(1))
    result = {}
    next_value = 0
    for raw in cleaned.split(','):
        entry = raw.strip()
        if not entry:
            continue
        match = re.match(r'([A-Z_0-9]+)\s*(?:=\s*(\d+))?$', entry)
        assert match, f'unparseable enum entry: {entry!r}'
        value = int(match.group(2)) if match.group(2) is not None else next_value
        result[match.group(1)] = value
        next_value = value + 1
    return result


class ActionTypeCatalogContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise unittest.SkipTest('Host C++17 compiler required')
        headers = sorted((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        if not headers:
            raise unittest.SkipTest('Installed ArduinoJson headers required')
        with tempfile.TemporaryDirectory(prefix='fastbee-action-type-catalog-') as directory:
            source = Path(directory) / 'catalog.cpp'
            binary = Path(directory) / 'catalog.exe'
            source.write_text(HARNESS.replace('    // @PRODUCTION_CONFIG_CHECKS@',
                                              production_action_checks()), encoding='utf-8')
            built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I' + str(ROOT / 'test/mocks'), '-I' + str(ROOT / 'include'),
                '-I' + str(headers[0].parent), str(source), '-o', str(binary)],
                capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, built.stdout + built.stderr
            ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            assert ran.returncode == 0, f'exit={ran.returncode}: {ran.stdout}{ran.stderr}'
            cls.lines = ran.stdout.splitlines()
        cls.actions = {}
        cls.schemas = {}
        cls.types = []
        cls.meta = None
        for line in cls.lines:
            parts = line.split(' ')
            if parts[0] == 'S':
                cls.meta = (int(parts[1]), parts[2])
            elif parts[0] == 'T':
                cls.types.append(int(parts[1]))
            elif parts[0] == 'A':
                cls.actions[int(parts[1])] = {
                    'name': parts[2],
                    'required': parts[3] == '1',
                    'targetTypes': [int(value) for value in parts[4:]],
                }
            elif parts[0] == 'V':
                cls.schemas[int(parts[1])] = {
                    'kind': parts[2],
                    'rest': parts[3:],
                }

    def test_catalog_covers_every_exec_action_type_with_enum_name(self):
        actions = parse_enum_values(ROOT / 'include/core/PeripheralExecution.h', 'ExecActionType')
        self.assertEqual(set(actions.values()), set(self.actions.keys()))
        for name, value in actions.items():
            self.assertEqual(self.actions[value]['name'], name)

    def test_catalog_enumerates_every_peripheral_type_value(self):
        types = parse_enum_values(ROOT / 'include/core/PeripheralTypes.h', 'PeripheralType')
        self.assertEqual(sorted(types.values()), sorted(self.types))

    def test_catalog_carries_schema_and_firmware_metadata(self):
        self.assertIsNotNone(self.meta)
        self.assertEqual(self.meta[0], 1)
        self.assertRegex(self.meta[1], r'^\d+\.\d+\.\d+$')

    def test_representative_production_configurations_remain_accepted(self):
        # setUpClass 已逐动作调用共享生产校验器；此处保证样本覆盖而非空跑
        self.assertEqual(len(PRODUCTION_CONFIG_PATHS), 6)
        self.assertEqual(len(PRODUCTION_ACTIONS), 39)
        self.assertTrue({17, 18, 19}.issubset({action['actionType'] for _, _, action in PRODUCTION_ACTIONS}))

    def test_target_types_match_production_predicates(self):        # 与 docs/generated-action-target-contracts.md 普通模式表一致（生产谓词直接证据）
        expected = {
            0: [12, 16, 17], 1: [12, 16, 17], 2: [12, 17], 3: [16, 17],
            4: [16, 17, 41], 5: [27], 11: [45], 12: [42], 13: [12, 16, 17],
            14: [12, 16, 17], 16: [51], 17: [51],
            19: [11, 13, 14, 15, 18, 19, 20, 26, 38, 43, 44, 48, 49],
            24: [47], 25: [47], 26: [47], 27: [36], 28: [48], 29: [1],
            30: [12, 16, 17], 35: [50], 36: [50], 37: [50], 38: [37],
            39: [12], 40: [37],
        }
        for value, types in expected.items():
            self.assertEqual(self.actions[value]['targetTypes'], types,
                             f'action {value} targetTypes mismatch')
        # 不需要外设目标的动作不得伪造类型清单
        for value in (6, 7, 8, 9, 15, 18, 20, 21, 22, 23, 31, 32, 33, 34):
            self.assertFalse(self.actions[value]['required'], f'action {value}')
        # CALL_PERIPHERAL(10) 需要目标但目标匹配命令相关：不展开类型清单
        self.assertTrue(self.actions[10]['required'])
        self.assertEqual(self.actions[10]['targetTypes'], [])

    def test_value_schemas_match_production_preflight(self):
        # 每个动作都有生产值 schema，kind 限定已知集合；所列动作不允许 any
        kinds = {'none', 'integer', 'nonempty-text', 'empty-only', 'enum', 'json-object', 'json-poll',
                 'modbus-coil', 'modbus-reg', 'uart-command'}
        for value in self.actions:
            self.assertIn(value, self.schemas, f'action {value} missing valueSchema')
        for value in (10, 16, 17, 18, 24, 25, 27, 28, 29):
            self.assertIn(self.schemas[value]['kind'], kinds, f'action {value} must not accept any')
        # 生产 preflight 边界与描述符一致（共享实现见下方同源断言）
        self.assertEqual(self.schemas[2]['kind'], 'integer')
        self.assertEqual(self.schemas[2]['rest'][:2], ['50', '60000'])
        self.assertEqual(self.schemas[4]['rest'][:2], ['0', '65535'])
        self.assertEqual(self.schemas[5]['rest'][:2], ['0', '255'])
        self.assertEqual(self.schemas[15]['kind'], 'nonempty-text')
        self.assertEqual(self.schemas[31]['kind'], 'enum')
        self.assertIn('webServer,mdns', self.schemas[31]['rest'])
        self.assertEqual(self.schemas[35]['kind'], 'enum')
        self.assertIn('start,stop', self.schemas[35]['rest'])
        self.assertEqual(self.schemas[34]['kind'], 'empty-only')
        self.assertEqual(self.schemas[16]['kind'], 'modbus-coil')
        self.assertEqual(self.schemas[17]['kind'], 'modbus-reg')
        self.assertEqual(self.schemas[29]['kind'], 'uart-command')
        self.assertEqual(self.schemas[18]['kind'], 'json-poll')
        self.assertEqual(self.schemas[19]['kind'], 'json-object')
        # RTC/SD/受限远程读禁止 useReceivedValue（生产 validateRtcAction/validateSdStatusAction）
        for value in (34, 35, 36, 37, 38):
            self.assertEqual(self.schemas[value]['rest'][-1], '0', f'action {value} must forbid received value')

    def test_preflight_and_catalog_share_one_value_validator(self):
        # 同源：生产 preflight 调用共享校验器，schema 常量不在路由内联平行定义
        value_contract = (ROOT / 'include/core/ActionValueContract.h').read_text(encoding='utf-8')
        self.assertIn('validateRuleActionValue', value_contract)
        self.assertIn('describeActionValueSchema', value_contract)
        self.assertIn('FastBee::validateRuleActionValue(', ROUTE_CPP)
        for parallel in ('INVALID_EFFECT_INTERVAL", "error", path + ".actionValue",\n                         "Blink/breathe',
                         'parseStrictLong(action.actionValue'):
            self.assertNotIn(parallel, ROUTE_CPP)
        self.assertNotIn('INVALID_DAC_VALUE", "error", path + ".actionValue",\n                         "DAC value', ROUTE_CPP)

    def test_route_is_read_only_and_authenticated(self):
        self.assertIn('"/api/periph-exec/action-types"', ROUTE_CPP)
        route = re.search(r'action-types[^\n]*\n[^\n]*handleGetActionTypes', ROUTE_CPP)
        self.assertIsNotNone(route)
        self.assertIn('HTTP_GET', route.group(0))
        self.assertNotIn('HTTP_POST', route.group(0))
        handler = re.search(r'void PeriphExecRouteHandler::handleGetActionTypes.*?\n\}', ROUTE_CPP, re.S)
        self.assertIsNotNone(handler)
        self.assertIn('requireAuth', handler.group(0))
        self.assertIn('ActionTypeCatalog::writeActionTypeCatalog', handler.group(0))
        self.assertIn('handleGetActionTypes', ROUTE_H)


if __name__ == '__main__':
    unittest.main()
