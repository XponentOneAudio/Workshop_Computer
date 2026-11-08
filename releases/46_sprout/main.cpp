#include "ComputerCard.h"
#include "pico/multicore.h"
#include "hardware/sync.h"
#include "hardware/flash.h"
#include "tusb.h"
#include <cmath>
#include <cstring>

/*
Sprout — plant biodata sonification card
========================================

A MIDI Sprout style card for the Workshop Computer. It plays notes from tiny
changes in a plant's electrical resistance, as pitch CV + gate, a built-in
sine voice and USB MIDI.

Measuring the plant
-------------------
  Audio Out 1 ──► electrode A ── [PLANT] ── electrode B ──► amplifier in
  Audio In 1  ◄────────────────────────────────────────── amplifier out

The plant and the amplifier's input resistance form a voltage divider, so the
level of the returned tone follows the plant's resistance. The level is
measured with lock-in (synchronous) detection: the input is multiplied by the
reference sine and a 90° copy and summed over a 20 ms window holding a whole
number of tone cycles. DC, drift and hum average out, and averaging 960
samples gives far finer resolution than one ADC reading.

Making notes
------------
The last 10 readings (200 ms) must pass two tests:

    shape (from the MIDI Sprout):  max - min  >  standard deviation x 2.6
    size  (added here):            max - min  >  typical wobble x sensitivity

The shape test picks out sudden jumps, but on its own it also fires on plain
measurement noise. The size test fixes that: "typical wobble" is a slow
(~20 s) average of the change between readings, so it adapts to each plant
and electrode setup, and the Main knob sets how many times bigger than that a
jump must be (20x CCW, few notes ... 2x CW, many notes).

When both pass, a note is chosen from the current scale using the fine detail
of the reading (like the Sprout's "average % 127"), the note length from the
size of the jump, and the velocity from how far it beat the size test.

Modes
-----
Play (normal power-on)
Calibrate (hold switch DOWN while powering on or pressing reset):
    Main = tone level, X = tone frequency, Y = zoom of the change signal.
    Tap DOWN = re-zero. Hold DOWN 2 s = save to flash and start playing.

Cores
-----
Core 0 (ProcessSample, 48 kHz): tone, lock-in sums, voice, gate, jacks.
Core 1: USB MIDI, and every 20 ms: level, detector, notes, LEDs, switch.
*/

class Sprout : public ComputerCard
{
public:
	enum class Mode : uint32_t { Starting, Calibrate, Play };

private:
	// ---- measurement ---------------------------------------------------
	static constexpr int kTableSize = 192;           // one excitation sine cycle
	static constexpr int kQuarter = kTableSize / 4;  // 90 degrees
	static constexpr int kWindow = 960;              // 20 ms at 48 kHz
	static constexpr int16_t kRefAmp = 2047;
	// Table steps per sample -> 250, 500, 1k, 2k, 4k Hz. 960 samples is a
	// whole number of cycles for every one of them.
	static constexpr int kSteps[5] = {1, 2, 4, 8, 16};

	// ---- detector ------------------------------------------------------
	static constexpr int kHistory = 10;
	static constexpr float kShapeThreshold = 2.6f;   // range/stdev must exceed this (Sprout test)
	static constexpr float kSizeMin = 2.0f;          // Main fully CW: jump > 2x typical wobble (many notes)
	static constexpr float kSizeMax = 20.0f;         // Main fully CCW: jump > 20x typical wobble (few notes)
	static constexpr float kCalSize = 6.0f;          // fixed, calibration preview
	static constexpr float kNoiseRate = 1.0f / 1000.0f;  // wobble tracker, ~20 s
	static constexpr int kRefractory = 3;            // readings (60 ms) between notes
	static constexpr float kMinJump = 0.25f;         // ADC counts; ignore smaller
	static constexpr float kMinLevel = 4.0f;         // ADC counts; below = no signal
	static constexpr float kPresentFraction = 0.3f;  // of calibrated level; below = plant disconnected
	static constexpr float kClipLevel = 1950.0f;

