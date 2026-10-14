(() => {
  'use strict';
  const canvas = document.getElementById('ambient-canvas');
  if (!canvas) return;
  let context;
  // Use normal compositor presentation so every canvas frame reaches the window.
  try { context = canvas.getContext('2d', { alpha: false }); } catch { return; }
  if (!context) return;

  const motion = window.matchMedia('(prefers-reduced-motion: reduce)');
  const frameLength = 1000 / 30;
  let width = 0, height = 0, scale = 1, frame = 0, lastDraw = 0, time = 0, disposed = false;
  let baseGradient;
  let nativeVisible = true, visibilityRevision = 0;
  let unsubscribeVisibility = () => {};
  const random = (min, max) => min + Math.random() * (max - min);

  function texture(size, paint) {
    const sprite = document.createElement('canvas'); sprite.width = sprite.height = size;
    paint(sprite.getContext('2d'), size);
    return sprite;
  }
  function glow(colour, core = false) {
    return texture(128, (ctx, size) => {
      const center = size / 2;
      const gradient = ctx.createRadialGradient(center, center, 0, center, center, center);
      gradient.addColorStop(0, `rgba(${colour},${core ? 1 : .46})`);
      gradient.addColorStop(core ? .035 : .20, `rgba(${colour},${core ? .95 : .34})`);
      gradient.addColorStop(core ? .12 : .50, `rgba(${colour},${core ? .28 : .12})`);
      gradient.addColorStop(.78, `rgba(${colour},.018)`);
      gradient.addColorStop(1, `rgba(${colour},0)`);
      ctx.fillStyle = gradient; ctx.fillRect(0, 0, size, size);
    });
  }
  const silver = glow('201, 208, 216');
  const warm = glow('213, 189, 149');
  const pale = glow('228, 224, 210');
  const dustGold = glow('255, 210, 132', true);
  const dustWhite = glow('250, 243, 224', true);
  const bokeh = glow('220, 219, 211');
  const wisp = texture(1024, (ctx, size) => {
    const gradient = ctx.createLinearGradient(0, 0, size, size * .5);
    gradient.addColorStop(0, 'rgba(213, 212, 207,0)');
    gradient.addColorStop(.38, 'rgba(213, 212, 207,.38)');
    gradient.addColorStop(.7, 'rgba(232, 224, 205,.6)');
    gradient.addColorStop(1, 'rgba(232, 224, 205,0)');
    ctx.strokeStyle = gradient; ctx.lineCap = 'round'; ctx.filter = 'blur(25px)';
    for (let layer = 0; layer < 6; layer++) {
      ctx.lineWidth = 24 + layer * 24; ctx.globalAlpha = .055;
      ctx.beginPath(); ctx.moveTo(-100, 760);
      ctx.bezierCurveTo(270, 1040, 300, 165, 1150, 350); ctx.stroke();
    }
  });

  // Different depths have independent motion; there is no repeating particle tile.
  const particles = Array.from({ length: 205 }, (_, index) => {
    const close = index < 24;
    return {
      x: random(-.08, 1.08), y: random(-.08, 1.08),
      band: index < 155, offset: Math.max(-.23, Math.min(.23, Math.sqrt(-2 * Math.log(random(.001, 1))) * Math.cos(random(0, Math.PI * 2)) * .092)),
      radius: close ? random(1.45, 3.0) : random(.42, 1.2),
      alpha: close ? random(.46, .82) : random(.24, .64),
      speed: close ? random(.008, .014) : random(.003, .006),
      phase: random(0, Math.PI * 2), frequency: random(.15, .45),
      gold: Math.random() > .24, crisp: index % 3 === 0
    };
  });
  const outOfFocus = Array.from({ length: 15 }, () => ({
    x: random(-.1, 1.1), y: random(-.15, .15), radius: random(5, 17),
    alpha: random(.09, .24), phase: random(0, Math.PI * 2)
  }));

  function ellipse(sprite, x, y, radiusX, radiusY, rotation, alpha) {
    context.save(); context.translate(x, y); context.rotate(rotation); context.globalAlpha = alpha;
    context.drawImage(sprite, -radiusX, -radiusY, radiusX * 2, radiusY * 2); context.restore();
  }
  function wrap(value) { return ((value + .1) % 1.2 + 1.2) % 1.2 - .1; }
  function draw() {
    if (!width || !height || disposed) return;
    context.globalAlpha = 1; context.globalCompositeOperation = 'source-over';
    context.fillStyle = baseGradient; context.fillRect(0, 0, width, height);
    const breath = Math.sin(time * .10), drift = Math.sin(time * .065);
    context.globalCompositeOperation = 'screen';
    // Feathered shafts descend from the top-left; the dust rises across them.
    ellipse(pale, width * (.16 + drift * .025), height * (.10 + breath * .035), width * .33, height * .96, -.28 + breath * .02, .30);
    ellipse(silver, width * (.30 + breath * .020), height * .23, width * .095, height * .90, -.28 + drift * .018, .22);
    ellipse(warm, width * (.40 - drift * .026), height * .10, width * .085, height * .90, -.31 - breath * .018, .21);
    ellipse(silver, width * .03, height * .88, width * .46, height * .40, -.36, .14);
    ellipse(pale, width * (.82 + drift * .025), height * .35, width * .51, height * .64, -.38, .13);
    ellipse(warm, width * (.82 + breath * .025), height * .82, width * .48, height * .45, -.34, .09);
    ellipse(wisp, width * (.36 + drift * .025), height * (.33 + breath * .025), width * .62, height * 1.02, -.29 + breath * .02, .39);
    ellipse(wisp, width * (.73 - drift * .025), height * (.52 - breath * .035), width * .54, height * .85, -.24 - breath * .025, .23);

    for (const particle of outOfFocus) {
      const horizontal = wrap(particle.x + time * .004);
      const x = horizontal * width;
      const y = (.98 - horizontal * .60 + particle.y + Math.sin(time * .055 + particle.phase) * .02) * height;
      const radius = particle.radius * Math.max(.75, Math.min(1.2, width / 1400));
      ellipse(bokeh, x, y, radius, radius, 0, particle.alpha);
    }
    for (const particle of particles) {
      const horizontal = wrap(particle.x + time * particle.speed + Math.sin(time * .07 + particle.phase) * .008);
      const x = horizontal * width;
      const y = (particle.band ? .98 - horizontal * .60 + particle.offset + Math.cos(time * .09 + particle.phase) * .012 : wrap(particle.y - time * particle.speed * .42)) * height;
      const shimmer = .74 + Math.sin(time * particle.frequency + particle.phase) * .26;
      const size = particle.radius * (particle.crisp ? 10 : 15);
      context.globalAlpha = particle.alpha * shimmer;
      context.drawImage(particle.gold ? dustGold : dustWhite, x - size / 2, y - size / 2, size, size);
      if (particle.crisp) {
        context.globalAlpha = Math.min(.85, particle.alpha * shimmer + .14);
        context.fillStyle = particle.gold ? '#ffe1a3' : '#fff7e7';
        context.beginPath(); context.arc(x, y, Math.max(.38, particle.radius * .42), 0, Math.PI * 2); context.fill();
      }
    }
    context.globalAlpha = 1; context.globalCompositeOperation = 'source-over';
    canvas.dataset.ready = 'true';
  }
  function tick(timestamp) {
    frame = 0;
    if (disposed || !nativeVisible || document.hidden) {
      canvas.dataset.motion = motion.matches ? 'reduced' : 'full';
      canvas.dataset.running = 'false'; return;
    }
    if (!lastDraw || timestamp - lastDraw >= frameLength) {
      if (lastDraw) time += Math.min(.15, (timestamp - lastDraw) / 1000);
      lastDraw = timestamp; draw();
    }
    frame = requestAnimationFrame(tick);
  }
  function pause() {
    if (frame) cancelAnimationFrame(frame);
    frame = 0; lastDraw = 0; canvas.dataset.running = 'false';
  }
  function synchronize() {
    pause(); if (disposed) return;
    canvas.dataset.motion = motion.matches ? 'reduced' : 'full';
    if (!nativeVisible || document.hidden) return;
    draw();
    canvas.dataset.running = 'true'; frame = requestAnimationFrame(tick);
  }
  function resize() {
    if (disposed) return;
    const bounds = canvas.getBoundingClientRect(); width = bounds.width; height = bounds.height;
    if (!width || !height) return;
    scale = Math.min(window.devicePixelRatio || 1, 1.5, 1920 / width, 1080 / height);
    canvas.width = Math.max(1, Math.round(width * scale)); canvas.height = Math.max(1, Math.round(height * scale));
    context.setTransform(scale, 0, 0, scale, 0, 0);
    baseGradient = context.createLinearGradient(0, height * .15, width, height * .70);
    baseGradient.addColorStop(0, '#0d1015'); baseGradient.addColorStop(.40, '#1c2027');
    baseGradient.addColorStop(.76, '#1b1e24'); baseGradient.addColorStop(1, '#121519');
    if (nativeVisible && !document.hidden) draw();
  }
  function dispose() {
    if (disposed) return;
    disposed = true; pause();
    unsubscribeVisibility();
    resizeObserver.disconnect();
    window.removeEventListener('resize', resize); window.removeEventListener('pagehide', dispose);
    document.removeEventListener('visibilitychange', synchronize); motion.removeEventListener('change', synchronize);
  }
  // Fullscreen chrome can change the canvas box after the native resize event.
  const resizeObserver = new ResizeObserver(resize);
  resizeObserver.observe(canvas);
  window.addEventListener('resize', resize, { passive: true });
  window.addEventListener('pagehide', dispose);
  document.addEventListener('visibilitychange', synchronize);
  motion.addEventListener('change', synchronize);
  unsubscribeVisibility = window.desktop.onWindowVisibility(visible => {
    ++visibilityRevision; nativeVisible = visible; synchronize();
  });
  const initialVisibilityRevision = visibilityRevision;
  window.desktop.windowVisibility().then(visible => {
    if (disposed || visibilityRevision !== initialVisibilityRevision) return;
    nativeVisible = visible; synchronize();
  }).catch(() => {});
  resize(); synchronize();
})();
