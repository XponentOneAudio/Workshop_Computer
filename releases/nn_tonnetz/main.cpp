// Tonnetz
//
// Four voices walk around a Tonnetz, the lattice of pitches in which each
// note's six neighbours are a perfect fifth, a major third and a minor third
// above and below it. Each step moves one voice, in turn, one place across
// the lattice in the chosen direction. After every voice has moved, the whole
// chord has been carried one place along the lattice; in between, the
// half-moved chords are the passing harmonies.
//
//   Main knob    - root note, C2 to C4 in semitones
//   Knob X / Y   - direction of travel (added to CV in 1 / 2 and the Nunchuk)
//   Switch       - up: glide, middle: no glide, down (momentary): reset the
//                  chord to its starting shape
//   CV in 1 / 2  - direction of travel, X / Y
//   Pulse in 1   - step: move the next voice
//   Pulse in 2   - toggle the fourth voice on/off
//   CV out 1     - voice 1 pitch, 1V/oct (calibrated)
//   CV out 2     - voice 2 pitch, 1V/oct (calibrated)
//   Audio out 1  - voice 1 sine, plus the fourth voice's sine when it is on
//   Audio out 2  - voice 2 sine, plus the fourth voice's sine when it is on
//   Pulse out 1  - voice 3 square
//   Pulse out 2  - sub square: one octave below the lowest sounding voice
//
// Wii Nunchuk (through the Pico Nunchuk adapter, which appears as an 8mu on
// the Computer's USB socket):
//
//   Joystick     - direction of travel (added to CV in and knobs X/Y)
//   Z button     - step the next voice in the chosen direction
//   C button     - step the next voice in the opposite direction
//
// Direction is quantised to the six neighbours of the lattice:
//
//     up-left    -m3   +M3    up-right         LED 0   LED 1
//     left       -P5    o    +P5   right    LED 2           LED 3
//     down-left  -M3   +m3    down-right       LED 4   LED 5
//
// The LED for the current direction is lit; it blinks off for a moment on
// each step. The other LEDs glow dimly while the fourth voice is on.


#include "ComputerCard.h"
#include "EightMU.h"

#include <math.h>


static constexpr int numVoices = 4;
static constexpr int controlInterval = 16;  // samples between control updates

// Pitches are held in 1/256 semitone units
static constexpr int32_t semi = 256;

// The six lattice directions, anticlockwise from +P5 (right), as intervals in
// semitones. Opposite directions are three apart.
static constexpr int numDirections = 6;
static constexpr int8_t dirInterval[numDirections] = {7, 4, -3, -7, -4, 3};

// Unit vectors of the six directions, scaled by 1024 (0, 60, ... 300 degrees)
static constexpr int32_t dirVecX[numDirections] = {1024, 512, -512, -1024, -512, 512};
static constexpr int32_t dirVecY[numDirections] = {0, 887, 887, 0, -887, -887};

// LED showing each direction
static constexpr uint8_t dirLed[numDirections] = {3, 1, 0, 2, 4, 5};

// Starting chord, semitones above the root: root, major third, fifth and
// major seventh - a small square on the lattice
static constexpr int8_t homeChord[numVoices] = {0, 4, 7, 11};

// Each voice stays within this many semitones of its starting note,
// folding by an octave if a step would take it further
static constexpr int voiceRange = 8;

// Squared length the direction vector must exceed before it counts
static constexpr int32_t deadZone2 = 300 * 300;

// Root note range, from the main knob
static constexpr int rootLowest = 36;  // C2
static constexpr int rootSteps = 25;   // to C4

static constexpr int32_t Clip(int32_t v, int32_t lo, int32_t hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}


