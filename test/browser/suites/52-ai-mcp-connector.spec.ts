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
      if (path === '/api/device/config') return structuredClone((window as any).DEVICE_CONFIG);
      throw new Error('unexpected device api: ' + path);
    };
    w.apiPost = async (path: string, body: any) => {
      w.apiPostCalls.push({ path, body });
      if (path === '/api/mcp/config/apply') return { success: true, data: { operationId: 'op-browser', state: 'SUCCEEDED' } };
      if (path === '/api/mcp/config/preflight') return { success: true, data: { allowed: true, persisted: false } };
      if (path === '/api/mcp/config/readback') {
        if (w.readbackDisconnect) throw new TypeError('network down');
        return { success: true, data: { operationId: 'op-browser', state: 'SUCCEEDED' } };
      }
      if (path === '/api/mcp/config/rollback') return { success: true, data: { operationId: 'op-browser', state: 'ROLLED_BACK' } };
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
  }, { DEVICE_INFO, SYSTEM_INFO, CAPABILITIES, DEVICE_CONFIG });
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
