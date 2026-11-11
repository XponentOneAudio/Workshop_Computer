// Shaker.h
//
// A physical model of a shaker (maraca, cabasa, sleigh bells, rainstick...).
//
// This file knows nothing about the Workshop Computer or the 8mu.  It is
// plain C++ that takes numbers in and gives audio samples out, which means
// it can also be compiled and listened to on a laptop (see test/render_wav.cpp).
// main.cpp is the part that connects it to the real hardware.
//
//
// HOW THE MODEL WORKS
// ===================
//
// A shaker is a hollow shell with loose grains inside.  We do not simulate
// every grain - a maraca has hundreds, and that would be far too slow.
// Instead the model is built from three simple layers, each feeding the next:
//
//   1. THE MASS OF GRAINS  (runs 1,000 times a second)
//      All the grains together are treated as ONE lump that slides around
//      inside a box.  The 8mu's accelerometer tells us which way "downhill"
//      is, so the lump accelerates that way, slides, and eventually thumps
//      into a wall.  Hitting a wall, or sliding fast, adds "rattle energy".
//      This is ordinary school physics: velocity += acceleration,
//      position += velocity.
//
//   2. THE INDIVIDUAL GRAINS  (runs 48,000 times a second)
//      While there is rattle energy, grains are colliding.  We don't know
//      exactly when, so we roll a dice every sample: with a small probability
//      a "collision" happens and makes a tiny click of noise.  More grains =
//      higher probability.  More energy = louder clicks.  The energy fades
//      away by itself, and as it does the collisions get rarer as well as
//      quieter, until the grains settle and stop.
//      (This idea is Perry Cook's "PhISM" - Physically Informed Stochastic
//      Event Modelling.  "Stochastic" just means "using randomness".)
//
//   3. THE SHELL  (runs 48,000 times a second)
//      The clicks are fed into three resonators - filters that ring at one
//      frequency each, like a struck object does.  Their pitch is the SIZE of
//      the shell, and how long they ring is its MATERIAL: a gourd goes "tk",
//      a metal shell goes "ting".
//
//
// A NOTE ON FIXED-POINT MATHS
// ===========================
//
// The Workshop Computer's processor (an RP2040) has no hardware for
// floating-point numbers (float / double).  It can still do them, but slowly,
// in software - far too slowly for work that must happen 48,000 times a
// second.  So the audio code here uses only whole numbers (integers).
//
// The trick for representing fractions with integers is called FIXED POINT.
// We agree that an integer secretly means "this many 65536ths", for example.
// Then 65536 means 1.0, 32768 means 0.5, and so on.  That particular format
// is called "Q16", because 65536 is 2 to the power 16.  Names in this file
// carry their format as a suffix, so `energyQ16` is a value where 65536 = 1.0.
//
// To multiply a number by a Q16 fraction: multiply, then shift right by 16
// bits (">> 16" divides by 65536) to get back to the original scale:
//
//     result = (value * fractionQ16) >> 16;
//
// The one thing to watch is OVERFLOW: an int32_t can only hold about +-2
// billion.  Where a multiplication could exceed that, we either keep the
// inputs small enough or do the multiply in 64 bits (int64_t).
//
// Floating point IS used in Init(), to fill in lookup tables.  That happens
// once at power-on, where speed does not matter.

#ifndef SHAKER_H
#define SHAKER_H

#include <math.h>
#include <stdint.h>


// The eight settings of the shaker.  Each one runs from 0 to 127, which is
// the resolution of an 8mu fader, and is used to look a value up in a table.
// The values given here are the defaults: a medium-sized maraca.
struct ShakerParams
{
	int size = 64;        // 0 = tiny and high pitched ... 127 = big and low
	int material = 30;    // 0 = dead (gourd, card) ... 127 = ringing (metal)
	int grains = 75;      // 0 = a few beads ... 127 = about a thousand
	int hardness = 90;    // 0 = soft and dull (sand) ... 127 = hard (steel shot)
	int decay = 32;       // 0 = rattle stops at once ... 127 = keeps going for seconds
	int sensitivity = 64; // 0 = needs a hard shake ... 127 = slightest movement
	int travel = 35;      // 0 = grains packed tight ... 127 = long tube (rainstick)
	int scatter = 0;      // 0 = one shell ... 127 = many differently-tuned objects
};


class Shaker
{
public:
	static constexpr int sampleRate = 48000;

