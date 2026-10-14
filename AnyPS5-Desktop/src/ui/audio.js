(() => {
  'use strict';
  if (window.AnyPS5Audio) return;

  const defaults = { enabled: true, musicVolume: 25, effectsVolume: 45 };
  // Warm, open voicings move through D major / B minor without a strong cadence.
  // Melody phrases share tones across the long chord crossfades.
  const phrases = [
    { chord: [73.416, 110, 164.814, 185], melody: [293.665, 369.994, 329.628, 246.942], seconds: 18 },
    { chord: [123.471, 185, 220, 277.183], melody: [246.942, 329.628, 369.994, 293.665], seconds: 23 },
    { chord: [98, 146.832, 220, 246.942], melody: [293.665, 246.942, 440, 369.994], seconds: 20 },
    { chord: [110, 164.814, 246.942, 277.183], melody: [329.628, 277.183, 246.942, 329.628], seconds: 26 },
    { chord: [82.407, 123.471, 146.832, 195.998], melody: [329.628, 293.665, 246.942, 293.665], seconds: 21 },
    { chord: [98, 146.832, 185, 246.942], melody: [246.942, 369.994, 293.665, 440], seconds: 24 },
    { chord: [73.416, 110, 185, 329.628], melody: [369.994, 329.628, 293.665, 246.942], seconds: 19 },
    { chord: [110, 164.814, 195.998, 246.942], melody: [329.628, 246.942, 293.665, 329.628], seconds: 25 }
  ];
  const cues = {
    navigate: [[330, 0, .052, .034]],
    confirm: [[329.628, 0, .15, .048], [493.883, .04, .18, .026]],
    back: [[293.665, 0, .13, .04], [220, .045, .16, .024]],
    open: [[293.665, 0, .2, .04], [440, .045, .23, .024]],
    error: [[164.814, 0, .2, .041], [246.942, .025, .22, .022]],
    success: [[293.665, 0, .23, .038], [440, .045, .27, .025], [586.33, .09, .3, .014]]
  };
  let settings = { ...defaults }, configured = false, disposed = false;
  let context = null, graph = null, resumePending = null, contextCount = 0;
  let nativeVisible = true, nativeRevision = 0, focused = document.hasFocus();
  let pauseTimer = 0, chordTimer = 0, chordIndex = 0, lastCueTime = -Infinity;
  let effectsPlayed = 0, lastError = '', unsubscribeVisibility = () => {};
  const reasons = new Set();
  const liveEffects = new Set();

  function percent(value, fallback) {
    return typeof value === 'number' && Number.isFinite(value) ? Math.max(0, Math.min(100, value)) : fallback;
  }
  function wanted() {
    return configured && !disposed && settings.enabled && nativeVisible && !document.hidden && focused &&
      reasons.size === 0 && (settings.musicVolume > 0 || settings.effectsVolume > 0);
  }
  function smooth(parameter, target, seconds = .09) {
    const now = context.currentTime;
    if (typeof parameter.cancelAndHoldAtTime === 'function') parameter.cancelAndHoldAtTime(now);
    else {
      const current = parameter.value;
      parameter.cancelScheduledValues(now);
      parameter.setValueAtTime(current, now);
    }
    parameter.setTargetAtTime(target, now, seconds);
  }
  function createGraph() {
    if (context || disposed) return;
    const AudioContext = window.AudioContext || window.webkitAudioContext;
    if (!AudioContext) { lastError = 'Web Audio is unavailable.'; return; }
    try {
      context = new AudioContext({ latencyHint: 'interactive' });
      contextCount++;
      const master = context.createGain();
      const music = context.createGain();
      const effects = context.createGain();
      const filter = context.createBiquadFilter();
      const effectsFilter = context.createBiquadFilter();
      const limiter = context.createDynamicsCompressor();
      const dry = context.createGain();
      const wet = context.createGain();
      const space = context.createConvolver();
      master.gain.value = 0; music.gain.value = 0; effects.gain.value = 0;
      filter.type = 'lowpass'; filter.frequency.value = 820; filter.Q.value = .25;
      effectsFilter.type = 'lowpass'; effectsFilter.frequency.value = 1350; effectsFilter.Q.value = .3;
      dry.gain.value = .56; wet.gain.value = .64;
      limiter.threshold.value = -14; limiter.knee.value = 20;
      limiter.ratio.value = 4; limiter.attack.value = .005; limiter.release.value = .25;
      filter.connect(dry); dry.connect(music);
      filter.connect(space); space.connect(wet); wet.connect(music);
      music.connect(master); effectsFilter.connect(effects); effects.connect(master);
      master.connect(limiter); limiter.connect(context.destination);

      // A short, dark stereo room is generated once. No repeating echoes or DSP in a JS frame loop.
      const impulse = context.createBuffer(2, Math.ceil(context.sampleRate * 3.4), context.sampleRate);
      for (let channel = 0; channel < 2; channel++) {
        const data = impulse.getChannelData(channel);
        let colour = 0;
        for (let sample = 0; sample < data.length; sample++) {
          const time = sample / context.sampleRate;
          colour += .12 * ((Math.random() * 2 - 1) - colour);
          const onset = Math.max(0, Math.min(1, (time - .025 - channel * .009) / .07));
          data[sample] = colour * onset * Math.pow(1 - sample / data.length, 2.7);
        }
      }
      space.buffer = impulse;

      const waveform = context.createPeriodicWave(
        new Float32Array([0, 0, 0, 0]),
        new Float32Array([0, 1, .13, .035])
      );
      const voices = [];
      const banks = [0, 1].map(() => {
        const gain = context.createGain();
        gain.gain.value = 0; gain.connect(filter);
        const bankVoices = phrases[0].chord.map((frequency, index) => {
          const oscillator = context.createOscillator();
          const volume = context.createGain();
          const pan = context.createStereoPanner();
          oscillator.setPeriodicWave(waveform);
          oscillator.frequency.value = frequency;
          oscillator.detune.value = [-.8, .6, -.5, .9][index];
          volume.gain.value = [.023, .03, .027, .019][index];
          pan.pan.value = [-.28, .22, -.6, .62][index];
          oscillator.connect(volume); volume.connect(pan); pan.connect(gain);
          oscillator.start();
          const voice = { volume, pan, baseLevel: volume.gain.value, oscillators: [oscillator] };
          voices.push(voice);
          return voice;
        });
        return { gain, voices: bankVoices };
      });

      // Four reusable soft melody voices: no sharp attacks, beat, or growing source pool.
      const melody = Array.from({ length: 4 }, () => {
        const oscillator = context.createOscillator();
        const volume = context.createGain();
        const pan = context.createStereoPanner();
        oscillator.type = 'sine'; volume.gain.value = 0;
        oscillator.connect(volume); volume.connect(pan); pan.connect(filter);
        oscillator.start();
        const voice = { volume, pan, oscillators: [oscillator] };
        voices.push(voice);
        return voice;
      });

      // Barely audible filtered air fills the space between sustained notes.
      const airBuffer = context.createBuffer(2, Math.ceil(context.sampleRate * 4), context.sampleRate);
      for (let channel = 0; channel < 2; channel++) {
        const data = airBuffer.getChannelData(channel);
        for (let sample = 0; sample < data.length; sample++) data[sample] = Math.random() * 2 - 1;
      }
      const air = context.createBufferSource();
      const airHighpass = context.createBiquadFilter();
      const airLowpass = context.createBiquadFilter();
      const airLevel = context.createGain();
      air.buffer = airBuffer; air.loop = true;
      airHighpass.type = 'highpass'; airHighpass.frequency.value = 140; airHighpass.Q.value = .3;
      airLowpass.type = 'lowpass'; airLowpass.frequency.value = 580; airLowpass.Q.value = .3;
      airLevel.gain.value = .007;
      air.connect(airHighpass); airHighpass.connect(airLowpass); airLowpass.connect(airLevel);
      airLevel.connect(filter); air.start();
      graph = { master, music, effects, effectsFilter, filter, limiter, voices, banks, melody, air,
        started: false, activeBank: 0, nextChordAt: 0, phrasesScheduled: 0 };

    } catch (error) {
      lastError = String(error?.message || error);
      if (context) { void context.close().catch(() => {}); }
      context = null; graph = null;
    }
  }
  function clearSchedule() {
    clearTimeout(chordTimer); chordTimer = 0;
  }
  function arrangePhrase(now) {
    const phrase = phrases[chordIndex % phrases.length];
    const cycle = Math.floor(chordIndex / phrases.length);
    const duration = phrase.seconds + (cycle % 3);
    const bank = graph.banks[graph.activeBank];
    bank.voices.forEach((voice, index) => {
      const level = voice.baseLevel;
      voice.volume.gain.cancelScheduledValues(now);
      voice.volume.gain.setValueAtTime(level * .82, now);
      voice.volume.gain.linearRampToValueAtTime(level * (1 + .09 * ((index + chordIndex) % 3)), now + duration * .42);
      voice.volume.gain.linearRampToValueAtTime(level * .82, now + duration);
      const position = [-.28, .22, -.6, .62][index];
      voice.pan.pan.cancelScheduledValues(now);
      voice.pan.pan.setValueAtTime(position, now);
      voice.pan.pan.linearRampToValueAtTime(position * .55, now + duration * .6);
      voice.pan.pan.linearRampToValueAtTime(position, now + duration);
    });
    // Uneven spacing and occasional rests leave room for the sustained harmonies.
    // Every voice finishes before its next use, so pitch changes happen in silence.
    graph.melody.forEach((voice, index) => {
      if ((chordIndex + cycle) % 3 === 2 && index === 2) return;
      const start = now + 2 + [0, 3.8, 9.1, 13.6][index] + .3 * (chordIndex % 3);
      const length = 5.6 + .4 * ((index + chordIndex) % 3);
      const level = [.018, .013, .016, .011][index];
      const noteIndex = cycle % 2 ? 3 - index : index;
      voice.oscillators[0].frequency.setValueAtTime(phrase.melody[noteIndex], start);
      voice.pan.pan.setValueAtTime((index % 2 ? .38 : -.38) * (cycle % 2 ? -1 : 1), start);
      voice.volume.gain.setValueAtTime(0, start);
      voice.volume.gain.linearRampToValueAtTime(level, start + 1.1);
      voice.volume.gain.exponentialRampToValueAtTime(.0001, start + length - .4);
      voice.volume.gain.linearRampToValueAtTime(0, start + length);
    });
    smooth(graph.filter.frequency, [760, 920, 840, 1020, 780, 900, 980, 820][chordIndex % 8], 4);
    graph.phrasesScheduled++;
    graph.nextChordAt = now + duration;
  }
  function scheduleChord() {
    clearSchedule();
    if (!wanted() || !graph || context.state !== 'running' || settings.musicVolume === 0) return;
    const now = context.currentTime;
    if (!graph.started) {
      graph.started = true;
      const first = graph.banks[0].gain.gain;
      first.setValueAtTime(0, now);
      first.linearRampToValueAtTime(1, now + 8);
      arrangePhrase(now);
    } else if (now >= graph.nextChordAt) {
      const outgoing = graph.banks[graph.activeBank];
      const incoming = graph.banks[1 - graph.activeBank];
      const notes = phrases[++chordIndex % phrases.length].chord;
      // Each phrase lasts longer than the eight-second bank crossfade.
      // Retune only that silent bank; audible oscillators never glide between notes.
      incoming.gain.gain.cancelScheduledValues(now);
      incoming.gain.gain.setValueAtTime(0, now);
      incoming.voices.forEach((voice, index) => {
        voice.oscillators[0].frequency.setValueAtTime(notes[index], now);
      });
      outgoing.gain.gain.cancelScheduledValues(now);
      outgoing.gain.gain.setValueAtTime(1, now);
      outgoing.gain.gain.linearRampToValueAtTime(0, now + 8);
      incoming.gain.gain.linearRampToValueAtTime(1, now + 8);
      graph.activeBank = 1 - graph.activeBank;
      arrangePhrase(now);
    }
    // Audio time freezes while suspended, so a return to the launcher continues the same fade.
    chordTimer = setTimeout(scheduleChord, Math.max(50, (graph.nextChordAt - now) * 1000));
  }
  function updateLevels() {
    if (!graph || !wanted() || context.state !== 'running') return;
    // Raise the ambient bed by about 12 dB without changing navigation cue levels.
    smooth(graph.music.gain, 4 * settings.musicVolume / 100, .4);
    smooth(graph.effects.gain, settings.effectsVolume / 100, .035);
    smooth(graph.master.gain, .8, .18);
    if (settings.musicVolume > 0 && !chordTimer) scheduleChord();
    else if (settings.musicVolume === 0) clearSchedule();
  }
  async function resume() {
    if (!wanted()) return false;
    clearTimeout(pauseTimer); pauseTimer = 0;
    createGraph();
    if (!context || !graph) return false;
    if (context.state !== 'running') {
      if (!resumePending) {
        resumePending = context.resume().then(() => {
          lastError = '';
        }, error => {
          lastError = String(error?.message || error);
        }).finally(() => { resumePending = null; });
      }
      await resumePending;
    }
    if (!wanted()) { reconcile(); return false; }
    if (context.state !== 'running') return false;
    updateLevels();
    return true;
  }
  function reconcile() {
    if (disposed) return;
    clearTimeout(pauseTimer); pauseTimer = 0;
    if (wanted()) { void resume(); return; }
    clearSchedule();
    if (!context || !graph || context.state === 'closed') return;
    smooth(graph.master.gain, 0, .04);
    // Stop the DSP after the short release, so hidden windows consume no audio CPU.
    pauseTimer = setTimeout(() => {
      pauseTimer = 0;
      if (!wanted() && context && context.state !== 'closed') {
        for (const effect of [...liveEffects]) {
          try { effect.oscillator.stop(); } catch {}
          stopEffect(effect);
        }
        void context.suspend().then(() => {
          if (wanted()) void resume();
        }).catch(() => {});
      }
    }, 260);
  }
  function configure(next = {}) {
    if (disposed) return { ...settings };
    settings = {
      enabled: typeof next.enabled === 'boolean' ? next.enabled : settings.enabled,
      musicVolume: percent(next.musicVolume, settings.musicVolume),
      effectsVolume: percent(next.effectsVolume, settings.effectsVolume)
    };
    configured = true;
    reconcile();
    return { ...settings };
  }
  function stopEffect(effect) {
    liveEffects.delete(effect);
    effect.oscillator.onended = null;
    try { effect.oscillator.disconnect(); effect.envelope.disconnect(); } catch {}
  }
  async function play(name) {
    if (name === 'nav') name = 'navigate';
    const notes = cues[name];
    if (!notes || !wanted() || settings.effectsVolume === 0) return false;
    const requestedAt = performance.now();
    if (!(await resume()) || performance.now() - requestedAt > 250 || !wanted() || settings.effectsVolume === 0) return false;
    const now = context.currentTime;
    // Holding an arrow key must not accumulate a loud stack of navigation cues.
    if (now - lastCueTime < (name === 'navigate' ? .075 : .035) || liveEffects.size + notes.length > 12) return false;
    lastCueTime = now;
    effectsPlayed++;
    for (const [frequency, offset, duration, level] of notes) {
      const oscillator = context.createOscillator();
      const envelope = context.createGain();
      const start = now + .006 + offset;
      const end = start + duration;
      oscillator.type = 'sine'; oscillator.frequency.value = frequency;
      envelope.gain.value = 0;
      envelope.gain.setValueAtTime(0, start);
      envelope.gain.linearRampToValueAtTime(level, start + (name === 'navigate' ? .009 : .02));
      envelope.gain.exponentialRampToValueAtTime(.0001, end);
      envelope.gain.linearRampToValueAtTime(0, end + .015);
      oscillator.connect(envelope); envelope.connect(graph.effectsFilter);
      const effect = { oscillator, envelope };
      liveEffects.add(effect);
      oscillator.onended = () => stopEffect(effect);
      oscillator.start(start); oscillator.stop(end + .025);
    }
    return true;
  }
  function suspend(reason = 'manual') {
    if (!disposed) { reasons.add(String(reason)); reconcile(); }
  }
  function release(reason = 'manual') {
    if (!disposed) { reasons.delete(String(reason)); reconcile(); }
  }
  function visibilityChanged() { reconcile(); }
  function focusChanged() { focused = true; reconcile(); }
  function blurChanged() { focused = false; reconcile(); }
  function trustedGesture(event) {
    if (event.isTrusted && !disposed) {
      focused = document.hasFocus();
      void resume();
    }
  }
  function destroy() {
    if (disposed) return;
    disposed = true;
    clearTimeout(pauseTimer); clearSchedule();
    document.removeEventListener('visibilitychange', visibilityChanged);
    window.removeEventListener('focus', focusChanged);
    window.removeEventListener('blur', blurChanged);
    window.removeEventListener('pointerdown', trustedGesture, true);
    window.removeEventListener('keydown', trustedGesture, true);
    window.removeEventListener('pagehide', destroy);
    unsubscribeVisibility();
    for (const effect of liveEffects) {
      try { effect.oscillator.stop(); } catch {}
      stopEffect(effect);
    }
    if (graph) {
      for (const voice of graph.voices) for (const oscillator of voice.oscillators) {
        try { oscillator.stop(); oscillator.disconnect(); } catch {}
      }
      try { graph.air.stop(); graph.air.disconnect(); } catch {}
      graph.master.gain.cancelScheduledValues(context.currentTime);
      graph.master.gain.setValueAtTime(0, context.currentTime);
    }
    if (context && context.state !== 'closed') void context.close().catch(() => {});
  }
  function getState() {
    return {
      settings: { ...settings }, configured, disposed,
      contextState: context?.state || 'uninitialized', contextCount,
      musicVoices: graph ? graph.voices.reduce((count, voice) => count + voice.oscillators.length, 0) : 0,
      musicPhrases: graph?.phrasesScheduled || 0,
      activeEffects: liveEffects.size, effectsPlayed,
      focused, nativeVisible, documentHidden: document.hidden,
      suspendedReasons: [...reasons], lastError
    };
  }

  document.addEventListener('visibilitychange', visibilityChanged);
  window.addEventListener('focus', focusChanged);
  window.addEventListener('blur', blurChanged);
  window.addEventListener('pointerdown', trustedGesture, true);
  window.addEventListener('keydown', trustedGesture, true);
  window.addEventListener('pagehide', destroy);
  if (window.desktop?.onWindowVisibility) {
    unsubscribeVisibility = window.desktop.onWindowVisibility(value => {
      nativeRevision++; nativeVisible = Boolean(value); reconcile();
    }) || (() => {});
  }
  if (window.desktop?.windowVisibility) {
    const revision = nativeRevision;
    Promise.resolve(window.desktop.windowVisibility()).then(value => {
      if (!disposed && nativeRevision === revision) { nativeVisible = Boolean(value); reconcile(); }
    }).catch(() => {});
  }
  window.AnyPS5Audio = Object.freeze({ configure, play, suspend, release, destroy, getState });
})();
