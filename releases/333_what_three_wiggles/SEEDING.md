# Seeding spec

How three words become random numbers. `seed.h` (C++, on the card) and `web/seed.js` (JS, in the
web page) both implement this. `test/host_test.cpp` and `web/seed.test.mjs` check both against the
vectors below.

All arithmetic is on unsigned 32-bit integers, wrapping. In JS, use `Math.imul` and `>>> 0`.

## 1. Normalise (web page only)

1. Unicode NFC, trim, lowercase.
2. Strip leading `/` characters, since what3words addresses start with `///`.
3. Treat spaces, commas and full-width dots as dots, collapse repeated dots, and drop dots at either
   end.
4. There must be exactly three non-empty words, and the result, `word.word.word`, must be at most 95
   bytes of UTF-8.

The card receives the normalised text as UTF-8 bytes and doesn't normalise it again.

## 2. Word seeds

For each word, hash its UTF-8 bytes with FNV-1a, then mix the result with the murmur3 finaliser:

```
fmix32(h):  h ^= h >> 16;  h *= 0x85EBCA6B;  h ^= h >> 13;  h *= 0xC2B2AE35;  h ^= h >> 16
fnv1a(bytes):  h = 2166136261;  for each byte b: h ^= b;  h *= 16777619
seed[n] = fmix32(fnv1a(word n))
```

Order matters for the combined seed: `a.b.c` gives a different patch from `c.b.a`.

```
combined = fmix32(seed[0] ^ rotl(seed[1], 10) ^ rotl(seed[2], 21))
```

## 3. Streams

Each engine draws from its own stream, so changing one engine never changes another:

```
stream_state(seed, id) = fmix32(seed + 0x9E3779B9 * (id + 1))
next():  state += 0x9E3779B9;  return fmix32(state)
below(n) = (next() * n) >> 32            (64-bit product; n <= 65536)
         = floor(next() * n / 2^32)      (in JS; exact in doubles)
range(lo, hi) = lo + below(hi - lo + 1)
unit16() = next() >> 16
```

| ID | Stream | Seeded from | Used for |
|---|---|---|---|
| 1 | FM | each word | Audio 1: operator n's ratio, detune, phase, FM depth and level |
| 2 | ADDITIVE | each word | Audio 2: levels and phases of word n's partials (1–5, 6–11, 12–16) |
| 3 | LFO | each word | CV 2: component n's rate ratio, phase and level |
| 4 | WALK | combined | CV 1: every step of the random walk |
| 5 | TRIGGERS | combined | Pulse 1: one draw per clock tick |
| 6 | DRONE | combined | Audio 1: drone pitch (C2 to G3) |
| 7 | LFO_FOLD | combined | CV 2: wavefolder gain, cross-modulation |
| 8 | CONFIG | combined | CV 1 and Pulse 1: leap and glide chances, hold length, start note, trigger density |

**Rules for changing this:**
- Never renumber a stream.
- Never change the order of the draws within an existing stream.
- New draws go at the end of a stream, and new streams get new IDs.

Any other change alters the sound of every location. `patch.h` shows the exact draws for each stream.

Streams 4 and 5 are drawn from while the card runs. The walk makes exactly six draws per step and
the triggers one per tick, whatever the knobs are doing. So after a reset the same clock gives the
same phrase.

Only the seeds and streams are specified bit for bit. On the card, `patch.h` turns some draws into
frequencies with float maths. A port of the engines (such as a preview in the web page) would match
those to within rounding, not bit for bit.

## 4. Test vectors

The first four `next()` values of the WALK stream (`stream_state(combined, 4)`):

| Words | seed[0] | seed[1] | seed[2] | combined | WALK stream |
|---|---|---|---|---|---|
| `what.three.wiggles` | 929983ca | 5fa8125a | 51018971 | d1c5100a | abaaf638 bb2e728b 0b6db697 1b16e93e |
| `index.home.raft` | a0ecfcd6 | 3b2fa9c5 | 4c12964c | 5bc1c7d8 | 48fe20c3 51f90a6b cc0c3dc4 f7c02b0f |
| `filled.count.soap` | 9becd478 | 9b11efec | efb2b764 | adb8f4e0 | 099ca06f 088303d6 0acdaa22 326e3662 |
| `été.café.crême` | a17d532d | df518d52 | 3a0bb5c7 | 35d06ec9 | 9dddc3cc 25685b94 aec69a63 28d43797 |
| `a.b.c` | 1a80b1b3 | 82c46232 | 46ad5f91 | 136dcfd1 | 1fb6eee4 b42ae3d3 269d605f 74b88fa7 |

`what.three.wiggles` is the card's default until it is sent other words.

## 5. Talking to the card

`protocol.h` describes the SysEx messages. The page sends the normalised words, and the card hashes
them itself and sends back its seeds. The page checks them against its own, which checks this spec
end to end on real hardware.
