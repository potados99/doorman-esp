import { defineConfig } from '@playwright/test';

/* Mock 서버를 webServer로 띄워 baseURL 통일.
 * MOCK_SERVE_FILE 미지정 시 mock-server는 frontend/index.html(swap 후의 v2)을
 * 기본으로 서빙 — 별도 env 불필요. */
export default defineConfig({
  testDir: 'tests/frontend/e2e',
  fullyParallel: false,
  retries: 0,
  workers: 1,
  reporter: 'list',
  use: {
    baseURL: 'http://127.0.0.1:8787',
    httpCredentials: { username: 'admin', password: 'doorman' },
    trace: 'retain-on-failure',
  },
  webServer: {
    command: 'node tests/frontend/mock-server.mjs',
    url: 'http://127.0.0.1:8787/__ctl/health',
    reuseExistingServer: !process.env.CI,
    timeout: 10_000,
  },
});