	// ---- notes ---------------------------------------------------------
	static constexpr int kBaseNote = 36;             // C2
	static constexpr int kNumScales = 7;
	// 12-bit masks, bit n = semitone n above C
	static constexpr uint16_t kScales[kNumScales] = {
		0xFFF,                                                          // chromatic
		(1<<0)|(1<<2)|(1<<4)|(1<<5)|(1<<7)|(1<<9)|(1<<11),              // major
		(1<<0)|(1<<2)|(1<<3)|(1<<5)|(1<<7)|(1<<8)|(1<<10),              // natural minor
		(1<<0)|(1<<2)|(1<<3)|(1<<5)|(1<<7)|(1<<9)|(1<<10),              // dorian
		(1<<0)|(1<<2)|(1<<4)|(1<<7)|(1<<9),                             // major pentatonic
		(1<<0)|(1<<3)|(1<<5)|(1<<7)|(1<<10),                            // minor pentatonic
		(1<<0)|(1<<2)|(1<<4)|(1<<6)|(1<<8)|(1<<10),                     // whole tone
	};
	static constexpr int kTrigSamples = 480;         // 10 ms
	static constexpr int kGateGap = 96;              // 2 ms gate low between retriggered notes
	static constexpr int kAttackSamples = 96;        // 2 ms voice attack
	static constexpr int kVoiceBits = 10;            // 1024-entry voice sine table
	static constexpr uint8_t kMidiChannel = 0;       // channel 1
	static constexpr uint8_t kMidiCC = 80;           // as the MIDI Sprout

	// ---- saved settings ------------------------------------------------
	static constexpr uint32_t kFlashOffset = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE;
	static constexpr uint32_t kMagic = 0x53505254;   // 'SPRT'
	static constexpr uint32_t kVersion = 1;
	struct Settings
	{
		uint32_t magic;
		uint32_t version;
		int32_t toneLevel;   // 0..2047
		int32_t freqZone;    // 0..4
		float zoom;          // 1..128
		float refLevel;      // level when saved, ADC counts
		uint32_t check;
	};

	Settings settings;
	bool calibrated = false;

	int16_t sine[kTableSize];
	int16_t voiceTable[1 << kVoiceBits];

	// ---- core 0 state --------------------------------------------------
	int phase = 0, step = 4, count = 0;
	int64_t accI = 0, accQ = 0;
	int32_t peak = 0;
	int32_t trigTimer = 0, gateTimer = 0, gateGap = 0;
	uint32_t lastTrigSeen = 0, lastNoteSeen = 0;
	uint32_t voicePhase = 0, voiceIncr = 0;
	int32_t env = 0, envTarget = 0, attackStep = 0, decayCoef = 0;
	bool attacking = false;
	int32_t cv2Slew = 0;  // Q8

	// ---- shared between cores ------------------------------------------
	volatile Mode mode = Mode::Starting;
	volatile uint32_t sampleCount = 0;

	volatile int64_t winI = 0, winQ = 0;   // core 0 -> core 1
	volatile int32_t winPeak = 0;
	volatile uint32_t winSeq = 0;

	volatile int16_t outLevel = 0;         // core 1 -> core 0
	volatile int16_t outChange = 0;
	volatile int16_t outPhase = 0;
	volatile bool outClip = false;
	volatile uint32_t trigSeq = 0;         // calibration: detector fired

	volatile uint8_t nNote = 60;           // core 1 -> core 0: new note
	volatile int32_t nDur = 0, nEnv = 0, nDecay = 0;
	volatile uint32_t nIncr = 0;
	volatile uint32_t noteSeq = 0;

	static Sprout *instance;

public:
	Sprout()
	{
		for (int i = 0; i < kTableSize; i++)
			sine[i] = int16_t(lroundf(kRefAmp * sinf(2.0f * float(M_PI) * float(i) / kTableSize)));
		for (int i = 0; i < (1 << kVoiceBits); i++)
			voiceTable[i] = int16_t(lroundf(2047.0f * sinf(2.0f * float(M_PI) * float(i) / float(1 << kVoiceBits))));
		LoadSettings();
	}

	void StartSecondCore()
	{
		instance = this;
		multicore_launch_core1(core1);
	}

	static void core1() { instance->Core1(); }

