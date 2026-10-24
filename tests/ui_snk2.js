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
  await page.click('#gSnk'); await page.click('#mTwo');
  const shoot = async (phi, power, a, b) => {
    await page.evaluate(([phi, power, a, b]) => { S.phi = phi; S.power = power; S.a = a; S.b = b; startStroke(); }, [phi, power, a, b]);
    await page.waitForFunction(() => S.phase === 'moving', null, { timeout: 20000 });
    await page.evaluate(() => { while (E.advance(0.5)) {} });
    await page.waitForFunction(() => S.phase !== 'moving', null, { timeout: 20000 });
    return page.evaluate(() => document.getElementById('msg').textContent + ' || ' + document.getElementById('onLine').textContent + ' || score ' + S.snk.score.join('-'));
  };
  // place cue ball in the D via engine, then break off at the pack
  console.log('place ok:', await page.evaluate(() => E.placeCue(-1.1, 0.1)));
  console.log(await shoot(Math.atan2(-0.05 - 0.1, 0.95 + 1.1), 0.55, 0, 0));
  console.log(await shoot(Math.PI, 0.25, 0, 0));
  await page.evaluate(() => setCam('2d')); await page.waitForTimeout(1000);
  await page.screenshot({ path: '/home/claude/pool/build/shots/s2_2d.png' });
  // back to 8-ball
  await page.evaluate(() => openMenu()); await page.click('#gPool'); await page.click('#mTwo');
  await page.waitForTimeout(1500);
  console.log('8-ball again: n balls', await page.evaluate(() => ballMeshes.length + ' R=' + S.R));
  await page.screenshot({ path: '/home/claude/pool/build/shots/s2_back8.png' });
  console.log(errs.join('\n'));
  await browser.close();
})();
