// Dual Borg — a model of a dual "Borg" style 12 dB filter / VCA
//
// Inspired by the Malekko / Richter Dual Borg: two deliberately *different*
// 2-pole low-pass filters side by side, drawing on the Buchla 292 low-pass gate
// and the Korg MS-20 filter. Each side can run as a resonant filter or, in VCA
// mode, as a combined VCA + filter for short percussive sounds.
//
//   Borg I  (left column)  slow, mellow, vactrol-ish response; soft saturation;
//                          resonance sings but never self-oscillates.
//   Borg II (right column) fast, bright, MS-20 style; the resonance loop clips
//                          hard, so it shrieks and will self-oscillate.
//
// I/O (left column of jacks = Borg I, right column = Borg II):
//   Audio In 1/2     filter input. Audio In 2 is normalled to Audio In 1, so a
//                    single source feeds both filters.
//   Audio Out 1/2    filter output
//   CV In 1/2        cutoff CV (~1V/oct, uncalibrated). In VCA mode it also
//                    opens the gate.
//   Pulse In 1/2     ping: a decaying envelope that sweeps the cutoff up
//                    (filter modes) or strikes the gate open (VCA mode).
//                    With no audio patched, a ping also injects a short
//                    noise burst, so the filters can be played as drums.
//   CV Out 1/2       each filter's (slewed) cutoff control, 0..+6V
//   Knob X / Knob Y  cutoff for Borg I / Borg II
//   Main knob        Switch Up / Down: resonance for both filters
//                    Switch Middle:    input drive for both filters
//   Switch           Down   = VCA mode (low-pass gate: level + cutoff together)
//                    Middle = Filter, resonance off (Main = drive)
//                    Up     = Filter, resonance on  (Main = resonance)
//   LEDs             top: output level, middle: cutoff, bottom: ping envelope
//
// All audio processing is integer; float is only used at a decimated control
// rate to turn cutoff/resonance into filter coefficients.

#include "ComputerCard.h"
#include <cmath>

static int32_t __not_in_flash_func(rnd12)()
{
	static uint32_t lcg_seed = 4321;
	lcg_seed = 1664525 * lcg_seed + 1013904223;
	return lcg_seed >> 20; // 0..4095
}

// Cubic soft clipper in 32-bit arithmetic. Linear near zero, smoothly reaching
// +/-L (L = 2^K) at |x| = 1.5L and flat beyond. Requires K >= 15.
template <unsigned K>
static inline int32_t __not_in_flash_func(softClip)(int32_t x)
{
	constexpr unsigned S = K - 15;
	constexpr int32_t LIM = 3 << (K - 1);  // 1.5L
	bool neg = x < 0;
	int32_t a = neg ? -x : x;
	if (a > LIM) a = LIM;
	// Only the cubic correction is computed at reduced precision, so small
	// signals pass through exactly.
	uint32_t s = uint32_t(a) >> S;         // L -> 32768
	uint32_t s2 = (s * s) >> 15;
	uint32_t s3 = (s2 * s) >> 15;
	int32_t y = a - (int32_t((s3 * 4855) >> 15) << S); // a - (4/27) a^3 / L^2
	return neg ? -y : y;
}

// Zero-delay-feedback state-variable low-pass (Cytomic/Simper form) with a
// saturating band-pass state. Signals are audio << 8 (full scale = 2^19);
// coefficients are Q24. SAT sets where the resonant state starts to clip:
// lower = grittier, more MS-20 "shriek".
template <unsigned SAT>
class NonlinearSVF
{
	int32_t ic1eq = 0, ic2eq = 0;
	int32_t a1 = 0, a2 = 0, a3 = 0;
public:
	void SetCoeffs(float g, float k)
	{
		float fa1 = 1.0f / (1.0f + g * (g + k));
		a1 = int32_t(fa1 * 16777216.0f);
		a2 = int32_t(fa1 * g * 16777216.0f);
		a3 = int32_t(fa1 * g * g * 16777216.0f);
	}

	int32_t __not_in_flash_func(operator())(int32_t x)
	{
		int32_t v3 = x - ic2eq;
		int32_t v1 = int32_t(((int64_t)a1 * ic1eq + (int64_t)a2 * v3 + (1 << 23)) >> 24);
		int32_t v2 = ic2eq + int32_t(((int64_t)a2 * ic1eq + (int64_t)a3 * v3 + (1 << 23)) >> 24);
		ic1eq = softClip<SAT>(2 * v1 - ic1eq);
		ic2eq = 2 * v2 - ic2eq;
		if (ic2eq > (1 << 22)) ic2eq = 1 << 22;
		if (ic2eq < -(1 << 22)) ic2eq = -(1 << 22);
		return v2;
	}
};

