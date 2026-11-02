import { test, expect } from '@playwright/test';
import { resolve } from 'node:path';

/**
 * AI MCP 浏览器连接器离线套件（Task 3）。
 *
 * 不连接真实设备/云服务：device 同源 API 由 apiGet mock 承担，
 * 云端 /connector/v1/** 由 fetch mock 承担，验证配对、白名单任务执行、
 * 有界长轮询、续期上限与离页/离线停止行为。
 */

const root = resolve(__dirname, '../../..');
const CONNECTOR = resolve(root, 'web-src/modules/ai/ai-mcp-connector.js');
const CLOUD = 'https://mcp.fastbee.test';
const ORIGIN = 'http://192.168.4.1';

const DEVICE_INFO = {
  success: true,
  data: {
    deviceId: 'FBE001122334455',
    deviceName: 'connector-test',
    chip: { model: 'ESP32-S3', macAddress: '00:11:22:33:44:55' },
  },
};
const SYSTEM_INFO = { success: true, data: { device: { firmwareVersion: '1.2.3' } } };
const CAPABILITIES = { success: true, data: { mqtt: true, modbus: true } };
const DEVICE_CONFIG = { success: true, data: { deviceName: 'connector-test', timezone: 'UTC+8' } };
// 与固件 /api/periph-exec/trigger-types 和 /api/periph-exec/events/static 契约同形状的执行目录
const TRIGGER_TYPES = {
  success: true,
  data: [
    { type: 0, name: '平台触发', description: 'IoT平台通过MQTT下发指令时触发' },
    { type: 1, name: '定时触发', description: '按指定时间间隔或每日时间点触发' },
    { type: 4, name: '事件触发', description: 'WiFi/MQTT/按键/数据/外设执行等事件触发' },
    { type: 5, name: '轮询触发', description: '本地数据源周期轮询条件评估触发' },
  ],
};
const STATIC_EVENTS = {
  success: true,
  data: [
    { id: 'wifi_connected', name: 'WiFi连接成功', category: 'WiFi', type: 1 },
    { id: 'system_boot', name: '系统启动', category: '系统', type: 70 },
  ],
};
// 与固件 /api/periph-exec/action-types 契约同形状的生产动作目录
const ACTION_TYPES = {
  success: true,
  data: {
    schemaVersion: 1,
    firmwareVersion: '1.2.3',
    actions: [
      { value: 0, name: 'ACTION_HIGH', requiresPeripheralTarget: true, targetTypes: [12, 16, 17],
        valueSchema: { kind: 'none', relaxedByReceivedValue: false, receivedValueAllowed: true } },
      { value: 4, name: 'ACTION_SET_PWM', requiresPeripheralTarget: true, targetTypes: [16, 17, 41],
        valueSchema: { kind: 'integer', min: 0, max: 65535, relaxedByReceivedValue: true, receivedValueAllowed: true } },
      { value: 6, name: 'ACTION_SYS_RESTART', requiresPeripheralTarget: false,
        valueSchema: { kind: 'none', relaxedByReceivedValue: false, receivedValueAllowed: true } },
      { value: 10, name: 'ACTION_CALL_PERIPHERAL', requiresPeripheralTarget: true,
        valueSchema: { kind: 'nonempty-text', relaxedByReceivedValue: false, receivedValueAllowed: true } },
    ],
  },
};
// 与固件 /api/periph-exec/controls 契约同形状的控制目录（分组原始行）
const CONTROLS = {
  success: true,
  data: {
    gpio: [{ id: 'rule_1', name: '规则1', actionType: 0, enabled: true, actionCount: 1,
      targetPeriphId: 'do1', targets: [{ id: 'do1' }], hasSystemAction: false, hasSetMode: false, pollTaskIndex: [] }],
    modbus: [], system: [], script: [], sensor: [], other: [],
  },
};
// 与固件 /api/mcp/config/snapshot 契约同形状的全局 revision
const MCP_SNAPSHOT = { success: true, revision: 'rev-a', sections: {} };

// 与固件 /api/peripherals/types 契约同形状的外设类型目录
const PERIPHERAL_TYPES = {
  success: true,
  profile: { name: 'ESP32-S3', max: 16, used: 0, remaining: 16 },
  hardware: { chip: 'ESP32-S3', maxGpio: 48, inputPins: [4], outputPins: [15, 16], adcPins: [], dacPins: [], touchPins: [], reservedPins: [26] },
  data: {
    gpio: [{ value: 12, name: 'Digital Output', pinCount: 1, implemented: true, configurable: true, pinRoles: ['GPIO'], operations: [] }],
  },
};

