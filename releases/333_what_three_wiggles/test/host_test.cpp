// Desktop tests for the seeding, DSP and engine code.
//
//   ./test/run.sh
//
// Built with -fsanitize=undefined, so any signed overflow in the fixed-point
// maths fails the run.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <initializer_list>

#include "../seed.h"
#include "../dsp.h"
#include "../patch.h"
#include "../engines.h"
#include "../protocol.h"

using namespace w3w;

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static bool Seeds(const char *text, uint32_t out[3])
{
	return SeedsFromWords(reinterpret_cast<const uint8_t *>(text), std::strlen(text), out);
}

// These vectors are also checked by web/seed.test.mjs, and listed in SEEDING.md.
struct Vector
{
	const char *words;
	uint32_t seed[3];
	uint32_t combined;
	uint32_t walkFirst[4]; // first four Next() outputs of the walk stream
};

static const Vector kVectors[] = {
	{"what.three.wiggles", {0x929983ca, 0x5fa8125a, 0x51018971}, 0xd1c5100a, {0xabaaf638, 0xbb2e728b, 0x0b6db697, 0x1b16e93e}},
	{"index.home.raft", {0xa0ecfcd6, 0x3b2fa9c5, 0x4c12964c}, 0x5bc1c7d8, {0x48fe20c3, 0x51f90a6b, 0xcc0c3dc4, 0xf7c02b0f}},
	{"filled.count.soap", {0x9becd478, 0x9b11efec, 0xefb2b764}, 0xadb8f4e0, {0x099ca06f, 0x088303d6, 0x0acdaa22, 0x326e3662}},
	{"\xc3\xa9t\xc3\xa9.caf\xc3\xa9.cr\xc3\xaame", {0xa17d532d, 0xdf518d52, 0x3a0bb5c7}, 0x35d06ec9, {0x9dddc3cc, 0x25685b94, 0xaec69a63, 0x28d43797}},
	{"a.b.c", {0x1a80b1b3, 0x82c46232, 0x46ad5f91}, 0x136dcfd1, {0x1fb6eee4, 0xb42ae3d3, 0x269d605f, 0x74b88fa7}},
};

static void PrintVector(const char *text)
{
	uint32_t s[3];
	Seeds(text, s);
	uint32_t c = CombineSeeds(s[0], s[1], s[2]);
	Rng r(DeriveStream(c, STREAM_WALK));
	uint32_t w[4];
	for (int i = 0; i < 4; i++) w[i] = r.Next(); // not in the printf: argument order is unspecified
	std::printf("  {\"%s\", {0x%08x, 0x%08x, 0x%08x}, 0x%08x, {0x%08x, 0x%08x, 0x%08x, 0x%08x}},\n",
		text, s[0], s[1], s[2], c, w[0], w[1], w[2], w[3]);
}

static void TestVectors()
{
	for (const Vector &v : kVectors)
	{
		uint32_t s[3];
		CHECK(Seeds(v.words, s));
		for (int i = 0; i < 3; i++) CHECK(s[i] == v.seed[i]);
		uint32_t c = CombineSeeds(s[0], s[1], s[2]);
		CHECK(c == v.combined);
		Rng r(DeriveStream(c, STREAM_WALK));
		for (int i = 0; i < 4; i++) CHECK(r.Next() == v.walkFirst[i]);
	}
}

static void TestParsing()
{
	uint32_t s[3], t[3];
	CHECK(Seeds("a.b.c", s));
	CHECK(!Seeds("a.b", s));
	CHECK(!Seeds("a.b.c.d", s));
	CHECK(!Seeds("a..c", s));
	CHECK(!Seeds(".b.c", s));
	CHECK(!Seeds("a.b.", s));
	CHECK(!Seeds("", s));
	// Order matters for the combined seed
	Seeds("index.home.raft", s);
	Seeds("raft.home.index", t);
	CHECK(CombineSeeds(s[0], s[1], s[2]) != CombineSeeds(t[0], t[1], t[2]));
	// Non-ASCII words are just bytes
	CHECK(Seeds("\xc3\xa9t\xc3\xa9.caf\xc3\xa9.cr\xc3\xaame", s));
}

