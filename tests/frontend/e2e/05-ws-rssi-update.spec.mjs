import { test, expect } from '../fixtures.mjs';

test('WS RSSI push 시 해당 카드만 RSSI 텍스트가 변경된다', async ({ page, request }) => {
    await page.goto('/');

    // WS 연결 대기
    await page.waitForTimeout(800);

    const targetMac = '11:22:33:44:55:66';  // 첫 fixture device (주방)
    const otherMac = 'AA:BB:CC:DD:EE:FF';   // 두 번째 (안방)

    // 첫 카드를 present 상태로 + RSSI -55
    await request.post('/__ctl/push', { data: `I (1) sm: ${targetMac} present` });
    await request.post('/__ctl/push', { data: `I (2) bt: ${targetMac} rssi=-55` });
    await page.waitForTimeout(300);

    // 카드의 mac 텍스트로 안정적 selector 구성
    const target = page.locator('.status-card').filter({ hasText: targetMac });
    const other = page.locator('.status-card').filter({ hasText: otherMac });

    // target 카드는 RSSI 표시
    await expect(target.locator('.card-status')).toContainText('재실');
    await expect(target.locator('.card-status')).toContainText('-55');

    // other 카드는 영향 없음 — '미감지'
    await expect(other.locator('.card-status')).toContainText('미감지');

    // 추가 push로 RSSI 변경 (target만)
    await request.post('/__ctl/push', { data: `I (3) bt: ${targetMac} rssi=-70` });
    await page.waitForTimeout(150);
    await expect(target.locator('.card-status')).toContainText('-70');
    await expect(other.locator('.card-status')).toContainText('미감지');
});