async function setup(page: any) {
  await page.clock.install();
  // 以固定设备 Origin 提供空页面，使 window.location.origin 与配对绑定一致
  await page.route(ORIGIN + '/**', (route: any) =>
    route.fulfill({ contentType: 'text/html', body: '<html><body></body></html>' }));
  await page.goto(ORIGIN + '/');
  await page.addScriptTag({ path: CONNECTOR });
  await page.evaluate(() => {
    const w = window as any;
    w.sessionStorage.clear();
    w.apiCalls = [] as string[];
    w.apiPostCalls = [] as { path: string; body: any }[];
    w.cloudCalls = [] as { path: string; body: any; auth: string | null }[];
    w.pendingPolls = [] as ((jobs: any[]) => void)[];
    w.pairResponse = {
      status: 200,
      body: {
        connectorToken: 'tok-1',
        tokenExpiresAt: Date.now() + 15 * 60_000,
        sessionExpiresAt: Date.now() + 8 * 3600_000,
        deviceId: 'FBE001122334455',
      },
    };
    w.apiGet = async (path: string) => {
      w.apiCalls.push(path);
      if (path === '/api/device/info') return structuredClone((window as any).DEVICE_INFO);
      if (path.startsWith('/api/system/info')) return structuredClone((window as any).SYSTEM_INFO);
      if (path === '/api/system/capabilities') return structuredClone((window as any).CAPABILITIES);
      if (path === '/api/peripherals/types') {
        if ((window as any).typesFail) throw new Error('types endpoint down');
        return structuredClone((window as any).PERIPHERAL_TYPES);
      }
      if (path === '/api/periph-exec/trigger-types') {
        if ((window as any).execCatalogFail) throw new Error('exec catalog down');
        return structuredClone((window as any).TRIGGER_TYPES);
      }
      if (path === '/api/periph-exec/events/static') {
        if ((window as any).execCatalogFail) throw new Error('exec catalog down');
        return structuredClone((window as any).STATIC_EVENTS);
      }
      if (path === '/api/periph-exec/action-types') {
        if ((window as any).execCatalogFail) throw new Error('exec catalog down');
        return structuredClone((window as any).ACTION_TYPES);
      }
      if (path === '/api/periph-exec/controls') {
        if ((window as any).execCatalogFail) throw new Error('exec catalog down');
        return structuredClone((window as any).CONTROLS);
      }
      if (path === '/api/mcp/config/snapshot') {
        if ((window as any).snapshotFail) throw new Error('snapshot down');
        return structuredClone((window as any).MCP_SNAPSHOT);
      }
      if (path === '/api/mcp/config/operations/op-browser') {
        return { success: true, operationId: 'op-browser', state: 'APPLYING', steps: [
          { section: 'features', state: 'PENDING', accepted: false, persisted: false, readback: false },
        ] };
      }
      if (path === '/api/device/config') return structuredClone((window as any).DEVICE_CONFIG);
      throw new Error('unexpected device api: ' + path);
    };
    w.apiPost = async (path: string, body: any) => {
      w.apiPostCalls.push({ path, body });
      if (path === '/api/mcp/config/apply') {
        if (w.applyResponses?.length) return w.applyResponses.shift();
        return { success: true, operationId: 'op-browser', state: 'SUCCEEDED', steps: [
          { section: 'features', state: 'SUCCEEDED', accepted: true, persisted: true, readback: true },
        ] };
      }
      if (path === '/api/mcp/config/preflight') return { success: true, allowed: true, persisted: false };
      if (path === '/api/mcp/config/readback') {
        if (w.readbackDisconnect) throw new TypeError('network down');
        return { success: true, operationId: 'op-browser', state: 'SUCCEEDED', steps: [
          { section: 'features', state: 'SUCCEEDED', accepted: true, persisted: true, readback: true },
        ] };
      }
      if (path === '/api/mcp/config/rollback') {
        if (w.rollbackResponses?.length) return w.rollbackResponses.shift();
        return { success: true, operationId: 'op-browser', state: 'ROLLED_BACK', steps: [
          { section: 'features', state: 'SUCCEEDED', accepted: true, persisted: true, readback: true },
        ] };
      }
      throw new Error('unexpected device write api: ' + path);
    };
    w.fetch = (url: string, opts: any = {}) => {
      const u = new URL(url);
      const body = opts.body ? JSON.parse(opts.body) : {};
      w.cloudCalls.push({ path: u.pathname, body, auth: opts.headers?.Authorization ?? null, keepalive: !!opts.keepalive });
      const respond = (status: number, payload: any) =>
        Promise.resolve({ status, ok: status >= 200 && status < 300, json: async () => payload });
      if (u.pathname === '/connector/v1/pair') return respond(w.pairResponse.status, w.pairResponse.body);
      if (u.pathname === '/connector/v1/token/refresh') {
        if (w.refreshInvalidExpiryOnce) {
          w.refreshInvalidExpiryOnce = false;
          return respond(200, { connectorToken: 'tok-invalid', tokenExpiresAt: Date.now() - 1,
            sessionExpiresAt: w.pairResponse.body.sessionExpiresAt });
        }
        if (w.refreshOutage) return respond(503, { code: 'UNAVAILABLE' });
        return respond(200, {
          connectorToken: 'tok-2',
          tokenExpiresAt: Date.now() + 15 * 60_000,
          sessionExpiresAt: w.pairResponse.body.sessionExpiresAt,
        });
      }
      if (u.pathname === '/connector/v1/heartbeat') return respond(200, { serverTime: Date.now() });
      if (u.pathname === '/connector/v1/revoke') return respond(200, { revoked: true });
      if (u.pathname === '/connector/v1/jobs/poll') {
        if (w.pollMode === '401') return respond(401, { code: 'SESSION_REVOKED' });
        if (w.pollMode === 'error') return Promise.reject(new TypeError('network down'));
        return new Promise((_resolve, reject) => {
          w.pendingPolls.push((jobs: any[]) => {
            if (!jobs && w.pollMode === '401') return _resolve({ status: 401, ok: false, json: async () => ({ code: 'SESSION_REVOKED' }) });
            if (!jobs && w.pollMode === 'error') return reject(new TypeError('network down'));
            return _resolve({ status: 200, ok: true, json: async () => ({ jobs: jobs || [], serverTime: Date.now(), waitMs: Math.min(body.waitMs ?? 0, 20000) }) });
          });
          opts.signal?.addEventListener('abort', () => reject(new DOMException('aborted', 'AbortError')));
        });
      }
      if (u.pathname.endsWith('/result')) return respond(200, { recorded: true, duplicate: false, status: body.status });
      return respond(404, { code: 'NOT_FOUND' });
    };
  },);
  await page.evaluate((fixtures: any) => {
    const w = window as any;
    w.DEVICE_INFO = fixtures.DEVICE_INFO;
    w.SYSTEM_INFO = fixtures.SYSTEM_INFO;
    w.CAPABILITIES = fixtures.CAPABILITIES;
    w.DEVICE_CONFIG = fixtures.DEVICE_CONFIG;
    w.PERIPHERAL_TYPES = fixtures.PERIPHERAL_TYPES;
    w.TRIGGER_TYPES = fixtures.TRIGGER_TYPES;
    w.STATIC_EVENTS = fixtures.STATIC_EVENTS;
    w.ACTION_TYPES = fixtures.ACTION_TYPES;
    w.CONTROLS = fixtures.CONTROLS;
    w.MCP_SNAPSHOT = fixtures.MCP_SNAPSHOT;
  }, { DEVICE_INFO, SYSTEM_INFO, CAPABILITIES, DEVICE_CONFIG, PERIPHERAL_TYPES, TRIGGER_TYPES, STATIC_EVENTS,
    ACTION_TYPES, CONTROLS, MCP_SNAPSHOT });
}

async function pair(page: any) {
  const result = await page.evaluate(async (cloud: string) => {
    return await (window as any).FastBeeAiMcpConnector.pair(cloud, 'ABCD234567');
  }, CLOUD);
  expect(result.ok).toBe(true);
}

test('pair stores token only in sessionStorage and never in URLs', async ({ page }) => {
  await setup(page);
  await pair(page);
  const stored = await page.evaluate(() => window.sessionStorage.getItem('fastbee.mcp.connector.session'));
  expect(stored).toContain('tok-1');
  const localDump = await page.evaluate(() => Object.keys(window.localStorage).map((k) => window.localStorage.getItem(k)).join('|'));
  expect(localDump).not.toContain('tok-1');
  const calls = await page.evaluate(() => (window as any).cloudCalls);
  expect(calls[0].path).toBe('/connector/v1/pair');
  expect(calls[0].body.code).toBe('ABCD234567');
  expect(calls[0].body.origin).toBe(ORIGIN);
  expect(calls[0].body.device.deviceId).toBe('FBE001122334455');
  expect(calls[0].body.device.macAddress).toBe('00:11:22:33:44:55');
  expect(calls[0].body.device.chipModel).toBe('ESP32-S3');
  expect(calls[0].body.device.firmwareVersion).toBe('1.2.3');
  // token 不得出现在任何请求 URL 中
  const urls = await page.evaluate(() => (window as any).cloudCalls.map((c: any) => c.path).join('|'));
  expect(urls).not.toContain('tok-1');
});