	// The grain-mass physics is run once every `tickLength` audio samples.
	// 48000 / 48 = 1000 times a second, so one "tick" is one millisecond.
	static constexpr int tickLength = 48;

	// The settings.  main.cpp writes to these directly.
	ShakerParams params;

	//------------------------------------------------------------------------
	// Init: fill in the lookup tables.  Call once, before any audio.
	//------------------------------------------------------------------------
	//
	// Almost every control wants an EXPONENTIAL response: each equal step of
	// the fader should MULTIPLY the value by the same amount, because that is
	// how we hear pitch, loudness and time.  Exponentials are slow to
	// calculate, so we work out all 128 possible answers here, store them in
	// arrays, and at audio time just look the answer up.
	void Init()
	{
		const float fs = float(sampleRate);

		// --- Pitch table -----------------------------------------------------
		// A resonator's frequency is set by an angle: how far round a circle
		// the wave moves in one sample.  The resonator needs the cosine and
		// sine of that angle, so both are stored.  Entry 0 is the lowest
		// pitch; every pitchStepsPerOctave entries the frequency doubles.
		for (int i = 0; i < pitchTableSize; i++)
		{
			float freq = lowestFreq * powf(2.0f, float(i) / float(pitchStepsPerOctave));
			float angle = 2.0f * pi * freq / fs;
			cosQ30[i] = int32_t(cosf(angle) * q30);
			sinQ30[i] = int32_t(sinf(angle) * q30);
		}

		for (int i = 0; i < 128; i++)
		{
			// `t` sweeps from 0.0 to 1.0 as the fader goes bottom to top.
			const float t = float(i) / 127.0f;

			// --- Material: how long the shell rings ---------------------------
			// From 0.4 milliseconds (a dull thud) to 300 ms (a clear ring).
			// `r` is how much of the ringing survives from one sample to the
			// next - always just under 1.  The closer to 1, the longer the ring.
			float ringSeconds = Sweep(0.0004f, 0.3f, t);
			float r = expf(-1.0f / (ringSeconds * fs));
			shellRQ30[i] = int32_t(r * q30);
			// A long-ringing resonator builds up a much bigger signal than a
			// short one.  This gain, applied to the shell's output, cancels
			// that out.
			shellGainQ16[i] = int32_t(sqrtf(1.0f - r * r) * 65536.0f);

			// --- Grains: how often a collision happens ------------------------
			// From 3 to 4000 collisions per second.  Stored as the chance of a
			// collision in any one sample, in Q24 (so 16777216 means "certain").
			float collisionsPerSecond = Sweep(3.0f, 4000.0f, t);
			grainProbQ24[i] = uint32_t(collisionsPerSecond / fs * 16777216.0f);
			// Thousands of quiet clicks add up to a lot of sound, so each one
			// is made quieter as the number of grains goes up.
			grainGainQ8[i] = int32_t(256.0f / sqrtf(1.0f + collisionsPerSecond / 15.0f));

			// --- Hardness: the sound of ONE grain hitting ---------------------
			// A soft grain makes a longer, duller click; a hard grain a very
			// short, bright one.  Two things change together:
			//  - how fast the click fades (6 ms down to 0.25 ms)
			float clickSeconds = Sweep(0.006f, 0.00025f, t);
			clickDecayQ12[i] = int32_t(4096.0f / (clickSeconds * fs));
			//  - how much treble is in it (a low-pass filter from 1.2 kHz,
			//    muffled, up to 20 kHz, wide open)
			float cutoff = Sweep(1200.0f, 20000.0f, t);
			brightnessQ15[i] = int32_t((1.0f - expf(-2.0f * pi * cutoff / fs)) * 32767.0f);
			// Long soft clicks overlap and pile up, where short hard ones do
			// not, so soft grains are turned down (0.25x) and hard ones up (2x)
			// to keep the overall loudness about the same.
			hardnessGainQ8[i] = int32_t(Sweep(64.0f, 512.0f, t));

			// --- Decay: how long the rattle lasts after a shake ---------------
			// From 20 ms to 3 seconds.  Stored as the fraction of the energy
			// that is LOST on each 1 ms tick.
			float rattleMs = Sweep(20.0f, 3000.0f, t);
			energyLossQ16[i] = int32_t((1.0f - expf(-1.0f / rattleMs)) * 65536.0f);

			// --- Sensitivity: multiplies the accelerometer, 0.25x to 4x -------
			sensitivityQ8[i] = int32_t(Sweep(0.25f, 4.0f, t) * 256.0f);

			// --- Travel: half the length of the box the grains slide in -------
			// The units are the model's own (see ControlTick).  100,000 takes
			// about 14 ms to cross under full gravity; 60,000,000 takes a
			// second, and many seconds at a gentle tilt.
			travelLength[i] = int32_t(Sweep(100000.0f, 60000000.0f, t));
		}

		UpdateShell();
	}

