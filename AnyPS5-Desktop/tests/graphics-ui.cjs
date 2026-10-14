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
    await fs.writeFile(path.join(profile, 'library.json'), JSON.stringify({ settings: { graphics: { resolution: '1920x1080' } }, games: [] }));
    desktop = await electron.launch({ executablePath: process.env.ANYPS5_TEST_APP || require('electron'), args: process.env.ANYPS5_TEST_APP ? [] : [path.resolve(__dirname, '..')], env: { ...process.env, ANYPS5_DESKTOP_TEST_DATA: profile } });
    const page = await desktop.firstWindow();
    page.setDefaultTimeout(10000);
    await page.locator('#settings-open').click();
    await page.locator('#settings-tab-graphics').click();
    const select = page.locator('#graphics-resolution');
    assert.equal(await select.inputValue(), 'native');
    assert.equal(await select.locator('option').count(), 6);
    assert.equal(await select.locator('option:disabled').count(), 5);
    await page.locator('#settings-form button[type=submit]').click();
    await page.locator('#settings-dialog').waitFor({ state: 'hidden' });
    const saved = JSON.parse(await fs.readFile(path.join(profile, 'library.json'), 'utf8'));
    assert.equal(saved.settings.graphics.resolution, 'native');
    await page.locator('#settings-open').click();
    await page.locator('#settings-tab-graphics').click();
    assert.equal(await select.inputValue(), 'native');
    await select.selectOption('native');
    await page.locator('#settings-form button[type=submit]').click();
    await page.locator('#settings-dialog').waitFor({ state: 'hidden' });
    await desktop.close(); desktop = null;
    desktop = await electron.launch({ executablePath: process.env.ANYPS5_TEST_APP || require('electron'), args: process.env.ANYPS5_TEST_APP ? [] : [path.resolve(__dirname, '..')], env: { ...process.env, ANYPS5_DESKTOP_TEST_DATA: profile } });
    const restored = await desktop.firstWindow();
    await restored.locator('#settings-open').click();
    await restored.locator('#settings-tab-graphics').click();
    assert.equal(await restored.locator('#graphics-resolution').inputValue(), 'native');
    if (process.env.ANYPS5_GRAPHICS_SCREENSHOT) await restored.screenshot({ path: process.env.ANYPS5_GRAPHICS_SCREENSHOT });
    console.log('PASS: legacy resolution migrates to original; unsafe presets stay disabled after restart.');
  } finally {
    if (desktop) await desktop.close();
    await fs.rm(temporary, { recursive: true, force: true });
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