test('whitelisted job executes via same-origin device api and submits idempotent result', async ({ page }) => {
  await setup(page);
  await pair(page);
  await page.evaluate(() => (window as any).FastBeeAiMcpConnector.start());
  await expect.poll(() => page.evaluate(() => (window as any).pendingPolls.length)).toBe(1);
  await page.evaluate(() => {
    (window as any).pendingPolls.shift()([
      { jobId: 'job-1', type: 'device.config.section.read', section: 'device', idempotencyKey: 'k-1', deadlineAt: Date.now() + 60_000, leaseExpiresAt: Date.now() + 60_000 },
    ]);
  });
  await expect.poll(() => page.evaluate(() => (window as any).cloudCalls.filter((c: any) => c.path.endsWith('/result')).length)).toBe(1);
  const apiCalls = await page.evaluate(() => (window as any).apiCalls);
  expect(apiCalls).toContain('/api/device/config');
  const result = (await page.evaluate(() => (window as any).cloudCalls)).find((c: any) => c.path.endsWith('/result'));
  expect(result.path).toBe('/connector/v1/jobs/job-1/result');
  expect(result.body.status).toBe('SUCCEEDED');
  expect(result.body.idempotencyKey).toBe('k-1');
  expect(result.body.result.deviceName).toBe('connector-test');
  expect(result.auth).toBe('Bearer tok-1');
});

test('managed apply job uses only the fixed same-origin apply API and forwards its idempotency envelope', async ({ page }) => {
  await setup(page);
  await pair(page);
  await page.evaluate(() => (window as any).FastBeeAiMcpConnector.start());
  await expect.poll(() => page.evaluate(() => (window as any).pendingPolls.length)).toBe(1);
  await page.evaluate(() => {
    (window as any).pendingPolls.shift()([
      { jobId: 'job-apply', type: 'device.config.apply', idempotencyKey: 'cloud-job-key', deadlineAt: Date.now() + 60_000,
        draftId: 'draft-1', baseRevision: 'rev-a', approvalHash: 'approval', deviceIdempotencyKey: 'device-apply-key',
        operations: [{ section: 'features', schemaVersion: 1, patch: { enabled: true } }] },
    ]);
  });
  await expect.poll(() => page.evaluate(() => (window as any).cloudCalls.filter((c: any) => c.path.includes('job-apply')).length)).toBe(1);
  const posts = await page.evaluate(() => (window as any).apiPostCalls);
  expect(posts).toHaveLength(1);
  expect(posts[0].path).toBe('/api/mcp/config/apply');
  expect(posts[0].body).toEqual({ draftId: 'draft-1', baseRevision: 'rev-a', approvalHash: 'approval',
    idempotencyKey: 'device-apply-key', operations: [{ section: 'features', schemaVersion: 1, patch: { enabled: true } }] });
  const result = (await page.evaluate(() => (window as any).cloudCalls)).find((c: any) => c.path.includes('job-apply'));
  expect(result.body.status).toBe('SUCCEEDED');
  expect(result.body.result.state).toBe('SUCCEEDED');
});

test('production top-level managed responses preserve success partial UNKNOWN snapshot and pending operation states', async ({ page }) => {
  await setup(page);
  await pair(page);
  await page.evaluate(() => {
    const w = window as any;
    w.applyResponses = [{ success: true, operationId: 'op-partial', state: 'PARTIAL_APPLY', error: 'PARTIAL_APPLY',
      manualRecoveryRequired: true, steps: [
        { section: 'features', state: 'SUCCEEDED', accepted: true, persisted: true, readback: true },
        { section: 'peripherals', state: 'PENDING', accepted: false, persisted: false, readback: false },
      ] }];
    w.rollbackResponses = [{ success: true, operationId: 'op-browser', state: 'PARTIAL_ROLLBACK', error: 'PARTIAL_ROLLBACK',
      manualRecoveryRequired: true, steps: [
        { section: 'features', state: 'FAILED', accepted: true, persisted: true, readback: false },
      ] }];
    w.FastBeeAiMcpConnector.start();
  });
  await expect.poll(() => page.evaluate(() => (window as any).pendingPolls.length)).toBe(1);
  await page.evaluate(() => {
    (window as any).pendingPolls.shift()([
      { jobId: 'job-snapshot', type: 'device.config.snapshot.read', idempotencyKey: 'k-s', deadlineAt: Date.now() + 60_000 },
      { jobId: 'job-preflight', type: 'device.config.preflight', idempotencyKey: 'k-p', deadlineAt: Date.now() + 60_000,
        baseRevision: 'rev-a', operations: [{ section: 'features' }] },
      { jobId: 'job-partial', type: 'device.config.apply', idempotencyKey: 'k-a', deadlineAt: Date.now() + 60_000,
        draftId: 'draft-1', baseRevision: 'rev-a', approvalHash: 'approval', deviceIdempotencyKey: 'd-a', operations: [] },
      { jobId: 'job-operation', type: 'device.config.operation.read', operationId: 'op-browser', idempotencyKey: 'k-o',
        deadlineAt: Date.now() + 60_000 },
      { jobId: 'job-readback', type: 'device.config.readback', operationId: 'op-browser', idempotencyKey: 'k-r',
        deadlineAt: Date.now() + 60_000 },
      { jobId: 'job-rollback', type: 'device.config.rollback', operationId: 'op-browser', currentRevision: 'rev-b',
        approvalHash: 'rollback-approval', deviceIdempotencyKey: 'd-r', idempotencyKey: 'k-b', deadlineAt: Date.now() + 60_000 },
    ]);
  });
  await expect.poll(() => page.evaluate(() => (window as any).cloudCalls.filter((c: any) => c.path.endsWith('/result')).length))
    .toBe(6);
  const calls = await page.evaluate(() => (window as any).cloudCalls);
  const result = (id: string) => calls.find((c: any) => c.path.includes(id)).body;
  expect(result('job-snapshot').status).toBe('SUCCEEDED');
  expect(result('job-snapshot').result.revision).toBe('rev-a');
  expect(result('job-preflight').status).toBe('SUCCEEDED');
  expect(result('job-preflight').result.allowed).toBe(true);
  expect(result('job-partial').status).toBe('SUCCEEDED');
  expect(result('job-partial').result.state).toBe('PARTIAL_APPLY');
  expect(result('job-partial').result.steps[1].state).toBe('PENDING');
  expect(result('job-operation').status).toBe('SUCCEEDED');
  expect(result('job-operation').result.state).toBe('APPLYING');
  expect(result('job-operation').result.steps[0].state).toBe('PENDING');
  expect(result('job-readback').status).toBe('SUCCEEDED');
  expect(result('job-readback').result.state).toBe('SUCCEEDED');
  expect(result('job-rollback').status).toBe('SUCCEEDED');
  expect(result('job-rollback').result.state).toBe('PARTIAL_ROLLBACK');
});

