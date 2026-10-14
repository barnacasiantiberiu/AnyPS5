const fs = require('node:fs/promises');
const path = require('node:path');
const crypto = require('node:crypto');
const { validWork, validWindow } = require('./loading-model.cjs');

function identity(metadata = {}) {
  return {
    gameId: String(metadata.gameId || metadata.id || ''),
    title: String(metadata.title || ''),
    executable: String(metadata.executable || '')
  };
}
function sameGame(left, right) {
  const executable = value => process.platform === 'win32' ? path.normalize(value).toLowerCase() : path.normalize(value);
  return Boolean(left.gameId && right.gameId && left.gameId === right.gameId) || Boolean(left.executable && right.executable && executable(left.executable) === executable(right.executable));
}
async function previousSession(root, metadata) {
  const files = (await fs.readdir(root)).filter(name => /^\d{13}-[\da-f-]+\.json$/.test(name)).sort().reverse().slice(0, 100);
  for (const name of files) {
    try {
      const file = path.join(root, name);
      if ((await fs.stat(file)).size > 65536) continue;
      const record = JSON.parse(await fs.readFile(file, 'utf8'));
      if (record.version === 1 && record.firstObservedAt && record.metadata && sameGame(record.metadata, metadata) && Number.isSafeInteger(record.lastCompletedMs) && record.lastCompletedMs >= 0) return record;
    } catch {}
  }
  return null;
}
async function createLoadingHistory(profile, metadata = {}) {
  const root = path.join(profile, 'loading-history');
  await fs.mkdir(root, { recursive: true });
  const game = identity(metadata);
  const previous = await previousSession(root, game);
  const started = Date.now();
  const sessionPath = path.join(root, `${String(started).padStart(13, '0')}-${crypto.randomUUID()}.json`);
  const state = {
    version: 1, metadata: game, startedAt: new Date(started).toISOString(), status: 'running',
    firstObservedAt: null, lastObservedAt: null, endedAt: null, runtimeElapsedMs: 0,
    lastCompletedMs: 0, completed: 0, failed: 0, observedActiveMs: 0,
    longestNoCompletionMs: 0, frames: 0, active: [0, 0, 0, 0],
    timingBasis: 'lastCompletedMs is renderer-start-relative. observedActiveMs and longestNoCompletionMs are sampled wall times, not total CPU time or an ETA.'
  };
  let sample = null, quietMs = 0, closed = false, pending = Promise.resolve();
  function update(work, window, now = Date.now()) {
    if (closed) return false;
    let changed = false;
    const fresh = validWork(work) && Math.abs(now - work.updatedAt) <= 2500;
    if (!fresh || (sample && (now - sample.observedAt > 2500 || now < sample.observedAt))) { sample = null; quietMs = 0; }
    if (fresh && (!sample || work.updatedAt > sample.updatedAt)) {
      const active = work.active.some(count => count > 0);
      const count = work.completed + work.failed;
      if (sample && work.elapsedMs >= sample.elapsedMs && work.updatedAt - sample.updatedAt <= 2500 && active && sample.active) {
        const duration = Math.max(0, Math.min(work.elapsedMs - sample.elapsedMs, work.updatedAt - sample.updatedAt, now - sample.observedAt));
        state.observedActiveMs += duration;
        quietMs = count === sample.count ? quietMs + duration : 0;
        state.longestNoCompletionMs = Math.max(state.longestNoCompletionMs, quietMs);
      } else quietMs = 0;
      state.firstObservedAt ||= new Date(now).toISOString();
      state.lastObservedAt = new Date(now).toISOString();
      state.runtimeElapsedMs = work.elapsedMs;
      state.lastCompletedMs = work.lastCompletedMs;
      state.completed = work.completed;
      state.failed = work.failed;
      state.active = [...work.active];
      sample = { observedAt: now, updatedAt: work.updatedAt, elapsedMs: work.elapsedMs, active, count };
      changed = true;
    }
    if (state.firstObservedAt && validWindow(window) && Math.abs(now - window.updatedAt) <= 2500 && state.frames !== window.frames) {
      state.frames = window.frames;
      changed = true;
    }
    return changed;
  }
  function save(reason = 'running') {
    if (!state.firstObservedAt) return pending.then(() => null);
    state.status = String(reason);
    state.savedAt = new Date().toISOString();
    const content = JSON.stringify(state, null, 2) + '\n';
    pending = pending.catch(() => {}).then(async () => {
      await fs.writeFile(sessionPath + '.tmp', content, 'utf8');
      await fs.rename(sessionPath + '.tmp', sessionPath);
      return sessionPath;
    });
    return pending;
  }
  async function stop(reason = 'exited') {
    if (closed) return pending;
    closed = true;
    state.endedAt = new Date().toISOString();
    return save(reason);
  }
  return { previous, sessionPath, update, save, stop };
}
module.exports = { createLoadingHistory };