	//------------------------------------------------------------------------
	// Inputs from the outside world.  All of these just store a number;
	// the real work happens later, in ControlTick() and Process().
	//------------------------------------------------------------------------

	// Which way the shaker is being pushed, each -2032 to 2032 where 2032 is
	// the full force of gravity (1 g).
	//   side:  positive pushes the grains to the right
	//   front: positive pushes the grains away from the player
	//   twist: how fast it is being spun flat on the table (either direction)
	void SetMotion(int32_t side, int32_t front, int32_t twist)
	{
		motionIn[0] = side;
		motionIn[1] = front;
		twistIn = twist;
	}

	// Shift the pitch of the shell, in table steps (64 steps = one octave up).
	void SetTune(int32_t steps) { tune = steps; }

	// How far grains are spread between left and right, 0 (mono) to 256.
	void SetWidth(int32_t w) { width = Clamp(w, 0, 256); }

	// Give the shaker a single jolt, as if it had been struck.
	// strengthQ16: 65536 is a firm shake.  Safe to call at any time.
	void Trigger(int32_t strengthQ16)
	{
		// Limit both the jolt and the running total, so that no number of
		// calls can ever make the total too big for an int32_t to hold.
		strengthQ16 = Clamp(strengthQ16, 0, maxEnergy);
		if (pendingTrigger < maxEnergy) pendingTrigger += strengthQ16;
	}

	// Keep the rattle going at at least this level (Q16) for as long as it is
	// set.  Used for "hold to roll" and for the CV input.  0 turns it off.
	void SetSustain(int32_t levelQ16) { sustainQ16 = levelQ16; }

	// While true, the shaker is gripped in a fist: the rattle dies almost at
	// once and the shell cannot ring.
	void SetChoke(bool c) { choke = c; }

	//------------------------------------------------------------------------
	// Outputs, for LEDs and CV jacks.
	//------------------------------------------------------------------------

	// How hard the shaker is rattling right now.  Q16, 0 to 131072 (2.0).
	int32_t Energy() const { return energyQ16; }

	// Where the lump of grains is, side to side: -2047 (left) to 2047 (right).
	int32_t Position() const { return positionOut; }

	// True if the grains hit a wall since the last time this was called.
	// Calling it resets it - a "read once" flag.
	bool TakeWallHit()
	{
		bool h = wallHit;
		wallHit = false;
		return h;
	}

	// True if a grain collision happened in the most recent sample.
	bool GrainHit() const { return grainHit; }

