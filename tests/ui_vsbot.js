const { chromium } = require('playwright');
(async () => {
  const browser = await chromium.launch({ args: ['--use-gl=swiftshader', '--enable-webgl', '--ignore-gpu-blocklist'] });
  const page = await browser.newPage({ viewport: { width: 1000, height: 620 } });
  const errs = [];
  page.on('pageerror', e => errs.push('PAGEERROR ' + e.message));
  await page.route('https://cdnjs.cloudflare.com/**', r => r.fulfill({ path: '/home/claude/pool/build/npm/node_modules/three/build/three.min.js', contentType: 'application/javascript' }));
  await page.route('https://fonts.googleapis.com/**', r => r.abort());
  await page.goto('file:///home/claude/pool/build/harness.html');
  await page.waitForTimeout(2000);
  await page.check('#optNoise');
  await page.click('#mBot');
  await page.evaluate(() => setCam('cue'));
  // human: right-click abort test during charge
  const bar = await page.locator('#power').boundingBox();
  await page.mouse.move(bar.x + bar.width / 2, bar.y + 5); await page.mouse.down();
  await page.mouse.move(bar.x + bar.width / 2, bar.y + bar.height * 0.6, { steps: 4 });
  await page.mouse.down({ button: 'right' }); await page.mouse.up({ button: 'right' });
  await page.mouse.up();
  await page.waitForTimeout(500);
  console.log('after abort: phase', await page.evaluate(() => S.phase), 'msg', await page.textContent('#msg'));
  // human breaks
  await page.evaluate(() => { S.power = 1; startStroke(); });
  await page.waitForFunction(() => S.phase === 'moving', null, { timeout: 30000 });
  await page.evaluate(() => { while (E.advance(0.5)) {} });
  const t0 = Date.now();
  let shotN = 0;
  while (Date.now() - t0 < 100000) {
    const st = await page.evaluate(() => { if (S.phase === 'moving') { while (E.advance(0.5)) {} } return { ph: S.phase, turn: S.game.turn, msg: document.getElementById('msg').textContent }; });
    if (st.ph === 'botaim' && shotN === 0) { await page.waitForTimeout(1500); await page.screenshot({ path: '/home/claude/pool/build/shots/vsbot.png' }); shotN++; }
    if (st.ph === 'aim' && st.turn === 0 && shotN > 0) { console.log('back to human:', st.msg); break; }
    if (st.ph === 'over') { console.log('over', st.msg); break; }
    await page.waitForTimeout(200);
  }
  console.log('final msg', await page.textContent('#msg'), '| report', (await page.innerText('#report')).replace(/\n/g, ' | '));
  console.log(errs.join('\n'));
  await browser.close();
})();
