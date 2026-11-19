// ============================================================================
//  m0_bench.cpp — tiny functions for tools/emulate_m0.py to run as ARM code
// ============================================================================
//  Compiled for the Cortex-M0+ (the RP2040's core) and run in an emulator, so
//  the hand-written assembly in zplane.h can be checked bit-for-bit and its
//  speed compared with the old version, without needing a Workshop Computer.
// ============================================================================
#include "zplane.h"
using namespace zp;
extern "C" {
int64_t test_smull(int32_t a, int32_t b) { return smull32(a, b); }
int32_t test_softclip(int32_t v) { return softClip(v); }
int32_t run_new(const FixedBiquad* q, BiquadState* st, int32_t x) { return runBiquad(*q, *st, x); }
int32_t run_old(const FixedBiquad* q, BiquadState* st, int32_t x) {
	int64_t acc = st->err;
	acc += int64_t(q->c[0]) * x; acc += int64_t(q->c[1]) * st->x1; acc += int64_t(q->c[2]) * st->x2;
	acc -= int64_t(q->c[3]) * st->y1; acc -= int64_t(q->c[4]) * st->y2;
	int64_t y = acc >> kCoeffFracBits; st->err = acc - (y << kCoeffFracBits);
	const int64_t lim = int64_t(1) << 30; if (y > lim) y = lim; if (y < -lim) y = -lim;
	st->x2 = st->x1; st->x1 = x; st->y2 = st->y1; st->y1 = int32_t(y); return int32_t(y);
}
}

// The Pico SDK replaces libgcc's 64-bit multiply with this faster one
// (pico_int64_ops, as disassembled from the firmware), so "run_old" is timed
// with the multiply the card really used before.
__asm__ (
	".syntax unified\n"
	".global __aeabi_lmul\n"
	".type __aeabi_lmul, %function\n"
	".thumb_func\n"
	"__aeabi_lmul:\n"
	"	muls r1, r2\n	muls r3, r0\n	adds r1, r1, r3\n	mov ip, r1\n"
	"	lsrs r1, r2, #16\n	uxth r3, r0\n	muls r3, r1\n	push {r4}\n"
	"	lsrs r4, r0, #16\n	muls r1, r4\n	uxth r2, r2\n	uxth r0, r0\n"
	"	muls r0, r2\n	muls r2, r4\n	lsls r4, r3, #16\n	lsrs r3, r3, #16\n"
	"	adds r0, r0, r4\n	pop {r4}\n	adcs r1, r3\n	lsls r3, r2, #16\n"
	"	lsrs r2, r2, #16\n	adds r0, r0, r3\n	adcs r1, r2\n	add r1, ip\n	bx lr\n");
