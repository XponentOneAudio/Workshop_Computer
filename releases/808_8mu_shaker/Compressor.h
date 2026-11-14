// Compressor.h
//
// A stereo "parallel compressor" for the output of the shaker.
//
// WHAT A COMPRESSOR DOES.  It is an automatic volume control: it listens to
// how loud the sound is and turns the volume DOWN when it is loud and UP
// when it is quiet.  Loud and quiet sounds come out closer together in
// level, and the quiet tail of each sound is lifted up where it can be
// heard.
//
// WHAT "PARALLEL" MEANS.  A compressor on its own flattens the sharp start
// of each hit (the "transient"), which makes percussion sound dull.  So
// instead of replacing the sound with the compressed version, we ADD the
// compressed version to the untouched original:
//
//     output = original + (compressed x amount)
//
// The original supplies the crisp attack of every hit; the compressed copy
// supplies a thick, even body and a long audible tail underneath it.
//
// Like Shaker.h, this file has no hardware code in it, and uses only whole
// number (fixed-point) maths.  See the notes at the top of Shaker.h.

#ifndef COMPRESSOR_H
#define COMPRESSOR_H

#include <stdint.h>

class Compressor
{
public:
	// Process one stereo sample.
	//
	// left, right:  the sound, in the same large units Shaker::Process()
	//               produces.  They are changed in place (the & again).
	// amountQ12:    how much compressed signal to add: 0 = none, 4096 = full.
	void Process(int32_t &left, int32_t &right, int32_t amountQ12)
	{
		// --- 1. Measure how loud the sound is right now ----------------------
		//
		// Take whichever channel is louder, ignoring sign.  Using one
		// measurement for both channels means both get exactly the same
		// gain, so the stereo picture does not wobble from side to side.
		const int32_t absL = left < 0 ? -left : left;
		const int32_t absR = right < 0 ? -right : right;
		const int32_t level = absL > absR ? absL : absR;

		// An "ENVELOPE FOLLOWER" turns the wiggling audio into a smooth
		// line that traces its loudness.  It rises quickly when the sound
		// gets louder (the "attack", about a fifth of a millisecond here)
		// and falls slowly when it gets quieter (the "release", about
		// 85 ms).  The slow fall is what makes it follow the overall
		// loudness and not every individual wave.
		if (level > envelope)
		{
			envelope += (level - envelope) >> attackShift;
		}
		else
		{
			envelope -= envelope >> releaseShift;
		}

		// --- 2. Work out the gain --------------------------------------------
		//
		// We want the compressed copy to come out at the same level
		// (`target`) however loud the input was.  The gain that does that
		// is simply target / envelope: a sound at half the target level
		// needs a gain of 2, a sound at twice the target needs 0.5.
		//
		// The gain is not allowed to go above maxGain (16x).  Otherwise
		// silence would be amplified without limit.  Limiting the smallest
		// number we divide by is the same thing as limiting the gain.
		int32_t divisor = envelope;
		if (divisor < target / maxGain) divisor = target / maxGain;
		const int32_t gainQ8 = (target * 256) / divisor;    // Q8: 256 = 1x

		// --- 3. Make the compressed copy and mix it in -----------------------
		left = Mix(left, gainQ8, amountQ12);
		right = Mix(right, gainQ8, amountQ12);
	}

private:
	static int32_t Mix(int32_t dry, int32_t gainQ8, int32_t amountQ12)
	{
		// The compressed ("wet") copy.  64-bit, as both numbers are large.
		int64_t wet = (int64_t(dry) * gainQ8) >> 8;

		// In the first instant of a hit, before the envelope has caught
		// up, the gain is still high and the wet copy would leap far above
		// the target.  Trim that spike off.  The untouched original is
		// providing the attack; the wet copy's job is the body.
		if (wet > 2 * target) wet = 2 * target;
		if (wet < -2 * target) wet = -2 * target;

		// original + (compressed x amount)
		return dry + int32_t((wet * amountQ12) >> 12);
	}

	// The level the compressed copy is held at.  In the shaker's output
	// units this is about three-quarters of the way to where the soft
	// clipper starts, with the volume at its normal (unity) setting.
	static constexpr int32_t target = 200000;

	// The most the compressor may turn the sound up: 16 times.
	static constexpr int32_t maxGain = 16;

	// Each of these is "divide the difference by 2^shift every sample".
	// A bigger shift is a slower response.
	static constexpr int attackShift = 3;      // about 8 samples, 0.2 ms
	static constexpr int releaseShift = 12;    // about 4096 samples, 85 ms

	int32_t envelope = 0;
};

#endif // COMPRESSOR_H
