// ============================================================================
//  host_test.cpp — run the Zeds Dead filter maths on a normal computer
// ============================================================================
//
//  Build & run (from this tools/ directory):
//
//      g++ -O2 -std=c++17 -I.. host_test.cpp -o host_test && ./host_test
//
//  What it does:
//    1. STABILITY: designs the filter at tens of thousands of knob settings
//       and checks every pole radius is < 1 (inside the unit circle).
//    2. ACCURACY: feeds sine waves through the INTEGER (fixed-point) audio path
//       and compares the measured gain with what the FLOAT design predicts.
//       This catches mistakes in the fixed-point conversion.
//    3. TORTURE: sweeps the morph/cutoff/resonance wildly while feeding noise
//       through the integer path — including a deliberately extreme 8-section
//       user bank — and checks the output never gets stuck ringing.
//    4. DATA: checks the USB byte encoding and the flash image round-trip,
//       and that garbage data is always turned into a safe filter.
//    5. PICTURES: writes SVG plots of the frequency response as the morph
//       control sweeps through each factory bank (open them in a browser).
//
//  With the argument "vectors" it instead prints reference numbers that
//  tools/check_web.js compares with the web editor's JavaScript, to prove the
//  editor's pictures match what the card will really do.
//
//  This is a learning tool as much as a test: try editing the presets in
//  bank.h, re-run, and look at how the pictures change.
// ============================================================================

#include "zplane.h"
#include "bank.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <initializer_list>

using namespace zp;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

static BankData gBanks[kNumBanks];   // the factory banks

// Magnitude (in dB) of the whole designed cascade at frequency hz, including
// the makeup gain — i.e. what the float maths says the card should do.
static float designDb(const Design& d, float hz)
{
	Probe p(2.0f * kPi * hz / kSampleRate);
	float m = d.makeup * d.makeup;
	for (int s = 0; s < d.numSections; ++s) m *= magSq(d.sec[s], p);
	return 10.0f * log10f(m + 1e-20f);
}

// A deliberately extreme user bank: up to 8 sections, poles nearly on the
// circle, zeros all over the place, slots with different section counts.
static BankData makeWildBank()
{
	BankData wild;
	std::memset(&wild, 0, sizeof(wild));
	wild.numSlots = kNumSlots;
	for (int sl = 0; sl < kNumSlots; ++sl)
	{
		wild.slot[sl].numSections = uint8_t(1 + (sl * 7 + 3) % kMaxSections);
		for (int s = 0; s < kMaxSections; ++s)
		{
			SectionData& q = wild.slot[sl].sec[s];
			q.poleRatio = 0.2f + 0.37f * float((sl * 3 + s * 5) % 11);
			q.poleDamp  = (s & 1) ? 0.0005f : 0.3f;
			q.resWeight = 1.0f;
			q.zeroMode  = uint8_t((s + sl) & 1);
			q.zeroA     = q.zeroMode == kZeroPair ? 0.3f * float(s + 1) : ((s & 2) ? 1.0f : -0.5f);
			q.zeroB     = q.zeroMode == kZeroPair ? 0.0f : -1.0f;
		}
	}
	return wild;
}

// ---------------------------------------------------------------------------
// 1. Stability sweep
// ---------------------------------------------------------------------------
static void testStability()
{
	static BankData wild = makeWildBank();
	const BankData* banks[3] = { &gBanks[0], &gBanks[1], &wild };
	int designs = 0;
	float worstR = 0.0f;
	for (const BankData* b : banks)
		for (float morph = 0.0f; morph <= 5.0f; morph += 0.05f)
			for (float res = 0.0f; res <= 1.0f; res += 0.1f)
				for (float hz = 20.0f; hz < 20000.0f; hz *= 1.25f)
				{
					Design d;
					designMorph(*b, { hz, morph, res }, d);
					++designs;
					for (int s = 0; s < d.numSections; ++s)
					{
						const float r = sqrtf(d.sec[s].a2);   // a2 = r²
						if (r > worstR) worstR = r;
						CHECK(r < 1.0f, "unstable pole r=%f morph=%f res=%f hz=%f", double(r), double(morph), double(res), double(hz));
						CHECK(std::isfinite(d.sec[s].b0) && std::isfinite(d.makeup), "non-finite coefficient");
					}
				}
	std::printf("[stability] %d designs checked, largest pole radius %.6f\n", designs, double(worstR));
}

