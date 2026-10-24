const { chromium } = require('playwright');
(async () => {
  const browser = await chromium.launch({ args: ['--use-gl=swiftshader', '--enable-webgl', '--ignore-gpu-blocklist'] });
  const page = await browser.newPage({ viewport: { width: 1100, height: 650 } });
  const errs = [];
  page.on('pageerror', e => errs.push('PAGEERROR ' + e.message));
  page.on('console', m => { if (m.type() === 'error') errs.push(m.text()); });
  await page.route('https://cdnjs.cloudflare.com/**', r => r.fulfill({ path: '/home/claude/pool/build/npm/node_modules/three/build/three.min.js', contentType: 'application/javascript' }));
  await page.route('https://fonts.googleapis.com/**', r => r.abort());
  await page.goto('file:///home/claude/pool/build/harness.html');
  await page.waitForTimeout(2000);
  await page.screenshot({ path: '/home/claude/pool/build/shots/s_menu.png' });
  await page.click('#gSnk');
  await page.click('#mWatch');
  await page.waitForTimeout(500);
  await page.evaluate(() => setCam('top'));
  await page.waitForTimeout(1200);
  await page.screenshot({ path: '/home/claude/pool/build/shots/s_top.png' });
  const msgs = []; const t0 = Date.now(); let shot = 0;
  while (Date.now() - t0 < 120000) {
    const st = await page.evaluate(() => { if (S.phase === 'moving') { while (E.advance(0.5)) {} } return { ph: S.phase, msg: document.getElementById('msg').textContent, on: document.getElementById('onLine').textContent, rep: document.getElementById('report').innerText }; });
    if (msgs[msgs.length - 1] !== st.msg) { msgs.push(st.msg + '  |  ' + st.on); }
    if (st.ph === 'botaim' && shot < 1) { await page.evaluate(() => setCam('cue')); await page.waitForTimeout(1300); await page.screenshot({ path: '/home/claude/pool/build/shots/s_cue.png' }); console.log('REPORT', st.rep.replace(/\n/g, ' | ')); shot++; }
    if (st.ph === 'over') break;
    await page.waitForTimeout(150);
  }
  await page.evaluate(() => setCam('2d')); await page.waitForTimeout(800);
  await page.screenshot({ path: '/home/claude/pool/build/shots/s_2d.png' });
  console.log(msgs.join('\n'));
  console.log(errs.join('\n'));
  await browser.close();
})();