	//------------------------------------------------------------------------
	// Process: make one sample of stereo audio.  Called 48,000 times a second.
	//------------------------------------------------------------------------
	//
	// audioIn:  an external sound to feed into the shell, -2048 to 2047
	// outL/R:   the results.  NOT yet limited to the 12-bit range of the
	//           hardware; main.cpp applies the volume control and clips.
	//           A typical loud shake peaks at around +-500,000.
	//
	// (The & means outL and outR are "references": the function writes its
	// answers straight into the caller's own variables.  It is how a C++
	// function can hand back more than one result.)
	void Process(int32_t audioIn, int32_t &outL, int32_t &outR)
	{
		// Every 48th sample, run the slow once-a-millisecond code.
		if (++tickCounter >= tickLength)
		{
			tickCounter = 0;
			ControlTick();
		}

		// --- Layer 2: individual grain collisions ----------------------------
		//
		// Roll the dice.  Random() gives a 32-bit random number; we keep its
		// top 24 bits and ask "is it below the probability?".
		//
		// A wall impact or a trigger always makes one collision straight
		// away (pendingHit), so that even a shaker with a single bead in it
		// clicks exactly when it is struck, not at some random moment after.
		grainHit = false;
		if (pendingHit || ((Random() >> 8) < grainProb && energyQ16 > 0))
		{
			pendingHit = false;
			grainHit = true;
			const uint32_t dice = Random();

			// How loud is this click?  Proportional to the rattle energy.
			// energyQ16 is at most 2^17 and grainGain at most 2^10, so the
			// product fits comfortably in 32 bits.
			int32_t amp = (energyQ16 >> 6) * grainGain;   // <= about 1,000,000

			// Where is it, left to right?  0 = hard left, 256 = hard right.
			// Clicks lean towards the side the lump of grains is on, plus a
			// random spread.  (positionOut >> 5 is -64..63: enough to hear
			// the grains move across, without one side ever going silent.)
			int32_t spread = int32_t(dice & 0xFF) - 128;           // -128..127
			int32_t pan = 128 + (((positionOut >> 5) + (spread >> 1)) * width >> 8);
			pan = Clamp(pan, 0, 256);

			// Add the click to the left and right "click envelopes".
			clickLevel[0] += (amp >> 8) * (256 - pan);
			clickLevel[1] += (amp >> 8) * pan;
			if (clickLevel[0] > maxClickLevel) clickLevel[0] = maxClickLevel;
			if (clickLevel[1] > maxClickLevel) clickLevel[1] = maxClickLevel;

			// Scatter: retune one of the three resonators to a random nearby
			// pitch, taking turns.  Each collision then seems to come from a
			// slightly different object - one bell among many on a sleigh
			// bell strap, one jingle on a tambourine.
			if (params.scatter > 0)
			{
				int32_t randomStep = int32_t((dice >> 8) & 0xFF) - 128;   // -128..127
				scatterOffset[scatterTurn] = (randomStep * params.scatter) >> 8;
				UpdateMode(scatterTurn);
				scatterTurn = (scatterTurn + 1) % numModes;
			}
		}

		// Two independent random numbers, one per channel, for the noise that
		// each click is made of.  int16_t(...) keeps the bottom 16 bits and
		// reads them as a signed number, -32768..32767; halving that keeps
		// the low-pass filter's multiplication within 32 bits.
		const uint32_t noiseBits = Random();
		int32_t noise[2] = {
			int16_t(noiseBits & 0xFFFF) >> 1,
			int16_t(noiseBits >> 16) >> 1
		};
		// Two different noises make each click sound different in each ear,
		// which is what gives the sound its width.  As the Width control is
		// turned down, the right-hand noise is moved towards being a copy of
		// the left, until at zero both channels are identical: true mono.
		noise[1] = noise[0] + (((noise[1] - noise[0]) * width) >> 8);

		// The external audio input, scaled up to match the clicks.
		const int32_t external = audioIn * 16;

		int32_t out[2];
		for (int ch = 0; ch < 2; ch++)   // ch 0 = left, ch 1 = right
		{
			// Low-pass filter the noise: this is the "hardness" tone control.
			// A one-pole low-pass is just "move part of the way from where
			// you are towards the new value".  A small part = slow to follow
			// = treble removed.
			noiseLP[ch] += ((noise[ch] - noiseLP[ch]) * brightness) >> 15;

			// A click is noise multiplied by a level that fades away quickly.
			int32_t excitation = ((clickLevel[ch] >> 4) * noiseLP[ch]) >> 10;

			// Fade the click level.  Subtracting a fraction of itself every
			// sample gives an exponential decay.  The "+ 1" makes sure it
			// really reaches zero: with integers, a small level times a small
			// fraction rounds down to nothing and would otherwise stick.
			clickLevel[ch] -= ((clickLevel[ch] * clickDecay) >> 12) + 1;
			if (clickLevel[ch] < 0) clickLevel[ch] = 0;

			excitation += external;

			// --- Layer 3: the shell -----------------------------------------
			//
			// The input is deliberately made a BIG number (up to about
			// twenty million): see "why big numbers" below.
			const int32_t in = excitation * 16;

			int32_t shellOut = 0;
			for (int m = 0; m < numModes; m++)
			{
				// A RESONATOR, built as a spinning, shrinking arrow.
				//
				// Picture an arrow drawn from the centre of a sheet of
				// paper, with its tip at the point (x, y).  Every sample we
				// do two things to it:
				//
				//   - ROTATE it by a small angle.  Watch only the x
				//     coordinate of a rotating arrow and you see it swing
				//     smoothly back and forth: a sine wave.  A bigger angle
				//     per sample means a faster swing - a higher pitch.
				//   - SHRINK it slightly, by the factor r.  So the wave
				//     dies away, quickly for a gourd, slowly for metal.
				//
				// The standard formula for rotating a point by an angle is
				//
				//     new x = x * cos(angle) - y * sin(angle)
				//     new y = x * sin(angle) + y * cos(angle)
				//
				// and modeCos / modeSin already have the shrink factor r
				// multiplied in.  Each click that arrives gives the arrow
				// a push (it is added to x), which sets it swinging again.
				//
				// WHY THIS SHAPE, AND WHY BIG NUMBERS.  Integer maths has
				// to round, and rounding is the enemy of a resonator: in
				// most designs the tiny rounding errors get fed back round
				// the loop and keep it buzzing quietly for ever, long after
				// it should have faded to silence.  Here every result is
				// rounded TOWARDS ZERO (see ShiftTowardsZero), so each step
				// can only ever make the arrow shorter than it should be,
				// never longer.  It is therefore certain to shrink all the
				// way to nothing.  The price is that the last, faintest
				// part of the ring is cut a little short - and keeping the
				// signal in the millions pushes that point down to where it
				// is hard to hear (about 40 dB below the start of the ring,
				// for the longest-ringing metal).
				Mode &mode = modes[ch][m];
				const int64_t rotatedX = int64_t(modeCosQ30[m]) * mode.x
				                       - int64_t(modeSinQ30[m]) * mode.y;
				const int64_t rotatedY = int64_t(modeSinQ30[m]) * mode.x
				                       + int64_t(modeCosQ30[m]) * mode.y;

				//
				// One refinement.  Rounding towards zero is like a tiny
				// amount of friction, and while the ring is still strong
				// that friction bends the wave slightly out of shape, which
				// is heard as a faint roughness.  So while the arrow is
				// long we round to the NEAREST whole number instead, which
				// is as likely to round up as down and does no harm; only
				// once the arrow is nearly gone do we switch to rounding
				// towards zero, to bring it to a clean stop.
				int32_t newX, newY;
				if (Abs(mode.x) + Abs(mode.y) > settleLevel)
				{
					newX = RoundingShift(rotatedX, 30);
					newY = RoundingShift(rotatedY, 30);
				}
				else
				{
					newX = ShiftTowardsZero(rotatedX, 30);
					newY = ShiftTowardsZero(rotatedY, 30);
				}
				mode.x = Clamp(in + newX, -maxModeLevel, maxModeLevel);
				mode.y = Clamp(newY, -maxModeLevel, maxModeLevel);

				// The x coordinate is the sound.  Mix the three resonators
				// together, each at its own level.
				shellOut += (mode.x >> 9) * modeLevelQ8[m];
			}

			// A long-ringing shell builds up a far bigger signal than a dead
			// one.  shellGain (see Init) scales it back, so that the Material
			// fader changes the tone and not the volume.  It is done here at
			// the output, not at the input, so that the resonators themselves
			// work with the biggest numbers exactly when they ring longest.
			// 64-bit multiply: both numbers can be in the tens of millions.
			shellOut = int32_t((int64_t(shellOut) * shellGain) >> 16);

			// A dead material lets some of the raw click through directly,
			// like the paper-thin wall of a gourd.  A metal shell does not.
			out[ch] = shellOut + (excitation >> 8) * directLevelQ8;
		}

		outL = out[0];
		outR = out[1];
	}

private:
	//------------------------------------------------------------------------
	// ControlTick: layer 1, the grain-mass physics.  Runs every millisecond.
	//------------------------------------------------------------------------
	void ControlTick()
	{
		// Copy the settings out of the tables.  Clamp() guards against a
		// value outside 0..127 reading beyond the end of an array, which in
		// C++ is not caught for you - it silently reads rubbish.
		const int32_t sens = sensitivityQ8[Index(params.sensitivity)];
		const int32_t boxHalf = travelLength[Index(params.travel)];
		grainGain = (grainGainQ8[Index(params.grains)]
		           * hardnessGainQ8[Index(params.hardness)]) >> 8;
		clickDecay = clickDecayQ12[Index(params.hardness)];
		brightness = brightnessQ15[Index(params.hardness)];

		// --- Move the lump of grains ----------------------------------------
		//
		// The same code runs twice: axis 0 is side-to-side, axis 1 is
		// front-to-back.  Each axis is its own little one-dimensional world
		// with a wall at +boxHalf and another at -boxHalf.
		int32_t fastest = 0;
		for (int axis = 0; axis < 2; axis++)
		{
			// The 8mu only reports 128 different tilt values, so its readings
			// arrive as a staircase.  Smooth the steps off first (the same
			// one-pole low-pass as before; "* 256" keeps 8 extra bits of
			// precision so the smoothing doesn't get stuck).
			motionSmooth[axis] += (motionIn[axis] * 256 - motionSmooth[axis]) >> 3;

			// Ignore very small tilts.  No table is perfectly level, and
			// without this "dead zone" the grains would creep to one side
			// and hiss forever.  Then apply the sensitivity control.
			int32_t accel = (DeadZone(motionSmooth[axis] >> 8, restThreshold) * sens) >> 8;

			// NEWTON'S LAWS, one millisecond at a time:
			//   acceleration changes velocity...
			velocity[axis] += accel;
			//   ...friction removes a little of the velocity (1/64th)...
			velocity[axis] -= velocity[axis] >> 6;
			//   ...and velocity changes position.
			position[axis] += velocity[axis];

			// Has it reached a wall?
			const bool hitFarWall = position[axis] > boxHalf;
			const bool hitNearWall = position[axis] < -boxHalf;
			if (hitFarWall || hitNearWall)
			{
				position[axis] = hitFarWall ? boxHalf : -boxHalf;

				// Only count it as an impact if the lump ARRIVED this tick.
				// If it was already against the wall a millisecond ago it is
				// just lying there being pressed into it, which is silent.
				int32_t impactSpeed = Abs(velocity[axis]);
				if (!restingOnWall[axis] && impactSpeed > minImpactSpeed)
				{
					// THUMP.  The harder the impact, the more the grains
					// are thrown about.
					AddEnergy(impactSpeed);
					wallHit = true;
					pendingHit = true;

					// A pile of grains does not bounce like a ball; it
					// rebounds at a quarter of the speed, which gives a
					// couple of quick, quieter after-hits as it settles.
					velocity[axis] = -velocity[axis] / 4;
				}
				else
				{
					velocity[axis] = 0;   // come to rest against the wall
				}
				restingOnWall[axis] = true;
			}
			else
			{
				restingOnWall[axis] = false;   // out in the middle of the box
			}

			if (Abs(velocity[axis]) > fastest) fastest = Abs(velocity[axis]);
		}

		// Grains that are SLIDING along the shell also rattle, without
		// needing to hit a wall.  How much depends on Travel.  In a short
		// shaker the grains are thrown across in one go and make almost no
		// sound until they land, which is what gives a maraca its crisp
		// "chk".  In a long tube they tumble the whole way down, and that
		// steady trickle is the entire sound of a rainstick.
		int32_t stir = (fastest >> 7) * Index(params.travel);

		// Spinning the shaker swirls the grains round the inside.
		int32_t swirl = (DeadZone(Abs(twistIn), restThreshold) * sens) >> 5;
		if (swirl > stir) stir = swirl;

		// "Hold to roll" and the CV input work the same way.
		if (sustainQ16 > stir) stir = sustainQ16;

		// These don't ADD energy every tick (that would pile up without
		// limit); they set a floor that the energy cannot fall below.
		if (stir > maxEnergy) stir = maxEnergy;
		if (energyQ16 < stir) energyQ16 = stir;

		// Any jolts requested by Trigger() since the last tick.
		if (pendingTrigger > 0)
		{
			AddEnergy(pendingTrigger);
			pendingTrigger = 0;
			pendingHit = true;
		}

		// --- Let the rattle die away ----------------------------------------
		//
		// Lose a fixed fraction of the energy every tick.  A fist round the
		// shaker (choke) loses a quarter of it per millisecond instead.
		const int32_t loss = choke ? 16384 : energyLossQ16[Index(params.decay)];
		// 64-bit multiply, because energy (up to 2^17) times loss (up to 2^14)
		// could just overflow 32 bits.
		energyQ16 -= int32_t((int64_t(energyQ16) * loss) >> 16) + 1;

		// Real grains do not rattle ever more quietly for ever: once they
		// are barely moving, friction stops them dead.  Below about half a
		// percent of a full shake, the grains have settled.
		if (energyQ16 < settledEnergy) energyQ16 = 0;

		// --- How often do grains collide right now? -------------------------
		//
		// The Grains fader gives the collision rate when the grains are well
		// stirred up (a quarter of a full shake, or more).  Below that, as
		// the rattle dies down, fewer grains are still on the move, so
		// collisions get RARER as well as quieter.  This is what makes the
		// sound settle like a real shaker, rather than fading like a volume
		// knob being turned down.  (Both numbers are shifted down by 4 bits
		// first so that the multiplication fits in 32 bits.)
		const uint32_t moving = uint32_t(energyQ16 > 16384 ? 65536 : energyQ16 * 4);
		grainProb = ((grainProbQ24[Index(params.grains)] >> 4) * (moving >> 4)) >> 8;

		// Report the side-to-side position as -2047..2047.  Both numbers are
		// scaled down first so the multiplication stays within 32 bits.
		positionOut = (position[0] >> 11) * 2047 / (boxHalf >> 11);

		UpdateShell();
	}

