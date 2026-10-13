const test = require('node:test');
const assert = require('node:assert/strict');
const { defaultPlayers, validatePlayers, migratePlayerSettings } = require('../src/player-settings.cjs');

const fourInputs = () => [
  { type: 'keyboardMouse', controllerSlot: null },
  { type: 'dualsense', controllerSlot: 1 },
  { type: 'dualsense', controllerSlot: 2 },
  { type: 'dualsense', controllerSlot: 3 }
];

test('new profiles start with one keyboard player and three disabled players', () => {
  assert.deepEqual(defaultPlayers(), [
    { type: 'keyboardMouse', controllerSlot: null },
    { type: 'disabled', controllerSlot: null },
    { type: 'disabled', controllerSlot: null },
    { type: 'disabled', controllerSlot: null }
  ]);
  const changed = defaultPlayers();
  changed[0].type = 'disabled';
  assert.equal(defaultPlayers()[0].type, 'keyboardMouse');
});

test('legacy settings gain player defaults without losing other settings', () => {
  const saved = { relinker: 'C:/AnyPS5/relinker.exe', libs: 'C:/AnyPS5/libs', toIntel: false, custom: { keep: true } };
  const migrated = migratePlayerSettings(saved);
  assert.deepEqual(migrated, { ...saved, players: defaultPlayers() });
  assert.equal(Object.hasOwn(saved, 'players'), false);
  assert.deepEqual(migratePlayerSettings().players, defaultPlayers());
});

test('saved four-player assignments survive a JSON round trip and migration', () => {
  const saved = JSON.parse(JSON.stringify({ players: fourInputs(), destinationParent: 'D:/Games' }));
  assert.deepEqual(migratePlayerSettings(saved), saved);
  assert.notStrictEqual(migratePlayerSettings(saved).players, saved.players);
});

test('validation normalizes unused slots and does not mutate the supplied profile', () => {
  const players = defaultPlayers();
  players[0].controllerSlot = 2;
  players[1].controllerSlot = 3;
  players[2].extra = 'ignored';
  assert.deepEqual(validatePlayers(players), defaultPlayers());
  assert.equal(players[0].controllerSlot, 2);
  assert.equal(players[1].controllerSlot, 3);
});

test('the keyboard can belong to any one player and every player may be disabled', () => {
  const players = fourInputs();
  [players[0], players[3]] = [players[3], players[0]];
  assert.deepEqual(validatePlayers(players), players);
  const disabled = Array.from({ length: 4 }, () => ({ type: 'disabled', controllerSlot: null }));
  assert.deepEqual(validatePlayers(disabled), disabled);
});

test('the backend rejects a second keyboard assignment', () => {
  const players = defaultPlayers();
  players[3] = { type: 'keyboardMouse', controllerSlot: null };
  assert.throws(() => validatePlayers(players), /Keyboard \+ Mouse is already assigned to Player 1/);
});

test('the backend rejects a controller assigned to two players', () => {
  const players = fourInputs();
  players[3].controllerSlot = 1;
  assert.throws(() => validatePlayers(players), /Controller 1 is already assigned to Player 2/);
});

test('Controller slots must be integer numbers from one to three', () => {
  for (const invalid of [undefined, null, 0, 4, -1, 1.5, '1', true, NaN]) {
    const players = fourInputs();
    players[1].controllerSlot = invalid;
    assert.throws(() => validatePlayers(players), /Player 2: choose Controller controller slot 1, 2 or 3/);
  }
});

test('exactly four concrete assignments with supported types are required', () => {
  for (const invalid of [undefined, null, {}, [], defaultPlayers().slice(0, 3), [...defaultPlayers(), { type: 'disabled' }]]) {
    assert.throws(() => validatePlayers(invalid), /exactly four player assignments/);
  }
  for (const invalid of [null, undefined, [], 'dualsense', {}, { type: 'gamepad' }]) {
    const players = defaultPlayers();
    players[2] = invalid;
    assert.throws(() => validatePlayers(players), /Player 3: choose Keyboard \+ Mouse, Controller or Disabled/);
  }
  assert.throws(() => validatePlayers(new Array(4)), /Player 1:/);
});

test('migration never silently resets explicitly invalid saved player settings', () => {
  for (const players of [null, undefined, [], 'players']) {
    assert.throws(() => migratePlayerSettings({ players }), /exactly four player assignments/);
  }
  for (const invalid of [null, [], 'settings']) {
    assert.throws(() => migratePlayerSettings(invalid), /Invalid saved settings/);
  }
});
