import { test, expect } from '@playwright/test';
import { resolve } from 'node:path';
import { readFileSync } from 'node:fs';

const root = resolve(__dirname, '../../..');
const ORIGIN = 'http://ai-local.test';

async function install(page: any, active = true) {
  await page.route(ORIGIN + '/**', (route: any) => route.fulfill({
    contentType: 'text/html', body: '<html><body><div id="content"></div></body></html>' }));
  await page.goto(ORIGIN + '/');
  await page.locator('#content').evaluate((el: HTMLElement, html: string) => { el.innerHTML = html; },
    readFileSync(resolve(root, 'web-src/pages/ai.html'), 'utf8'));
  await page.evaluate(({ active }) => {
    const w = window as any;
    w.calls = [] as any[];
    w.jobs = 0;
    w.apiGet = async (path: string) => {
      w.calls.push({ method: 'GET', path });
      if (path === '/api/ai/config') return { success: true, data: {
        enabled: active, active, runtimeReason: active ? '' : 'user_disabled',
        baseUrl: 'https://api.deepseek.com', model: 'deepseek-chat', apiKeyConfigured: false, mcpBaseUrl: '' } };
      if (path.startsWith('/api/ai/jobs/')) return { success: true, data: { jobId: 'job-1', state: w.holdJob ? 'running' : 'completed',
        text: '已生成配置草案，请确认执行', proposalId: 'proposal-1' } };
      throw new Error('unexpected GET ' + path);
    };
    w.apiPost = async (path: string, body: any) => {
      w.calls.push({ method: 'POST', path, body });
      if (path === '/api/ai/jobs') return { success: true, data: { jobId: 'job-1' } };
      if (path === '/api/ai/proposals/proposal-1/execute') return { success: true, data: {
        accepted: true, persisted: true, readback: true, observed: false, physicalConfirmed: false } };
      if (path.endsWith('/cancel')) return { success: true, data: { cancelRequested: true } };
      throw new Error('unexpected POST ' + path);
    };
    w.apiPut = async (path: string, body: any) => {
      w.calls.push({ method: 'PUT', path, body });
      return { success: true, data: {} };
    };
    w.apiDelete = async (path: string) => { w.calls.push({ method: 'DELETE', path }); return { success: true, data: {} }; };
    w.AppState = { registerModule: (name: string, methods: any) => { w.ai = methods; } };
  }, { active });
  await page.addScriptTag({ path: resolve(root, 'web-src/modules/runtime/ai.js') });
  await page.evaluate(() => (window as any).ai.loadAiPage());
}

test('AI-LOCAL two tabs, independent provider/MCP config and no-MCP proposal flow', async ({ page }) => {
  await install(page);
  await expect(page.locator('[data-ai-tab="chat"]')).toBeVisible();
  await expect(page.locator('[data-ai-tab="config"]')).toBeVisible();
  await expect(page.locator('[data-ai-tab]')).toHaveCount(2);
  await expect(page.locator('#ai-document-upload')).toBeDisabled();
  await page.click('[data-ai-tab="config"]');
  await page.click('#ai-deepseek-preset');
  await page.click('#ai-save-provider');
  await page.click('#ai-save-mcp');
  const saves = await page.evaluate(() => (window as any).calls.filter((c: any) => c.method === 'PUT'));
  expect(saves[0].path).toBe('/api/ai/config');
  expect(saves[0].body.baseUrl).toBe('https://api.deepseek.com');
  expect(saves[1]).toEqual({ method: 'PUT', path: '/api/ai/mcp-config', body: { mcpBaseUrl: '' } });
  await page.click('[data-ai-tab="chat"]');
  await page.fill('#ai-message', '在 GPIO4 添加继电器');
  await page.click('#ai-send');
  await expect(page.locator('#ai-proposal')).toContainText('尚未写入设备');
  await page.click('#ai-confirm-proposal');
  await expect(page.locator('#ai-execution-result')).toContainText('已持久化');
  await expect(page.locator('#ai-execution-result')).toContainText('物理动作未确认');
  const apply = await page.evaluate(() => (window as any).calls.find((c: any) => c.path.includes('/execute')));
  expect(apply.body).toEqual({ confirmationText: '确认执行' });
});

test('AI-LOCAL disabled feature, loading errors and pagehide cleanup fail closed', async ({ page }) => {
  await install(page, false);
  await expect(page.locator('#ai-message')).toBeDisabled();
  await expect(page.locator('#ai-feature-hint')).toContainText('功能配置');
  await install(page, true);
  await page.evaluate(() => { (window as any).holdJob = true; });
  await page.fill('#ai-message', '问题');
  await page.click('#ai-send');
  await page.evaluate(() => window.dispatchEvent(new Event('pagehide')));
  const cancel = await page.evaluate(() => (window as any).calls.find((c: any) => c.path.endsWith('/cancel')));
  expect(cancel).toBeTruthy();
});

test('AI-LOCAL narrow layout and issue draft contain no secrets', async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await install(page);
  await page.click('#ai-issue-draft');
  const draft = await page.locator('#ai-issue-preview').inputValue();
  expect(draft).toContain('runtimeReason');
  expect(draft).not.toContain('apiKey');
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
});
