import { test, expect } from '../fixtures.mjs';

test('thumb을 우측 끝까지 끌면 mutation이 호출되고 성공 메시지가 표시된다', async ({ page }) => {
    await page.goto('/');

    const slider = page.locator('.door-slider');
    const handle = page.locator('.door-slider-handle');

    const sliderBox = await slider.boundingBox();
    const handleBox = await handle.boundingBox();

    const startX = handleBox.x + handleBox.width / 2;
    const y      = handleBox.y + handleBox.height / 2;
    const endX   = sliderBox.x + sliderBox.width - 4;

    /* 정확히 끝(maxX)에 닿아야 트리거 — Playwright mouse → pointer events 매핑. */
    await page.mouse.move(startX, y);
    await page.mouse.down();
    await page.mouse.move(endX, y, { steps: 12 });
    await page.mouse.up();

    await expect(page.locator('.status.ok')).toContainText('문이 열렸습니다');
});
