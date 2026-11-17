// ============================================================================
//  Zeds Dead — stereo morphing Z-plane multimode filter
//  for the Music Thing Modular Workshop System Computer
// ============================================================================
//
//  PROTOTYPE. See README.md for the full walkthrough of the maths, and
//  zplane.h for the filter design itself. This file is the "glue" that
//  connects the maths to the knobs, jacks and LEDs.
//
//  ---------------------------------------------------------------------------
//  Panel
//  ---------------------------------------------------------------------------
//    Audio In 1 / 2  : left / right input. If only In 1 is patched, it feeds
//                      both channels (mono in → stereo out).
//    Audio Out 1 / 2 : left / right filtered output.
//    CV In 1         : cutoff, 1 V/octave (uses the module's EEPROM
//                      calibration), added to the Main knob.
//    CV In 2         : morph, added to knob X (5 V sweeps the whole bank).
//    Main knob       : cutoff frequency, 20 Hz … 18 kHz (exponential).
//    Knob X          : MORPH — moves through the 5 shapes of the current bank.
//    Knob Y          : resonance.
//    Switch UP       : "Classic" bank: LP → BP → HP → Notch → Twin peaks.
//    Switch MIDDLE   : "Vowel" bank:   A  → E  → I  → O  → U.
//    Switch DOWN     : (momentary) cycle the STEREO SPREAD mode:
//                        0 linked   – both channels identical
//                        1 cutoff   – left a half-octave lower, right higher
//                        2 morph    – left lags, right leads in the morph
//                        3 both     – both of the above
//    LEDs 0 / 1      : left / right output level.
//    LEDs 2–5        : morph position (a dot of light gliding across 4 LEDs);
//                      after pressing DOWN, briefly shows the spread mode.
//
//  ---------------------------------------------------------------------------
//  Why two cores?
//  ---------------------------------------------------------------------------
//  The RP2040 has two ARM Cortex-M0+ cores and no floating-point hardware.
//
//   * CORE 0 runs ComputerCard's ProcessSample() 48 000 times a second. It
//     does only cheap INTEGER work: push each sample through 3 biquads per
//     channel, apply gain and soft clipping.
//
//   * CORE 1 runs designLoop() forever, as fast as it can (a few thousand
//     times a second). It reads the knobs, does all the expensive FLOAT
//     maths (exp, cos, sqrt …) to work out new biquad coefficients, converts
//     them to integers and hands them to core 0.
//
//  Handing data between cores safely is the subtle part — see publish() and
//  the start of ProcessSample().
// ============================================================================

#include "ComputerCard.h"
#include "pico/multicore.h"
#include "hardware/sync.h"     // __dmb(): memory barrier
#include "zplane.h"

#include <cmath>
#include <cstring>

using namespace zp;

class ZedsDead : public ComputerCard
{
public:
	ZedsDead()
	{
		// Lets us ask "is a cable plugged into Audio In 2?" (for mono→stereo).
		EnableNormalisationProbe();

		// Start with all coefficients as "pass straight through": b0 = 1,
		// everything else 0. Core 1 will replace these within a millisecond.
		std::memset(frames, 0, sizeof(frames));
		for (int f = 0; f < 2; ++f)
			for (int ch = 0; ch < 2; ++ch)
			{
				for (int s = 0; s < kSections; ++s) frames[f].sec[ch][s].c[0] = kCoeffOne;
				frames[f].makeup[ch] = 1 << kMakeupFracBits;
			}
		current = frames[0];
	}

