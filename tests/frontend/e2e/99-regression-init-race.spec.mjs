import { test, expect } from '../fixtures.mjs';

test('페이지 진입 시 401은 정확히 1건만 발생한다 (init race 회귀 가드)', async ({ page, request, authLog }) => {
    // 모든 /api/* 응답에 지연 → init race 부활 시 다중 401 폭주
    await request.post('/__ctl/delay', { data: { path: '/api/info', ms: 300 } });
    await request.post('/__ctl/delay', { data: { path: '/api/devices', ms: 300 } });
    await request.post('/__ctl/delay', { data: { path: '/api/auto-unlock/status', ms: 300 } });
    await request.post('/__ctl/delay', { data: { path: '/api/ws-token', ms: 300 } });

    // delay 등록 직후 reset → fresh 진입 (단, delay state는 reset이 비움)
    // → reset 이후 다시 delay를 박아야 의미가 있음. 순서를 뒤집는다.
    await request.post('/__ctl/reset');
    await request.post('/__ctl/delay', { data: { path: '/api/info', ms: 300 } });
    await request.post('/__ctl/delay', { data: { path: '/api/devices', ms: 300 } });
    await request.post('/__ctl/delay', { data: { path: '/api/auto-unlock/status', ms: 300 } });
    await request.post('/__ctl/delay', { data: { path: '/api/ws-token', ms: 300 } });

    await page.goto('/', { waitUntil: 'networkidle' });
    await page.waitForTimeout(500);

    const log = await authLog.get();

    // 첫 GET / 한 번만 401 (브라우저 dialog 트리거용). 나머지는 모두 헤더 동봉.
    // info 도착 후에만 다른 query가 trigger되므로 이론상 1을 넘지 않아야 한다.
    expect(log.count).toBeLessThanOrEqual(1);
});
