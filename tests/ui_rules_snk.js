const { chromium } = require('playwright');
(async () => {
  const browser = await chromium.launch({ args: ['--use-gl=swiftshader', '--enable-webgl', '--ignore-gpu-blocklist'] });
  const page = await browser.newPage({ viewport: { width: 1280, height: 760 } });
  const errs = [];
  page.on('pageerror', e => errs.push('PAGEERROR ' + e.message));
  page.on('console', m => { if (m.type() === 'error') errs.push(m.text()); });
  await page.route('https://cdnjs.cloudflare.com/**', r => r.fulfill({ path: '/home/claude/pool/build/npm/node_modules/three/build/three.min.js', contentType: 'application/javascript' }));
  await page.route('https://fonts.googleapis.com/**', r => r.abort());
  await page.goto('file:///home/claude/pool/build/harness.html');
  await page.waitForTimeout(2000);
  await page.evaluate(() => { document.getElementById('gSnk').click(); });
  await page.screenshot({ path: '/home/claude/pool/build/shots/r_menu.png' });
  await page.evaluate(() => { document.getElementById('optNoise').checked = true; document.getElementById('mTwo').click(); setCam('2d'); });
  await page.waitForTimeout(500);
  const shoot = async (phi, power) => {
    await page.evaluate(([phi, power]) => { S.phi = phi; S.power = power; S.a = 0; S.b = 0; startStroke(); }, [phi, power]);
    await page.waitForFunction(() => S.phase === 'moving', null, { timeout: 20000 });
    await page.evaluate(() => { while (E.advance(0.5)) {} });
    await page.waitForFunction(() => S.phase !== 'moving', null, { timeout: 20000 });
  };
  // Scenario 1: free ball. Build a snooker: red behind the blue, everything else out of the way.
  await page.evaluate(() => {
    for (let i = 1; i <= 21; i++) { if (i === 19) continue; E.moveBall(i, 1.2 - (i % 8) * 0.06, 0.75 - Math.floor(i / 8) * 0.06); }
    E.moveBall(1, 1.0, 0.1); E.moveBall(19, 0.5, 0.05); E.placeCue(-1.1, -0.1); readState();
  });
  // player 1 plays the cue ball to (0,0)-ish so that it ends behind the blue: just roll gently toward x=0 and miss everything is complicated,
  // instead: player 1 plays straight away from all balls (towards baulk cushion) -> foul, no ball hit
  await shoot(Math.PI, 0.18);
  let st = await page.evaluate(() => ({ phase: S.phase, msg: document.getElementById('msg').textContent, title: document.getElementById('chTitle').textContent, ref: document.getElementById('chRef').innerText, btns: [...document.querySelectorAll('#chBtns button')].map(b => b.innerText.replace(/\n/g, ' ')), k: S.snk }));
  console.log('AFTER FOUL', JSON.stringify({ phase: st.phase, title: st.title, ref: st.ref, btns: st.btns, pending: st.k.pending, freeAvail: st.k.freeAvail, ref2: st.k.ref }));
  await page.waitForTimeout(600);
  await page.screenshot({ path: '/home/claude/pool/build/shots/r_choice.png' });
  // choose "replace" if offered, else play
  const res = await page.evaluate(() => { const b = [...document.querySelectorAll('#chBtns button')]; const r = b.find(x => x.innerText.includes('free ball')) || b[0]; r.click(); return { turn: S.game.turn, cue: [S.balls[0].x.toFixed(3), S.balls[0].y.toFixed(3)], msg: document.getElementById('msg').textContent }; });
  console.log('AFTER CHOICE', JSON.stringify(res));
  await page.waitForFunction(() => S.snk.freeNom > 0 || !document.getElementById('msg').textContent.includes('picking'), null, { timeout: 30000 });
  const fb = await page.evaluate(() => ({ msg: document.getElementById('msg').textContent, on: document.getElementById('onLine').innerText, fb: S.snk.freeBall, nom: S.snk.freeNom, ring: S.fbRing.visible }));
  console.log('FREE BALL', JSON.stringify(fb));
  await page.screenshot({ path: '/home/claude/pool/build/shots/r_freeball.png' });
  // right-click nomination via the same code path
  const rc = await page.evaluate(() => { const b = S.balls[17]; const v = E2T(b.x, b.y, S.R).project(activeCam()); const cv = renderer.domElement.getBoundingClientRect();
     const ev = { clientX: cv.left + (v.x + 1) / 2 * cv.width, clientY: cv.top + (1 - v.y) / 2 * cv.height }; nominateAt(ev); return { nom: S.snk.freeNom, msg: document.getElementById('msg').textContent }; });
  console.log('RIGHT-CLICK', JSON.stringify(rc));
  // shoot with the free ball
  
  console.log(errs.join('\n'));
  await browser.close();
})();