	// Call from main() before Run(). (Same pattern as the ComputerCard
	// "second_core" example: core 1 needs a pointer to this object.)
	void StartSecondCore()
	{
		instance = this;
		multicore_launch_core1(core1Entry);
	}

protected:
	// ========================================================================
	//  CORE 0 — the 48 kHz audio callback
	// ========================================================================
	void __not_in_flash_func(ProcessSample)() override
	{
		// ---- 1. Pick up the newest coefficients from core 1, and SMOOTH. ----
		//
		// Core 1 delivers fresh coefficients a few thousand times a second.
		// Jumping straight to them would cause small steps ("zipper noise"),
		// so each sample we move 1/32 of the way from where we are towards the
		// latest target: a one-pole low-pass on every coefficient, with a time
		// constant of 32 samples ≈ 0.7 ms.
		//
		// Is it safe to blend coefficients like this? For a single biquad the
		// set of stable (a1, a2) values is a TRIANGLE:
		//        |a2| < 1   and   |a1| < 1 + a2
		// A triangle is CONVEX: any weighted average of two points inside it
		// is also inside it. Our blend is exactly such a weighted average of
		// the old and new (both stable) coefficients, so every intermediate
		// filter is stable too. (Numerator coefficients can't affect
		// stability, so they can be blended freely.)
		const FixedFrame& target = frames[readIndex];
		for (int ch = 0; ch < 2; ++ch)
		{
			for (int s = 0; s < kSections; ++s)
			{
				int32_t* cur = current.sec[ch][s].c;
				const int32_t* tgt = target.sec[ch][s].c;
				for (int k = 0; k < 5; ++k) cur[k] += (tgt[k] - cur[k]) >> 5;
			}
			current.makeup[ch] += (target.makeup[ch] - current.makeup[ch]) >> 5;
		}

		// ---- 2. Read the audio inputs. ------------------------------------
		const int16_t inL = AudioIn1();
		const int16_t inR = Connected(Input::Audio2) ? AudioIn2() : inL;

		// ---- 3. Filter each channel. --------------------------------------
		const int16_t outL = processChannel(0, inL);
		const int16_t outR = processChannel(1, inR);
		AudioOut1(outL);
		AudioOut2(outR);

		// ---- 4. Bookkeeping for the UI (read by core 1). -------------------
		// Simple peak meters with slow decay.
		trackLevel(levelL, outL);
		trackLevel(levelR, outR);

		// Switch DOWN is spring-loaded: each press steps the spread mode.
		if (SwitchChanged() && SwitchVal() == Switch::Down)
		{
			spreadMode = (spreadMode + 1) & 3;
			spreadFlash = 48000;   // show the new mode on the LEDs for ~1 s
		}
		if (spreadFlash > 0) spreadFlash = spreadFlash - 1;
	}

private:
	// One channel: 12-bit in → 3 cascaded biquads → makeup gain → soft clip.
	int16_t __not_in_flash_func(processChannel)(int ch, int16_t in)
	{
		// Scale the 12-bit input up to our 24-bit internal format (see zplane.h)
		int32_t x = int32_t(in) << kSampleShift;

		for (int s = 0; s < kSections; ++s)
			x = runBiquad(current.sec[ch][s], state[ch][s], x);

		// Makeup gain is Q16 (65536 = ×1).
		int64_t y = (int64_t(x) * current.makeup[ch]) >> kMakeupFracBits;
		const int64_t lim = int64_t(1) << 30;
		if (y >  lim) y =  lim;
		if (y < -lim) y = -lim;

		// Soft clip, then back down to 12 bits for the DAC.
		int32_t out = softClip(int32_t(y)) >> kSampleShift;
		if (out >  2047) out =  2047;
		if (out < -2048) out = -2048;
		return int16_t(out);
	}

	static void __not_in_flash_func(trackLevel)(volatile int32_t& level, int16_t v)
	{
		int32_t a = v < 0 ? -v : v;
		int32_t l = level;
		if (a > l) l = a;               // instant attack
		else       l -= (l >> 11) + 1;  // slow exponential release (~40 ms)
		if (l < 0) l = 0;
		level = l;
	}

	// ========================================================================
	//  CORE 1 — the coefficient designer
	// ========================================================================
	static void core1Entry() { instance->designLoop(); }

	void designLoop()
	{
		// Smoothed copies of the controls. The ADC readings jitter by a few
		// steps; smoothing them here stops that jitter wobbling the filter.
		float smCutoffOct = 0.0f, smMorph = 0.0f, smRes = 0.0f;
		bool first = true;
		int bank = 0;
		float smSpreadOct = 0.0f, smSpreadMorph = 0.0f;

		// log2(18000 / 20): the number of octaves the Main knob covers.
		const float kKnobOctaves = log2f(18000.0f / 20.0f);

		while (true)
		{
			// ---- Read controls -----------------------------------------------
			// KnobVal: 0..4095.
			const float knobMain = float(KnobVal(Knob::Main)) / 4095.0f;
			const float knobX    = float(KnobVal(Knob::X)) / 4095.0f;
			const float knobY    = float(KnobVal(Knob::Y)) / 4095.0f;

			// CV inputs, in CALIBRATED millivolts.
			//
			// Each Workshop Computer is calibrated at the factory (or with the
			// calibration mode of the Simple MIDI card), and the results are
			// stored in a small EEPROM chip on the module. For each CV input
			// that's two numbers:
			//   adcOffset   – the raw ADC reading when 0 V is applied
			//   mvPerAdcQ16 – millivolts per ADC step (Q16 fixed point)
			// ComputerCard reads them at power-up, and CVInMillivolts() applies
			//   mV = (raw − adcOffset) × mvPerAdcQ16 / 65536
			// Without valid EEPROM data it falls back to an ideal ±6 V range,
			// i.e. the same "roughly right" scaling as the raw CVIn().
			//
			// When nothing is plugged in, ComputerCard sets the raw reading to
			// 0, but the calibrated value of a raw 0 is (−adcOffset × scale),
			// a small non-zero voltage. So we treat "unplugged" as exactly 0 V
			// ourselves; otherwise the cutoff would be slightly detuned with
			// no cable patched.
			const float cv1Volts = Connected(Input::CV1) ? float(CVIn1Millivolts()) * 0.001f : 0.0f;
			const float cv2Volts = Connected(Input::CV2) ? float(CVIn2Millivolts()) * 0.001f : 0.0f;

			// Morph CV: 5 V sweeps one whole bank (knob X's full range is 1.0).
			const float cv2 = cv2Volts * 0.2f;

			// Cutoff is handled in OCTAVES above 20 Hz, so CV can simply be added.
			const float cutoffOct = knobMain * kKnobOctaves + cv1Volts;
			const float morph     = (knobX + cv2) * float(kFramesPerBank - 1);
			const float res       = knobY;

			// Switch: UP = classic, MIDDLE = vowels. DOWN is momentary and used
			// for the spread mode, so while it's held we keep the last bank.
			const Switch sw = SwitchVal();
			if (sw == Switch::Up) bank = 0;
			else if (sw == Switch::Middle) bank = 1;

			// Stereo spread targets for the current mode.
			const int mode = spreadMode;
			const float spreadOct   = (mode & 1) ? 0.5f  : 0.0f;  // ± half an octave
			const float spreadMorph = (mode & 2) ? 0.35f : 0.0f;  // ± a third of a frame

			// One-pole smoothing (per loop, not per sample; the loop rate
			// varies a little, which is fine for this purpose).
			if (first)
			{
				smCutoffOct = cutoffOct; smMorph = morph; smRes = res;
				first = false;
			}
			smCutoffOct   += 0.2f * (cutoffOct - smCutoffOct);
			smMorph       += 0.2f * (morph - smMorph);
			smRes         += 0.2f * (res - smRes);
			smSpreadOct   += 0.02f * (spreadOct - smSpreadOct);   // slow glide when
			smSpreadMorph += 0.02f * (spreadMorph - smSpreadMorph); // the mode changes

			// ---- Design both channels ---------------------------------------
			FixedFrame& next = frames[1 - readIndex];   // the buffer core 0 is NOT reading
			for (int ch = 0; ch < 2; ++ch)
			{
				const float sign = ch == 0 ? -1.0f : 1.0f;    // left down, right up
				DesignParams p;
				p.cutoffHz  = 20.0f * exp2f(smCutoffOct + sign * smSpreadOct);
				p.morph     = smMorph + sign * smSpreadMorph;
				p.resonance = smRes;
				p.bank      = bank;
				Design d;
				designCascade(p, d);   // clamps morph & cutoff internally
				toFixed(d, next.sec[ch], next.makeup[ch]);
			}
			publish();

			updateLeds(smMorph);
		}
	}