// ---------------------------------------------------------------------------
// The integer audio path, exactly as main.cpp runs it (one channel).
// ---------------------------------------------------------------------------
struct FixedChannel
{
	FixedBiquad cur[kMaxSections];
	int32_t makeup = 1 << kMakeupFracBits;
	BiquadState st[kMaxSections];
	int n = 0;   // sections running

	void snapTo(const FixedBiquad tgt[kMaxSections], int32_t mk, int numSections)
	{
		for (int s = 0; s < kMaxSections; ++s) cur[s] = tgt[s];
		makeup = mk;
		n = numSections;
	}
	// Mirrors ProcessSample(): newly enabled sections start as a clean wire,
	// then every coefficient moves 1/32 of the way to its target.
	void smoothTo(const FixedBiquad tgt[kMaxSections], int32_t mk, int numSections)
	{
		for (int s = n; s < numSections; ++s) { cur[s] = fixedPassthrough(); st[s] = BiquadState(); }
		n = numSections;
		for (int s = 0; s < n; ++s)
			for (int k = 0; k < 5; ++k)
				cur[s].c[k] += (tgt[s].c[k] - cur[s].c[k]) >> 5;
		makeup += (mk - makeup) >> 5;
	}
	int16_t process(int16_t in)
	{
		int32_t x = int32_t(in) << kSampleShift;
		for (int s = 0; s < n; ++s) x = runBiquad(cur[s], st[s], x);
		int64_t y = smull32(x, makeup) >> kMakeupFracBits;
		if (y >  (int64_t(1) << 30)) y =  (int64_t(1) << 30);
		if (y < -(int64_t(1) << 30)) y = -(int64_t(1) << 30);
		int32_t out = softClip(int32_t(y)) >> kSampleShift;
		if (out > 2047) out = 2047;
		if (out < -2048) out = -2048;
		return int16_t(out);
	}
};

// ---------------------------------------------------------------------------
// 2. Fixed-point vs float accuracy
// ---------------------------------------------------------------------------
static void testAccuracy()
{
	struct Case { DesignParams p; int bank; };
	const Case cases[] = {
		{ { 1000.0f, 0.0f, 0.0f }, 0 },   // lowpass
		{ { 1000.0f, 1.0f, 0.3f }, 0 },   // bandpass
		{ { 1000.0f, 2.0f, 0.0f }, 0 },   // highpass
		{ { 1000.0f, 3.0f, 0.0f }, 0 },   // notch
		{ { 1000.0f, 3.5f, 0.5f }, 0 },   // between notch & twin peaks
		{ { 1000.0f, 4.6f, 0.4f }, 0 },   // twin peaks → quad notch (4th section growing)
		{ { 800.0f,  0.0f, 0.2f }, 1 },   // vowel A
		{ { 800.0f,  2.5f, 0.2f }, 1 },   // between I and O
		{ { 60.0f,   0.0f, 0.8f }, 0 },   // low, resonant lowpass (hardest case)
	};
	const float testHz[] = { 100.0f, 300.0f, 700.0f, 1000.0f, 1500.0f, 3000.0f, 8000.0f };
	float worst = 0.0f;
	int measured = 0;
	for (const auto& c : cases)
	{
		Design d; designMorph(gBanks[c.bank], c.p, d);
		FixedBiquad fq[kMaxSections]; int32_t mk; toFixed(d, fq, mk);
		for (float hz : testHz)
		{
			const float predDb = designDb(d, hz);
			// Choose an amplitude that should give a ~-12 dBFS output so the soft
			// clipper stays out of the way, but not below ~-60 dBFS in.
			float amp = 2047.0f * powf(10.0f, (-12.0f - predDb) / 20.0f);
			if (amp > 1500.0f) amp = 1500.0f;
			if (amp < 2.0f) continue;   // response too far down to measure with 12-bit input
			FixedChannel ch; ch.snapTo(fq, mk, d.numSections);
			const int N = 48000, settle = 24000;
			double sumIn = 0, sumOut = 0;
			for (int n = 0; n < N; ++n)
			{
				float s = amp * sinf(2.0f * kPi * hz * float(n) / kSampleRate);
				int16_t in = int16_t(lrintf(s));
				int16_t out = ch.process(in);
				if (n >= settle) { sumIn += double(in) * in; sumOut += double(out) * out; }
			}
			float measDb = 10.0f * float(log10((sumOut + 1e-9) / sumIn));
			float outDbfs = 10.0f * float(log10(sumOut / (N - settle) / (2047.0 * 2047.0 / 2.0) + 1e-12));
			if (outDbfs < -55.0f) continue;   // too close to 12-bit output noise floor to judge
			float err = fabsf(measDb - predDb);
			if (err > worst) worst = err;
			++measured;
			CHECK(err < 1.0f, "fixed vs float mismatch %.2f dB (pred %.2f meas %.2f) hz=%g morph=%g bank=%d cutoff=%g",
			      double(err), double(predDb), double(measDb), double(hz), double(c.p.morph), c.bank, double(c.p.cutoffHz));
		}
	}
	std::printf("[accuracy] %d measurements, worst fixed-vs-float gain error %.3f dB\n", measured, double(worst));
}