class DualBorg : public ComputerCard
{
	static constexpr int CTRL_DECIM = 8;            // control rate = 6kHz
	static constexpr float CTRL_RATE = 48000.0f / CTRL_DECIM;

	// Cutoff is handled in "steps" of 1/96 octave above F_MIN
	static constexpr int STEPS_PER_OCT = 96;
	static constexpr int NUM_STEPS = 1024;
	static constexpr float F_MIN = 25.0f;

	static constexpr int PING_OCTAVES = 4;          // ping sweep in filter modes
	static constexpr int BURST_LEN = 240;           // 5ms self-ping noise

	// Per-filter character
	struct Character
	{
		float fMax;          // top of the cutoff knob
		float attackMs;      // control response, opening
		float releaseMs;     // control response, closing
		float pingMs;        // ping envelope decay time constant
		float kMinFilter;    // 1/Q at full resonance (switch Up)
		float kMinVCA;       // 1/Q at full resonance (switch Down)
		float kNoRes;        // 1/Q with resonance switched off (Middle)
	};
	static constexpr Character chars[2] = {
		// Borg I: slow, mellow, never self-oscillates
		{ 9000.0f,  5.0f, 80.0f, 350.0f,  0.17f, 0.45f, 1.4f },
		// Borg II: fast, bright, screams
		{ 16000.0f, 0.5f, 12.0f, 120.0f, -0.03f, 0.25f, 1.1f },
	};

	float gTable[NUM_STEPS + 1]; // tan(pi f / fs) per cutoff step

	NonlinearSVF<21> borg1;      // clips at 4x full scale: soft, round
	NonlinearSVF<19> borg2;      // clips at full scale: gritty, shrieking

	int32_t maxStep[2];          // knob range, in steps
	int32_t attackQ16[2], releaseQ16[2], pingDecayQ16[2];
	int32_t smooth[2];           // slewed cutoff, steps << 16
	int32_t ping[2];             // ping envelope, Q16 (65536 = 1)
	bool pinged[2];
	int burst[2];
	int32_t level[2];            // output level follower for LEDs
	int32_t gainQ15[2];          // VCA gain
	int32_t driveQ8;
	int ctrl;

	static int32_t onePoleQ16(float ms)
	{
		return int32_t(65536.0f * (1.0f - expf(-1000.0f / (ms * CTRL_RATE))));
	}

public:
	DualBorg()
	{
		for (int s = 0; s <= NUM_STEPS; s++)
		{
			float f = F_MIN * exp2f(s / float(STEPS_PER_OCT));
			if (f > 20000.0f) f = 20000.0f;
			gTable[s] = tanf(float(M_PI) * f / 48000.0f);
		}
		for (int i = 0; i < 2; i++)
		{
			maxStep[i] = int32_t(STEPS_PER_OCT * log2f(chars[i].fMax / F_MIN));
			attackQ16[i] = onePoleQ16(chars[i].attackMs);
			releaseQ16[i] = onePoleQ16(chars[i].releaseMs);
			pingDecayQ16[i] = 65536 - onePoleQ16(chars[i].pingMs);
			smooth[i] = 0;
			ping[i] = 0;
			pinged[i] = false;
			burst[i] = 0;
			level[i] = 0;
			gainQ15[i] = 0;
		}
		driveQ8 = 256;
		ctrl = 0;
		borg1.SetCoeffs(gTable[0], 1.4f);
		borg2.SetCoeffs(gTable[0], 1.1f);
	}