test('malformed success data and structured UNKNOWN are UNKNOWN while definitive rejection stays FAILED', async ({ page }) => {
  await setup(page);
  await pair(page);
  await page.evaluate(() => {
    (window as any).applyResponses = [
      { success: true, state: 'SUCCEEDED' },
      { success: true, operationId: 'op-bad-state', state: 'MAYBE' },
      { success: false, data: { message: 'bad request' } },
      { success: false, code: 'OUTCOME_UNKNOWN' },
      { success: false, code: 'INVALID_ARGUMENT' },
      { success: true, operationId: 'op-success-unknown', state: 'FAILED', error: 'OUTCOME_UNKNOWN' },
    ];
    (window as any).FastBeeAiMcpConnector.start();
  });
  await expect.poll(() => page.evaluate(() => (window as any).pendingPolls.length)).toBe(1);
  await page.evaluate(() => {
    (window as any).pendingPolls.shift()([
      { jobId: 'job-missing-op', type: 'device.config.apply', idempotencyKey: 'k-m1', deadlineAt: Date.now() + 60_000,
        draftId: 'draft-1', baseRevision: 'rev-a', approvalHash: 'approval', deviceIdempotencyKey: 'd-m1', operations: [] },
      { jobId: 'job-bad-state', type: 'device.config.apply', idempotencyKey: 'k-m2', deadlineAt: Date.now() + 60_000,
        draftId: 'draft-1', baseRevision: 'rev-a', approvalHash: 'approval', deviceIdempotencyKey: 'd-m2', operations: [] },
      { jobId: 'job-bad-error', type: 'device.config.apply', idempotencyKey: 'k-m3', deadlineAt: Date.now() + 60_000,
        draftId: 'draft-1', baseRevision: 'rev-a', approvalHash: 'approval', deviceIdempotencyKey: 'd-m3', operations: [] },
      { jobId: 'job-structured-unknown', type: 'device.config.apply', idempotencyKey: 'k-m4', deadlineAt: Date.now() + 60_000,
        draftId: 'draft-1', baseRevision: 'rev-a', approvalHash: 'approval', deviceIdempotencyKey: 'd-m4', operations: [] },
      { jobId: 'job-known-rejection', type: 'device.config.apply', idempotencyKey: 'k-m5', deadlineAt: Date.now() + 60_000,
        draftId: 'draft-1', baseRevision: 'rev-a', approvalHash: 'approval', deviceIdempotencyKey: 'd-m5', operations: [] },
      { jobId: 'job-success-unknown', type: 'device.config.apply', idempotencyKey: 'k-m6', deadlineAt: Date.now() + 60_000,
        draftId: 'draft-1', baseRevision: 'rev-a', approvalHash: 'approval', deviceIdempotencyKey: 'd-m6', operations: [] },
    ]);
  });
  await expect.poll(() => page.evaluate(() => (window as any).cloudCalls.filter((c: any) => c.path.includes('job-')).length >= 6))
    .toBe(true);
  const calls = await page.evaluate(() => (window as any).cloudCalls);
  for (const id of ['job-missing-op', 'job-bad-state', 'job-bad-error', 'job-structured-unknown', 'job-success-unknown']) {
    const result = calls.find((c: any) => c.path.includes(id));
    expect(result.body.status).toBe('UNKNOWN');
    expect(result.body.error).toBe('OUTCOME_UNKNOWN');
  }
  const rejection = calls.find((c: any) => c.path.includes('job-known-rejection'));
  expect(rejection.body.status).toBe('FAILED');
  expect(rejection.body.error).toBe('INVALID_ARGUMENT');
});

test('readback disconnect reports UNKNOWN instead of a known device failure', async ({ page }) => {
  await setup(page);
  await pair(page);
  await page.evaluate(() => {
    (window as any).readbackDisconnect = true;
    (window as any).FastBeeAiMcpConnector.start();
  });
  await expect.poll(() => page.evaluate(() => (window as any).pendingPolls.length)).toBe(1);
  await page.evaluate(() => {
    (window as any).pendingPolls.shift()([
      { jobId: 'job-readback-down', type: 'device.config.readback', operationId: 'op-browser', idempotencyKey: 'k-r',
        deadlineAt: Date.now() + 60_000 },
    ]);
  });
  await expect.poll(() => page.evaluate(() => (window as any).cloudCalls.filter((c: any) => c.path.includes('job-readback-down')).length)).toBe(1);
  const result = (await page.evaluate(() => (window as any).cloudCalls)).find((c: any) => c.path.includes('job-readback-down'));
  expect(result.body.status).toBe('UNKNOWN');
  expect(result.body.error).toBe('OUTCOME_UNKNOWN');
});

test('job carrying url or method is rejected without touching the url', async ({ page }) => {
  await setup(page);
  await pair(page);
  await page.evaluate(() => (window as any).FastBeeAiMcpConnector.start());
  await expect.poll(() => page.evaluate(() => (window as any).pendingPolls.length)).toBe(1);
  await page.evaluate(() => {
    (window as any).pendingPolls.shift()([
      { jobId: 'job-evil', type: 'device.info.read', url: 'http://evil.example/x', method: 'POST', idempotencyKey: 'k-e', deadlineAt: Date.now() + 60_000, leaseExpiresAt: Date.now() + 60_000 },
    ]);
  });
  await expect.poll(() => page.evaluate(() => (window as any).cloudCalls.filter((c: any) => c.path.endsWith('/result')).length)).toBe(1);
  const calls = await page.evaluate(() => (window as any).cloudCalls);
  const result = calls.find((c: any) => c.path.endsWith('/result'));
  expect(result.path).toBe('/connector/v1/jobs/job-evil/result');
  expect(result.body.status).toBe('FAILED');
  expect(result.body.error).toBe('INVALID_ARGUMENT');
  // 不得请求云端任务夹带的任意地址
  expect(calls.every((c: any) => !c.path.includes('evil.example'))).toBe(true);
  const apiCalls = await page.evaluate(() => (window as any).apiCalls);
  expect(apiCalls.some((p: string) => p.includes('evil'))).toBe(false);
});

