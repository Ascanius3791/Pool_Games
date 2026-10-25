// times bot decisions in the browser engine (WASM) at several effort levels, snooker
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
  const res = await page.evaluate(() => {
    const out = [];
    for (const [noisy, budget] of [[0, 3000], [1, 3000], [1, 20000], [1, 50000]]) {
      E.newGame(5, 0, 1);
      const t0 = performance.now(); E.botThink(noisy, 0, budget); const ms = performance.now() - t0;
      const o = OUT;
      const sims = o[8]; out.push({ noisy, budget, ms: Math.round(ms), sims: Math.round(sims), usPerSim: +(ms * 1000 / sims).toFixed(1) });
    }
    return out;
  }).catch(e => 'ERR ' + e.message);
  console.log(JSON.stringify(res, null, 1));
  console.log('effort slider:', await page.evaluate(() => [effortFromSlider(0), effortFromSlider(0.5), effortFromSlider(1), S.effort]));
  console.log(errs.join('\n'));
  await browser.close();
})();