	// =====================================================================
	// Core 0, 48 kHz
	// =====================================================================
	virtual void ProcessSample()
	{
		sampleCount = sampleCount + 1;
		Mode m = mode;

		// ---- excitation tone + lock-in sums -----------------------------
		int32_t ref = sine[phase];
		int32_t refQ = sine[(phase + kQuarter) % kTableSize];
		int32_t level = (m == Mode::Calibrate) ? (KnobVal(Knob::Main) >> 1) : settings.toneLevel;
		AudioOut1(int16_t((ref * level) >> 11));

		int32_t in = AudioIn1();
		accI += in * ref;
		accQ += in * refQ;
		int32_t mag = in < 0 ? -in : in;
		if (mag > peak) peak = mag;

		phase += step;
		if (phase >= kTableSize) phase -= kTableSize;

		if (++count == kWindow)
		{
			winI = accI;
			winQ = accQ;
			winPeak = peak;
			__dmb();
			winSeq = winSeq + 1;
			accI = accQ = 0;
			peak = 0;
			count = 0;

			// Change frequency only between windows (keeps whole cycles)
			int zone = (m == Mode::Calibrate) ? KnobZone(KnobVal(Knob::X), 5) : settings.freqZone;
			step = kSteps[zone];
		}

		if (m == Mode::Calibrate)
		{
			// Probe outputs
			uint32_t t = trigSeq;
			if (t != lastTrigSeen) { lastTrigSeen = t; trigTimer = kTrigSamples; }
			if (trigTimer > 0) trigTimer--;

			CVOut1(outLevel);
			CVOut2(outChange);
			AudioOut2(outPhase);
			PulseOut1(trigTimer > 0);
			PulseOut2(outClip);
			return;
		}

		if (m != Mode::Play)
		{
			AudioOut2(0);
			PulseOut1(false);
			PulseOut2(false);
			return;
		}

		// ---- new note from core 1 ---------------------------------------
		uint32_t ns = noteSeq;
		if (ns != lastNoteSeen)
		{
			lastNoteSeen = ns;
			__dmb();
			CVOut1MIDINote(nNote);
			gateGap = (gateTimer > 0) ? kGateGap : 0;  // re-strike a held gate
			gateTimer = nDur;
			trigTimer = kTrigSamples;
			voiceIncr = nIncr;
			decayCoef = nDecay;
			envTarget = nEnv;
			attackStep = (envTarget - env) / kAttackSamples;
			attacking = attackStep > 0;
			if (!attacking) env = envTarget;
		}

		// ---- gate and trigger -------------------------------------------
		bool gate;
		if (gateGap > 0) { gateGap--; gate = false; }
		else gate = gateTimer > 0;
		if (gateTimer > 0) gateTimer--;
		if (trigTimer > 0) trigTimer--;
		PulseOut1(gate);
		PulseOut2(trigTimer > 0);
		LedOn(0, gate);

		// ---- built-in voice: sine, 2 ms attack, exponential decay -------
		voicePhase += voiceIncr;
		int32_t s = voiceTable[voicePhase >> (32 - kVoiceBits)];
		if (attacking)
		{
			env += attackStep;
			if (env >= envTarget) { env = envTarget; attacking = false; }
		}
		else
		{
			env = int32_t((int64_t(env) * decayCoef) >> 31);
		}
		AudioOut2(int16_t((s * (env >> 12)) >> 12));  // env is Q24

		// ---- plant change signal, smoothed (~10 ms) ---------------------
		int32_t target = int32_t(outChange) << 8;
		cv2Slew += (target - cv2Slew) >> 9;
		CVOut2(int16_t(cv2Slew >> 8));
	}

	// =====================================================================
	// Core 1: USB MIDI + 50 Hz processing
	// =====================================================================
	void Core1()
	{
		tusb_init();

		// Let knobs and switch readings settle, then choose the mode
		WaitSamples(9600);  // 200 ms
		bool cal = (SwitchVal() == Switch::Down);
		if (cal)
		{
			FlashAllLeds(3);
			mode = Mode::Calibrate;
		}
		else
		{
			mode = Mode::Play;
		}

		uint32_t seenSeq = winSeq;
		while (true)
		{
			tud_task();
			MidiNoteOffIfDue();
			if (winSeq != seenSeq)
			{
				seenSeq = winSeq;
				__dmb();
				ProcessWindow(float(winI), float(winQ), winPeak);
			}
		}
	}

private:
	// ---- core 1 state --------------------------------------------------
	float baseline = 0.0f;
	bool haveBaseline = false;
	float noise = 0.0f, lastLevel = 0.0f;
	bool haveNoise = false, haveLast = false;
	float history[kHistory] = {0};
	int histCount = 0, histPos = 0;
	int refractory = 0;
	bool prevDown = true;      // treat switch as held at start, so boot-hold is ignored
	bool armed = false;        // calibration: switch released since power-on
	int downWindows = 0;
	int clipHold = 0, flashHold = 0, blink = 0;
	bool midiNoteOn = false;
	uint8_t midiNote = 0;
	uint32_t midiNoteOffAt = 0;
	int lastCC = -1;

