const { chromium } = require('playwright');
(async () => {
  const browser = await chromium.launch({ args: ['--use-gl=swiftshader', '--enable-webgl', '--ignore-gpu-blocklist'] });
  const page = await browser.newPage({ viewport: { width: 800, height: 500 } });
  await page.route('https://cdnjs.cloudflare.com/**', r => r.fulfill({ path: '/home/claude/pool/build/npm/node_modules/three/build/three.min.js', contentType: 'application/javascript' }));
  await page.route('https://fonts.googleapis.com/**', r => r.abort());
  await page.goto('file:///home/claude/pool/build/harness.html');
  await page.waitForTimeout(2000);
  await page.click('#gSnk'); await page.click('#mTwo');
  const r = await page.evaluate(() => {
    const log = [];
    const shot = (phi, p) => { E.shoot(phi, p, 0, 0, 0); while (E.advance(0.5)) {} E.endShot(); const fh = OUT[0], np = OUT[1]; E.sync(); log.push({ fh, np, turn: OUT[1], foul: OUT[8], score: [OUT[300], OUT[301]], cue: [OUT[16].toFixed(3), OUT[17].toFixed(3)], blue: [OUT[16 + 12 * 19].toFixed(3), OUT[17 + 12 * 19].toFixed(3)] }); };
    E.placeCue(-1.1, 0.1);
    shot(Math.atan2(-0.15, 2.05), 0.55);
    shot(Math.PI, 0.25);
    return log;
  });
  console.log(JSON.stringify(r, null, 0));
  await browser.close();
})();