	// Recompute cutoff/resonance/gain for both filters (control rate)
	void UpdateControls(Switch mode)
	{
		float r = KnobVal(Main) * (1.0f / 4095.0f);
		float inv = (1.0f - r) * (1.0f - r);
		driveQ8 = (mode == Middle) ? 256 + int32_t(r * 7.0f * 256.0f) : 256;

		for (int i = 0; i < 2; i++)
		{
			const Character &c = chars[i];

			// Ping envelope
			if (pinged[i]) { ping[i] = 65536; pinged[i] = false; }
			else ping[i] = int32_t(((int64_t)ping[i] * pingDecayQ16[i]) >> 16);

			// Target cutoff in steps: knob + CV (~1V/oct) + ping
			int32_t knob = (KnobVal(i ? Y : X) * maxStep[i]) >> 12;
			int32_t cv = (CVIn(i) * 288) >> 10;   // ~341 counts/V -> 96 steps/V
			int32_t pingAmt = (mode == Down) ? maxStep[i] : PING_OCTAVES * STEPS_PER_OCT;
			int32_t target = knob + cv + ((ping[i] * pingAmt) >> 16);
			if (target < 0) target = 0;
			if (target > maxStep[i]) target = maxStep[i];

			// Asymmetric slew: the "response time" of each Borg
			int32_t t16 = target << 16;
			int32_t coef = (t16 > smooth[i]) ? attackQ16[i] : releaseQ16[i];
			smooth[i] += int32_t(((int64_t)(t16 - smooth[i]) * coef) >> 16);

			// Interpolated cutoff coefficient
			int32_t idx = smooth[i] >> 16;
			float frac = (smooth[i] & 0xFFFF) * (1.0f / 65536.0f);
			float g = gTable[idx] + (gTable[idx + 1] - gTable[idx]) * frac;

			float k;
			if (mode == Middle) k = c.kNoRes;
			else
			{
				float kMin = (mode == Up) ? c.kMinFilter : c.kMinVCA;
				k = kMin + (1.6f - kMin) * inv;
			}
			if (i == 0) borg1.SetCoeffs(g, k);
			else borg2.SetCoeffs(g, k);

			// VCA mode: level follows the same control, squared for a natural taper
			if (mode == Down)
			{
				int32_t open = (smooth[i] >> 1) / maxStep[i]; // 0..32768
				gainQ15[i] = (open * open) >> 15;
			}
			else gainQ15[i] = 32768;
		}
	}

	virtual void __not_in_flash_func(ProcessSample)()
	{
		Switch mode = SwitchVal();

		for (int i = 0; i < 2; i++)
		{
			if (PulseInRisingEdge(i))
			{
				pinged[i] = true;
				if (Disconnected(Audio1) && Disconnected(i ? Audio2 : Audio1))
					burst[i] = BURST_LEN;
			}
		}

		if (--ctrl <= 0)
		{
			ctrl = CTRL_DECIM;
			UpdateControls(mode);
		}

		// Audio In 2 is normalled to Audio In 1
		int32_t in1 = AudioIn1();
		int32_t in2 = Connected(Audio2) ? AudioIn2() : in1;
		int32_t ins[2] = { in1, in2 };

		for (int i = 0; i < 2; i++)
		{
			int32_t x = ins[i];
			if (burst[i] > 0) { burst[i]--; x += (rnd12() - 2048) >> 1; }

			// To internal scale, with drive and a touch of dither so a silent,
			// fully resonant Borg II can still start to self-oscillate.
			x *= driveQ8;                     // audio << 8, times drive
			x += (rnd12() - 2048) >> 6;

			int32_t y;
			if (i == 0) y = borg1(softClip<20>(x));
			else y = borg2(softClip<19>(x));

			y = int32_t(((int64_t)y * gainQ15[i]) >> 15);
			int32_t out = softClip<19>(y) >> 8;
			if (out > 2047) out = 2047;
			if (out < -2048) out = -2048;
			AudioOut(i, out);

			// Cutoff control out, 0..+6V
			int32_t cvOut = (smooth[i] >> 5) / maxStep[i];
			CVOut(i, int16_t(cvOut > 2047 ? 2047 : cvOut));

			int32_t a = out < 0 ? -out : out;
			level[i] += ((a << 4) - level[i]) >> 9;
		}

		if (ctrl == CTRL_DECIM)
		{
			for (int i = 0; i < 2; i++)
			{
				int32_t lv = level[i] >> 2;
				if (lv > 4095) lv = 4095;
				LedBrightness(0 + i, lv);
				int32_t cut = (smooth[i] >> 4) / maxStep[i];
				int32_t env = ping[i] >> 4;
				LedBrightness(2 + i, cut > 4095 ? 4095 : cut);
				LedBrightness(4 + i, env > 4095 ? 4095 : env);
			}
		}
	}
};

// Hardware entry point; skipped by the host simulator in sim/.
#ifndef COMPUTERCARD_HOST_SIM
int main()
{
	set_sys_clock_khz(192000, true);

	static DualBorg borg; // static: ~4KB of tables, too big for the stack
	borg.EnableNormalisationProbe();
	borg.Run();
}
#endif