test('unknown section and oversized results fail explicitly without fake success', async ({ page }) => {
  await setup(page);
  await pair(page);
  await page.evaluate(() => (window as any).FastBeeAiMcpConnector.start());
  await expect.poll(() => page.evaluate(() => (window as any).pendingPolls.length)).toBe(1);
  await page.evaluate(() => {
    (window as any).pendingPolls.shift()([
      { jobId: 'job-bad-section', type: 'device.config.section.read', section: 'ota', idempotencyKey: 'k-2', deadlineAt: Date.now() + 60_000, leaseExpiresAt: Date.now() + 60_000 },
    ]);
  });
  await expect.poll(() => page.evaluate(() => (window as any).cloudCalls.filter((c: any) => c.path.includes('job-bad-section')).length)).toBe(1);
  let calls = await page.evaluate(() => (window as any).cloudCalls);
  expect(calls.find((c: any) => c.path.includes('job-bad-section')).body.status).toBe('FAILED');

  await page.evaluate(() => {
    (window as any).DEVICE_CONFIG = { success: true, data: { blob: 'x'.repeat(40000) } };
    (window as any).pendingPolls.shift()([
      { jobId: 'job-big', type: 'device.config.section.read', section: 'device', idempotencyKey: 'k-3', deadlineAt: Date.now() + 60_000, leaseExpiresAt: Date.now() + 60_000 },
    ]);
  });
  await expect.poll(() => page.evaluate(() => (window as any).cloudCalls.filter((c: any) => c.path.includes('job-big')).length)).toBe(1);
  calls = await page.evaluate(() => (window as any).cloudCalls);
  const big = calls.find((c: any) => c.path.includes('job-big'));
  expect(big.body.status).toBe('FAILED');
  expect(big.body.error).toBe('RESULT_TOO_LARGE');
  expect(JSON.stringify(big.body).length).toBeLessThan(20000);
});

test('poll waitMs is clamped to twenty seconds', async ({ page }) => {
  await setup(page);
  await pair(page);
  await page.evaluate(() => (window as any).FastBeeAiMcpConnector.start());
  await expect.poll(() => page.evaluate(() => (window as any).pendingPolls.length)).toBe(1);
  const calls = await page.evaluate(() => (window as any).cloudCalls);
  const poll = calls.find((c: any) => c.path === '/connector/v1/jobs/poll');
  expect(poll.body.waitMs).toBeLessThanOrEqual(20000);
  expect(poll.body.waitMs).toBe(20000);
});

test('pagehide aborts polling, sends final heartbeat and stays silent afterwards', async ({ page }) => {
  await setup(page);
  await pair(page);
  await page.evaluate(() => (window as any).FastBeeAiMcpConnector.start());
  await expect.poll(() => page.evaluate(() => (window as any).pendingPolls.length)).toBe(1);
  await page.evaluate(() => window.dispatchEvent(new Event('pagehide')));
  await expect.poll(() => page.evaluate(() => (window as any).cloudCalls.filter((c: any) => c.path === '/connector/v1/heartbeat').length)).toBeGreaterThanOrEqual(1);
  let calls = await page.evaluate(() => (window as any).cloudCalls);
  const finalHeartbeat = calls.filter((c: any) => c.path === '/connector/v1/heartbeat').pop();
  expect(finalHeartbeat.body.state).toBe('offline');
  expect(finalHeartbeat.keepalive).toBe(true);
  const pollsBefore = calls.filter((c: any) => c.path === '/connector/v1/jobs/poll').length;
  await page.clock.runFor(120000);
  calls = await page.evaluate(() => (window as any).cloudCalls);
  expect(calls.filter((c: any) => c.path === '/connector/v1/jobs/poll').length).toBe(pollsBefore);
});

test('token refreshes before expiry and session stops at absolute cap', async ({ page }) => {
  await setup(page);
  await page.evaluate(() => {
    const w = window as any;
    w.pairResponse.body.tokenExpiresAt = Date.now() + 61_000;
    w.pairResponse.body.sessionExpiresAt = Date.now() + 120_000;
  });
  await pair(page);
  await page.evaluate(() => (window as any).FastBeeAiMcpConnector.start());
  await expect.poll(() => page.evaluate(() => (window as any).pendingPolls.length)).toBe(1);
  // 距 token 过期 1 秒（小于 60s 边距）→ 时钟前进触发续期
  await page.clock.runFor(2000);
  await expect.poll(() => page.evaluate(() => (window as any).cloudCalls.filter((c: any) => c.path === '/connector/v1/token/refresh').length)).toBe(1);
  const stored = await page.evaluate(() => window.sessionStorage.getItem('fastbee.mcp.connector.session'));
  expect(stored).toContain('tok-2');
  // 越过 8 小时总会话上限后停止并清除会话
  await page.clock.runFor(200000);
  await expect.poll(() => page.evaluate(() => (window as any).FastBeeAiMcpConnector.status().running)).toBe(false);
  const cleared = await page.evaluate(() => window.sessionStorage.getItem('fastbee.mcp.connector.session'));
  expect(cleared).toBeNull();
});

test('refresh outage and invalid returned expiry use bounded exponential backoff then unreachable lifecycle', async ({ page }) => {
  await setup(page);
  await page.evaluate(() => {
    const w = window as any;
    w.pairResponse.body.tokenExpiresAt = Date.now() + 61_000;
    w.refreshInvalidExpiryOnce = true;
    w.refreshOutage = true;
  });
  await pair(page);
  await page.evaluate(() => (window as any).FastBeeAiMcpConnector.start());
  await page.clock.runFor(2000);
  await expect.poll(() => page.evaluate(() => (window as any).cloudCalls.filter((c: any) => c.path === '/connector/v1/token/refresh').length))
    .toBe(1);
  expect(await page.evaluate(() => window.sessionStorage.getItem('fastbee.mcp.connector.session'))).not.toContain('tok-invalid');
  expect(await page.evaluate(() => (window as any).FastBeeAiMcpConnector.status().refreshFailures)).toBe(1);
  await page.clock.runFor(500);
  expect(await page.evaluate(() => (window as any).cloudCalls.filter((c: any) => c.path === '/connector/v1/token/refresh').length))
    .toBe(1);
  for (let i = 0; i < 80; i++) {
    await page.clock.runFor(5000);
    if (!(await page.evaluate(() => (window as any).FastBeeAiMcpConnector.status().running))) break;
  }
  const status = await page.evaluate(() => (window as any).FastBeeAiMcpConnector.status());
  expect(status.running).toBe(false);
  expect(status.state).toBe('unreachable');
  const calls = await page.evaluate(() => (window as any).cloudCalls);
  const refreshCount = calls.filter((c: any) => c.path === '/connector/v1/token/refresh').length;
  expect(refreshCount).toBeGreaterThan(2);
  expect(refreshCount).toBeLessThanOrEqual(10);
  const total = calls.length;
  await page.clock.runFor(300000);
  expect(await page.evaluate(() => (window as any).cloudCalls.length)).toBe(total);
});