// ---------------------------------------------------------------------------
// 3. Torture test: fast modulation + noise, three banks
// ---------------------------------------------------------------------------
static void testTorture()
{
	static BankData wild = makeWildBank();
	FixedChannel ch;
	uint32_t rng = 12345;
	int peak = 0;
	long clipped = 0;
	const int N = 48000 * 30;  // 30 seconds
	Design d; FixedBiquad tgt[kMaxSections]; int32_t mk = 1 << kMakeupFracBits;
	int numSections = 0;
	for (int n = 0; n < N; ++n)
	{
		// Redesign every 16 samples, like core 1 might (it is probably faster).
		if ((n & 15) == 0)
		{
			float tsec = float(n) / kSampleRate;
			const int which = (n / (48000 * 5)) % 3;   // change bank every 5 s
			const BankData& b = which == 2 ? wild : gBanks[which];
			DesignParams p;
			p.cutoffHz  = 20.0f * powf(2.0f, 10.0f * (0.5f + 0.5f * sinf(2.0f * kPi * 3.1f * tsec)));
			p.morph     = (0.5f + 0.5f * sinf(2.0f * kPi * 7.3f * tsec)) * float(b.numSlots - 1);
			p.resonance = 0.5f + 0.5f * sinf(2.0f * kPi * 0.37f * tsec);
			designMorph(b, p, d);
			toFixed(d, tgt, mk);
			numSections = d.numSections;
		}
		ch.smoothTo(tgt, mk, numSections);
		rng = rng * 1664525u + 1013904223u;
		int16_t in = int16_t(int32_t(rng >> 20) - 2048);   // full-scale white noise
		int16_t out = ch.process(in);
		int a = abs(out);
		if (a > peak) peak = a;
		if (a >= 2047) ++clipped;
	}
	// Output is bounded by the soft clipper anyway; the interesting check is
	// that the internal states settle again afterwards (no stuck oscillation).
	for (int n = 0; n < 48000; ++n) ch.process(0);
	int tail = 0;
	for (int n = 0; n < 4800; ++n) { int a = abs(ch.process(0)); if (a > tail) tail = a; }
	CHECK(tail <= 1, "filter still ringing %d after 1 s of silence (limit cycle?)", tail);
	std::printf("[torture] 30 s fast modulation over 3 banks: peak %d, %.3f%% samples at full scale, silent tail %d\n",
	            peak, 100.0 * double(clipped) / N, tail);
}

// ---------------------------------------------------------------------------
// 4. Data formats
// ---------------------------------------------------------------------------
static bool sameSlot(const SlotData& a, const SlotData& b)
{
	if (a.numSections != b.numSections) return false;
	for (int i = 0; i < kMaxSections; ++i)
	{
		const SectionData &x = a.sec[i], &y = b.sec[i];
		if (x.poleRatio != y.poleRatio || x.poleDamp != y.poleDamp || x.resWeight != y.resWeight ||
		    x.zeroMode != y.zeroMode || x.zeroA != y.zeroA || x.zeroB != y.zeroB) return false;
	}
	return true;
}

