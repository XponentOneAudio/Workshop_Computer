// ============================================================================
//  Zeds Dead — stereo morphing Z-plane multimode filter
//  for the Music Thing Modular Workshop System Computer
// ============================================================================
//
//  PROTOTYPE. See README.md for the full walkthrough of the maths, zplane.h
//  for the filter design itself, bank.h for presets and data formats, and
//  web/index.html for the browser editor. This file is the "glue" that
//  connects the maths to the knobs, jacks, LEDs, USB and flash memory.
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
//    Knob X          : MORPH — moves through the slots of the current bank.
//    Knob Y          : resonance.
//    Switch UP       : bank A (factory: LP → BP → HP → notch → twin peaks → quad notch)
//    Switch MIDDLE   : bank B (factory: vowels A → E → I → O → U → ER)
//    Switch DOWN     : (momentary) cycle the STEREO SPREAD mode:
//                        0 linked   – both channels identical
//                        1 cutoff   – left a half-octave lower, right higher
//                        2 morph    – left lags, right leads in the morph
//                        3 both     – both of the above
//    LEDs 0 / 1      : left / right output level.
//    LEDs 2–5        : morph position (a dot of light gliding across 4 LEDs);
//                      after pressing DOWN, briefly shows the spread mode;
//                      all four glow while the web editor is auditioning.
//    USB             : connect to a computer and open web/index.html in
//                      Chrome/Edge to design slots, audition them, see the
//                      CPU load and save banks to the card's flash memory.
//
//  ---------------------------------------------------------------------------
//  Who does what: the two cores
//  ---------------------------------------------------------------------------
//  The RP2040 has two ARM Cortex-M0+ cores and no floating-point hardware.
//
//   * CORE 0 runs ComputerCard's ProcessSample() 48 000 times a second. It
//     does only cheap INTEGER work: push each sample through the biquads,
//     apply gain and soft clipping.
//
//   * CORE 1 runs core1Loop() forever. Each time round it:
//       1. services USB (TinyUSB) and handles messages from the web editor,
//       2. reads the knobs and CV and does all the expensive FLOAT maths
//          (exp, cos, sqrt …) to design new biquad coefficients,
//       3. hands them to core 0, updates the LEDs, and saves to flash when
//          asked.
//
//  ---------------------------------------------------------------------------
//  The firmware runs entirely from RAM
//  ---------------------------------------------------------------------------
//  Normally an RP2040 program runs straight out of the external flash chip.
//  But while flash is being ERASED or WRITTEN it can't be read, so any code
//  running from flash at that moment would crash. We build with
//  "copy_to_ram" (see CMakeLists.txt): at power-up the whole program is
//  copied into RAM. Core 0 then never touches flash, so audio carries on
//  uninterrupted while core 1 saves your banks. (Same approach as the
//  Resonator card, 21_resonator.)
// ============================================================================

#include "ComputerCard.h"
#include "pico/multicore.h"
#include "hardware/sync.h"     // __dmb(), save_and_disable_interrupts()
#include "hardware/flash.h"
#include "hardware/clocks.h"
#include "pico/time.h"
#include "tusb.h"
#include "zplane.h"
#include "bank.h"

#include <cmath>
#include <cstring>

using namespace zp;

