const { chromium } = require('playwright');
(async () => {
  const browser = await chromium.launch({ args: ['--use-gl=swiftshader', '--enable-webgl', '--ignore-gpu-blocklist'] });
  for (const vp of [{ width: 1100, height: 680, name: 'p_desk' }, { width: 400, height: 760, name: 'p_phone' }]) {
    const page = await browser.newPage({ viewport: { width: vp.width, height: vp.height } });
    const errs = [];
    page.on('pageerror', e => errs.push('PAGEERROR ' + e.message));
    await page.route('https://cdnjs.cloudflare.com/**', r => r.fulfill({ path: '/home/claude/pool/build/npm/node_modules/three/build/three.min.js', contentType: 'application/javascript' }));
    await page.route('https://fonts.googleapis.com/**', r => r.abort());
    await page.goto('file:///home/claude/pool/build/harness.html');
    await page.waitForTimeout(2000);
    await page.screenshot({ path: `/home/claude/pool/build/shots/${vp.name}_menu.png` });
    await page.check('#optNoise');
    await page.click('#mTwo');
    await page.evaluate(() => { S.phi = 0.02; S.power = 0.9; S.a = 0.2; S.b = -0.3; updatePowerUI(); drawSpin(); });
    await page.waitForTimeout(1500);
    await page.screenshot({ path: `/home/claude/pool/build/shots/${vp.name}_aim.png` });
    await page.evaluate(() => startStroke());
    await page.waitForFunction(() => S.phase === 'moving', null, { timeout: 20000 });
    await page.evaluate(() => { while (E.advance(0.5)) {} });
    await page.waitForTimeout(1500);
    await page.screenshot({ path: `/home/claude/pool/build/shots/${vp.name}_after.png` });
    console.log(vp.name, await page.textContent('#report'), errs.join('\n'));
    await page.close();
  }
  await browser.close();
})();