test('without pairing the connector stays inert and issues no requests', async ({ page }) => {
  await setup(page);
  const started = await page.evaluate(() => (window as any).FastBeeAiMcpConnector.start());
  expect(started).toBe(false);
  await page.clock.runFor(300000);
  const calls = await page.evaluate(() => (window as any).cloudCalls);
  expect(calls.length).toBe(0);
  const apiCalls = await page.evaluate(() => (window as any).apiCalls);
  expect(apiCalls.length).toBe(0);
});

test('terminal 401 clears the session and stops polling and heartbeat', async ({ page }) => {
  await setup(page);
  await pair(page);
  await page.evaluate(() => (window as any).FastBeeAiMcpConnector.start());
  await expect.poll(() => page.evaluate(() => (window as any).pendingPolls.length)).toBe(1);
  await page.evaluate(() => {
    const w = window as any;
    w.pollMode = '401';
    w.pendingPolls.shift()();
  });
  await expect.poll(() => page.evaluate(() => (window as any).FastBeeAiMcpConnector.status().running)).toBe(false);
  // 终态 401：会话清除，轮询与心跳都不得继续
  expect(await page.evaluate(() => window.sessionStorage.getItem('fastbee.mcp.connector.session'))).toBeNull();
  const before = await page.evaluate(() => (window as any).cloudCalls.length);
  await page.clock.runFor(300000);
  const after = await page.evaluate(() => (window as any).cloudCalls);
  expect(after.length).toBe(before);
  expect(after.filter((c: any) => c.path === '/connector/v1/jobs/poll').length).toBe(1);
});

test('consecutive network failures are bounded and halt with unreachable state', async ({ page }) => {
  await setup(page);
  await pair(page);
  await page.evaluate(() => (window as any).FastBeeAiMcpConnector.start());
  await expect.poll(() => page.evaluate(() => (window as any).pendingPolls.length)).toBe(1);
  await page.evaluate(() => {
    const w = window as any;
    w.pollMode = 'error';
    w.pendingPolls.shift()();
  });
  // 退避重试有上限：推进时钟直到停机
  for (let i = 0; i < 60; i++) {
    await page.clock.runFor(5000);
    const running = await page.evaluate(() => (window as any).FastBeeAiMcpConnector.status().running);
    if (!running) break;
  }
  const status = await page.evaluate(() => (window as any).FastBeeAiMcpConnector.status());
  expect(status.running).toBe(false);
  expect(status.state).toBe('unreachable');
  const calls = await page.evaluate(() => (window as any).cloudCalls);
  const pollCount = calls.filter((c: any) => c.path === '/connector/v1/jobs/poll').length;
  expect(pollCount).toBeLessThanOrEqual(12);
  // 停机前上报一次 unreachable；会话保留（非终态 401，可恢复）
  expect(calls.some((c: any) => c.path === '/connector/v1/heartbeat' && c.body.state === 'unreachable')).toBe(true);
  expect(await page.evaluate(() => window.sessionStorage.getItem('fastbee.mcp.connector.session'))).not.toBeNull();
  // 停机后不再有任何云端请求
  const total = calls.length;
  await page.clock.runFor(300000);
  const later = await page.evaluate(() => (window as any).cloudCalls);
  expect(later.length).toBe(total);
});

test('pageshow with persisted resumes polling after pagehide', async ({ page }) => {
  await setup(page);
  await pair(page);
  await page.evaluate(() => (window as any).FastBeeAiMcpConnector.start());
  await expect.poll(() => page.evaluate(() => (window as any).pendingPolls.length)).toBe(1);
  await page.evaluate(() => window.dispatchEvent(new Event('pagehide')));
  await expect.poll(() => page.evaluate(() => (window as any).FastBeeAiMcpConnector.status().running)).toBe(false);
  // bfcache 恢复：pageshow.persisted 后继续轮询
  await page.evaluate(() => window.dispatchEvent(new PageTransitionEvent('pageshow', { persisted: true })));
  await expect.poll(() => page.evaluate(() => (window as any).FastBeeAiMcpConnector.status().running)).toBe(true);
  await expect.poll(() => page.evaluate(() => (window as any).pendingPolls.length)).toBe(2);
});

test('pair sends the real peripheral type catalog with device capabilities', async ({ page }) => {
  await setup(page);
  await pair(page);
  const calls = await page.evaluate(() => (window as any).cloudCalls);
  const pairCall = calls.find((c: any) => c.path === '/connector/v1/pair');
  // 能力载荷必须携带真实 /api/peripherals/types 目录（云端外设工具的 schema 来源）
  expect(pairCall.body.device.capabilities.peripheralCatalog.data.gpio[0].value).toBe(12);
  expect(pairCall.body.device.capabilities.peripheralCatalog.hardware.maxGpio).toBe(48);
  const apiCalls = await page.evaluate(() => (window as any).apiCalls);
  expect(apiCalls).toContain('/api/peripherals/types');
});

test('pair proceeds without catalog when the types endpoint fails and marks it unavailable', async ({ page }) => {
  await setup(page);
  await page.evaluate(() => { (window as any).typesFail = true; });
  const result = await page.evaluate(async (cloud: string) => {
    return await (window as any).FastBeeAiMcpConnector.pair(cloud, 'ABCD234567');
  }, CLOUD);
  // 目录读取失败不阻断配对；云端外设工具按目录缺失失败关闭
  expect(result.ok).toBe(true);
  const calls = await page.evaluate(() => (window as any).cloudCalls);
  const pairCall = calls.find((c: any) => c.path === '/connector/v1/pair');
  expect(pairCall.body.device.capabilities.peripheralCatalog).toBeUndefined();
  expect(pairCall.body.device.capabilities.peripheralCatalogAvailable).toBe(false);
});

test('capabilities refresh job returns the combined bounded catalog payload', async ({ page }) => {
  await setup(page);
  await pair(page);
  await page.evaluate(() => (window as any).FastBeeAiMcpConnector.start());
  await expect.poll(() => page.evaluate(() => (window as any).pendingPolls.length)).toBe(1);
  await page.evaluate(() => {
    (window as any).pendingPolls.shift()([
      { jobId: 'job-cap', type: 'device.capabilities.read', idempotencyKey: 'k-cap', deadlineAt: Date.now() + 60_000, leaseExpiresAt: Date.now() + 60_000 },
    ]);
  });
  await expect.poll(() => page.evaluate(() =>
    (window as any).cloudCalls.filter((c: any) => c.path.endsWith('/result')).length)).toBe(1);
  const calls = await page.evaluate(() => (window as any).cloudCalls);
  const result = calls.find((c: any) => c.path.endsWith('/result'));
  expect(result.body.status).toBe('SUCCEEDED');
  expect(result.body.result.mqtt).toBe(true);
  expect(result.body.result.peripheralCatalog.data.gpio[0].name).toBe('Digital Output');
});