// ----------------------------------------------------------------------------
//  USB-MIDI SysEx protocol with the web editor
// ----------------------------------------------------------------------------
//
//  Every message is F0 7D <type> <data...> F7. 0x7D is the MIDI manufacturer
//  ID reserved for "non-commercial / prototyping" use. All data bytes are
//  0..127. The editor ASKS and the card ANSWERS; the card never sends
//  anything unprompted, so it can't clog up the USB link when no editor is
//  listening. (Message bytes from the card have the top "reply" bit 0x40 set.)
//
//    editor → card                         card → editor
//    0x01 HELLO                            0x41 INFO  version, limits, clock
//    0x02 GET_SLOT  bank slot              0x42 SLOT  bank slot numSlots <slot>
//    0x03 SET_SLOT  bank slot <slot>       0x43 ACK   bank slot
//    0x04 SET_NUMSLOTS bank n              0x44 ACK   bank n
//    0x05 PREVIEW   on [<slot>]            (no reply — sent many times a second)
//    0x06 SAVE                             0x46 SAVED ok
//    0x07 STATUS                           0x47 STATUS cpu, rates, late count
//                                          0x67 STATUS (part 2) knobs, levels ...
//    0x08 FACTORY   bank                   0x48 ACK   bank
//    0x09 REVERT                           0x49 ACK   ok (reloaded from flash)
//
//  <slot> is kSlotBytes bytes, encoded by encodeSlot() in bank.h.

namespace msg
{
	constexpr uint8_t kHello = 0x01, kGetSlot = 0x02, kSetSlot = 0x03, kSetNumSlots = 0x04,
	                  kPreview = 0x05, kSave = 0x06, kStatus = 0x07, kFactory = 0x08, kRevert = 0x09;
	constexpr uint8_t kReply = 0x40;
	constexpr uint8_t kManufacturerId = 0x7D;
}

constexpr uint8_t kVersionMajor = 0, kVersionMinor = 2, kVersionPatch = 0;

