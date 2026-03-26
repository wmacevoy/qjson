// @ts-check
const { test, expect } = require('@playwright/test');

test.describe('Tutorial (docs/tutorial.html)', () => {
  test('loads without crash', async ({ page }) => {
    await page.goto('/docs/tutorial.html');
    // Should have a status element
    const status = page.locator('#status');
    await expect(status).toBeVisible();
  });

  test('shows WASM status message', async ({ page }) => {
    await page.goto('/docs/tutorial.html');
    // Wait for init() to complete (either success or error)
    await page.waitForFunction(() => {
      var s = document.getElementById('status');
      return s && s.innerHTML.length > 0 && !s.innerHTML.includes('Loading');
    }, null, { timeout: 15000 });

    const status = page.locator('#status');
    const html = await status.innerHTML();
    // Without WASM files present, should show helpful error (not crash)
    const isReady = html.includes('WASM ready');
    const isError = html.includes('WASM not found') || html.includes('WASM failed');
    expect(isReady || isError).toBeTruthy();
  });

  test('has playground sections with Run buttons', async ({ page }) => {
    await page.goto('/docs/tutorial.html');
    // Should have at least one Run button
    const runButtons = page.locator('button:has-text("Run")');
    const count = await runButtons.count();
    expect(count).toBeGreaterThan(0);
  });

  test('has code textareas', async ({ page }) => {
    await page.goto('/docs/tutorial.html');
    const textareas = page.locator('textarea');
    const count = await textareas.count();
    expect(count).toBeGreaterThan(0);
  });

  test('has link to full docs', async ({ page }) => {
    await page.goto('/docs/tutorial.html');
    await expect(page.locator('a[href="qjson.md"]')).toBeVisible();
  });
});

test.describe('Tutorial with WASM', () => {
  test.skip(
    () => !process.env.QJSON_WASM_AVAILABLE,
    'WASM files not available (set QJSON_WASM_AVAILABLE=1)'
  );

  test('WASM initializes and shows ready', async ({ page }) => {
    await page.goto('/docs/tutorial.html');
    await page.waitForFunction(() => {
      var s = document.getElementById('status');
      return s && s.innerHTML.includes('WASM ready');
    }, null, { timeout: 20000 });
    await expect(page.locator('#status')).toContainText('WASM ready');
  });

  test('compound interest solver runs', async ({ page }) => {
    await page.goto('/docs/tutorial.html');
    await page.waitForFunction(() => {
      var s = document.getElementById('status');
      return s && s.innerHTML.includes('WASM ready');
    }, null, { timeout: 20000 });

    // Find and click the first Run button (compound interest)
    const firstRun = page.locator('button:has-text("Run")').first();
    await firstRun.click();

    // Wait for output
    await page.waitForTimeout(2000);
    const output = page.locator('.output').first();
    const text = await output.textContent();
    expect(text.length).toBeGreaterThan(0);
  });
});