	// Add to the rattle energy, never letting it go past the maximum.
	void AddEnergy(int32_t amountQ16)
	{
		if (amountQ16 > maxEnergy) amountQ16 = maxEnergy;
		energyQ16 += amountQ16;
		if (energyQ16 > maxEnergy) energyQ16 = maxEnergy;
	}

	// Work out the shell's resonator settings from Size, Material and Tune.
	void UpdateShell()
	{
		// A choked shell behaves like the deadest material there is.
		const int material = choke ? 0 : Index(params.material);
		shellR = shellRQ30[material];
		shellGain = shellGainQ16[material];

		// With Scatter at zero, put the three resonances back exactly on
		// their proper pitches.  Without this they would stay wherever the
		// last random retuning happened to leave them.
		if (params.scatter <= 0)
		{
			for (int m = 0; m < numModes; m++) scatterOffset[m] = 0;
		}

		// The lowest resonance: a big shell is low, a small one high.
		// Size 0..127 covers 0..296 table steps, a little over 4.5 octaves.
		basePitch = ((127 - Index(params.size)) * 7) / 3 + tune;

		// The first resonance is always at full level.  The two higher ones
		// are quiet in a dull material and strong in a ringing one, which is
		// a large part of why metal sounds like metal.
		modeLevelQ8[0] = 256;
		modeLevelQ8[1] = 80 + material;
		modeLevelQ8[2] = 40 + material;
		directLevelQ8 = 127 - material;

		for (int m = 0; m < numModes; m++) UpdateMode(m);
	}

