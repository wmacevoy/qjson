// @ts-check
const { test, expect } = require('@playwright/test');

test.describe('Landing page (index.html)', () => {
  test('loads and has correct title', async ({ page }) => {
    await page.goto('/');
    await expect(page).toHaveTitle(/QJSON/);
  });

  test('shows install commands', async ({ page }) => {
    await page.goto('/');
    await expect(page.locator('.install >> text=pip install qjson')).toBeVisible();
    await expect(page.locator('.install >> text=npm install qjson')).toBeVisible();
  });

  test('has links to tutorial and docs', async ({ page }) => {
    await page.goto('/');
    const tutorial = page.locator('a[href="docs/tutorial.html"]');
    await expect(tutorial).toBeVisible();
    const docs = page.locator('a[href="docs/qjson.md"]');
    await expect(docs).toBeVisible();
  });

  test('tells the story', async ({ page }) => {
    await page.goto('/');
    // The problem
    await expect(page.locator('text=0.30000000000000004')).toBeVisible();
    // The trick
    await expect(page.locator('text=indexed speed')).toBeVisible();
    // Solve backwards
    await expect(page.locator('text=Solve backwards')).toBeVisible();
    // Datalog
    await expect(page.locator('text=Datalog in JSON')).toBeVisible();
  });

  test('shows feature cards', async ({ page }) => {
    await page.goto('/');
    await expect(page.locator('.card >> text=Encrypted storage')).toBeVisible();
    await expect(page.locator('.card >> text=Complex keys')).toBeVisible();
    await expect(page.locator('.card >> text=One extension')).toBeVisible();
  });
});
