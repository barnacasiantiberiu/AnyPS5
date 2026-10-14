const { _electron: electron } = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const { selfExecutable } = require('./fixtures/self.cjs');
const { desktopLaunchOptions } = require('../scripts/launch.cjs');

(async () => {
  const parent = path.resolve(os.tmpdir());
  const temporary = await fs.mkdtemp(path.join(parent, 'anyps5-self-ui-'));
  let desktop;
  try {
    desktop = await electron.launch(desktopLaunchOptions([], {
      ...process.env,
      ANYPS5_DESKTOP_EXE: process.env.ANYPS5_TEST_APP || '',
      ANYPS5_DESKTOP_TEST_DATA: path.join(temporary, 'profile')
    }));
    const page = await desktop.firstWindow();
    page.setDefaultTimeout(10000);
    await page.locator('#nav-convert').click();
    for (const ps5 of [true, false]) {
      for (const encrypted of [true, false]) {
        const folder = path.join(temporary, `${ps5}-${encrypted}`);
        await fs.mkdir(folder);
        const file = path.join(folder, 'eboot.bin');
        const bytes = selfExecutable({ ps5, encrypted });
        await fs.writeFile(file, bytes);
        await desktop.evaluate(({ dialog }, selected) => {
          dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [selected] });
        }, folder);
        await page.locator('#pick-source').click();
        await page.waitForFunction(() => !document.getElementById('pick-source').disabled);
        assert.equal(await page.locator('#to-step-2').isDisabled(), encrypted);
        if (encrypted) {
          assert.equal(await page.locator('#wizard-error').isVisible(), true);
          assert.match(await page.locator('#wizard-error').textContent(), /Encrypted SELF payload.*decrypted ELF.*cannot decrypt/);
        } else {
          assert.equal(await page.locator('#wizard-error').isVisible(), false);
          assert.equal(await page.locator('#entry-input').inputValue(), file);
        }
        assert.deepEqual(await fs.readFile(file), bytes);
      }
    }
    console.log('PASS: both SELF signatures show encrypted-input guidance, block Continue, accept plain inputs and preserve originals.');
  } finally {
    if (desktop) await desktop.close();
    assert.equal(path.dirname(path.resolve(temporary)), parent);
    assert.ok(path.basename(temporary).startsWith('anyps5-self-ui-'));
    await fs.rm(temporary, { recursive: true, force: true });
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