	// Set the pitch of one resonator.
	void UpdateMode(int m)
	{
		int32_t i = Clamp(basePitch + modePitchOffset[m] + scatterOffset[m], 0, pitchTableSize - 1);
		// Multiply the shrink factor r into the cosine and sine, once, here,
		// so the audio loop doesn't have to.  Q30 times Q30 is Q60; shifting
		// down by 30 gives Q30 again.
		modeCosQ30[m] = ShiftTowardsZero(int64_t(shellR) * cosQ30[i], 30);
		modeSinQ30[m] = ShiftTowardsZero(int64_t(shellR) * sinQ30[i], 30);
	}

	//------------------------------------------------------------------------
	// Small helper functions.  "static" means they don't touch any of the
	// shaker's own variables - they just work on the numbers given to them.
	//------------------------------------------------------------------------

	static int32_t Clamp(int32_t v, int32_t lo, int32_t hi)
	{
		return v < lo ? lo : (v > hi ? hi : v);
	}

	static int32_t Abs(int32_t v) { return v < 0 ? -v : v; }

	// Divide a 64-bit number by 2^bits, rounding TOWARDS ZERO.
	//
	// A plain ">>" on a negative number rounds DOWN, away from zero: -5 >> 1
	// is -3, not -2.  For the resonators it matters that positive and
	// negative numbers are treated alike and both end up closer to zero, so
	// negative numbers are flipped positive, shifted, and flipped back.
	static int32_t ShiftTowardsZero(int64_t v, int bits)
	{
		return v >= 0 ? int32_t(v >> bits) : -int32_t((-v) >> bits);
	}

