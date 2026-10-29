// Host test of the card logic (paging, soft takeover, switch, I/O) using the
// real src/main.cpp against a mock ComputerCard.
//
//   g++ -O2 -std=c++17 -Imock -include mock/ComputerCard.h -I../src test_card.cpp -o test_card && ./test_card

#include <cmath>
#include <cstdio>
#define main app_main
#include "../src/main.cpp"
#undef main

static int failures = 0;
#define CHECK(cond, ...)                                  \
	do                                                    \
	{                                                     \
		if (!(cond))                                      \
		{                                                 \
			printf("FAIL line %d: ", __LINE__);           \
			printf(__VA_ARGS__);                          \
			printf("\n");                                 \
			failures++;                                   \
		}                                                 \
	} while (0)

static void Run(Hypster &c, int n)
{
	for (int i = 0; i < n; i++) c.Tick();
}
// core 1 stand-in
static void ServiceShape()
{
	const int32_t req = gShapeRequest;
	if (req != gShapeBuilt)
	{
		const int32_t back = 1 - gShapeFront;
		BuildShape(gShape[back], req);
		gShapeBuilt = req;
		gShapeFront = back;
	}
}

int main()
{
	hyp::BuildShaperTables(gTabs);
	BuildShape(gShape[0], 0);
	gShapeBuilt = 0;
	Hypster card;

	// --- page 1 follows the knobs from power on
	card.knob[0] = 1000;
	Run(card, 8);
	const int32_t k1000 = gS.core.kRate;
	card.knob[0] = 3000;
	Run(card, 8);
	const int32_t k3000 = gS.core.kRate;
	CHECK(gS.page == 0 && gS.param[0][0] == 3000, "page 1 live knobs");
	const double ratio = (double)k3000 / k1000, want = std::exp2(2000.0 * 16 / 4096);
	CHECK(std::fabs(ratio / want - 1) < 0.01, "rate is exponential: ratio %.2f want %.2f", ratio, want);

	// --- CV In 1: ignored when unpatched, 1V/oct at full amount when patched
	card.cvIn[0] = 341; // ~1V
	Run(card, 8);
	CHECK(gS.core.kRate == k3000, "unpatched CV ignored");
	card.connected[ComputerCard::CV1] = true;
	Run(card, 8);
	CHECK(std::fabs((double)gS.core.kRate / k3000 - 2.0) < 0.02, "1V/oct: %.3f", (double)gS.core.kRate / k3000);
	card.cvIn[0] = 0;

	// --- tap Z down -> page 2, knobs must be picked up
	card.sw = ComputerCard::Down;
	Run(card, 4800);
	card.sw = ComputerCard::Middle;
	Run(card, 8);
	CHECK(gS.page == 1, "tap -> page 2 (page=%d)", gS.page);
	// Shape mode stored at 0, knob sits at 2048: must not jump
	card.knob[1] = 2048;
	Run(card, 64);
	CHECK(gS.param[1][1] == 0, "soft takeover holds the stored value (%d)", (int)gS.param[1][1]);
	card.knob[1] = 20; // reaches the stored value -> caught
	Run(card, 8);
	card.knob[1] = 2600; // -> zone 3 of 6: option 1, squirrelly, wild
	Run(card, 16);
	CHECK(gS.param[1][1] == 2600, "caught and following");
	CHECK((gShapeRequest >> 8) == 3, "shape mode request %d", (int)(gShapeRequest >> 8));
	ServiceShape();
	Run(card, 8);
	CHECK(gShapeFront == 1, "audio core switched to the rebuilt shape table");
	// page 1's Rate was not touched by the page 2 knobs
	CHECK(gS.param[0][0] == 3000, "page 1 values kept");

	// --- more taps cycle 2 -> 3 -> 4 -> 1
	for (int t = 0; t < 3; t++)
	{
		card.sw = ComputerCard::Down;
		Run(card, 2000);
		card.sw = ComputerCard::Middle;
		Run(card, 2000);
	}
	CHECK(gS.page == 0, "pages wrap (page=%d)", gS.page);

	// --- hold down 1s from page 3 -> page 1, and the release is not a tap
	card.sw = ComputerCard::Down; Run(card, 2000); card.sw = ComputerCard::Middle; Run(card, 100);
	card.sw = ComputerCard::Down; Run(card, 2000); card.sw = ComputerCard::Middle; Run(card, 100);
	CHECK(gS.page == 2, "two taps -> page 3");
	card.sw = ComputerCard::Down;
	Run(card, 48000 + 10);
	CHECK(gS.page == 0, "hold -> page 1");
	card.sw = ComputerCard::Middle;
	Run(card, 10);
	CHECK(gS.page == 0, "release after hold stays on page 1");

	// --- Z up: limiter
	card.sw = ComputerCard::Up;
	Run(card, 8);
	CHECK(gS.limiter, "Z up fits the limiter");
	card.sw = ComputerCard::Middle;
	Run(card, 8);
	CHECK(!gS.limiter, "Z middle removes it");

	// --- outputs: Audio Out 1 = x scaled 10.8V -> 2047 at level 1x
	card.knob[0] = 2600; card.knob[1] = 3000; card.knob[2] = 2000;
	Run(card, 48000 * 2);
	const double x = hyp::FromQ24(gS.core.X());
	const double expect = x * 2047.0 / 10.8;
	CHECK(std::fabs(card.audioOut[0] - expect) < 2.5, "audio out 1 = x: %d vs %.1f", card.audioOut[0], expect);
	CHECK(card.pulseOut[0] == (x > 0) || std::fabs(x) < 0.25, "pulse out 1 follows sign of x");

	// --- it is oscillating at this setting
	int16_t lo = 2047, hi = -2048;
	for (int i = 0; i < 48000; i++)
	{
		card.Tick();
		lo = std::min(lo, card.audioOut[0]);
		hi = std::max(hi, card.audioOut[0]);
	}
	CHECK(hi - lo > 300, "x swings (%d..%d)", lo, hi);

	// --- Pulse In 2 holds, Pulse In 1 kicks
	card.pulseIn[1] = true;
	Run(card, 16);
	const int32_t held = gS.core.X();
	Run(card, 4800);
	CHECK(gS.core.X() == held, "hold freezes");
	card.pulseIn[1] = false;
	card.pulseIn[0] = true;
	Run(card, 1);
	card.pulseIn[0] = false;
	CHECK(std::fabs(hyp::FromQ24(gS.core.X())) < 0.6, "kick re-seeds near zero");

	printf("%s\n", failures ? "FAILED" : "card logic tests passed");
	return failures ? 1 : 0;
}
