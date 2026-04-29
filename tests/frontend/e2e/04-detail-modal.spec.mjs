import { test, expect } from '../fixtures.mjs';

test('카드 클릭 시 상세 모달이 열리고 alias 변경이 저장된다', async ({ page }) => {
    await page.goto('/');

    // 첫 카드 클릭
    await page.locator('.status-card').first().click();

    // 모달 열림
    const modal = page.locator('dialog.modal[open]');
    await expect(modal).toBeVisible();

    // alias prefill = '주방' (fixture)
    const aliasInput = modal.locator('input[type=text]').first();
    await expect(aliasInput).toHaveValue('주방');

    // alias 변경 후 저장
    await aliasInput.fill('주방-변경');
    await modal.locator('button.btn-blue:has-text("저장")').click();

    // 성공 status
    await expect(modal.locator('.status.ok')).toContainText('저장됨');

    // 모달 닫기 (X 버튼)
    await modal.locator('button.modal-close').click();
    await expect(page.locator('dialog.modal[open]')).toHaveCount(0);
});
