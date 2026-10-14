const PLAYER_COUNT = 4;
// The legacy 'dualsense' key represents a saved controller slot, including generic pads.
const INPUT_TYPES = new Set(['keyboardMouse', 'dualsense', 'disabled']);

function defaultPlayers() {
  return Array.from({ length: PLAYER_COUNT }, (_, index) => ({
    type: index === 0 ? 'keyboardMouse' : 'disabled',
    controllerSlot: null
  }));
}

function validatePlayers(players) {
  if (!Array.isArray(players) || players.length !== PLAYER_COUNT) {
    throw new Error('Player settings must contain exactly four player assignments.');
  }
  let keyboardPlayer = null;
  const controllerPlayers = new Map();
  return Array.from(players, (player, index) => {
    const number = index + 1;
    if (!player || typeof player !== 'object' || Array.isArray(player) || !INPUT_TYPES.has(player.type)) {
      throw new Error('Player ' + number + ': choose Keyboard + Mouse, Controller or Disabled.');
    }
    if (player.type === 'keyboardMouse') {
      if (keyboardPlayer !== null) {
        throw new Error('Keyboard + Mouse is already assigned to Player ' + keyboardPlayer + '. Assign it to only one player.');
      }
      keyboardPlayer = number;
    }
    if (player.type !== 'dualsense') return { type: player.type, controllerSlot: null };
    const slot = player.controllerSlot;
    if (!Number.isInteger(slot) || slot < 1 || slot > 3) {
      throw new Error('Player ' + number + ': choose Controller controller slot 1, 2 or 3.');
    }
    if (controllerPlayers.has(slot)) {
      throw new Error('Controller ' + slot + ' is already assigned to Player ' + controllerPlayers.get(slot) + '. Each controller can be assigned only once.');
    }
    controllerPlayers.set(slot, number);
    return { type: 'dualsense', controllerSlot: slot };
  });
}

function migratePlayerSettings(settings = {}) {
  if (!settings || typeof settings !== 'object' || Array.isArray(settings)) throw new Error('Invalid saved settings.');
  return {
    ...settings,
    players: Object.hasOwn(settings, 'players') ? validatePlayers(settings.players) : defaultPlayers()
  };
}

module.exports = { defaultPlayers, validatePlayers, migratePlayerSettings };
