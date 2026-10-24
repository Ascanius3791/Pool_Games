const { chromium } = require('playwright');
(async () => {
  const browser = await chromium.launch({ args: ['--use-gl=swiftshader', '--enable-webgl', '--ignore-gpu-blocklist'] });
  const page = await browser.newPage({ viewport: { width: 900, height: 560 } });
  const errs = [];
  page.on('pageerror', e => errs.push('PAGEERROR ' + e.message));
  await page.route('https://cdnjs.cloudflare.com/**', r => r.fulfill({ path: '/home/claude/pool/build/npm/node_modules/three/build/three.min.js', contentType: 'application/javascript' }));
  await page.route('https://fonts.googleapis.com/**', r => r.abort());
  await page.goto('file:///home/claude/pool/build/harness.html');
  await page.waitForTimeout(2000);
  await page.click('#mTwo');
  const views = JSON.parse(process.argv[2]);
  let i = 0;
  for (const v of views) {
    await page.evaluate((v) => { if (v.js) eval(v.js); if (v.pos) S.debugCam = { pos: new THREE.Vector3(...v.pos), look: new THREE.Vector3(...v.look) }; else S.debugCam = null; }, v);
    await page.waitForTimeout(v.wait || 1500);
    await page.screenshot({ path: `/home/claude/pool/build/shots/c${i++}.png` });
  }
  console.log(errs.join('\n'));
  await browser.close();
})();