	// Hand a finished set of coefficients to core 0.
	//
	// DOUBLE BUFFERING: there are two FixedFrame buffers. Core 0 only ever
	// reads frames[readIndex]; core 1 only ever writes the other one. When
	// core 1 has completely finished writing, it flips readIndex. Core 0 then
	// sees a complete, consistent set — never half old / half new (which
	// could momentarily pair an a1 from one filter with an a2 from another,
	// and that pair might not be stable!).
	//
	// __dmb() is a "data memory barrier": it makes sure all the coefficient
	// writes have really landed in memory before the flip becomes visible to
	// the other core.
	//
	// (Strictly, core 0 could still be part-way through reading the old buffer
	// when core 1 starts filling it again. But core 0 reads all of it within a
	// microsecond or so, while core 1 takes hundreds of microseconds to design
	// the next set, so in practice that can't happen.)
	void publish()
	{
		__dmb();
		readIndex = 1 - readIndex;
		__dmb();
	}

	void updateLeds(float morph)
	{
		// Output level LEDs (top row).
		// Level is 0..2048; double it to the LED's 0..4095 range.
		const int32_t lL = levelL * 2, lR = levelR * 2;
		LedBrightness(0, uint16_t(lL > 4095 ? 4095 : lL));
		LedBrightness(1, uint16_t(lR > 4095 ? 4095 : lR));

		if (spreadFlash > 0)
		{
			// Show spread mode 0..3 as a single lit LED among LEDs 2..5.
			for (int i = 0; i < 4; ++i) LedOn(2 + i, i == spreadMode);
			return;
		}

		// Morph position: frames 0..4 mapped onto LEDs 2..5 (positions 0..3),
		// each LED lit by how close the morph "dot" is to it.
		float pos = morph * 3.0f / float(kFramesPerBank - 1);
		for (int i = 0; i < 4; ++i)
		{
			float d = fabsf(pos - float(i));
			float b = d < 1.0f ? 1.0f - d : 0.0f;
			LedBrightness(2 + i, uint16_t(b * 4095.0f));
		}
	}

	// ---- State ------------------------------------------------------------

	// Shared between cores (hence volatile: "always really read/write memory,
	// another core might change this behind your back").
	FixedFrame frames[2];                  // double buffer, core 1 → core 0
	volatile int readIndex = 0;
	volatile int spreadMode = 0;           // core 0 writes, core 1 reads
	volatile int32_t spreadFlash = 0;
	volatile int32_t levelL = 0, levelR = 0;

	// Core 0 only.
	FixedFrame current;                    // smoothed coefficients in use
	BiquadState state[2][kSections];       // filter memories [channel][section]

	static ZedsDead* instance;
};

ZedsDead* ZedsDead::instance = nullptr;

int main()
{
	// 192 MHz (up from the default 125 MHz) gives the audio core 4000 clock
	// cycles per sample. This speed is widely used on Computer cards.
	set_sys_clock_khz(192000, true);

	static ZedsDead card;   // static: keep this big object off the small stack
	card.StartSecondCore();
	card.Run();
}
