// @ts-check
const { defineConfig } = require('@playwright/test');

module.exports = defineConfig({
  testDir: '.',
  timeout: 30000,
  retries: 0,
  use: {
    headless: true,
    baseURL: 'http://localhost:8080',
  },
  webServer: {
    command: 'python3 -m http.server 8080 -d ../../',
    port: 8080,
    reuseExistingServer: true,
  },
});