static void TestRng()
{
	Rng r(1234);
	int counts[6] = {0};
	for (int i = 0; i < 60000; i++)
	{
		int32_t v = r.Range(1, 6);
		CHECK(v >= 1 && v <= 6);
		counts[v - 1]++;
	}
	for (int i = 0; i < 6; i++) CHECK(counts[i] > 9500 && counts[i] < 10500);
}

static void TestTuning()
{
	double c4 = 261.625565 * 4294967296.0 / 48000.0;
	double worst = 0;
	for (int32_t mv = -16000; mv <= 4999; mv += 7)
	{
		double want = c4 * std::pow(2.0, mv / 1000.0);
		double got = IncFromMillivolts(mv);
		double cents = 1200.0 * std::log2(got / want);
		// Very low LFO rates lose precision from the shift; audio range must be tight
		if (mv >= -6000 && std::fabs(cents) > worst) worst = std::fabs(cents);
		if (mv >= -12000) CHECK(std::fabs(cents) < 5.0);
	}
	std::printf("  worst tuning error from -6 V to +5 V: %.3f cents\n", worst);
	CHECK(worst < 0.5);

	// Sine table
	CHECK(Sine(0) == 0);
	CHECK(std::abs(Sine(0x40000000u) - 32767) <= 1);
	CHECK(std::abs(Sine(0xC0000000u) + 32767) <= 1);
}

static void TestQuantiser()
{
	const Scale &major = kScales[1];
	CHECK(NearestInScale(0, major) == 0);
	CHECK(NearestInScale(1400, major) == 2);   // 1.4 semitones -> D
	CHECK(NearestInScale(-600, major) == -1);  // just below C4 -> B3
	CHECK(NearestInScale(11600, major) == 12); // close to C5
	CHECK(DegreeToSemitone(7, major) == 12);
	CHECK(DegreeToSemitone(-1, major) == -1);

	Quantiser q;
	CHECK(q.Process(0, major) == 0);
	// 0.55 semitones is nearer C# but C# isn't in major: stays C
	CHECK(q.Process(46, major) == 0);
	// Hysteresis between D (2) and E (4): at 3.1 semitones, stay on D
	CHECK(q.Process(SemitonesToMillivolts(2), major) == 2);
	CHECK(q.Process(3100 / 12, major) == 2);
	CHECK(q.Process(3300 / 12, major) == 4);
	CHECK(SemitonesToMillivolts(12) == 1000);
	CHECK(SemitonesToMillivolts(-12) == -1000);
}

// Run every engine for a while and hash what comes out
static uint32_t RunEngines(const Patch &p, int samples, int32_t depth, bool ringMod, int32_t density)
{
	Clock clock;
	Triggers triggers;
	Walk walk;
	ComplexLfo lfo;
	Additive additive;
	FmLoop fm;
	const Scale &s = kScales[5];
	int32_t maxDeg = WalkMaxDegree(24, s);
	uint32_t clockInc = IncFromMillivolts(-5000);
	uint32_t lfoInc = IncFromMillivolts(-6000);
	uint32_t pitchInc = IncFromMillivolts(-1000);

	clock.Reset();
	triggers.Reset(p);
	walk.Reset(p, maxDeg, s);
	lfo.Reset(p);
	fm.Reset(p);

	uint32_t h = 2166136261u;
	auto mix = [&h](int32_t v) { h = (h ^ uint32_t(v)) * 16777619u; };
	int32_t maxAbs = 0;
	for (int i = 0; i < samples; i++)
	{
		if (clock.Process(false, false, clockInc))
		{
			triggers.Tick(TriggerDensity(p.triggerDensity, density), clock.period);
			walk.Tick(p, maxDeg, s, clock.period);
		}
		int32_t a1 = fm.Process(p, depth, ringMod);
		int32_t a2 = additive.Process(p, pitchInc);
		int32_t cv1 = walk.Process();
		int32_t cv2 = lfo.Process(p, lfoInc);
		bool p1 = triggers.Process();
		CHECK(a1 >= -32767 && a1 <= 32767);
		CHECK(a2 >= -32767 && a2 <= 32767);
		CHECK(cv2 >= -32767 && cv2 <= 32767);
		CHECK(cv1 >= 0 && cv1 <= SemitonesToMillivolts(24));
		if (std::abs(a2) > maxAbs) maxAbs = std::abs(a2);
		mix(a1); mix(a2); mix(cv1); mix(cv2); mix(p1); mix(triggers.flipFlop);
	}
	CHECK(maxAbs > 1000); // the additive osc makes sound
	return h;
}

