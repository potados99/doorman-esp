import { test, expect } from '../fixtures.mjs';

test('로그인 후 메인 탭이 디바이스 카드와 함께 렌더링된다', async ({ page }) => {
    await page.goto('/');

    // Header
    await expect(page.locator('h1')).toHaveText('Doorman');

    // 메인 탭 default active
    await expect(page.locator('.tab.active')).toHaveText('메인');

    // 문 열기 버튼
    await expect(page.locator('button.door-btn')).toBeVisible();

    // 자동 열림 토글
    await expect(page.locator('button.pill')).toBeVisible();

    // 디바이스 카드 (fixture 3개)
    await expect(page.locator('.status-card')).toHaveCount(3);

    // 첫 카드의 alias '주방'
    await expect(page.locator('.status-card .card-alias').first()).toHaveText('주방');
});