// Where in flash the banks live: the very last 4 KB sector of the 2 MB chip,
// well away from the program itself.
constexpr uint32_t kFlashOffset = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE;
// Flash is programmed in 256-byte pages, so round the image size up.
constexpr uint32_t kFlashProgramBytes = ((sizeof(FlashImage) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE;
static_assert(kFlashProgramBytes <= FLASH_SECTOR_SIZE, "banks must fit in one flash sector");

// Two 14-bit numbers per value is plenty for status readouts.
static inline void put14(uint8_t* p, uint32_t v)
{
	if (v > 16383) v = 16383;
	p[0] = uint8_t(v & 0x7F);
	p[1] = uint8_t((v >> 7) & 0x7F);
}

class ZedsDead : public ComputerCard
{
public:
	ZedsDead()
	{
		// Lets us ask "is a cable plugged into Audio In 2 / CV In 1 / ...?"
		EnableNormalisationProbe();

		// Load the saved banks, or fall back to the factory presets.
		flashLoaded = loadFromFlash();
		if (!flashLoaded)
			for (int b = 0; b < kNumBanks; ++b) factoryBank(b, banks[b]);

		// Start with all coefficients as "pass straight through". Core 1 will
		// replace them within a millisecond.
		for (int f = 0; f < 2; ++f)
		{
			frames[f].numSections = 0;
			for (int ch = 0; ch < 2; ++ch)
			{
				for (int s = 0; s < kMaxSections; ++s) frames[f].sec[ch][s] = fixedPassthrough();
				frames[f].makeup[ch] = 1 << kMakeupFracBits;
			}
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
		const FixedFrame& target = frames[readIndex];

		// ---- 0. Switch off the CV-output interrupt (once). ------------------
		// ComputerCard drives the two CV outputs from a second interrupt on
		// this core, ~96 000 times a second, to dither its PWM. This card
		// doesn't use the CV outputs, so that is pure overhead: turning it off
		// gives a few percent of the audio core back. (They just hold still.)
		if (!cvPwmIrqOff)
		{
			irq_set_enabled(PWM_IRQ_WRAP, false);
			cvPwmIrqOff = true;
		}

		// ---- 1. How many biquads to run? ---------------------------------
		// This depends on the bank (its largest slot). Running only what's
		// needed is what keeps the CPU load down for simple shapes. If more
		// sections are switched on, start them from a clean "wire" so the
		// smoothing below fades them in rather than starting from leftovers.
		const int n = target.numSections;
		if (n > numRunning)
		{
			for (int s = numRunning; s < n; ++s)
				for (int ch = 0; ch < 2; ++ch)
				{
					current.sec[ch][s] = fixedPassthrough();
					state[ch][s] = BiquadState();
				}
		}
		numRunning = n;

		// ---- 2. SMOOTH towards the newest coefficients. ------------------
		//
		// Core 1 delivers fresh coefficients a thousand or more times a
		// second. Jumping straight to them would cause small steps ("zipper
		// noise"), so we glide: move a fraction of the way from where we are
		// towards the latest target — a one-pole low-pass on every coefficient.
		//
		// To save time on this core we don't glide every biquad every sample.
		// The samples take turns in groups of four: on each sample only the
		// biquads whose number matches (section number mod 4) are moved, by
		// 1/8 of the way. Each coefficient is therefore updated every 4th
		// sample by 1/8 — on average the same glide as 1/32 every sample,
		// a time constant of about 32 samples ≈ 0.7 ms — for a quarter of
		// the work.
		//
		// Is it safe to blend coefficients like this? For a single biquad the
		// set of stable (a1, a2) values is a TRIANGLE:
		//        |a2| < 1   and   |a1| < 1 + a2
		// A triangle is CONVEX: any weighted average of two points inside it
		// is also inside it. Our blend is exactly such a weighted average of
		// the old and new (both stable) coefficients, so every intermediate
		// filter is stable too. (Numerator coefficients can't affect
		// stability, so they can be blended freely.)
		smoothPhase = (smoothPhase + 1) & 3;
		for (int ch = 0; ch < 2; ++ch)
		{
			for (int s = smoothPhase; s < n; s += 4)
			{
				int32_t* cur = current.sec[ch][s].c;
				const int32_t* tgt = target.sec[ch][s].c;
				for (int k = 0; k < 5; ++k) cur[k] += (tgt[k] - cur[k]) >> 3;
			}
			current.makeup[ch] += (target.makeup[ch] - current.makeup[ch]) >> 5;
		}

		// ---- 3. Read, filter, write. --------------------------------------
		const int16_t inL = AudioIn1();
		const int16_t inR = Connected(Input::Audio2) ? AudioIn2() : inL;
		const int16_t outL = processChannel(0, inL, n);
		const int16_t outR = processChannel(1, inR, n);
		AudioOut1(outL);
		AudioOut2(outR);

		// ---- 4. Bookkeeping for the UI (read by core 1). -------------------
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
	// One channel: 12-bit in → n cascaded biquads → makeup gain → soft clip.
	int16_t __not_in_flash_func(processChannel)(int ch, int16_t in, int n)
	{
		// Scale the 12-bit input up to our 24-bit internal format (see zplane.h)
		int32_t x = int32_t(in) << kSampleShift;

		for (int s = 0; s < n; ++s)
			x = runBiquad(current.sec[ch][s], state[ch][s], x);

		// Makeup gain is Q16 (65536 = ×1).
		int64_t y = smull32(x, current.makeup[ch]) >> kMakeupFracBits;
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
	//  CORE 1 — USB, coefficient design, LEDs, flash
	// ========================================================================
	static void core1Entry() { instance->core1Loop(); }

	void core1Loop()
	{
		tusb_init();

		// Smoothed copies of the controls. The ADC readings jitter by a few
		// steps; smoothing them here stops that jitter wobbling the filter.
		float smCutoffOct = 0.0f, smMorph = 0.0f, smRes = 0.0f;
		float smSpreadOct = 0.0f, smSpreadMorph = 0.0f;
		bool first = true;

		// log2(18000 / 20): the number of octaves the Main knob covers.
		const float kKnobOctaves = log2f(18000.0f / 20.0f);

		uint32_t rateWindowStart = time_us_32();
		uint32_t designsInWindow = 0;

		while (true)
		{
			// ---- 1. USB ------------------------------------------------------
			tud_task();
			while (tud_midi_available())
			{
				uint8_t rx[64];
				const uint32_t got = tud_midi_stream_read(rx, sizeof(rx));
				parseMidiBytes(rx, got);
			}
			if (saveRequested)
			{
				saveRequested = false;
				const bool ok = saveToFlash();
				uint8_t reply[2] = { uint8_t(msg::kSave | msg::kReply), uint8_t(ok ? 1 : 0) };
				sendSysEx(reply, sizeof(reply));
			}
			// If the editor goes quiet (closed tab, unplugged), stop auditioning.
			if (previewActive && time_us_32() - lastHostMessageUs > 3000000u) previewActive = false;

			// ---- 2. Read controls -------------------------------------------
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
			// Without valid EEPROM data it falls back to an ideal ±6 V range.
			//
			// When nothing is plugged in, ComputerCard sets the raw reading to
			// 0, but the calibrated value of a raw 0 is (−adcOffset × scale),
			// a small non-zero voltage. So we treat "unplugged" as exactly 0 V
			// ourselves; otherwise the cutoff would be slightly detuned with
			// no cable patched.
			const float cv1Volts = Connected(Input::CV1) ? float(CVIn1Millivolts()) * 0.001f : 0.0f;
			const float cv2Volts = Connected(Input::CV2) ? float(CVIn2Millivolts()) * 0.001f : 0.0f;

			// Switch: UP = bank A, MIDDLE = bank B. DOWN is momentary and used
			// for the spread mode, so while it's held we keep the last bank.
			const Switch sw = SwitchVal();
			if (sw == Switch::Up) activeBank = 0;
			else if (sw == Switch::Middle) activeBank = 1;
			const BankData& bank = banks[activeBank];

			// Cutoff is handled in OCTAVES above 20 Hz, so CV can simply be added.
			// Morph: knob X covers all the slots in use; 5 V on CV 2 does too.
			const float slotsSpan = float(bank.numSlots - 1);
			const float cutoffOct = knobMain * kKnobOctaves + cv1Volts;
			const float morph     = (knobX + cv2Volts * 0.2f) * slotsSpan;
			const float res       = knobY;

			// Stereo spread targets for the current mode.
			const int mode = spreadMode;
			const float spreadOct   = (mode & 1) ? 0.5f  : 0.0f;  // ± half an octave
			const float spreadMorph = (mode & 2) ? 0.35f : 0.0f;  // ± a third of a slot

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
			smSpreadOct   += 0.02f * (spreadOct - smSpreadOct);     // slow glide when
			smSpreadMorph += 0.02f * (spreadMorph - smSpreadMorph); // the mode changes

			// Remember for the status readout.
			statusCutoffHz = 20.0f * exp2f(smCutoffOct);
			statusMorph = smMorph;
			statusRes = smRes;

			// ---- 3. Design both channels --------------------------------------
			FixedFrame& next = frames[1 - readIndex];   // the buffer core 0 is NOT reading
			int numSections = 0;
			for (int ch = 0; ch < 2; ++ch)
			{
				const float sign = ch == 0 ? -1.0f : 1.0f;    // left down, right up
				const float cutoffHz = 20.0f * exp2f(smCutoffOct + sign * smSpreadOct);
				Design d;
				if (previewActive)
				{
					// Auditioning: play exactly the shape being edited. (Copy it
					// first — the USB handler may replace it on the next loop.)
					const SlotData p = previewSlot;
					designBetween(p, p, 0.0f, cutoffHz, smRes, p.numSections, d);
				}
				else
				{
					DesignParams p;
					p.cutoffHz  = cutoffHz;
					p.morph     = smMorph + sign * smSpreadMorph;
					p.resonance = smRes;
					designMorph(bank, p, d);   // clamps morph & cutoff internally
				}
				toFixed(d, next.sec[ch], next.makeup[ch]);
				numSections = d.numSections;
			}
			next.numSections = numSections;
			publish();
			++designsInWindow;

			// Designs per second, measured over half-second windows.
			const uint32_t now = time_us_32();
			if (now - rateWindowStart >= 500000u)
			{
				designRate = uint32_t(uint64_t(designsInWindow) * 1000000u / (now - rateWindowStart));
				designsInWindow = 0;
				rateWindowStart = now;
			}

			updateLeds(smMorph, slotsSpan);
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

	void updateLeds(float morph, float slotsSpan)
	{
		// Output level LEDs (top row). Level is 0..2048; double it for 0..4095.
		const int32_t lL = levelL * 2, lR = levelR * 2;
		LedBrightness(0, uint16_t(lL > 4095 ? 4095 : lL));
		LedBrightness(1, uint16_t(lR > 4095 ? 4095 : lR));

		if (spreadFlash > 0)
		{
			// Show spread mode 0..3 as a single lit LED among LEDs 2..5.
			for (int i = 0; i < 4; ++i) LedOn(2 + i, i == spreadMode);
			return;
		}
		if (previewActive)
		{
			for (int i = 0; i < 4; ++i) LedBrightness(2 + i, 1500);
			return;
		}

		// Morph position: the slots in use mapped onto LEDs 2..5 (positions
		// 0..3), each LED lit by how close the morph "dot" is to it.
		const float pos = slotsSpan > 0.0f ? morph * 3.0f / slotsSpan : 0.0f;
		for (int i = 0; i < 4; ++i)
		{
			float d = fabsf(pos - float(i));
			float b = d < 1.0f ? 1.0f - d : 0.0f;
			LedBrightness(2 + i, uint16_t(b * 4095.0f));
		}
	}

	// ------------------------------------------------------------------------
	//  USB-MIDI: sending and receiving SysEx (adapted from the ComputerCard
	//  web_interface example)
	// ------------------------------------------------------------------------

	// Write bytes to USB, waiting while TinyUSB's small buffer drains. Gives up
	// after 50 ms, so a stalled host can never freeze core 1 (which would stop
	// the filter responding to the knobs).
	bool midiWrite(const uint8_t* data, uint32_t size)
	{
		uint32_t sent = 0;
		const uint32_t start = time_us_32();
		while (sent < size)
		{
			const uint32_t n = tud_midi_stream_write(0, data + sent, size - sent);
			sent += n;
			if (!n)
			{
				if (time_us_32() - start > 50000u) return false;
				tud_task();
			}
		}
		return true;
	}

	void sendSysEx(const uint8_t* data, uint32_t size)
	{
		if (!tud_midi_mounted()) return;
		const uint8_t header[2] = { 0xF0, msg::kManufacturerId };
		const uint8_t footer[1] = { 0xF7 };
		if (midiWrite(header, 2) && midiWrite(data, size)) midiWrite(footer, 1);
	}

	// A small "state machine" that picks complete SysEx messages out of the
	// stream of MIDI bytes. A long message may arrive split over several USB
	// packets, so we collect bytes until we see the F7 terminator.
	void parseMidiBytes(const uint8_t* rx, uint32_t count)
	{
		for (uint32_t i = 0; i < count; ++i)
		{
			const uint8_t b = rx[i];
			if (!sysexActive)
			{
				if (b == 0xF0) { sysexActive = true; sysexLen = 0; }
				continue;
			}
			if (b == 0xF7)
			{
				// Complete: check it is ours (manufacturer 0x7D) and handle it.
				if (sysexLen >= 2 && sysexBuf[0] == msg::kManufacturerId)
					handleMessage(sysexBuf + 1, sysexLen - 1);
				sysexActive = false;
				continue;
			}
			if (b & 0x80) { sysexActive = (b == 0xF0); sysexLen = 0; continue; }  // broken message
			if (sysexLen < sizeof(sysexBuf)) sysexBuf[sysexLen++] = b;
		}
	}

	void handleMessage(const uint8_t* d, uint32_t len)
	{
		lastHostMessageUs = time_us_32();
		const uint8_t type = d[0];
		uint8_t out[8 + kSlotBytes];

		if (type == msg::kHello)
		{
			const uint32_t clk = clock_get_hz(clk_sys);
			const uint8_t m = uint8_t(msg::kHello | msg::kReply);
			out[0] = m;
			out[1] = kVersionMajor; out[2] = kVersionMinor; out[3] = kVersionPatch;
			out[4] = kMaxSections; out[5] = kNumSlots; out[6] = kNumBanks;
			put14(out + 7, clk / 1000000u);
			put14(out + 9, clk / 48000u);              // clock cycles per audio sample
			out[11] = InputsCalibrated() ? 1 : 0;
			out[12] = flashLoaded ? 1 : 0;
			sendSysEx(out, 13);
		}
		else if (type == msg::kGetSlot && len >= 3)
		{
			const int b = d[1], s = d[2];
			if (b >= kNumBanks || s >= kNumSlots) return;
			out[0] = uint8_t(msg::kGetSlot | msg::kReply);
			out[1] = uint8_t(b); out[2] = uint8_t(s); out[3] = banks[b].numSlots;
			encodeSlot(banks[b].slot[s], out + 4);
			sendSysEx(out, 4 + kSlotBytes);
		}
		else if (type == msg::kSetSlot && len >= 3 + kSlotBytes)
		{
			const int b = d[1], s = d[2];
			if (b >= kNumBanks || s >= kNumSlots) return;
			SlotData slot;
			if (!decodeSlot(d + 3, int(len) - 3, slot)) return;   // decodeSlot sanitizes
			banks[b].slot[s] = slot;
			out[0] = uint8_t(msg::kSetSlot | msg::kReply); out[1] = uint8_t(b); out[2] = uint8_t(s);
			sendSysEx(out, 3);
		}
		else if (type == msg::kSetNumSlots && len >= 3)
		{
			const int b = d[1];
			if (b >= kNumBanks) return;
			banks[b].numSlots = d[2];
			sanitize(banks[b]);
			out[0] = uint8_t(msg::kSetNumSlots | msg::kReply); out[1] = uint8_t(b); out[2] = banks[b].numSlots;
			sendSysEx(out, 3);
		}
		else if (type == msg::kPreview && len >= 2)
		{
			if (d[1] && len >= 2 + kSlotBytes)
			{
				SlotData slot;
				if (decodeSlot(d + 2, int(len) - 2, slot)) { previewSlot = slot; previewActive = true; }
			}
			else previewActive = false;
		}
		else if (type == msg::kSave)
		{
			saveRequested = true;   // done in the main loop, after this returns
		}
		else if (type == msg::kStatus)
		{
			// CPU load of core 0, in tenths of a percent (1000 = 100 %).
			const uint32_t perSample = clock_get_hz(clk_sys) / 48000u;
			const uint32_t avg  = ISRCyclesAverage() * 1000u / perSample;
			const uint32_t peak = ISRCyclesPeak() * 1000u / perSample;
			ResetISRPeak();
			out[0] = uint8_t(msg::kStatus | msg::kReply);
			put14(out + 1, avg);
			put14(out + 3, peak);
			put14(out + 5, designRate);
			out[7] = uint8_t(numRunning);
			put14(out + 8, ISRLateCount() & 0x3FFF);   // audio interrupts that started late
			sendSysEx(out, 10);   // first part: the CPU figures
			uint8_t more[16];
			more[0] = uint8_t(msg::kStatus | msg::kReply | 0x20);   // 0x67: second part
			more[1] = uint8_t(activeBank);
			put14(more + 2, uint32_t(statusMorph * 1000.0f + 0.5f));
			put14(more + 4, uint32_t(statusCutoffHz * 0.5f));
			more[6] = uint8_t(statusRes * 127.0f);
			more[7] = uint8_t(spreadMode);
			more[8] = previewActive ? 1 : 0;
			more[9]  = uint8_t((levelL >> 4) > 127 ? 127 : (levelL >> 4));
			more[10] = uint8_t((levelR >> 4) > 127 ? 127 : (levelR >> 4));
			more[11] = banks[activeBank].numSlots;
			more[12] = InputsCalibrated() ? 1 : 0;
			sendSysEx(more, 13);
		}
		else if (type == msg::kFactory && len >= 2)
		{
			const int b = d[1];
			if (b >= kNumBanks) return;
			factoryBank(b, banks[b]);
			out[0] = uint8_t(msg::kFactory | msg::kReply); out[1] = uint8_t(b);
			sendSysEx(out, 2);
		}
		else if (type == msg::kRevert)
		{
			const bool ok = loadFromFlash();
			out[0] = uint8_t(msg::kRevert | msg::kReply); out[1] = ok ? 1 : 0;
			sendSysEx(out, 2);
		}
	}

	// ------------------------------------------------------------------------
	//  Flash storage
	// ------------------------------------------------------------------------

	// Read the banks from flash. Flash is "memory-mapped": its contents appear
	// at address XIP_BASE onwards, so we can simply copy from there.
	bool loadFromFlash()
	{
		FlashImage img;
		std::memcpy(&img, reinterpret_cast<const void*>(XIP_BASE + kFlashOffset), sizeof(img));
		if (!imageValid(img)) return false;
		for (int b = 0; b < kNumBanks; ++b)
		{
			sanitize(img.banks[b]);
			banks[b] = img.banks[b];
		}
		return true;
	}

	// Erase + program the bank sector. Runs on core 1 with core 1's own
	// interrupts disabled. Core 0 keeps running audio throughout, safe because
	// the whole program runs from RAM (see the top of this file). Takes ~50 ms,
	// during which core 1 doesn't update the coefficients, so the filter just
	// holds still for a moment.
	bool saveToFlash()
	{
		static uint8_t buffer[kFlashProgramBytes] __attribute__((aligned(4)));
		std::memset(buffer, 0xFF, sizeof(buffer));
		FlashImage img;
		std::memset(&img, 0, sizeof(img));   // zero the padding too, so the CRC is repeatable
		img.magic = kFlashMagic;
		img.version = kFlashVersion;
		for (int b = 0; b < kNumBanks; ++b) img.banks[b] = banks[b];
		img.crc = imageCrc(img);
		std::memcpy(buffer, &img, sizeof(img));

		const uint32_t ints = save_and_disable_interrupts();
		flash_range_erase(kFlashOffset, FLASH_SECTOR_SIZE);
		flash_range_program(kFlashOffset, buffer, kFlashProgramBytes);
		restore_interrupts(ints);

		// Read it back and check: this is what the editor's "Saved ✓" shows.
		return std::memcmp(reinterpret_cast<const void*>(XIP_BASE + kFlashOffset), buffer, kFlashProgramBytes) == 0;
	}

	// ---- State ------------------------------------------------------------

	// Shared between cores (volatile: "always really read/write memory,
	// another core might change this behind your back").
	FixedFrame frames[2];                  // double buffer, core 1 → core 0
	volatile int readIndex = 0;
	volatile int spreadMode = 0;           // core 0 writes, core 1 reads
	volatile int32_t spreadFlash = 0;
	volatile int32_t levelL = 0, levelR = 0;
	volatile int numRunning = 0;           // sections core 0 is running

	// Core 0 only.
	FixedFrame current;                    // smoothed coefficients in use
	BiquadState state[2][kMaxSections];    // filter memories [channel][section]
	int smoothPhase = 0;                   // which quarter of the biquads glide this sample
	bool cvPwmIrqOff = false;

	// Core 1 only.
	BankData banks[kNumBanks];
	SlotData previewSlot;
	bool previewActive = false;
	bool saveRequested = false;
	bool flashLoaded = false;
	int activeBank = 0;
	uint32_t lastHostMessageUs = 0;
	uint32_t designRate = 0;
	float statusCutoffHz = 0.0f, statusMorph = 0.0f, statusRes = 0.0f;
	bool sysexActive = false;
	uint32_t sysexLen = 0;
	uint8_t sysexBuf[512];

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
