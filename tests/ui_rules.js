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
  await page.keyboard.press('c'); await page.keyboard.press('c');  // top
  const shoot = async (phi, power, a, b) => {
    await page.evaluate(([phi, power, a, b]) => { S.phi = phi; S.power = power; S.a = a; S.b = b; startStroke(); }, [phi, power, a, b]);
    await page.waitForFunction(() => S.phase === 'moving', null, { timeout: 20000 });
    await page.evaluate(() => { while (E.advance(0.5)) {} });
    await page.waitForFunction(() => S.phase !== 'moving', null, { timeout: 20000 });
    return page.evaluate(() => ({ msg: document.getElementById('msg').textContent, g: S.game, on: S.balls.map(b => b.on).join('') }));
  };
  console.log('break:', JSON.stringify(await shoot(0, 1, 0, -0.1)));
  await page.waitForTimeout(800);
  await page.screenshot({ path: '/home/claude/pool/build/shots/r0.png' });
  // shoot away from everything into a cushion -> likely a "no ball hit" foul
  console.log('shot2:', JSON.stringify(await shoot(Math.PI, 0.3, 0, 0)));
  await page.waitForTimeout(800);
  await page.screenshot({ path: '/home/claude/pool/build/shots/r1.png' });
  console.log(errs.join('\n'));
  await browser.close();
})();