class Tonnetz : public ComputerCard
{
public:
	Tonnetz()
	{
		for (int i = 0; i < sineSize + 1; i++)
		{
			sine[i] = int16_t(32767.0f * sinf(float(i) * (6.2831853f / float(sineSize))));
		}

		// Phase increments per sample for MIDI notes 0 to 12; higher octaves
		// are these shifted left
		for (int i = 0; i < 13; i++)
		{
			float freq = 440.0f * powf(2.0f, float(i - 69) / 12.0f);
			octaveInc[i] = uint32_t(freq * (4294967296.0f / 48000.0f));
		}

		Reset();
		for (int i = 0; i < numVoices; i++)
		{
			pitch[i] = Target(i);
		}

		// Give the USB power circuitry time to settle, then start the USB
		// host for the Nunchuk adapter only if this is a host port
		sleep_us(150000);
		if (USBPowerState() == DFP)
		{
			mu.Start(); // claims core1
		}
	}

	virtual void ProcessSample()
	{
		// Triggers
		if (PulseIn1RisingEdge()) Step(1);
		if (PulseIn2RisingEdge()) ToggleFourth();

		bool z = mu.Connected() && mu.Button(0);
		bool c = mu.Connected() && mu.Button(1);
		if (z && !lastZ) Step(1);
		if (c && !lastC) Step(-1);
		lastZ = z;
		lastC = c;

		Switch sw = SwitchVal();
		if (sw == Down && SwitchChanged()) Reset();

		if (++controlCount >= controlInterval)
		{
			controlCount = 0;
			Control(sw);
		}

		// Oscillators
		for (int i = 0; i < numVoices; i++)
		{
			phase[i] += inc[i];
		}
		subPhase += subInc;

		// Fourth voice fades in and out over about 5ms
		if (fourthOn)
		{
			if (fourthGain < 4096) fourthGain += 16;
		}
		else
		{
			if (fourthGain > 0) fourthGain -= 16;
		}

		int32_t s4 = (Sine(phase[3]) * fourthGain) >> 12;
		AudioOut1(int16_t((Sine(phase[0]) * 1400 + s4 * 600) >> 15));
		AudioOut2(int16_t((Sine(phase[1]) * 1400 + s4 * 600) >> 15));

		PulseOut1(phase[2] < 0x80000000u);
		PulseOut2(subPhase < 0x80000000u);
	}

private:
	////////////////////////////////////////
	// Lattice

	// Move the next voice one place in the current direction (sign 1) or
	// the opposite direction (sign -1)
	void Step(int sign)
	{
		int interval = dirInterval[direction] * sign;

		// The same pitch class can be reached up or down; a fifth up is also
		// a fourth down. Take whichever lands nearer the voice's home note,
		// then fold by an octave if it has wandered too far.
		int v = nextVoice;
		int d = ((interval % 12) + 12) % 12;  // 0 to 11
		int up = note[v] + d;
		int down = note[v] + d - 12;
		int n = (abs(up - homeChord[v]) <= abs(down - homeChord[v])) ? up : down;
		if (n > homeChord[v] + voiceRange) n -= 12;
		if (n < homeChord[v] - voiceRange) n += 12;
		note[v] = int8_t(n);

		nextVoice = (nextVoice + 1) % ActiveVoices();
		stepFlash = 48000 / 25 / controlInterval;  // 40ms
	}

	void ToggleFourth()
	{
		fourthOn = !fourthOn;
		if (nextVoice >= ActiveVoices()) nextVoice = 0;
	}

	void Reset()
	{
		for (int i = 0; i < numVoices; i++)
		{
			note[i] = homeChord[i];
		}
		nextVoice = 0;
	}

	int ActiveVoices() const {return fourthOn ? 4 : 3;}

	// Wanted pitch of voice i, in 1/256 semitones as a MIDI note
	int32_t Target(int i) const {return (root + note[i]) * semi;}

	////////////////////////////////////////
	// Control rate (3kHz)