	void ProcessWindow(float fI, float fQ, int32_t pk)
	{
		Mode m = mode;

		// sum(in*ref) over whole cycles = A * kRefAmp * N / 2
		float level = 2.0f * sqrtf(fI * fI + fQ * fQ) / (float(kRefAmp) * float(kWindow));
		float ph = atan2f(fQ, fI);
		bool clip = float(pk) > kClipLevel;

		// ---- switch -----------------------------------------------------
		bool down = (SwitchVal() == Switch::Down);
		bool rezero = !haveBaseline;
		bool save = false;
		if (m == Mode::Calibrate)
		{
			// Ignore the hold that entered calibration until it is released
			if (!down) armed = true;
			if (!armed) down = false;
			if (down && !prevDown) downWindows = 0;
			if (down) { downWindows++; if (downWindows == 100) save = true; }  // 2 s
			if (!down && prevDown && downWindows > 0 && downWindows < 100) rezero = true;
			if (!down) downWindows = 0;
		}
		else if (down && !prevDown)
		{
			rezero = true;
		}
		prevDown = down;

		// ---- baseline and change signal ---------------------------------
		if (rezero) { baseline = level; haveBaseline = true; }
		else baseline += (level - baseline) * (1.0f / 64.0f);  // ~1.3 s

		float zoom = (m == Mode::Calibrate)
			? exp2f(float(KnobVal(Knob::Y)) * (7.0f / 4096.0f))
			: settings.zoom;
		float change = (level - baseline) * zoom;

		// ---- detector ---------------------------------------------------
		history[histPos] = level;
		histPos = (histPos + 1) % kHistory;
		if (histCount < kHistory) histCount++;
		if (refractory > 0) refractory--;

		// Typical wobble: slow average of the change between readings.
		// Jumps must be several times this to count, so sensitivity adapts
		// to each plant and electrode setup.
		if (haveLast)
		{
			float diff = level - lastLevel;
			if (diff < 0) diff = -diff;
			noise = haveNoise ? noise + (diff - noise) * kNoiseRate : diff;
			haveNoise = true;
		}
		lastLevel = level;
		haveLast = true;

		float sizeMult = kCalSize;
		if (m == Mode::Play)
		{
			int32_t k = KnobVal(Knob::Main) + CVIn1();
			if (k < 0) k = 0;
			if (k > 4095) k = 4095;
			// exponential: CCW = kSizeMax, CW = kSizeMin
			sizeMult = kSizeMax * exp2f(-float(k) * (log2f(kSizeMax / kSizeMin) / 4095.0f));
		}
		float minJump = noise * sizeMult;
		if (minJump < kMinJump) minJump = kMinJump;

		bool present = level > kMinLevel &&
			(!calibrated || settings.refLevel <= 0.0f || level > settings.refLevel * kPresentFraction);

		float mean = 0.0f, range = 0.0f;
		bool fire = false;
		if (histCount == kHistory && refractory == 0 && present)
		{
			float mn = history[0], mx = history[0], sum = 0.0f, sumSq = 0.0f;
			for (int i = 0; i < kHistory; i++)
			{
				float v = history[i];
				if (v < mn) mn = v;
				if (v > mx) mx = v;
				sum += v;
				sumSq += v * v;
			}
			mean = sum / kHistory;
			float var = sumSq / kHistory - mean * mean;
			float sd = sqrtf(var > 0.0025f ? var : 0.0025f);
			range = mx - mn;
			if (range > sd * kShapeThreshold && range > minJump)
			{
				fire = true;
				refractory = kRefractory;
			}
		}

		// ---- publish to core 0 ------------------------------------------
		outLevel = Clamp12(level);
		outChange = Clamp12(change);
		outPhase = Clamp12(ph * (2047.0f / float(M_PI)));
		outClip = clip;

		if (clip) clipHold = 10;
		if (fire) flashHold = 3;
		blink++;

		if (m == Mode::Calibrate)
		{
			if (fire) trigSeq = trigSeq + 1;

			// Level meter (left column), clip, change, detector
			LedBrightness(4, Segment(level, 0.0f));
			LedBrightness(2, Segment(level, 512.0f));
			LedBrightness(0, Segment(level, 1024.0f));
			LedOn(1, clipHold > 0);
			float ac = change < 0 ? -change : change;
			LedBrightness(3, uint16_t(ac * 2.0f > 4095.0f ? 4095.0f : ac * 2.0f));
			LedOn(5, flashHold > 0);

			if (save)
			{
				settings.toneLevel = KnobVal(Knob::Main) >> 1;
				settings.freqZone = KnobZone(KnobVal(Knob::X), 5);
				settings.zoom = zoom;
				settings.refLevel = level;
				SaveSettings();
				FlashAllLeds(3);
				LedOff(0);
				haveBaseline = false;   // re-zero on entering Play
				mode = Mode::Play;
			}
		}
		else if (m == Mode::Play)
		{
			if (fire) PlayNote(mean, range, range / minJump);

			// MIDI CC 80 follows the change signal (64 = baseline)
			int cc = 64 + int(change * (1.0f / 32.0f));
			if (cc < 0) cc = 0;
			if (cc > 127) cc = 127;
			if (cc != lastCC && tud_midi_mounted())
			{
				uint8_t msg[3] = {uint8_t(0xB0 | kMidiChannel), kMidiCC, uint8_t(cc)};
				tud_midi_stream_write(0, msg, 3);
				lastCC = cc;
			}

			// LED 0 is the gate (core 0)
			LedOn(1, clipHold > 0);
			float pos = change > 0 ? change : 0.0f;
			float neg = change < 0 ? -change : 0.0f;
			LedBrightness(3, uint16_t(pos * 2.0f > 4095.0f ? 4095.0f : pos * 2.0f));
			LedBrightness(2, uint16_t(neg * 2.0f > 4095.0f ? 4095.0f : neg * 2.0f));
			LedOn(4, present || ((blink >> 3) & 1));   // blinks when no plant signal
			LedOn(5, tud_midi_mounted());
		}

		if (clipHold > 0) clipHold--;
		if (flashHold > 0) flashHold--;
	}