	// Divide a 64-bit number by 2^bits, rounding to the NEAREST whole number.
	// Adding half of the divisor first turns "round down" into "round to
	// nearest", in the same way that adding 0.5 before chopping off the
	// decimals does.
	static int32_t RoundingShift(int64_t v, int bits)
	{
		return int32_t((v + (int64_t(1) << (bits - 1))) >> bits);
	}

	// Make sure a setting is a legal table index.
	static int Index(int v) { return int(Clamp(v, 0, 127)); }

	// Values closer to zero than `zone` become zero; everything else is moved
	// in by `zone` so there is no sudden jump at the edge.
	static int32_t DeadZone(int32_t v, int32_t zone)
	{
		if (v > zone) return v - zone;
		if (v < -zone) return v + zone;
		return 0;
	}

	// An exponential sweep from `from` (at t = 0) to `to` (at t = 1).
	// Only used in Init(), so floating point is fine.
	static float Sweep(float from, float to, float t)
	{
		return from * powf(to / from, t);
	}

	// A random number generator ("xorshift").  Three shift-and-XOR steps
	// scramble the bits thoroughly enough to sound like perfect white noise,
	// and it is extremely fast.  It must never be started at zero.
	uint32_t Random()
	{
		randomState ^= randomState << 13;
		randomState ^= randomState >> 17;
		randomState ^= randomState << 5;
		return randomState;
	}