	void Control(Switch sw)
	{
		// Root note from the main knob, with hysteresis so that it doesn't
		// flicker between two notes
		int32_t k = KnobVal(Main) * rootSteps;  // 0 to ~rootSteps*4096
		int32_t centre = (rootIndex * 4096) + 2048;
		if (k > centre + 2048 + 512 || k < centre - 2048 - 512)
		{
			rootIndex = Clip(k >> 12, 0, rootSteps - 1);
		}
		root = rootLowest + rootIndex;

		// Direction of travel: CV ins, knobs X/Y and the Nunchuk joystick
		int32_t x = CVIn1() + KnobVal(X) - 2048;
		int32_t y = CVIn2() + KnobVal(Y) - 2048;
		if (mu.Connected())
		{
			x += mu.Fader(0) - 2048;
			y += mu.Fader(1) - 2048;
		}
		if (x * x + y * y > deadZone2)
		{
			int best = 0;
			int32_t bestDot = INT32_MIN;
			for (int i = 0; i < numDirections; i++)
			{
				int32_t dot = x * dirVecX[i] + y * dirVecY[i];
				if (dot > bestDot)
				{
					bestDot = dot;
					best = i;
				}
			}
			direction = best;
		}

		// Glide towards the wanted pitches: about 60ms with the switch up
		for (int i = 0; i < numVoices; i++)
		{
			int32_t t = Target(i);
			if (sw == Up)
			{
				pitch[i] += (t - pitch[i]) >> 6;
				if (abs(t - pitch[i]) < 64) pitch[i] += (t > pitch[i]) - (t < pitch[i]);
			}
			else
			{
				pitch[i] = t;
			}
			inc[i] = Increment(pitch[i]);
		}

		// Sub follows the lowest sounding voice, an octave down
		int32_t lowest = pitch[0];
		for (int i = 1; i < ActiveVoices(); i++)
		{
			if (pitch[i] < lowest) lowest = pitch[i];
		}
		subInc = Increment(lowest - 12 * semi);

		// Pitch CV for voices 1 and 2: 0V at MIDI note 60, 1V/oct
		CVOut1Millivolts(((pitch[0] - 60 * semi) * 1000) / (12 * semi));
		CVOut2Millivolts(((pitch[1] - 60 * semi) * 1000) / (12 * semi));

		// LEDs
		if (stepFlash > 0) stepFlash--;
		for (int i = 0; i < numDirections; i++)
		{
			uint16_t b = fourthOn ? 600 : 0;
			if (i == direction) b = stepFlash ? 0 : 4095;
			LedBrightness(dirLed[i], b);
		}
	}

	////////////////////////////////////////
	// Oscillators

	// Phase increment for a pitch in 1/256 semitones (MIDI note numbers)
	uint32_t Increment(int32_t p) const
	{
		p = Clip(p, 0, 127 * semi);
		int32_t s = p >> 8;
		int32_t frac = p & 0xFF;
		int octave = s / 12;
		int n = s - octave * 12;
		uint32_t a = octaveInc[n], b = octaveInc[n + 1];
		uint32_t i = a + (((b - a) * uint32_t(frac)) >> 8);
		return i << octave;
	}

	int32_t Sine(uint32_t ph) const
	{
		uint32_t idx = ph >> (32 - sineBits);
		int32_t frac = int32_t((ph >> (32 - sineBits - 16)) & 0xFFFF);
		int32_t a = sine[idx], b = sine[idx + 1];
		return a + (((b - a) * frac) >> 16);
	}

	static constexpr int sineBits = 9;
	static constexpr int sineSize = 1 << sineBits;
	int16_t sine[sineSize + 1];
	uint32_t octaveInc[13];

	////////////////////////////////////////
	// State

	EightMU mu;

	int8_t note[numVoices];       // semitones above the root
	int32_t pitch[numVoices];     // current (gliding) pitch, 1/256 semitone
	uint32_t phase[numVoices] = {};
	uint32_t inc[numVoices] = {};
	uint32_t subPhase = 0, subInc = 0;

	int nextVoice = 0;
	int direction = 0;
	bool fourthOn = false;
	int32_t fourthGain = 0;

	int rootIndex = 0;
	int root = rootLowest;

	int controlCount = 0;
	int stepFlash = 0;
	bool lastZ = false, lastC = false;
};


int main()
{
	set_sys_clock_khz(200000, true);

	static Tonnetz card;
	card.Run();
}