test('multibyte catalog oversize drops the catalog by UTF-8 bytes without pairing rejection', async ({ page }) => {
  await setup(page);
  // 多字节目录：UTF-16 长度低于上限但 UTF-8 字节数超限；必须按字节口径放弃目录，
  // 且配对不被拒绝（云端按目录缺失失败关闭，不产生注册拒绝）
  await page.evaluate(() => {
    const w = window as any;
    const big = structuredClone(w.PERIPHERAL_TYPES);
    const cjk = '温度传感器汉字填充'.repeat(600); // 10 字 × 600 ≈ UTF-16 6000 字符 / UTF-8 18000 字节
    big.data.gpio[0].name = cjk;
    w.PERIPHERAL_TYPES = big;
  });
  const result = await page.evaluate(async (cloud: string) => {
    return await (window as any).FastBeeAiMcpConnector.pair(cloud, 'ABCD234567');
  }, CLOUD);
  expect(result.ok).toBe(true);
  const calls = await page.evaluate(() => (window as any).cloudCalls);
  const pairCall = calls.find((c: any) => c.path === '/connector/v1/pair');
  const capabilities = pairCall.body.device.capabilities;
  expect(capabilities.peripheralCatalog).toBeUndefined();
  expect(capabilities.peripheralCatalogAvailable).toBe(false);
  expect(capabilities.peripheralCatalogOversize).toBe(true);
});

test('pair sends the real execution catalog with device capabilities', async ({ page }) => {
  await setup(page);
  await pair(page);
  const calls = await page.evaluate(() => (window as any).cloudCalls);
  const pairCall = calls.find((c: any) => c.path === '/connector/v1/pair');
  // 执行目录经独立的 device.executionCatalog 字段（不占能力载荷预算），能力只带状态标记
  expect(pairCall.body.device.capabilities.executionCatalog).toBeUndefined();
  expect(pairCall.body.device.capabilities.executionCatalogAvailable).toBe(true);
  const exec = pairCall.body.device.executionCatalog;
  expect(exec.schemaVersion).toBe(1);
  expect(exec.firmwareVersion).toBe('1.2.3');
  expect(exec.configRevision).toBe('rev-a');
  expect(exec.triggerTypes.map((t: any) => t.type)).toEqual([0, 1, 4, 5]);
  expect(exec.events.map((e: any) => e.id)).toEqual(['wifi_connected', 'system_boot']);
  expect(exec.actions.map((a: any) => a.value)).toEqual([0, 4, 6, 10]);
  expect(exec.actions[0].targetTypes).toEqual([12, 16, 17]);
  expect(exec.actions[1].valueSchema).toEqual({ kind: 'integer', min: 0, max: 65535,
    relaxedByReceivedValue: true, receivedValueAllowed: true });
  expect(exec.controls.gpio[0]).toEqual({ id: 'rule_1', actionType: 0, enabled: true,
    targetPeriphId: 'do1', targets: [{ id: 'do1' }] });
  const apiCalls = await page.evaluate(() => (window as any).apiCalls);
  expect(apiCalls).toContain('/api/periph-exec/trigger-types');
  expect(apiCalls).toContain('/api/periph-exec/events/static');
  expect(apiCalls).toContain('/api/periph-exec/action-types');
  expect(apiCalls).toContain('/api/periph-exec/controls');
  expect(apiCalls).toContain('/api/mcp/config/snapshot');
});

test('pair proceeds without execution catalog when its endpoints fail and marks it unavailable', async ({ page }) => {
  await setup(page);
  await page.evaluate(() => { (window as any).execCatalogFail = true; });
  const result = await page.evaluate(async (cloud: string) => {
    return await (window as any).FastBeeAiMcpConnector.pair(cloud, 'ABCD234567');
  }, CLOUD);
  // 执行目录读取失败不阻断配对；云端规则工具按目录缺失失败关闭
  expect(result.ok).toBe(true);
  const calls = await page.evaluate(() => (window as any).cloudCalls);
  const pairCall = calls.find((c: any) => c.path === '/connector/v1/pair');
  expect(pairCall.body.device.executionCatalog).toBeNull();
  expect(pairCall.body.device.capabilities.executionCatalogAvailable).toBe(false);
  expect(pairCall.body.device.capabilities.executionCatalogFailed).toBe(true);
  // 外设目录不受影响
  expect(pairCall.body.device.capabilities.peripheralCatalog.data.gpio[0].value).toBe(12);
});

test('capabilities refresh job carries flags only; catalog travels on its own bounded job', async ({ page }) => {
  await setup(page);
  await pair(page);
  await page.evaluate(() => (window as any).FastBeeAiMcpConnector.start());
  await expect.poll(() => page.evaluate(() => (window as any).pendingPolls.length)).toBe(1);
  await page.evaluate(() => {
    (window as any).pendingPolls.shift()([
      { jobId: 'job-cap-exec', type: 'device.capabilities.read', idempotencyKey: 'k-capx', deadlineAt: Date.now() + 60_000, leaseExpiresAt: Date.now() + 60_000 },
    ]);
  });
  await expect.poll(() => page.evaluate(() =>
    (window as any).cloudCalls.filter((c: any) => c.path.endsWith('/result')).length)).toBe(1);
  let calls = await page.evaluate(() => (window as any).cloudCalls);
  let result = calls.find((c: any) => c.path.endsWith('/result'));
  expect(result.body.status).toBe('SUCCEEDED');
  // 能力刷新载荷只带状态标记与固件版本；目录本体不占用该载荷
  expect(result.body.result.executionCatalog).toBeUndefined();
  expect(result.body.result.executionCatalogAvailable).toBe(true);
  expect(result.body.result.firmwareVersion).toBe('1.2.3');

  // 独立的执行目录任务：有界载荷返回完整目录
  await page.evaluate(() => {
    (window as any).pendingPolls.shift()([
      { jobId: 'job-exec-cat', type: 'device.execution-catalog.read', idempotencyKey: 'k-exec', deadlineAt: Date.now() + 60_000, leaseExpiresAt: Date.now() + 60_000 },
    ]);
  });
  await expect.poll(() => page.evaluate(() =>
    (window as any).cloudCalls.filter((c: any) => c.path.includes('job-exec-cat')).length)).toBe(1);
  calls = await page.evaluate(() => (window as any).cloudCalls);
  result = calls.find((c: any) => c.path.includes('job-exec-cat'));
  expect(result.body.status).toBe('SUCCEEDED');
  expect(result.body.result.configRevision).toBe('rev-a');
  expect(result.body.result.actions).toHaveLength(4);
  expect(result.body.result.controls.gpio[0].id).toBe('rule_1');
});