	//------------------------------------------------------------------------
	// Constants
	//------------------------------------------------------------------------

	static constexpr float pi = 3.14159265f;
	static constexpr float q30 = 1073741824.0f;   // 2 to the power 30

	// Pitch table: 416 steps of 1/64th of an octave, from 200 Hz to 18 kHz.
	static constexpr int pitchTableSize = 416;
	static constexpr int pitchStepsPerOctave = 64;
	static constexpr float lowestFreq = 200.0f;

	// The shell has three resonances ("modes").  Real objects ring at several
	// frequencies at once, and they are not neat multiples of each other.
	// These offsets put the upper two at about 1.6x and 2.4x the lowest.
	static constexpr int numModes = 3;
	static constexpr int32_t modePitchOffset[numModes] = {0, 42, 81};

	static constexpr int32_t maxEnergy = 131072;       // 2.0 in Q16
	static constexpr int32_t settledEnergy = 300;      // below this, grains stop
	static constexpr int32_t maxClickLevel = 1 << 20;
	static constexpr int32_t maxModeLevel = 1 << 29;
	// Below this the resonators round towards zero, to come to rest.
	static constexpr int32_t settleLevel = 1 << 14;

	// Tilt smaller than this is ignored: about 3 degrees.
	static constexpr int32_t restThreshold = 110;
	// A wall hit slower than this makes no sound.
	static constexpr int32_t minImpactSpeed = 400;

	//------------------------------------------------------------------------
	// Lookup tables, filled in by Init()
	//------------------------------------------------------------------------

	int32_t cosQ30[pitchTableSize];
	int32_t sinQ30[pitchTableSize];
	int32_t shellRQ30[128];
	int32_t shellGainQ16[128];
	uint32_t grainProbQ24[128];
	int32_t grainGainQ8[128];
	int32_t clickDecayQ12[128];
	int32_t brightnessQ15[128];
	int32_t hardnessGainQ8[128];
	int32_t energyLossQ16[128];
	int32_t sensitivityQ8[128];
	int32_t travelLength[128];

	//------------------------------------------------------------------------
	// State: everything the shaker remembers from one sample to the next
	//------------------------------------------------------------------------

	// Inputs
	int32_t motionIn[2] = {0, 0};
	int32_t twistIn = 0;
	int32_t tune = 0;
	int32_t width = 256;
	int32_t pendingTrigger = 0;
	int32_t sustainQ16 = 0;
	bool choke = false;

	// Layer 1: the lump of grains
	int tickCounter = 0;
	int32_t motionSmooth[2] = {0, 0};
	int32_t velocity[2] = {0, 0};
	int32_t position[2] = {0, 0};
	bool restingOnWall[2] = {false, false};
	int32_t positionOut = 0;
	int32_t energyQ16 = 0;
	bool wallHit = false;

	// Layer 2: grain collisions
	uint32_t randomState = 0x2F6E2B1;
	uint32_t grainProb = 0;
	int32_t grainGain = 0;
	int32_t clickDecay = 0;
	int32_t brightness = 0;
	int32_t clickLevel[2] = {0, 0};
	int32_t noiseLP[2] = {0, 0};
	bool grainHit = false;
	bool pendingHit = false;
	int scatterTurn = 0;
	int32_t scatterOffset[numModes] = {0, 0, 0};

	// Layer 3: the shell
	struct Mode
	{
		int32_t x = 0;   // the tip of the spinning arrow: x is the sound,
		int32_t y = 0;   // y is its hidden partner a quarter-turn behind
	};
	Mode modes[2][numModes];   // [left or right][which resonance]
	int32_t modeCosQ30[numModes] = {0, 0, 0};   // r * cos(angle)
	int32_t modeSinQ30[numModes] = {0, 0, 0};   // r * sin(angle)
	int32_t modeLevelQ8[numModes] = {256, 0, 0};
	int32_t directLevelQ8 = 0;
	int32_t shellR = 0;
	int32_t shellGain = 0;
	int32_t basePitch = 0;
};

#endif // SHAKER_H
