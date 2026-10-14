const stages = ['Compiling shaders', 'Building graphics pipelines', 'Optimizing pipelines', 'Building compute pipelines'];
const integer = value => Number.isSafeInteger(value) && value >= 0;
function validWork(value) {
  return value?.version === 1 && integer(value.updatedAt) && integer(value.elapsedMs) && integer(value.lastCompletedMs) && value.lastCompletedMs <= value.elapsedMs && integer(value.completed) && integer(value.failed) && Array.isArray(value.active) && value.active.length === stages.length && value.active.every(integer);
}
function validWindow(value) {
  return value?.version === 1 && integer(value.updatedAt) && integer(value.frames) && typeof value.focused === 'boolean' && ['x', 'y', 'width', 'height'].every(key => Number.isSafeInteger(value[key])) && value.width > 0 && value.height > 0 && value.width < 65536 && value.height < 65536;
}
class LoadingModel {
  constructor() { this.frames = null; this.frameAt = 0; this.shownAt = null; this.completed = 0; this.completedAt = 0; this.activeAt = null; this.activeElapsedMs = 0; this.wasActive = false; this.finishedAt = null; }
  update(work, window, now = Date.now()) {
    if (!validWindow(window) || Math.abs(now - window.updatedAt) > 2500 || !window.focused) return { visible: false };
    if (this.frames !== window.frames) { this.frames = window.frames; this.frameAt = now; }
    const idleMs = now - this.frameAt;
    const known = validWork(work);
    const fresh = known && Math.abs(now - work.updatedAt) <= 2500;
    if (fresh && this.completed !== work.completed + work.failed) {
      this.completed = work.completed + work.failed;
      this.completedAt = now;
    }
    const active = fresh ? work.active.reduce((sum, count) => sum + count, 0) : 0;
    if (active > 0) {
      if (this.activeAt === null) { this.activeAt = now; this.activeElapsedMs = work.elapsedMs; }
      this.finishedAt = null;
    } else {
      if (fresh && this.wasActive) this.finishedAt = now;
      this.activeAt = null;
    }
    this.wasActive = active > 0;
    const waiting = idleMs >= 5000;
    const busy = active > 0 && now - this.activeAt >= 1200;
    const finished = fresh && this.finishedAt !== null && now - this.finishedAt < 4000 && !waiting;
    if (!busy && !waiting && !finished) {
      this.shownAt = null;
      return { visible: false };
    }
    if (this.shownAt === null) this.shownAt = now;
    const quietMs = fresh ? work.elapsedMs - work.lastCompletedMs : now - (this.completedAt || this.shownAt);
    const activeQuietMs = active > 0 ? Math.min(quietMs, Math.max(0, work.elapsedMs - this.activeElapsedMs)) : 0;
    const slow = active > 0 && activeQuietMs >= 15000;
    const title = !fresh && known ? 'Progress updates paused' : active > 0 ? stages[work.active.findIndex(count => count > 0)] : finished ? 'Compilation idle' : 'Waiting for the next frame';
    const detail = !fresh && known ? 'The renderer is not reporting. This may be a stall.' : active > 0
      ? slow ? `No compilation completed for ${Math.floor(activeQuietMs / 1000)}s. A slow driver call or stall is possible.` : `${active} compilation ${active === 1 ? 'step is' : 'steps are'} active. More may be discovered as the game loads.`
      : finished ? 'No active compilation is reported. More shaders may be discovered later.' : 'No active shader compilation is reported. The game may be loading or stalled.';
    return { visible: true, title, detail, completed: known ? work.completed : 0, failed: known ? work.failed : 0, elapsed: Math.floor((known ? work.elapsedMs : idleMs) / 1000), quietSeconds: Math.floor(quietMs / 1000), active, animate: active > 0 && !slow, warning: slow || !fresh || (active === 0 && !finished), bounds: { x: window.x, y: window.y, width: window.width, height: window.height } };
  }
}
module.exports = { LoadingModel, validWork, validWindow };
