/**
 * Playwright test fixture — doorman-esp 프론트엔드 v2 e2e용.
 *
 * 매 test 시작 전 mock 서버 상태를 reset하여 test 간 격리를 보장합니다.
 * mock 서버(/__ctl/auth_log)는 GET이므로 헬퍼도 GET으로 호출합니다.
 */

import { test as base, expect } from '@playwright/test';

export const test = base.extend({
    /** 매 테스트 시작 전 mock 상태 reset (state pollution 방지). */
    page: async ({ page, request }, use) => {
        await request.post('/__ctl/reset');
        await use(page);
    },
    /** 401 카운터 헬퍼 — 회귀 가드용. */
    authLog: async ({ request }, use) => {
        await use({
            async get() {
                const res = await request.get('/__ctl/auth_log');
                return res.json();
            },
        });
    },
});

export { expect };
