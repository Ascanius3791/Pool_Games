const { chromium } = require('playwright');
(async () => {
  const browser = await chromium.launch({ args: ['--use-gl=swiftshader', '--enable-webgl', '--ignore-gpu-blocklist'] });
  const page = await browser.newPage({ viewport: { width: 1280, height: 760 } });
  const errs = [];
  page.on('pageerror', e => errs.push('PAGEERROR ' + e.message));
  await page.route('https://cdnjs.cloudflare.com/**', r => r.fulfill({ path: '/home/claude/pool/build/npm/node_modules/three/build/three.min.js', contentType: 'application/javascript' }));
  await page.route('https://fonts.googleapis.com/**', r => r.abort());
  await page.goto('file:///home/claude/pool/build/harness.html');
  await page.waitForTimeout(2000);
  await page.evaluate(() => { document.getElementById('gSnk').click(); document.getElementById('mTwo').click(); setCam('2d'); });
  await page.waitForTimeout(400);
  const run = async () => {
    await page.waitForFunction(() => S.phase === 'moving' || S.phase === 'nominate' || S.phase === 'aim', null, { timeout: 20000 });
    await page.waitForFunction(() => S.phase !== 'stroke', null, { timeout: 20000 });
    if (await page.evaluate(() => S.phase === 'moving')) {
      await page.evaluate(() => { while (E.advance(0.5)) {} });
      await page.waitForFunction(() => S.phase !== 'moving', null, { timeout: 20000 });
    }
  };
  // pot a red into the top-right corner
  const potted = await page.evaluate(() => {
    E.moveBall(1, 1.55, 0.65); E.placeCue(-1.1, 0.0); E.moveBall(0, 1.25, 0.66); readState();
    const R = S.R, px = 1.7845 - 0.02, py = 0.889 - 0.02, r = S.balls[1];
    const ux = px - r.x, uy = py - r.y, l = Math.hypot(ux, uy);
    const gx = r.x - 2 * R * ux / l, gy = r.y - 2 * R * uy / l, c = S.balls[0];
    S.phi = Math.atan2(gy - c.y, gx - c.x); S.power = 0.3; S.a = 0; S.b = -0.15; startStroke(); return S.phase;
  });
  await run();
  console.log('after red:', await page.evaluate(() => JSON.stringify({ phase: S.snk.phase, msg: document.getElementById('msg').textContent, on: document.getElementById('onLine').innerText })));
  const clear = async () => page.evaluate(() => { let k = 0; for (let i = 1; i <= 15; i++) if (S.balls[i].on) { E.moveBall(i, 0.9 + (k % 8) * 0.07, -0.80 + Math.floor(k / 8) * 0.07); k++; } readState(); });
  await clear();
  // (a) obvious: straight at the blue
  await page.evaluate(() => { const c = S.balls[0], b = S.balls[19]; S.phi = Math.atan2(b.y - c.y, b.x - c.x); S.power = 0.3; S.a = 0; S.b = 0; startStroke(); });
  console.log('obvious:', await page.evaluate(() => JSON.stringify({ phase: S.phase, msg: document.getElementById('msg').textContent, nom: S.snk.nominated })));
  await run();
  console.log('after obvious shot:', await page.evaluate(() => JSON.stringify({ foul: S.game.foul, msg: document.getElementById('msg').textContent })));
  // get back to a colour-on position for player to move: pot another red
  // (b) ambiguous: yellow and green side by side, aim between them
  const amb = await page.evaluate(() => {
    const R = S.R; E.moveBall(16, 0.3, 0.0); E.moveBall(17, 0.3, 2 * R + 0.0015); readState();
    const c = S.balls[0]; const mx = 0.3, my = R + 0.00075;
    const nd = E.nomGuess(Math.atan2(my - c.y, mx - c.x), 0.3, 0, 0, 120);
    const out = []; for (let j = 0; j < nd; j++) out.push([OUT[1 + 2 * j], OUT[2 + 2 * j]]); return out;
  });
  console.log('ambiguous guess counts:', JSON.stringify(amb));
  // player 2 pots a red, then plays an ambiguous shot
  await page.evaluate(() => {
    let rid = -1; for (let i = 1; i <= 15; i++) if (S.balls[i].on) { rid = i; break; }
    E.moveBall(rid, 1.55, 0.65); E.moveBall(0, 1.25, 0.66); readState();
    const R = S.R, px = 1.7845 - 0.02, py = 0.889 - 0.02, r = S.balls[rid];
    const ux = px - r.x, uy = py - r.y, l = Math.hypot(ux, uy);
    const gx = r.x - 2 * R * ux / l, gy = r.y - 2 * R * uy / l, c = S.balls[0];
    S.phi = Math.atan2(gy - c.y, gx - c.x); S.power = 0.3; S.a = 0; S.b = -0.15; startStroke();
  });
  await run();
  console.log('p2 red:', await page.evaluate(() => JSON.stringify({ phase: S.snk.phase, turn: S.game.turn, msg: document.getElementById('msg').textContent })));
  await page.evaluate(() => { const R = S.R; E.moveBall(16, 0.3, 0.0); E.moveBall(17, 0.3, 2 * R + 0.0015); readState();
    const c = S.balls[0]; S.phi = Math.atan2(R + 0.00075 - c.y, 0.3 - c.x); S.power = 0.3; S.a = 0; S.b = 0; startStroke(); });
  await page.waitForTimeout(800);
  console.log('panel:', await page.evaluate(() => JSON.stringify({ phase: S.phase, title: document.getElementById('chTitle').textContent, text: document.getElementById('chRef').innerText, btns: [...document.querySelectorAll('#chBtns button')].map(b => b.innerText.replace(/\n/g, ' ')) })));
  await page.screenshot({ path: '/home/claude/pool/build/shots/n_panel.png' });
  await page.evaluate(() => document.querySelector('#chBtns button').click());
  await run();
  console.log('after nominated shot:', await page.evaluate(() => JSON.stringify({ foul: S.game.foul, msg: document.getElementById('msg').textContent })));
  console.log(errs.join('\n'));
  await browser.close();
})();
