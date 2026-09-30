// Checks web/seed.js against the same vectors as test/host_test.cpp.
//   node web/seed.test.mjs

import { createRequire } from 'node:module';
import assert from 'node:assert/strict';

const require = createRequire(import.meta.url);
const W = require('./seed.js');

// words, seeds, combined seed, first four draws of the walk stream
const vectors = [
  ['what.three.wiggles', [0x929983ca, 0x5fa8125a, 0x51018971], 0xd1c5100a, [0xabaaf638, 0xbb2e728b, 0x0b6db697, 0x1b16e93e]],
  ['index.home.raft', [0xa0ecfcd6, 0x3b2fa9c5, 0x4c12964c], 0x5bc1c7d8, [0x48fe20c3, 0x51f90a6b, 0xcc0c3dc4, 0xf7c02b0f]],
  ['filled.count.soap', [0x9becd478, 0x9b11efec, 0xefb2b764], 0xadb8f4e0, [0x099ca06f, 0x088303d6, 0x0acdaa22, 0x326e3662]],
  ['été.café.crême', [0xa17d532d, 0xdf518d52, 0x3a0bb5c7], 0x35d06ec9, [0x9dddc3cc, 0x25685b94, 0xaec69a63, 0x28d43797]],
  ['a.b.c', [0x1a80b1b3, 0x82c46232, 0x46ad5f91], 0x136dcfd1, [0x1fb6eee4, 0xb42ae3d3, 0x269d605f, 0x74b88fa7]],
];

for (const [words, seeds, combined, walk] of vectors) {
  const got = W.seedsFromText(words);
  assert.deepEqual(got.seeds, seeds, words);
  assert.equal(got.combined, combined, words);
  const r = new W.Rng(W.deriveStream(got.combined, W.STREAM_WALK));
  assert.deepEqual([r.next(), r.next(), r.next(), r.next()], walk, words);
}

// Tidying up typed or pasted input
const ok = (input, text) => assert.equal(W.normalise(input).text, text, input);
ok('index.home.raft', 'index.home.raft');
ok('///Index.Home.Raft ', 'index.home.raft');
ok('  index home  raft', 'index.home.raft');
ok('index, home, raft', 'index.home.raft');
ok('index..home.raft.', 'index.home.raft');
ok('ÉTÉ.café.crême', 'été.café.crême');
ok('e\u0301te\u0301.cafe\u0301.cre\u0302me', 'été.café.crême'); // decomposed accents become NFC
const bad = (input) => assert.ok(W.normalise(input).error, input);
bad('index.home');
bad('index.home.raft.extra');
bad('');
bad('a'.repeat(40) + '.' + 'b'.repeat(40) + '.' + 'c'.repeat(40));

// below() matches the C++ (uint64(next) * n) >> 32
const r = new W.Rng(1234);
for (let i = 0; i < 1000; i++) {
  const state = r.state;
  const v = r.below(65536);
  const next = W.fmix32((state + 0x9e3779b9) >>> 0);
  assert.equal(v, Number((BigInt(next) * 65536n) >> 32n));
}

console.log('web tests passed');
