// Browser test: the real web/index.html, talking over a fake WebMIDI port to
// the card emulator in build/host_test (the firmware's own protocol.h and
// seed.h). Needs Playwright with Chromium.
//   node test/web_test.mjs [screenshot.png]

import { createRequire } from 'node:module';
import { execFileSync } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';
import path from 'node:path';
import assert from 'node:assert/strict';

const require = createRequire(import.meta.url);
const { chromium } = require('playwright');

const here = path.dirname(fileURLToPath(import.meta.url));
const hostTest = path.join(here, '..', 'build', 'host_test');
const page_url = pathToFileURL(path.join(here, '..', 'web', 'index.html')).href;

let cardWords = 'what.three.wiggles';

// Page -> card: strip F0 7D ... F7, run the emulator, wrap its reply
function card(bytes) {
  assert.equal(bytes[0], 0xf0);
  assert.equal(bytes[1], 0x7d);
  assert.equal(bytes[bytes.length - 1], 0xf7);
  for (const b of bytes.slice(1, -1)) assert.ok(b < 0x80, 'SysEx data must be 7-bit');
  const hex = bytes.slice(2, -1).map((b) => b.toString(16).padStart(2, '0')).join('');
  const [words, reply] = execFileSync(hostTest, ['--card', cardWords, hex], { encoding: 'utf8' }).trim().split('\n');
  cardWords = words;
  if (!reply) return null;
  return [0xf0, 0x7d, ...reply.match(/../g).map((h) => parseInt(h, 16)), 0xf7];
}

const browser = await chromium.launch();
const page = await browser.newPage();
await page.exposeFunction('cardSend', card);
await page.addInitScript(() => {
  navigator.requestMIDIAccess = async () => {
    const input = { name: 'MTMComputer', state: 'connected', onmidimessage: null };
    const output = {
      name: 'MTMComputer',
      state: 'connected',
      send: (bytes) => {
        window.cardSend(Array.from(bytes)).then((reply) => {
          if (reply) input.onmidimessage({ data: new Uint8Array(reply) });
        });
      },
    };
    return {
      inputs: new Map([['in', input]]),
      outputs: new Map([['out', output]]),
      addEventListener() {},
    };
  };
});

const errors = [];
page.on('pageerror', (e) => errors.push(e.message));

try {
  await page.goto(page_url + '?w=' + encodeURIComponent('///Index.Home.Raft'));

  // On connect the page says hello and the card reports its words
  await page.getByText('Card connected').waitFor();
  await page.locator('#cardState', { hasText: '///what.three.wiggles' }).waitFor();
  assert.match(await page.textContent('#cardState'), /✓ seeds match/);

  // Words from the URL are tidied up and hashed as you type
  assert.equal(await page.inputValue('#words'), '///Index.Home.Raft');
  const seedText = await page.textContent('#seeds');
  assert.match(seedText, /index.*a0ecfcd6.*home.*3b2fa9c5.*raft.*4c12964c/s);

  // Send them: the card hashes them itself, and the seeds agree
  await page.click('#send');
  await page.locator('#cardState', { hasText: '///index.home.raft' }).waitFor();
  assert.match(await page.textContent('#cardState'), /✓ seeds match/);
  assert.equal(cardWords, 'index.home.raft');
  assert.match(page.url(), /\?w=index\.home\.raft$/);

  // Non-ASCII words go through as UTF-8
  await page.fill('#words', 'été café crême');
  await page.press('#words', 'Enter');
  await page.locator('#cardState', { hasText: '///été.café.crême' }).waitFor();
  assert.match(await page.textContent('#cardState'), /✓ seeds match/);

  // CPU readout (the emulator's numbers are made up; this checks the format)
  await page.locator('#cpu', { hasText: '1234 of 3000 cycles' }).waitFor();

  // Two words can't be sent
  await page.fill('#words', 'index.home');
  assert.ok(await page.isDisabled('#send'));
  assert.match(await page.textContent('#hint'), /exactly three words/);

  // Leave a good state on screen for the screenshot
  await page.fill('#words', 'filled.count.soap');
  await page.click('#send');
  await page.locator('#cardState', { hasText: '///filled.count.soap' }).waitFor();
  if (process.argv[2]) await page.screenshot({ path: process.argv[2], fullPage: true });

  assert.deepEqual(errors, []);
  console.log('browser test passed');
} finally {
  await browser.close();
}