static void TestDeterminism()
{
	uint32_t s[3];
	Patch a, b;
	Seeds("index.home.raft", s);
	BuildPatch(s, a);
	Seeds("index.home.raft", s);
	BuildPatch(s, b);
	CHECK(std::memcmp(&a, &b, sizeof(Patch)) == 0);

	uint32_t first = RunEngines(a, 96000, 2048, false, 2048);
	uint32_t again = RunEngines(a, 96000, 2048, false, 2048);
	CHECK(first == again);

	Patch c;
	Seeds("index.home.rafts", s);
	BuildPatch(s, c);
	CHECK(RunEngines(c, 96000, 2048, false, 2048) != first);
}

// Many random patches, extreme settings: nothing may overflow or leave range
static void TestExtremes()
{
	Rng r(42);
	for (int i = 0; i < 300; i++)
	{
		uint32_t s[3] = {r.Next(), r.Next(), r.Next()};
		Patch p;
		BuildPatch(s, p);
		int32_t levels = 0;
		for (int k = 0; k < kNumPartials; k++) levels += p.partialLevel[k];
		CHECK(levels <= 32767);
		CHECK(p.partialLevel[0] > 0);
		CHECK(p.fmLevel[0] + p.fmLevel[1] + p.fmLevel[2] <= 32767);
		CHECK(p.lfoFold >= 256 && p.lfoFold <= 768);
		CHECK(p.walkMaxHold >= 1 && p.walkMaxHold <= 4);
		RunEngines(p, 4000, 4095, i & 1, (i * 37) % 4096);
		RunEngines(p, 2000, 0, i & 1, 4095);
	}

	// Additive at the top and bottom of the pitch range
	Patch p;
	uint32_t s[3] = {1, 2, 3};
	BuildPatch(s, p);
	Additive add;
	for (int32_t mv : {-6000, 0, 4999})
	{
		for (int i = 0; i < 20000; i++)
		{
			int32_t v = add.Process(p, IncFromMillivolts(mv));
			CHECK(v >= -32767 && v <= 32767);
		}
	}
}

static void TestTriggerDensity()
{
	CHECK(TriggerDensity(30000, 0) == 0);
	CHECK(TriggerDensity(30000, 2048) == 30000);
	CHECK(TriggerDensity(30000, 4095) >= 65500);

	// Turning density up only ever adds hits
	Patch p;
	uint32_t s[3] = {7, 8, 9};
	BuildPatch(s, p);
	Triggers lo, hi;
	lo.Reset(p);
	hi.Reset(p);
	for (int i = 0; i < 1000; i++)
	{
		bool a = lo.Tick(20000, 24000);
		bool b = hi.Tick(40000, 24000);
		CHECK(!a || b);
	}
}