static void testData()
{
	// Every factory slot survives encode → decode exactly.
	for (int b = 0; b < kNumBanks; ++b)
		for (int s = 0; s < kNumSlots; ++s)
		{
			uint8_t bytes[kSlotBytes];
			encodeSlot(gBanks[b].slot[s], bytes);
			for (uint8_t v : bytes) CHECK(v < 128, "SysEx byte >= 128");
			SlotData back;
			CHECK(decodeSlot(bytes, kSlotBytes, back), "decode failed");
			CHECK(sameSlot(back, gBanks[b].slot[s]), "slot %d/%d changed in encode/decode", b, s);
		}

	// Random garbage decodes into something safe and stable.
	uint32_t rng = 99;
	for (int trial = 0; trial < 2000; ++trial)
	{
		uint8_t bytes[kSlotBytes];
		for (auto& v : bytes) { rng = rng * 1664525u + 1013904223u; v = uint8_t((rng >> 24) & 0x7F); }
		SlotData s;
		decodeSlot(bytes, kSlotBytes, s);
		CHECK(s.numSections <= kMaxSections, "garbage section count");
		Design d;
		designBetween(s, s, 0.0f, 1000.0f, 1.0f, s.numSections, d);
		for (int i = 0; i < d.numSections; ++i)
		{
			CHECK(std::isfinite(d.sec[i].b0) && std::isfinite(d.sec[i].b1) && std::isfinite(d.sec[i].b2) &&
			      std::isfinite(d.sec[i].a1) && std::isfinite(d.sec[i].a2), "garbage gave non-finite coefficient");
			CHECK(sqrtf(d.sec[i].a2) < 1.0f, "garbage gave unstable pole");
		}
		CHECK(std::isfinite(d.makeup), "garbage gave non-finite makeup");
	}

	// smull32 (the portable C version; tools/emulate_m0.py checks the ARM
	// assembly version) against ordinary 64-bit multiplication.
	for (int trial = 0; trial < 1000000; ++trial)
	{
		rng = rng * 1664525u + 1013904223u; const int32_t a = int32_t(rng);
		rng = rng * 1664525u + 1013904223u; const int32_t b = int32_t(rng) >> (trial & 15);
		CHECK(smull32(a, b) == int64_t(a) * int64_t(b), "smull32(%d, %d) wrong", a, b);
	}
	for (int32_t a : { 0, 1, -1, 0x7FFFFFFF, int32_t(-0x7FFFFFFF - 1), 0xFFFF, -0xFFFF, 0x8000, -0x8000 })
		for (int32_t b : { 0, 1, -1, 0x7FFFFFFF, int32_t(-0x7FFFFFFF - 1), 0xFFFF, -0xFFFF, 0x8000, -0x8000 })
			CHECK(smull32(a, b) == int64_t(a) * int64_t(b), "smull32 edge (%d, %d) wrong", a, b);

	// Flash image: valid when built properly, invalid when any byte flips.
	FlashImage img;
	std::memset(&img, 0, sizeof(img));
	img.magic = kFlashMagic; img.version = kFlashVersion;
	for (int b = 0; b < kNumBanks; ++b) img.banks[b] = gBanks[b];
	img.crc = imageCrc(img);
	CHECK(imageValid(img), "fresh flash image not valid");
	FlashImage bad = img;
	reinterpret_cast<uint8_t*>(&bad)[100] ^= 1;
	CHECK(!imageValid(bad), "corrupted flash image passed CRC");
	FlashImage erased;
	std::memset(&erased, 0xFF, sizeof(erased));   // what blank flash reads as
	CHECK(!imageValid(erased), "blank flash accepted as valid");
	std::printf("[data] slot = %d SysEx bytes, flash image = %d bytes; encode/decode, garbage, smull32 and CRC checks done\n",
	            kSlotBytes, int(sizeof(FlashImage)));
}

