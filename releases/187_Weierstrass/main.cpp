// WEIERSTRASS - an endless zoom into a nowhere-differentiable function, for the
// Music Thing Modular Workshop System Computer.
//
// The DSP is in weierstrass.h; this file connects it to the panel.
//
//   Main      ZOOM. Noon stops. Left zooms in: pitch falls and finer detail
//             comes down from above. Right zooms out: pitch rises. + CV In 1.
//   X         ROUGHNESS, as fractal dimension D = 1.0 .. 1.95.  + CV In 2.
//   Y         RATIO b, 2..7 in six detented bands.
//   Switch    Up: SCATTER.  Middle: TRUE.  Down (held): FREEZE.
//   Audio In 1  moves the read head in time (phase modulation through the fractal).
//   Audio In 2  WINDOW: shifts the lowest partial up or down from 20 Hz.
//   Pulse In 1  RESET to x = 0.   Pulse In 2  REVERSE while high.
//   Audio Out 1 W(x) (cosines).   Audio Out 2  the same with sines.
//   CV Out 1    zoom position, a ramp per zoom step.
//   CV Out 2    the same zooming function, 1024x slower.
//   Pulse Out 1 a trigger per zoom step.   Pulse Out 2  CV Out 2 > 0.
//   LEDs        partial levels, lowest top-left, reading across then down.

#include "ComputerCard.h"
#include "weierstrass.h"

class Weierstrass : public ComputerCard
{
public:
	Weierstrass()
	{
		EnableNormalisationProbe();
	}

protected:
	void __not_in_flash_func(ProcessSample)() override
	{
		// --- zoom rate -----------------------------------------------------
		// Square law with a deadzone at noon, so the slow end is fine-grained
		// and the stopped position can be found by feel. Full turn is about
		// 2 zoom steps per second; CV In 1 adds about +-1.
		int32_t k = KnobVal(Main) - 2048;
		int32_t d = k > kDeadzone ? k - kDeadzone : (k < -kDeadzone ? k + kDeadzone : 0);
		int32_t mag = (d * d * 47) >> 10;
		int32_t rate = (d < 0 ? -mag : mag) + CVIn1() * 44;
		if (rate > kMaxRate) rate = kMaxRate;
		if (rate < -kMaxRate) rate = -kMaxRate;
		if (PulseIn2()) rate = -rate;
		if (SwitchVal() == Down) rate = 0;
		engine.SetZoomRate(rate);

		// --- shape ---------------------------------------------------------
		engine.SetRoughness(KnobVal(X) + CVIn2());

		int32_t y = KnobVal(Y);
		if (y < kBandEdge[band] - kHysteresis || y >= kBandEdge[band + 1] + kHysteresis)
			band = (y * 6) >> 12;
		engine.SetRatio(band + 2);

		if (SwitchVal() == Up) engine.SetScatter(true);
		else if (SwitchVal() == Middle) engine.SetScatter(false);
		// Down is momentary freeze: leave scatter as it was.

		// --- audio inputs --------------------------------------------------
		// Unpatched inputs are ignored rather than read as zero: a couple of
		// counts of ADC noise on the window would be an audible pitch wobble.
		int32_t window = 0;
		if (Connected(Audio2))
		{
			windowLp += (AudioIn2() * 256 - windowLp) >> 6;
			window = (windowLp >> 8) * 192;   // ~341 counts/V -> Q16 octaves
		}
		engine.SetWindow(window);

		int32_t pm = Connected(Audio1) ? AudioIn1() : 0;

		if (PulseIn1RisingEdge()) engine.Reset();

		// --- render --------------------------------------------------------
		int16_t c, s;
		if (engine.Process(pm, c, s)) trigger = kTriggerLen;
		AudioOut1(c);
		AudioOut2(s);

		// --- panel ---------------------------------------------------------
		CVOut1((int16_t)((int32_t)(engine.Zoom() >> 20) - 2048));
		int32_t lfo = engine.Lfo() >> 4;
		CVOut2((int16_t)(lfo > 2047 ? 2047 : (lfo < -2048 ? -2048 : lfo)));

		if (trigger > 0) trigger--;
		PulseOut1(trigger > 0);
		PulseOut2(lfo > 0);

		if (++ledTick >= 64)
		{
			ledTick = 0;
			for (int i = 0; i < 6; i++)
				LedBrightness(i, (uint16_t)(engine.PartialLevel(i) >> 3));
		}
	}

private:
	static constexpr int32_t kDeadzone = 100;
	static constexpr int32_t kMaxRate = 357913;   // 4 zoom steps per second
	static constexpr int32_t kHysteresis = 40;
	static constexpr int32_t kBandEdge[7] = {0, 683, 1366, 2048, 2731, 3414, 4096};
	static constexpr int kTriggerLen = 480;       // 10 ms

	weier::Engine engine;
	int band = 1;
	int32_t windowLp = 0;
	int trigger = 0;
	int ledTick = 0;
};

int main()
{
	set_sys_clock_khz(200000, true);

	static Weierstrass card;
	card.Run();
}