	// excess = how many times bigger the jump was than needed (>= 1)
	void PlayNote(float mean, float range, float excess)
	{
		// Notes available: scale (X) within range (Y) above C2
		int scale = KnobZone(KnobVal(Knob::X), kNumScales);
		int span = 12 + (KnobVal(Knob::Y) * 48) / 4095;   // 1..5 octaves
		uint16_t mask = kScales[scale];
		uint8_t notes[61];
		int n = 0;
		for (int note = kBaseNote; note <= kBaseNote + span && n < 61; note++)
			if (mask & (1 << (note % 12))) notes[n++] = uint8_t(note);
		if (n == 0) return;

		// CV In 2 transposes, roughly 1 V/oct (CV inputs are uncalibrated)
		int transpose = int(lroundf(float(CVIn2()) * (1.0f / 28.44f)));
		if (transpose < -24) transpose = -24;
		if (transpose > 24) transpose = 24;

		// Like the Sprout: the fine detail of the reading picks the note,
		// and the size of the jump sets the length
		uint32_t fine = uint32_t(mean * 64.0f);
		int note = notes[fine % uint32_t(n)] + transpose;
		if (note < 0) note = 0;
		if (note > 127) note = 127;

		uint32_t jump = uint32_t(range * 64.0f) % 127;
		bool longNotes = (SwitchVal() == Switch::Up);
		int32_t durMs = longNotes ? int32_t(150 + jump * 20) : int32_t(40 + jump * 2);

		// Bigger jump = louder: 40 at the threshold, 127 at 8x it
		float v = 40.0f + 29.0f * log2f(excess);
		int vel = int(v);
		if (vel < 1) vel = 1;
		if (vel > 127) vel = 127;

		// Voice: frequency, envelope level and decay (tau = 1/3 of note length)
		float freq = 440.0f * exp2f(float(note - 69) * (1.0f / 12.0f));
		float tau = float(durMs) * (48.0f / 3.0f);  // samples
		nNote = uint8_t(note);
		nDur = durMs * 48;
		nIncr = uint32_t(freq * (4294967296.0f / 48000.0f));
		nEnv = int32_t(float(vel) * (16777216.0f / 127.0f));
		nDecay = int32_t(expf(-1.0f / tau) * 2147483647.0f);
		__dmb();
		noteSeq = noteSeq + 1;

		// USB MIDI
		if (tud_midi_mounted())
		{
			if (midiNoteOn)
			{
				uint8_t off[3] = {uint8_t(0x80 | kMidiChannel), midiNote, 0};
				tud_midi_stream_write(0, off, 3);
			}
			uint8_t on[3] = {uint8_t(0x90 | kMidiChannel), uint8_t(note), uint8_t(vel)};
			tud_midi_stream_write(0, on, 3);
			midiNoteOn = true;
			midiNote = uint8_t(note);
			midiNoteOffAt = sampleCount + uint32_t(durMs * 48);
		}
	}