test('malformed or duplicate catalog rows drop the whole execution catalog', async ({ page }) => {
  await setup(page);
  // 重复事件 ID：整个目录不可用（不挑选部分行伪造可用目录）
  await page.evaluate(() => {
    const w = window as any;
    const bad = structuredClone(w.STATIC_EVENTS);
    bad.data.push({ id: 'system_boot', name: '重复事件', category: '系统', type: 70 });
    w.STATIC_EVENTS = bad;
  });
  await pair(page);
  let calls = await page.evaluate(() => (window as any).cloudCalls);
  let pairCall = calls.find((c: any) => c.path === '/connector/v1/pair');
  expect(pairCall.body.device.executionCatalog).toBeNull();
  expect(pairCall.body.device.capabilities.executionCatalogAvailable).toBe(false);
});

test('action catalog without firmware metadata or with duplicate values is rejected', async ({ page }) => {
  await setup(page);
  await page.evaluate(() => {
    const w = window as any;
    const bad = structuredClone(w.ACTION_TYPES);
    delete bad.data.firmwareVersion;
    w.ACTION_TYPES = bad;
  });
  await pair(page);
  let calls = await page.evaluate(() => (window as any).cloudCalls);
  expect(calls.find((c: any) => c.path === '/connector/v1/pair')
    .body.device.capabilities.executionCatalogAvailable).toBe(false);

  await page.evaluate(() => {
    const w = window as any;
    const bad = structuredClone(w.ACTION_TYPES);
    bad.data.actions.push({ value: 0, name: 'ACTION_HIGH', requiresPeripheralTarget: true, targetTypes: [12] });
    w.ACTION_TYPES = bad;
  });
  const pairCall = await page.evaluate((cloud: string) => {
    return (window as any).FastBeeAiMcpConnector.pair(cloud, 'ABCD234567').then(() =>
      (window as any).cloudCalls.filter((c: any) => c.path === '/connector/v1/pair').pop());
  }, CLOUD);
  expect(pairCall.body.device.capabilities.executionCatalogAvailable).toBe(false);
});

test('oversize execution catalog is dropped from its own bounded channel without touching capabilities', async ({ page }) => {
  await setup(page);
  // 控制目录超界：事件/触发类型已裁剪展示名，超限压力来自控制行目标引用
  await page.evaluate(() => {
    const w = window as any;
    const big = structuredClone(w.CONTROLS);
    const targets = [];
    for (let j = 0; j < 90; j++) targets.push({ id: 'target_peripheral_pad_' + String(j).padStart(8, '0') });
    for (let i = 0; i < 95; i++) {
      big.data.gpio.push({ id: 'rule_pad_' + String(i).padStart(6, '0'), name: 'x', actionType: 0,
        enabled: true, actionCount: 1, targets: targets, hasSystemAction: false, hasSetMode: false, pollTaskIndex: [] });
    }
    w.CONTROLS = big;
  });
  const result = await page.evaluate(async (cloud: string) => {
    return await (window as any).FastBeeAiMcpConnector.pair(cloud, 'ABCD234567');
  }, CLOUD);
  expect(result.ok).toBe(true);
  const calls = await page.evaluate(() => (window as any).cloudCalls);
  const pairCall = calls.find((c: any) => c.path === '/connector/v1/pair');
  const capabilities = pairCall.body.device.capabilities;
  expect(pairCall.body.device.executionCatalog).toBeNull();
  expect(capabilities.executionCatalogAvailable).toBe(false);
  expect(capabilities.executionCatalogOversize).toBe(true);
  expect(capabilities.peripheralCatalog.data.gpio[0].value).toBe(12);
});

test('near-boundary execution catalog DTO round-trips intact through the bounded channel', async ({ page }) => {
  await setup(page);
  // 出站 DTO 压力样本：34 动作 + 33 控制行；41 生产 schema + 32 控制上限由 ADMIN 集成用例覆盖
  await page.evaluate(() => {
    const w = window as any;
    const big = structuredClone(w.CONTROLS);
    for (let i = 0; i < 32; i++) {
      big.data.gpio.push({ id: 'rule_near_max_' + String(i).padStart(16, '0'), name: '规则',
        actionType: 0, enabled: true, actionCount: 4, targetPeriphId: 'do1',
        targets: [{ id: 'do1' }, { id: 'pwm1' }, { id: 'i2c1' }, { id: 'modbus1' }],
        hasSystemAction: false, hasSetMode: false, pollTaskIndex: [] });
    }
    w.CONTROLS = big;
    const acts = structuredClone(w.ACTION_TYPES);
    for (let v = 11; v <= 40; v++) acts.data.actions.push({ value: v, name: 'ACTION_' + v,
      requiresPeripheralTarget: true, targetTypes: [12, 16, 17],
      valueSchema: { kind: 'nonempty-text', relaxedByReceivedValue: false, receivedValueAllowed: true } });
    w.ACTION_TYPES = acts;
  });
  await pair(page);
  const pairCall = (await page.evaluate(() => (window as any).cloudCalls))
    .find((c: any) => c.path === '/connector/v1/pair');
  const exec = pairCall.body.device.executionCatalog;
  // 近上界目录完整往返：动作数量、控制行数、值 schema 均不丢
  expect(exec.actions).toHaveLength(34);
  expect(exec.controls.gpio).toHaveLength(33);
  expect(exec.controls.gpio[1].targets).toHaveLength(4);
  expect(exec.actions[1].valueSchema.kind).toBe('integer');
  expect(pairCall.body.device.capabilities.executionCatalogAvailable).toBe(true);
  expect(pairCall.body.device.capabilities.executionCatalogOversize).toBeUndefined();
});

test('snapshot failure marks the execution catalog fetch as failed, not merely unavailable', async ({ page }) => {
  await setup(page);
  // revision 绑定读取失败：目录整体标记失败（供云端能力刷新保留旧注册），不伪造内容
  await page.evaluate(() => { (window as any).snapshotFail = true; });
  const result = await page.evaluate(async (cloud: string) => {
    return await (window as any).FastBeeAiMcpConnector.pair(cloud, 'ABCD234567');
  }, CLOUD);
  expect(result.ok).toBe(true);
  const calls = await page.evaluate(() => (window as any).cloudCalls);
  const capabilities = calls.find((c: any) => c.path === '/connector/v1/pair').body.device.capabilities;
  expect(capabilities.executionCatalog).toBeUndefined();
  expect(capabilities.executionCatalogAvailable).toBe(false);
  expect(capabilities.executionCatalogFailed).toBe(true);
});
