import { test, expect } from '@playwright/test';

const ORIGIN = 'http://mcp-config.test';

async function installDevice(page: any) {
  await page.route(ORIGIN + '/**', async (route: any) => {
    const request = route.request();
    const url = new URL(request.url());
    if (url.pathname === '/') {
      return route.fulfill({ contentType: 'text/html', body: '<html><body></body></html>' });
    }
    return route.continue();
  });
  await page.goto(ORIGIN + '/');
  await page.evaluate(() => {
    const w = window as any;
    w.device = {
      revision: 'rev-a',
      applyCalls: 0,
      rollbackCalls: 0,
      operation: null as any,
      values: { features: { enabled: false }, peripherals: { count: 0 }, protocol: { password: 'secret' } },
    };
    w.api = async (path: string, method = 'GET', body?: any) => {
      const response = await fetch(path, {
        method,
        headers: body ? { 'Content-Type': 'application/json' } : undefined,
        body: body ? JSON.stringify(body) : undefined,
      });
      return { status: response.status, body: await response.json() };
    };
  });
  await page.route(ORIGIN + '/api/mcp/**', async (route: any) => {
    const request = route.request();
    const url = new URL(request.url());
    const method = request.method();
    const input = request.postData() ? JSON.parse(request.postData()) : null;
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify(await page.evaluate(({ path, method, input }: any) => {
        const d = (window as any).device;
        if (path === '/api/mcp/capabilities') {
          return { success: true, revision: d.revision, sections: {
            features: { schemaVersion: 1, readOnly: false },
            network: { schemaVersion: 1, readOnly: true },
            aiKey: { schemaVersion: 1, readOnly: true },
          } };
        }
        if (path === '/api/mcp/config/snapshot') {
          return { success: true, revision: d.revision, sections: {
            features: { hash: 'feature-hash', value: d.values.features },
            protocol: { hash: 'protocol-hash', value: { password: '********', passwordConfigured: true } },
          } };
        }
        if (path === '/api/mcp/config/preflight') {
          return { success: true, allowed: true, operations: input.operations.length, persisted: false };
        }
        if (path === '/api/mcp/config/apply') {
          if (d.operation) return d.operation;
          if (input.baseRevision !== d.revision) {
            return { success: false, code: 'CONFIG_REVISION_CONFLICT' };
          }
          ++d.applyCalls;
          d.operation = { success: !input.operations[0].patch.unknown, operationId: 'op-1',
            state: input.operations[0].patch.unknown ? 'UNKNOWN' : 'SUCCEEDED',
            pendingRestart: !!input.operations[0].patch.pendingRestart,
            readbackRequired: !!input.operations[0].patch.unknown,
            steps: input.operations.map((op: any) => ({ section: op.section, persisted: !op.patch.unknown,
              readback: !op.patch.unknown, observed: false, physicalConfirmed: false,
              pendingRestart: !!op.patch.pendingRestart })) };
          if (!d.operation.readbackRequired) d.revision = 'rev-b';
          return d.operation;
        }
        if (path === '/api/mcp/config/operations/op-1') {
          return d.operation;
        }
        if (path === '/api/mcp/config/rollback') {
          if (d.operation.state === 'UNKNOWN') return { ...d.operation, success: false, code: 'OUTCOME_UNKNOWN' };
          ++d.rollbackCalls;
          d.operation.state = 'ROLLED_BACK';
          d.operation.success = true;
          return d.operation;
        }
        return { success: false, code: 'INVALID_ARGUMENT' };
      }, { path: url.pathname, method, input }))
    });
  });
}

test('snapshot is redacted and read-only capabilities share a stable revision', async ({ page }) => {
  await installDevice(page);
  const result = await page.evaluate(async () => {
    const w = window as any;
    return { capabilities: await w.api('/api/mcp/capabilities'), first: await w.api('/api/mcp/config/snapshot'),
      second: await w.api('/api/mcp/config/snapshot') };
  });
  expect(result.capabilities.body.sections.network.readOnly).toBe(true);
  expect(result.capabilities.body.sections.aiKey.readOnly).toBe(true);
  expect(result.first.body.revision).toBe(result.second.body.revision);
  expect(result.first.body.sections.protocol.value.password).toBe('********');
  expect(JSON.stringify(result)).not.toContain('secret"');
});

test('conflict, idempotent apply, pending restart and operation readback stay distinct', async ({ page }) => {
  await installDevice(page);
  const result = await page.evaluate(async () => {
    const w = window as any;
    const operations = [{ section: 'peripherals', schemaVersion: 1, patch: { pendingRestart: true } }];
    const conflict = await w.api('/api/mcp/config/apply', 'POST', { baseRevision: 'stale', operations });
    const body = { draftId: 'draft-1', baseRevision: 'rev-a', approvalHash: 'approval', idempotencyKey: 'key-1', operations };
    const preflight = await w.api('/api/mcp/config/preflight', 'POST', { operations });
    const first = await w.api('/api/mcp/config/apply', 'POST', body);
    const duplicate = await w.api('/api/mcp/config/apply', 'POST', body);
    const operation = await w.api('/api/mcp/config/operations/op-1');
    return { conflict, preflight, first, duplicate, operation, applyCalls: w.device.applyCalls };
  });
  expect(result.conflict.body.code).toBe('CONFIG_REVISION_CONFLICT');
  expect(result.preflight.body.persisted).toBe(false);
  expect(result.first.body.operationId).toBe(result.duplicate.body.operationId);
  expect(result.applyCalls).toBe(1);
  expect(result.operation.body.pendingRestart).toBe(true);
  expect(result.operation.body.steps[0].observed).toBe(false);
  expect(result.operation.body.steps[0].physicalConfirmed).toBe(false);
});

test('unknown outcome permits readback but blocks rollback or replay until resolved', async ({ page }) => {
  await installDevice(page);
  const result = await page.evaluate(async () => {
    const w = window as any;
    const body = { draftId: 'draft-u', baseRevision: 'rev-a', approvalHash: 'approval', idempotencyKey: 'key-u',
      operations: [{ section: 'features', schemaVersion: 1, patch: { unknown: true } }] };
    const first = await w.api('/api/mcp/config/apply', 'POST', body);
    const duplicate = await w.api('/api/mcp/config/apply', 'POST', body);
    const operation = await w.api('/api/mcp/config/operations/op-1');
    const rollback = await w.api('/api/mcp/config/rollback', 'POST', {
      operationId: 'op-1', approvalHash: 'new-recovery-approval', idempotencyKey: 'rollback-u' });
    return { first, duplicate, operation, rollback, applyCalls: w.device.applyCalls, rollbackCalls: w.device.rollbackCalls };
  });
  expect(result.first.body.state).toBe('UNKNOWN');
  expect(result.duplicate.body.operationId).toBe('op-1');
  expect(result.operation.body.readbackRequired).toBe(true);
  expect(result.applyCalls).toBe(1);
  expect(result.rollbackCalls).toBe(0);
  expect(result.rollback.body.code).toBe('OUTCOME_UNKNOWN');
});
