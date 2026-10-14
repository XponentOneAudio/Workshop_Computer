// Cycle estimate for MatrixVerb's audio engine on the RP2040 (see build.sh).
// Runs the engine with busy settings (long decay, the limiter working, a
// matrix crossfade at sample 1001), writing gMarker after every sample so
// that emu.py can cost each one.  Sample 1000-1001 also include working out
// and taking a new design, which the card does elsewhere: ignore slots 8-9.
#include "design.h"
#include "fdn.h"
using namespace mv;
extern "C" { volatile uint32_t gMarker; volatile uint32_t gSlot; volatile uint32_t gDone; }
static uint32_t rng = 1;
static int32_t Noise(int32_t a) {rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return int32_t((int64_t(int32_t(rng)) * a) >> 31);}
extern "C" void bench()
{
	static uint8_t v[kNumValues];
	Defaults(v);
	v[PageReverb * 8 + RvDecay] = 110;      // long
	v[PageLab * 8 + LbOutput] = 127;        // +12dB, so the limiter works
	v[PageLab * 8 + LbCeiling] = 60;
	static Controls c; memcpy(c.v, v, kNumValues); c.decayOct = 0; c.sizeOct = 0; c.dampOffset = 0;
	static Designer dz; static Design d; dz.Compute(c, d);
	static Engine e; e.Init(); e.SetDesign(d);
	gMarker = 0xFFFFFFFF;   // timing starts
	for (uint32_t n = 0; n < 4096; n++)
	{
		int32_t in = Noise(20000), l, r;
		if (n == 1000) { v[0] = 30; memcpy(c.v, v, kNumValues); dz.Compute(c, d); }  // (not timed separately)
		if (n == 1001) e.SetDesign(d);           // start a matrix crossfade
		e.Process(in, in, l, r);
		gSlot = uint32_t(e.Slot());
		gMarker = n;
	}
	gDone = 1;
	for (;;) {}
}
extern "C" __attribute__((naked, section(".text.start"))) void _start()
{
	__asm volatile("ldr r0, =0x20070000\n mov sp, r0\n bl bench\n b .\n");
}