// ---------------------------------------------------------------------------
// 5. SVG plots
// ---------------------------------------------------------------------------
static void plotBank(int bank, float cutoff, float res, const char* filename)
{
	static const char* names[kNumBanks][kNumSlots] = {
		{ "Lowpass 6-pole", "Bandpass", "Highpass 6-pole", "Triple notch", "Twin peaks", "Quad notch" },
		{ "A (father)", "E (bed)", "I (beet)", "O (bought)", "U (boot)", "ER (bird)" } };
	const int W = 900, H = 500, L = 60, R = 20, T = 40, B = 50;
	const float fLo = 20.0f, fHi = 20000.0f, dbLo = -60.0f, dbHi = 18.0f;
	auto X = [&](float hz) { return double(L + (W - L - R) * log10f(hz / fLo) / log10f(fHi / fLo)); };
	auto Y = [&](float db) { if (db < dbLo) db = dbLo; if (db > dbHi) db = dbHi;
	                         return double(T + (H - T - B) * (dbHi - db) / (dbHi - dbLo)); };

	FILE* f = std::fopen(filename, "w");
	if (!f) return;
	std::fprintf(f, "<svg xmlns='http://www.w3.org/2000/svg' width='%d' height='%d' font-family='sans-serif' font-size='12'>\n", W, H);
	std::fprintf(f, "<rect width='100%%' height='100%%' fill='white'/>\n");
	std::fprintf(f, "<text x='%d' y='22' font-size='15'>Bank %c (%s): morph sweep slot 1 to 6, cutoff %.0f Hz, resonance %.1f</text>\n",
	             L, 'A' + bank, bank == 0 ? "classic" : "vowels", double(cutoff), double(res));
	for (float hz : { 20.f, 50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f, 20000.f })
		std::fprintf(f, "<line x1='%.1f' x2='%.1f' y1='%d' y2='%d' stroke='#ddd'/><text x='%.1f' y='%d' text-anchor='middle'>%g</text>\n",
		             X(hz), X(hz), T, H - B, X(hz), H - B + 16, double(hz));
	for (float db = dbLo; db <= dbHi; db += 12.0f)
		std::fprintf(f, "<line x1='%d' x2='%d' y1='%.1f' y2='%.1f' stroke='#ddd'/><text x='%d' y='%.1f' text-anchor='end'>%+.0f dB</text>\n",
		             L, W - R, Y(db), Y(db), L - 4, Y(db) + 4, double(db));
	for (int k = 0; k <= 20; ++k)
	{
		const float morph = float(k) * 0.25f;
		Design d; designMorph(gBanks[bank], { cutoff, morph, res }, d);
		const int hue = int(240.0f - 240.0f * morph / 5.0f);   // blue (slot 1) → red (slot 6)
		const bool whole = (k % 4 == 0);
		std::fprintf(f, "<polyline fill='none' stroke='hsl(%d,75%%,45%%)' stroke-width='%s' stroke-opacity='%s' points='",
		             hue, whole ? "2.5" : "1", whole ? "1" : "0.55");
		for (float hz = fLo; hz <= fHi; hz *= 1.02f) std::fprintf(f, "%.1f,%.1f ", X(hz), Y(designDb(d, hz)));
		std::fprintf(f, "'/>\n");
		if (whole)
			std::fprintf(f, "<text x='%d' y='%d' fill='hsl(%d,75%%,40%%)'>%d: %s</text>\n",
			             W - R - 150, T + 16 + 16 * (k / 4), hue, k / 4 + 1, names[bank][k / 4]);
	}
	std::fprintf(f, "</svg>\n");
	std::fclose(f);
	std::printf("[plot] wrote %s\n", filename);
}

// ---------------------------------------------------------------------------
// Reference vectors for the JavaScript port (tools/check_web.js)
// ---------------------------------------------------------------------------
static void printVectors()
{
	// Encoded factory slots, as hex.
	for (int b = 0; b < kNumBanks; ++b)
		for (int s = 0; s < kNumSlots; ++s)
		{
			uint8_t bytes[kSlotBytes];
			encodeSlot(gBanks[b].slot[s], bytes);
			std::printf("SLOT %d %d ", b, s);
			for (uint8_t v : bytes) std::printf("%02x", v);
			std::printf("\n");
		}
	// Designed responses (dB) at a few frequencies, for a grid of settings.
	const float hzs[] = { 50.0f, 200.0f, 700.0f, 1000.0f, 1500.0f, 4000.0f, 12000.0f };
	for (int b = 0; b < kNumBanks; ++b)
		for (float morph : { 0.0f, 0.5f, 1.3f, 2.0f, 3.7f, 4.5f, 5.0f })
			for (float res : { 0.0f, 0.6f })
				for (float cut : { 150.0f, 1000.0f, 5000.0f })
				{
					Design d; designMorph(gBanks[b], { cut, morph, res }, d);
					std::printf("RESP %d %g %g %g", b, double(morph), double(res), double(cut));
					for (float hz : hzs) std::printf(" %.4f", double(designDb(d, hz)));
					std::printf("\n");
				}
}

int main(int argc, char** argv)
{
	for (int b = 0; b < kNumBanks; ++b) factoryBank(b, gBanks[b]);
	if (argc > 1 && std::strcmp(argv[1], "vectors") == 0) { printVectors(); return 0; }

	testStability();
	testAccuracy();
	testTorture();
	testData();
	plotBank(0, 1000.0f, 0.0f, "response_classic_res0.svg");
	plotBank(0, 1000.0f, 0.6f, "response_classic_res06.svg");
	plotBank(1, 800.0f, 0.3f, "response_vowels.svg");
	std::printf(failures ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", failures);
	return failures ? 1 : 0;
}
