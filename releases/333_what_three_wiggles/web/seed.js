// what.three.wiggles: turning three words into seeds, in JavaScript.
//
// This is the JS half of the seeding spec in SEEDING.md. seed.h is the C++
// half, and both are checked against the same vectors (test/host_test.cpp,
// web/seed.test.mjs).
//
// A classic script (no modules), so index.html also works when opened
// straight from disk. In a browser it defines window.W3WSeed; in node it
// exports the same object.

(function (root) {
  'use strict';

  const STREAM_WALK = 4;

  function fmix32(h) {
    h ^= h >>> 16;
    h = Math.imul(h, 0x85ebca6b);
    h ^= h >>> 13;
    h = Math.imul(h, 0xc2b2ae35);
    h ^= h >>> 16;
    return h >>> 0;
  }

  function rotl32(x, r) {
    return ((x << r) | (x >>> (32 - r))) >>> 0;
  }

  // FNV-1a over UTF-8 bytes, then fmix32
  function hashWord(bytes) {
    let h = 2166136261;
    for (const b of bytes) {
      h ^= b;
      h = Math.imul(h, 16777619);
    }
    return fmix32(h >>> 0);
  }

  function combineSeeds(s0, s1, s2) {
    return fmix32((s0 ^ rotl32(s1, 10) ^ rotl32(s2, 21)) >>> 0);
  }

  function deriveStream(seed, id) {
    return fmix32((seed + Math.imul(0x9e3779b9, id + 1)) >>> 0);
  }

  class Rng {
    constructor(state) {
      this.state = state >>> 0;
    }
    next() {
      this.state = (this.state + 0x9e3779b9) >>> 0;
      return fmix32(this.state);
    }
    // Uniform integer in [0, n), n <= 65536. Exact in doubles, and equal to
    // the C++ (uint64(next) * n) >> 32.
    below(n) {
      return Math.floor((this.next() * n) / 4294967296);
    }
  }

  // Tidy up what someone typed or pasted: "///Index.Home.Raft " -> "index.home.raft".
  // Returns {text, words} or {error}.
  function normalise(input) {
    let text = String(input).normalize('NFC').trim().toLowerCase();
    text = text.replace(/^\/+/, '');            // what3words addresses start with ///
    text = text.replace(/[\s,。．｡]+/g, '.');   // spaces, commas and full-width dots
    text = text.replace(/\.+/g, '.').replace(/^\.|\.$/g, '');
    const words = text.split('.').filter((w) => w.length > 0);
    if (words.length !== 3) {
      return { error: 'Enter exactly three words, like index.home.raft' };
    }
    text = words.join('.');
    if (new TextEncoder().encode(text).length > 95) {
      return { error: 'Those words are too long for the card (95 bytes at most)' };
    }
    return { text, words };
  }

  // Seeds for a normalised "word.word.word"
  function seedsFromText(text) {
    const enc = new TextEncoder();
    const seeds = text.split('.').map((w) => hashWord(enc.encode(w)));
    return {
      seeds,
      combined: combineSeeds(seeds[0], seeds[1], seeds[2]),
    };
  }

  const hex = (n) => (n >>> 0).toString(16).padStart(8, '0');

  const api = {
    STREAM_WALK, fmix32, hashWord, combineSeeds, deriveStream, Rng,
    normalise, seedsFromText, hex,
  };

  if (typeof module !== 'undefined' && module.exports) {
    module.exports = api;
  } else {
    root.W3WSeed = api;
  }
})(typeof window !== 'undefined' ? window : globalThis);
