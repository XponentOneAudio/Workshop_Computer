# Weierstrass

An endless zoom into the Weierstrass function, for the Music Thing Modular
Workshop System Computer.

    W(x) = sum_n  a^n cos(2π b^n x)

This curve is continuous everywhere and differentiable nowhere. Zoom in by a
factor of b and it looks the same. This card reads the curve at audio rate, so
each term becomes a partial and the partials are spaced by the ratio b.
Zooming in slows the read, so every partial falls in pitch. After one zoom
step, each partial has reached the place of the one below, and a new layer of
detail has come down from above. The zoom loops with no seam, so you hear a
Shepard-Risset glissando built from a fractal. With b = 2 it is the classic
Shepard tone.

The operator manual (controls, jacks, patch ideas) is in [`info.yaml`](info.yaml)
under `readme:`, and is shown on the card's page on the metadata site.

## How the seam is hidden

- **The lens.** Each partial's amplitude depends on its continuous position
  `m = n + z`, where `z` is how far through the current zoom step we are.
  The weight is `a^m`, with a raised-cosine fade-in over the bottom unit of `m`
  and a fade-out over the top unit. When `z` wraps, every partial has exactly
  the weight its neighbour had a moment before.
- **Phase lock.** The engine stores one 32-bit read phase `P`. Partial `k` is
  at `b^k · P mod 2^32`. Multiplication modulo 2^32 is exact, so the partials
  can never drift apart and the waveform really is the curve. When the zoom
  wraps, `P` becomes `P/b` or `P·b`, and every partial keeps its phase.

## Building

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S . -B build && cmake --build build
```

The released firmware is `UF2/weierstrass.uf2`.

## Testing without hardware

`weierstrass.h` has no Pico dependencies. `tools/render.cpp` builds it on a
desktop, renders zooms to WAV files, and checks them:

```sh
g++ -O2 -std=c++17 -I. tools/render.cpp -o render && ./render /tmp
```

It checks that:

- the output matches a double-precision Weierstrass sum,
- the waveform has no jump where the zoom wraps,
- the spectrum is the same one zoom step later,
- the level stays within 6 dB through the zoom.

## Status

Beta. This build has been tested in the desktop simulator only, not yet on a
Workshop Computer. If you flash it, please report how it sounds and behaves.

## License

MIT. `ComputerCard.h` is by Chris Johnson, also MIT; see the notice in that file.