// Card emulator for the browser test (test/web_test.mjs):
//   host_test --card <words on the card> <hex SysEx body between 7D and F7>
// prints the words now on the card, then the hex body of the reply.
// Mirrors ProcessIncomingSysEx in main.cpp, using the same protocol.h.
static int EmulateCard(const char *wordsArg, const char *hex)
{
	uint8_t words[kMaxWordsLen];
	uint32_t wordsLen = uint32_t(std::strlen(wordsArg));
	std::memcpy(words, wordsArg, wordsLen);

	uint8_t in[512];
	uint32_t size = 0;
	for (const char *c = hex; c[0] && c[1] && size < sizeof(in); c += 2)
	{
		unsigned v;
		std::sscanf(c, "%2x", &v);
		in[size++] = uint8_t(v);
	}

	const uint8_t version[3] = {0, 1, 0};
	uint8_t out[kMaxStateLen];
	uint32_t n = 0;
	if (size >= 2 && in[0] == kCardId && in[1] == kMsgGetStats)
	{
		n = EncodeStats(1234, 3000, out); // made-up numbers: only the format is real
	}
	else if (size >= 2 && in[0] == kCardId && in[1] == kMsgHello)
	{
		n = EncodeState(words, wordsLen, version, out);
	}
	else if (size >= 2 && in[0] == kCardId && in[1] == kMsgSetWords)
	{
		uint8_t text[kMaxWordsLen];
		uint32_t len;
		uint32_t seeds[3];
		if (!DecodeSetWords(in, size, text, len))
		{
			out[n++] = kCardId; out[n++] = kMsgError; out[n++] = kErrBadMessage;
		}
		else if (!SeedsFromWords(text, len, seeds))
		{
			out[n++] = kCardId; out[n++] = kMsgError; out[n++] = kErrBadWords;
		}
		else
		{
			std::memcpy(words, text, len);
			wordsLen = len;
			n = EncodeState(words, wordsLen, version, out);
		}
	}
	std::printf("%.*s\n", int(wordsLen), reinterpret_cast<const char *>(words));
	for (uint32_t i = 0; i < n; i++) std::printf("%02x", out[i]);
	std::printf("\n");
	return 0;
}

static void TestProtocol()
{
	// SET_WORDS for "a.b.c", as the webapp builds it
	const uint8_t msg[] = {0x33, 0x03, 0, 5, 6, 1, 2, 14, 6, 2, 2, 14, 6, 3};
	uint8_t text[kMaxWordsLen];
	uint32_t len = 0;
	CHECK(DecodeSetWords(msg, sizeof(msg), text, len));
	CHECK(len == 5 && std::memcmp(text, "a.b.c", 5) == 0);
	CHECK(!DecodeSetWords(msg, sizeof(msg) - 1, text, len)); // truncated
	const uint8_t tooLong[] = {0x33, 0x03, 6, 0};            // 96 bytes
	CHECK(!DecodeSetWords(tooLong, sizeof(tooLong), text, len));

	uint8_t out[kMaxStateLen];
	const uint8_t version[3] = {0, 1, 0};
	uint32_t n = EncodeState(text, len, version, out);
	CHECK(n == 2 + 3 + 24 + 2 + 10);
	uint32_t seed0 = 0;
	for (int i = 0; i < 8; i++) seed0 = (seed0 << 4) | out[5 + i];
	CHECK(seed0 == 0x1a80b1b3); // matches the "a.b.c" vector
	for (uint32_t i = 0; i < n; i++) CHECK(out[i] < 0x80); // valid SysEx data
}

int main(int argc, char **argv)
{
	gTables.Init();

	if (argc == 4 && std::strcmp(argv[1], "--card") == 0)
	{
		return EmulateCard(argv[2], argv[3]);
	}

	if (argc > 1 && std::strcmp(argv[1], "--vectors") == 0)
	{
		for (const char *w : {"what.three.wiggles", "index.home.raft", "filled.count.soap",
		                      "\xc3\xa9t\xc3\xa9.caf\xc3\xa9.cr\xc3\xaame", "a.b.c"})
		{
			PrintVector(w);
		}
		return 0;
	}

	TestVectors();
	TestParsing();
	TestProtocol();
	TestRng();
	TestTuning();
	TestQuantiser();
	TestTriggerDensity();
	TestDeterminism();
	TestExtremes();

	if (failures)
	{
		std::printf("%d check(s) failed\n", failures);
		return 1;
	}
	std::printf("host tests passed\n");
	return 0;
}
