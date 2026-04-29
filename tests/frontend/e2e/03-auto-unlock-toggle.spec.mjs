import { test, expect } from '../fixtures.mjs';

test('자동 열림 토글이 OFF→ON 전환되고 다시 클릭 시 OFF로 돌아온다', async ({ page }) => {
    await page.goto('/');

    const pill = page.locator('button.pill');
    await expect(pill).toHaveText('OFF');

    await pill.click();
    await expect(pill).toHaveText('ON');

    await pill.click();
    await expect(pill).toHaveText('OFF');
});