	void MidiNoteOffIfDue()
	{
		if (midiNoteOn && int32_t(sampleCount - midiNoteOffAt) >= 0)
		{
			if (tud_midi_mounted())
			{
				uint8_t off[3] = {uint8_t(0x80 | kMidiChannel), midiNote, 0};
				tud_midi_stream_write(0, off, 3);
			}
			midiNoteOn = false;
		}
	}

	// ---- settings in the last flash sector ------------------------------
	static uint32_t Checksum(const Settings &s)
	{
		const uint32_t *w = reinterpret_cast<const uint32_t *>(&s);
		uint32_t c = 0x12345678;
		for (size_t i = 0; i < offsetof(Settings, check) / 4; i++) c = (c ^ w[i]) * 16777619u;
		return c;
	}

	void LoadSettings()
	{
		std::memcpy(&settings, reinterpret_cast<const void *>(XIP_BASE + kFlashOffset), sizeof(Settings));
		calibrated = settings.magic == kMagic && settings.version == kVersion &&
			settings.check == Checksum(settings) &&
			settings.toneLevel >= 0 && settings.toneLevel <= 2047 &&
			settings.freqZone >= 0 && settings.freqZone <= 4;
		if (!calibrated)
		{
			settings.magic = kMagic;
			settings.version = kVersion;
			settings.toneLevel = 512;
			settings.freqZone = 2;   // 1 kHz
			settings.zoom = 16.0f;
			settings.refLevel = 0.0f;
		}
	}

	void SaveSettings()
	{
		settings.magic = kMagic;
		settings.version = kVersion;
		settings.check = Checksum(settings);

		static uint8_t buf[FLASH_PAGE_SIZE];
		std::memset(buf, 0xFF, sizeof(buf));
		std::memcpy(buf, &settings, sizeof(Settings));

		// The whole program runs from RAM (copy_to_ram), so core 0's audio
		// keeps running during the write; only core 1's interrupts are paused.
		uint32_t ints = save_and_disable_interrupts();
		flash_range_erase(kFlashOffset, FLASH_SECTOR_SIZE);
		flash_range_program(kFlashOffset, buf, FLASH_PAGE_SIZE);
		restore_interrupts(ints);
		calibrated = true;
	}

	// ---- helpers ------------------------------------------------------
	void WaitSamples(uint32_t n)
	{
		uint32_t start = sampleCount;
		while (sampleCount - start < n) tud_task();
	}

	void FlashAllLeds(int times)
	{
		for (int t = 0; t < times; t++)
		{
			for (int i = 0; i < 6; i++) LedOn(i, true);
			WaitSamples(4800);
			for (int i = 0; i < 6; i++) LedOn(i, false);
			WaitSamples(4800);
		}
	}

	static int KnobZone(int32_t k, int zones)
	{
		int z = (k * zones) >> 12;
		return z >= zones ? zones - 1 : (z < 0 ? 0 : z);
	}

	static int16_t Clamp12(float v)
	{
		if (v > 2047.0f) return 2047;
		if (v < -2048.0f) return -2048;
		return int16_t(v);
	}

	static uint16_t Segment(float level, float start)
	{
		float x = (level - start) * (4095.0f / 512.0f);
		if (x < 0.0f) return 0;
		if (x > 4095.0f) return 4095;
		return uint16_t(x);
	}
};

Sprout *Sprout::instance = nullptr;
constexpr int Sprout::kSteps[5];
constexpr uint16_t Sprout::kScales[Sprout::kNumScales];

int main()
{
	set_sys_clock_khz(144000, true);

	static Sprout card;
	card.StartSecondCore();
	card.Run();
}
