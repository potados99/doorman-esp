import { test, expect } from '../fixtures.mjs';

test('문 열기 버튼 클릭 시 mutation이 호출되고 성공 메시지가 표시된다', async ({ page }) => {
    await page.goto('/');

    const doorBtn = page.locator('button.door-btn');
    await doorBtn.click();

    // mutation 성공 → status div에 표시
    await expect(page.locator('.status.ok')).toContainText('문이 열렸습니다');
});
