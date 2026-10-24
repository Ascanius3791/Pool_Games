const { chromium } = require('playwright');
(async () => {
  const browser = await chromium.launch({ args: ['--use-gl=swiftshader', '--enable-webgl', '--ignore-gpu-blocklist'] });
  const page = await browser.newPage({ viewport: { width: 1000, height: 620 } });
  const errs = [];
  page.on('pageerror', e => errs.push('PAGEERROR ' + e.message));
  page.on('console', m => { if (m.type() === 'error') errs.push(m.text()); });
  await page.route('https://cdnjs.cloudflare.com/**', r => r.fulfill({ path: '/home/claude/pool/build/npm/node_modules/three/build/three.min.js', contentType: 'application/javascript' }));
  await page.route('https://fonts.googleapis.com/**', r => r.abort());
  await page.goto('file:///home/claude/pool/build/harness.html');
  await page.waitForTimeout(2000);
  if (process.argv[2] === 'noisy') await page.check('#optNoise');
  await page.click('#mWatch');
  await page.evaluate(() => setCam('2d'));
  const msgs = []; let shots = 0, shot = 0; const t0 = Date.now();
  while (Date.now() - t0 < 150000) {
    const st = await page.evaluate(() => { if (S.phase === 'moving') { while (E.advance(0.5)) {} } return { ph: S.phase, msg: document.getElementById('msg').textContent, rep: document.getElementById('report').innerText }; });
    if (msgs[msgs.length - 1] !== st.msg) { msgs.push(st.msg); }
    if (st.ph === 'botaim' && shot < 2) { await page.screenshot({ path: `/home/claude/pool/build/shots/bot${shot++}.png` }); console.log('REPORT', st.rep.replace(/\n/g, ' | ')); }
    if (st.ph === 'over') break;
    await page.waitForTimeout(150);
  }
  await page.screenshot({ path: '/home/claude/pool/build/shots/bot_end.png' });
  console.log(msgs.join('\n'));
  console.log(errs.join('\n'));
  await browser.close();
})();
