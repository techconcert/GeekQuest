// Browser test for the firmware update page (firmware/safety.cpp): Switch game mode and the older
// "Check for a new version" button. Run: node test/browser/update_page_test.js (needs playwright).
// The clock and GitHub are mocked; nothing leaves the machine.
const { chromium } = require('playwright'); const fs = require('fs'); const path = require('path');
const src = fs.readFileSync(path.join(__dirname, '../../firmware/safety.cpp'), 'utf8');
const raw = n => src.match(new RegExp('static const char ' + n + '\\[\\] PROGMEM = R"HTML\\(([\\s\\S]*?)\\)HTML";'))[1];
const page = raw('PAGE_HEAD') + raw('PAGE_UPDATE_A') + raw('PAGE_UPDATE_B') + '</body></html>';
function image(n, good) { const b = Buffer.alloc(n, 7); b[0] = good ? 0xE9 : 0; b[2] = 0; b[3] = 0x40; Buffer.from('GQ-OTA-PAGE-v1').copy(b, 1000); return b; }
const CORS = { 'access-control-allow-origin': '*' };
let failed = 0; const ok = (c, m) => { console.log((c ? 'ok   ' : 'FAIL ') + m); if (!c) failed = 1; };
async function open(o) {
  const b = await chromium.launch(), p = await b.newPage(), posts = [], errs = [];
  p.on('pageerror', e => errs.push(String(e)));
  await p.route('http://clock/**', r => { const q = r.request();
    if (q.method() === 'POST') { posts.push(q.url()); return r.fulfill({ status: 200, body: 'Installed.' }); }
    if (q.url().endsWith('/info')) return r.fulfill({ json: { version: 'r12-aaaaaaa', free_ota_bytes: 593920 } });
    return r.fulfill({ contentType: 'text/html', body: page }); });
  await p.route('https://raw.githubusercontent.com/**', r => { const u = r.request().url();
    if (u.includes('/builds/latest.json')) { const ai = u.includes('GeekQuestAI'); return r.fulfill({ headers: CORS, json: ai ? { version: 'r5', commit: 'bbbbbbb', bin: 'firmware/ai.bin' } : { version: 'r12', commit: 'aaaaaaa', bin: 'firmware/gq.bin' } }); }
    if (u.endsWith('ai.bin')) return r.fulfill({ headers: CORS, body: image(385024, !o.bad) });
    return r.fulfill({ status: 404, body: '' }); });
  await p.goto('http://clock/update'); return { b, p, posts, errs };
}
(async () => {
  let t = await open({}); await t.p.click('#gm button:nth-of-type(2)'); await t.p.waitForTimeout(500);
  let s = await t.p.textContent('#gmm');
  ok(/GeekQuestAI r5/.test(s) && /not carry over/.test(s) && t.posts.length === 0, 'shows version and warning, installs nothing before confirmation');
  await t.p.click('#gmm button'); await t.p.waitForTimeout(2500);
  ok(t.posts.length === 1 && /size=385024/.test(t.posts[0]) && /crc=[0-9a-f]{8}/.test(t.posts[0]), 'confirmation uploads through /update with crc and size');
  ok(t.errs.length === 0, 'no page errors'); await t.b.close();
  t = await open({}); await t.p.click('#gm button:nth-of-type(1)'); await t.p.waitForTimeout(500);
  ok(/already running/.test(await t.p.textContent('#gmm')) && t.posts.length === 0, 'the game that is already running is not reinstalled'); await t.b.close();
  t = await open({ bad: true }); await t.p.click('#gm button:nth-of-type(2)'); await t.p.waitForTimeout(500); await t.p.click('#gmm button'); await t.p.waitForTimeout(2500);
  ok(/failed the checks/.test(await t.p.textContent('#gmm')) && t.posts.length === 0, 'a file that fails the page checks is never uploaded'); await t.b.close();
  t = await open({}); await t.p.click('#ck'); await t.p.waitForTimeout(500);
  ok(/Up to date/.test(await t.p.textContent('#up')), 'Check for a new version still works'); await t.b.close();
  process.exit(failed);
})();
