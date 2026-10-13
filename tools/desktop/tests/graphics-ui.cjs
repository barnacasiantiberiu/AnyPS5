const { _electron: electron } = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
(async () => {
  const temporary = await fs.mkdtemp(path.join(os.tmpdir(), 'anyps5-graphics-ui-'));
  let desktop;
  try {
    const profile = path.join(temporary, 'profile');
    await fs.mkdir(profile);
    await fs.writeFile(path.join(profile, 'library.json'), JSON.stringify({ settings: {}, games: [] }));
    desktop = await electron.launch({ executablePath: process.env.ANYPS5_TEST_APP || require('electron'), args: process.env.ANYPS5_TEST_APP ? [] : [path.resolve(__dirname, '..')], env: { ...process.env, ANYPS5_DESKTOP_TEST_DATA: profile } });
    const page = await desktop.firstWindow();
    page.setDefaultTimeout(10000);
    await page.locator('#settings-open').click();
    await page.locator('#settings-tab-graphics').click();
    const select = page.locator('#graphics-resolution');
    assert.equal(await select.inputValue(), '1920x1080');
    assert.equal(await select.locator('option').count(), 6);
    await select.selectOption('1280x720');
    await page.locator('#settings-form button[type=submit]').click();
    await page.locator('#settings-dialog').waitFor({ state: 'hidden' });
    const saved = JSON.parse(await fs.readFile(path.join(profile, 'library.json'), 'utf8'));
    assert.equal(saved.settings.graphics.resolution, '1280x720');
    await page.locator('#settings-open').click();
    await page.locator('#settings-tab-graphics').click();
    assert.equal(await select.inputValue(), '1280x720');
    await select.selectOption('1920x1080');
    await page.locator('#settings-form button[type=submit]').click();
    await page.locator('#settings-dialog').waitFor({ state: 'hidden' });
    await desktop.close(); desktop = null;
    desktop = await electron.launch({ executablePath: process.env.ANYPS5_TEST_APP || require('electron'), args: process.env.ANYPS5_TEST_APP ? [] : [path.resolve(__dirname, '..')], env: { ...process.env, ANYPS5_DESKTOP_TEST_DATA: profile } });
    const restored = await desktop.firstWindow();
    await restored.locator('#settings-open').click();
    await restored.locator('#settings-tab-graphics').click();
    assert.equal(await restored.locator('#graphics-resolution').inputValue(), '1920x1080');
    if (process.env.ANYPS5_GRAPHICS_SCREENSHOT) await restored.screenshot({ path: process.env.ANYPS5_GRAPHICS_SCREENSHOT });
    console.log('PASS: packaged Graphics selector, presets, saved 720p/1080p and restart persistence.');
  } finally {
    if (desktop) await desktop.close();
    await fs.rm(temporary, { recursive: true, force: true });
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
