const { _electron: electron } = require('playwright');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '..');
(async () => {
  const temporary = await fs.mkdtemp(path.join(os.tmpdir(), 'anyps5-controller-ui-'));
  let desktop, page;
  try {
    const profile = path.join(temporary, 'profile'), game = path.join(temporary, 'game');
    await fs.mkdir(profile); await fs.mkdir(game); await fs.writeFile(path.join(game, 'app.exe'), 'Controller UI fixture; launch is intercepted.');
    await fs.writeFile(path.join(profile, 'library.json'), JSON.stringify({ settings: { audio: { enabled: false, musicVolume: 25, effectsVolume: 45 } }, games: ['Alpha', 'Beta', 'Gamma'].map(title => ({ id: title.toLowerCase(), title, directory: game, executable: 'app.exe', installedAt: '2026-10-13T12:00:00Z' })) }));
    desktop = await electron.launch({ executablePath: process.env.ANYPS5_TEST_APP || require('electron'), args: process.env.ANYPS5_TEST_APP ? [] : [root], env: { ...process.env, ANYPS5_DESKTOP_TEST_DATA: profile } });
    page = await desktop.firstWindow(); page.setDefaultTimeout(10000);
    const errors = []; page.on('pageerror', error => errors.push(error.message));
    await desktop.evaluate(({ BrowserWindow, ipcMain }) => {
      const window = BrowserWindow.getAllWindows()[0]; window.webContents.setAudioMuted(true); window.show(); window.focus();
      globalThis.controllerLaunches = [];
      ipcMain.removeHandler('game:launch'); ipcMain.handle('game:launch', (_event,id) => { globalThis.controllerLaunches.push(id); return {ok:true,value:{inputSource:'launcher'}}; });
    });
    await page.locator('#library-empty h1').waitFor();
    await page.evaluate(() => {
      window.testController = { index: 0, id: 'Xbox 360 Controller (STANDARD GAMEPAD)', mapping: 'standard', connected: true, axes: [0,0,0,0], buttons: Array.from({length:17},()=>({pressed:false,value:0})) };
      Object.defineProperty(navigator, 'getGamepads', {configurable:true,value:()=>[window.testController]});
      window.dispatchEvent(new Event('gamepadconnected'));
    });
    const focus = () => page.evaluate(() => document.activeElement.id || document.activeElement.dataset.navKey || document.activeElement.tagName);
    const focused = async target => { try { await page.waitForFunction(value => (document.activeElement.id || document.activeElement.dataset.navKey || document.activeElement.tagName) === value, target); } catch (error) { console.error('Expected focus '+target, await page.evaluate(()=>({active:document.activeElement.id||document.activeElement.dataset.navKey,focus:document.hasFocus(),hidden:document.hidden,hint:document.getElementById('keyboard-hint').textContent})));throw error; } };
    const setButton = (index,on) => page.evaluate(({index,on}) => { window.testController.buttons[index]={pressed:on,value:on?1:0}; },{index,on});
    const neutral = async () => { await page.evaluate(() => { testController.buttons.forEach(button=>{button.pressed=false;button.value=0;});testController.axes.fill(0); }); await page.waitForTimeout(120); };
    const press = async (index,hold=140) => { await setButton(index,true);await page.waitForTimeout(hold);await setButton(index,false);await page.waitForTimeout(120); };
    const toward = async (target,index,max=8) => { for(let n=0;n<max && await focus()!==target;n++)await press(index);assert.equal(await focus(),target); };
    await page.waitForFunction(()=>document.hasFocus()); await page.waitForTimeout(300); await neutral(); await focused('library');
    await press(15); await focused('game:alpha');assert.equal(await page.locator('#game-title').textContent(),'Alpha');
    await press(0,750);assert.deepEqual(await desktop.evaluate(()=>globalThis.controllerLaunches),['alpha']);
    assert.match(await page.locator('#keyboard-hint').textContent(),/A Select/);
    await press(1);await focused('library');await press(1,650);assert.ok(await page.locator('#exit-dialog').isVisible());
    await press(0);assert.ok(await page.locator('#exit-dialog').isHidden());
    console.log('PASS: controller shelf preview, one launch per held A, and B/Stay exit-menu behavior.');

    await press(12);await focused('nav-library');await toward('settings-open',15);await press(0);await focused('settings-tab-players');
    await press(13);await focused('player-1-source');await press(13);await focused('player-2-source');
    await press(0);await press(13);assert.equal(await page.locator('#player-2-source').inputValue(),'dualsense-1');
    await press(1);assert.equal(await page.locator('#player-2-source').inputValue(),'disabled');assert.ok(await page.locator('#settings-dialog').isVisible());
    await press(0);await press(13);await press(0);await press(1);assert.ok(await page.locator('#settings-dialog').isHidden());
    assert.equal((await page.evaluate(()=>desktop.state())).settings.players[1].type,'disabled');
    await press(0);await focused('settings-tab-players');await press(15);await focused('settings-tab-engine');await press(15);await focused('settings-tab-audio');
    await page.locator('#audio-enabled').click();
    await page.locator('#music-volume').click({position:{x:50,y:8}});const pointerValue=Number(await page.locator('#music-volume').inputValue());
    await press(15);assert.equal(Number(await page.locator('#music-volume').inputValue()),pointerValue+1);
    await press(1);assert.equal(await page.locator('#music-volume').inputValue(),'25');
    await press(0);await press(15);await press(0);assert.equal(await page.locator('#music-volume').inputValue(),'26');
    await press(1);assert.ok(await page.locator('#settings-dialog').isHidden());
    assert.equal((await page.evaluate(()=>desktop.state())).settings.audio.musicVolume,25);
    console.log('PASS: controller Settings tabs, select editing/confirm/revert, mixed mouse/controller slider editing and unsaved-close behavior.');

    await page.locator('#settings-open').click();await page.locator('#settings-tab-input').click();await page.locator('#input-left-deadzone').click();
    await press(15);assert.equal(await page.locator('#input-left-deadzone').inputValue(),'1');await press(1);assert.equal(await page.locator('#input-left-deadzone').inputValue(),'0');
    await press(0);await press(15);await press(0);await page.locator('#settings-form button[type=submit]').click();
    await page.locator('#settings-dialog').waitFor({state:'hidden'}); await focused('settings-open');
    assert.equal((await page.evaluate(()=>desktop.state())).settings.input.leftStickDeadzone,1);
    await page.keyboard.press('ArrowLeft');await focused('search-toggle');await page.keyboard.press('Enter');await page.keyboard.type('Alpha');
    await press(1);assert.equal(await page.locator('#search-input').inputValue(),'Alpha');await press(1);assert.ok(await page.locator('#search-box').isHidden());
    await page.locator('#nav-convert').click();await focused('pick-source');await press(1);await focused('library');
    await page.keyboard.press('ArrowRight');await focused('game:alpha');await press(15);await focused('game:beta');
    console.log('PASS: numeric settings with controller, mouse Save, keyboard typing, controller Back and seamless mouse/keyboard/controller handoff.');

    // Losing native focus must suspend the real frame loop, not just ignore one event.
    await desktop.evaluate(({BrowserWindow})=>BrowserWindow.getAllWindows()[0].hide());await setButton(0,true);await page.waitForTimeout(180);
    assert.deepEqual(await desktop.evaluate(()=>globalThis.controllerLaunches),['alpha']);
    await desktop.evaluate(({BrowserWindow})=>{const w=BrowserWindow.getAllWindows()[0];w.show();w.focus();});await page.waitForTimeout(180);
    assert.deepEqual(await desktop.evaluate(()=>globalThis.controllerLaunches),['alpha']);await neutral();await press(0);
    assert.deepEqual(await desktop.evaluate(()=>globalThis.controllerLaunches),['alpha','beta']);
    await page.evaluate(()=>{testController.connected=false;});await page.waitForTimeout(90);await setButton(0,true);await page.evaluate(()=>{testController.connected=true;});await page.waitForTimeout(180);
    assert.deepEqual(await desktop.evaluate(()=>globalThis.controllerLaunches),['alpha','beta']);await neutral();
    await page.evaluate(()=>{testController.mapping='';});await press(15);await focused('game:beta');
    await page.evaluate(()=>{testController.mapping='standard';testController.id='Sony DualSense Wireless Controller';});await neutral();await press(14);await focused('game:alpha');
    assert.match(await page.locator('#keyboard-hint').textContent(),/Cross Select/);
    await page.evaluate(()=>{testController.axes[0]=.2;});await page.waitForTimeout(160);await focused('game:alpha');
    await page.evaluate(()=>{testController.axes[0]=.8;});await focused('game:beta');await neutral();
    console.log('PASS: background input ignored, focus/reconnect waits for release, raw mappings ignored, PlayStation hints and analog-stick navigation.');
    await press(1);await focused('library');await press(1);await focused('exit-cancel');await press(15);await focused('exit-confirm');
    await fs.mkdir(path.join(root,'artifacts'),{recursive:true});await page.screenshot({path:path.join(root,'artifacts','20-controller-exit.png'),timeout:12000});
    assert.deepEqual(errors,[]);const closed=desktop.waitForEvent('close');await setButton(0,true);await closed;desktop=null;
    console.log('PASS: controller-only Exit closes the app. No renderer errors; game launches were simulated.');
  } finally { if(desktop)await desktop.close();if(path.dirname(temporary)===os.tmpdir()&&path.basename(temporary).startsWith('anyps5-controller-ui-'))await fs.rm(temporary,{recursive:true,force:true}); }
})().catch(error=>{console.error(error);process.exitCode=1;});
