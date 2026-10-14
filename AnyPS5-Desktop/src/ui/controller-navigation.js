(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.AnyPS5ControllerNavigation = api;
})(typeof window === 'object' ? window : globalThis, function () {
  'use strict';
  const pressed = (pad, index) => !!pad.buttons?.[index]?.pressed || (pad.buttons?.[index]?.value || 0) > .5;
  function direction(pad, previous) {
    const horizontal = Number(pressed(pad, 15)) - Number(pressed(pad, 14));
    const vertical = Number(pressed(pad, 13)) - Number(pressed(pad, 12));
    if (horizontal || vertical) return vertical ? (vertical > 0 ? 'ArrowDown' : 'ArrowUp') : (horizontal > 0 ? 'ArrowRight' : 'ArrowLeft');
    const x = Number.isFinite(pad.axes?.[0]) ? pad.axes[0] : 0;
    const y = Number.isFinite(pad.axes?.[1]) ? pad.axes[1] : 0;
    const threshold = previous ? .32 : .55;
    if (Math.max(Math.abs(x), Math.abs(y)) < threshold) return '';
    return Math.abs(x) > Math.abs(y) ? (x > 0 ? 'ArrowRight' : 'ArrowLeft') : (y > 0 ? 'ArrowDown' : 'ArrowUp');
  }
  function createSampler() {
    const states = new Map();
    return {
      reset() { states.clear(); },
      sample(pads, now, enabled = true) {
        if (!enabled) { states.clear(); return null; }
        const present = new Set();
        let command = null;
        for (const pad of Array.from(pads || [])) {
          // Unknown raw button orders must never trigger Select/Exit by guesswork.
          if (!pad?.connected || pad.mapping !== 'standard') continue;
          const identity = pad.index + ':' + pad.id; present.add(identity);
          let state = states.get(identity);
          if (!state) { state = { armed: false, confirm: false, back: false, direction: '', nextRepeat: 0 }; states.set(identity, state); }
          const confirm = pressed(pad, 0), back = pressed(pad, 1);
          const next = direction(pad, state.direction);
          if (!state.armed) {
            // After connect/focus, release controls once before they can activate the UI.
            if (!confirm && !back && !next) state.armed = true;
            state.confirm = confirm; state.back = back; state.direction = next;
            continue;
          }
          let key = '';
          if (back && !state.back) key = 'Escape';
          else if (confirm && !state.confirm) key = 'Enter';
          else if (next && !confirm && !back && (next !== state.direction || now >= state.nextRepeat)) key = next;
          if (next !== state.direction) state.nextRepeat = now + 380;
          else if (key === next && next) state.nextRepeat = now + 115;
          state.confirm = confirm; state.back = back; state.direction = next;
          // Consume all device edges but emit at most one command per frame.
          if (key && !command) command = { key, id: pad.id, index: pad.index };
        }
        for (const identity of states.keys()) if (!present.has(identity)) states.delete(identity);
        return command;
      }
    };
  }
  function start(onCommand, desktop) {
    const sampler = createSampler();
    let frame = 0, stopped = false, nativeVisible = true, nativeRevision = 0;
    const visible = () => !stopped && nativeVisible && !document.hidden;
    function tick(now) {
      frame = 0;
      if (!visible()) { sampler.reset(); return; }
      let pads = [];
      const focused = document.hasFocus();
      if (focused) { try { pads = navigator.getGamepads?.() || []; } catch {} }
      const command = sampler.sample(pads, now, focused);
      if (command) onCommand(command);
      if (visible() && !frame) frame = requestAnimationFrame(tick);
    }
    function reconcile() {
      if (!visible()) { cancelAnimationFrame(frame); frame = 0; sampler.reset(); }
      else if (!frame) frame = requestAnimationFrame(tick);
    }
    const focus = () => reconcile();
    const blur = () => { sampler.reset(); reconcile(); };
    const visibility = () => reconcile();
    window.addEventListener('focus', focus); window.addEventListener('blur', blur);
    document.addEventListener('visibilitychange', visibility);
    const unsubscribe = desktop?.onWindowVisibility?.(visible => { nativeRevision++; nativeVisible = visible; reconcile(); });
    if (desktop?.windowVisibility) {
      const revision = nativeRevision;
      Promise.resolve(desktop.windowVisibility()).then(visible => { if (!stopped && revision === nativeRevision) { nativeVisible = visible; reconcile(); } }).catch(() => {});
    }
    function stop() {
      stopped = true; reconcile(); unsubscribe?.();
      window.removeEventListener('focus', focus); window.removeEventListener('blur', blur);
      document.removeEventListener('visibilitychange', visibility); window.removeEventListener('pagehide', stop);
    }
    window.addEventListener('pagehide', stop); reconcile();
    return { stop };
  }
  return { createSampler, start };
});
