const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const { defaultInput, validateInput, profileText, prepareInput } = require('../src/input-profile.cjs');
async function fixture(t) {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'anyps5-input-'));
  t.after(async () => { assert.equal(path.dirname(root), os.tmpdir()); assert.ok(path.basename(root).startsWith('anyps5-input-')); await fs.rm(root, { recursive: true, force: true }); });
  const game = path.join(root, 'game with spaces'); await fs.mkdir(game);
  return { root, game, exe: path.join(game, 'app.exe'), data: path.join(root, 'profile') };
}
test('input defaults and accepted numeric boundaries match AnyPS5', () => {
  assert.deepEqual(defaultInput(), { leftStickDeadzone: 0, rightStickDeadzone: 0, mouseSensitivity: 1 });
  assert.deepEqual(validateInput({ leftStickDeadzone: 90, rightStickDeadzone: 0, mouseSensitivity: .001 }), { leftStickDeadzone: 90, rightStickDeadzone: 0, mouseSensitivity: .001 });
  for (const key of ['leftStickDeadzone', 'rightStickDeadzone']) for (const value of [-1, 91, 1.2, '1', NaN, null]) assert.throws(() => validateInput({ ...defaultInput(), [key]: value }), /deadzones/);
  for (const value of [0, -1, 11, Infinity, NaN, '1', null]) assert.throws(() => validateInput({ ...defaultInput(), mouseSensitivity: value }), /sensitivity/);
  for (const value of [null, [], {}, 'invalid']) assert.throws(() => validateInput(value));
});
test('launch passes a complete absolute profile and preserves unrelated environment without writing game files', async t => {
  const f = await fixture(t); const inherited = { PATH: 'fixture-path', SDL_GAMECONTROLLERCONFIG: 'preserve custom SDL map', anyps5_input_config: 'stale' };
  const launch = await prepareInput(f.exe, f.data, { leftStickDeadzone: 12, rightStickDeadzone: 8, mouseSensitivity: 2.5 }, inherited);
  assert.equal(launch.source, 'launcher'); assert.ok(path.isAbsolute(launch.configPath));
  assert.equal(launch.env.ANYPS5_INPUT_CONFIG, launch.configPath); assert.equal(launch.env.anyps5_input_config, undefined);
  assert.equal(launch.env.SDL_GAMECONTROLLERCONFIG, inherited.SDL_GAMECONTROLLERCONFIG); assert.equal(inherited.anyps5_input_config, 'stale');
  const lines=(await fs.readFile(launch.configPath,'utf8')).split('\n').filter(line=>line && !line.startsWith('#'));
  assert.deepEqual(lines,['LeftStickDeadzone = 12','RightStickDeadzone = 8','MouseSensitivity = 2.5']);
  assert.deepEqual(await fs.readdir(f.game), []);
});
test('existing per-game input config takes precedence and remains byte-for-byte intact', async t => {
  const f=await fixture(t); const custom=path.join(f.game,'anyps5-input.ini'); const original='\ufeffCross = KEY:F\r\nLeftStickDeadzone = 20\r\n'; await fs.writeFile(custom,original);
  const launch=await prepareInput(f.exe,f.data,defaultInput()); assert.equal(launch.source,'game'); assert.equal(launch.configPath,custom); assert.equal(await fs.readFile(custom,'utf8'),original); assert.equal(await fs.access(f.data).then(()=>true,()=>false),false);
});
test('profiles stay independent across launches and concurrent preparation never exposes partial files', async t => {
  const f=await fixture(t);const launches=await Promise.all(Array.from({length:8},()=>prepareInput(f.exe,f.data,defaultInput())));
  assert.equal(new Set(launches.map(x=>x.configPath)).size,1); assert.equal(await fs.readFile(launches[0].configPath,'utf8'),profileText(defaultInput()));
  const second=await prepareInput(f.exe,f.data,{...defaultInput(),mouseSensitivity:3});assert.notEqual(second.configPath,launches[0].configPath);assert.equal(await fs.readFile(launches[0].configPath,'utf8'),profileText(defaultInput()));
  assert.equal((await fs.readdir(path.join(f.data,'input-profiles'))).filter(x=>x.endsWith('.tmp')).length,0);
});
test('invalid per-game config target and altered cache fail without replacing them', async t => {
  const f=await fixture(t);const launch=await prepareInput(f.exe,f.data,defaultInput());await fs.writeFile(launch.configPath,'user edit');await assert.rejects(prepareInput(f.exe,f.data,defaultInput()),/modified/);assert.equal(await fs.readFile(launch.configPath,'utf8'),'user edit');
  await fs.mkdir(path.join(f.game,'anyps5-input.ini'));await assert.rejects(prepareInput(f.exe,f.data,defaultInput()),/regular/);
});
