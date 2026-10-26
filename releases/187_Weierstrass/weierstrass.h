// WEIERSTRASS - the engine.
//
// Weierstrass (1872) wrote down a curve that is continuous everywhere and
// differentiable nowhere:
//
//     W(x) = sum_n  a^n cos(b^n * 2pi x)        0 < a < 1,  b > 1 integer
//
// Zoom in by a factor of b and you get the same curve back: every term has
// moved down one place, and the next term up has come out of the fine detail
// to take the top place. That self-similarity is the whole card.
//
// Playing W as audio means reading it at some speed. Term n then sounds at
// b^n times the read rate, so the partials are spaced by the ratio b. Zooming
// changes the read rate. Zooming in lowers every partial, and new detail
// arrives from above. One zoom step of b moves each partial onto the place
// of the one below, so zooming at a steady rate gives a Shepard-Risset
// glissando, with partials spaced by b instead of by an octave. With b = 2 it
// is the classic Shepard tone.
//
// Two things make the zoom loop with no seam:
//
// 1. A LENS. Amplitudes are a function of the continuous position m = n + z,
//    where z in [0,1) is how far through one zoom step we are. The weight is
//    a^m, with a raised-cosine fade over the first unit of m (the bottom of
//    the window) and over the last unit (the top). At the moment z wraps
//    round, every partial has the weight its neighbour had a moment ago.
//
// 2. PHASE COHERENCE. The real function has all its terms locked to one read
//    position X: term n is at phase b^n X. The engine keeps exactly that. It
//    stores ONE 32-bit read phase P, and partial k is at b^k * P mod 2^32.
//    Multiplying mod 2^32 is exact, so the partials can never drift apart.
//    When the zoom wraps and the labels shift, P is replaced by P/b or P*b,
//    and every partial keeps its phase. (Integer division drops at most b-1
//    LSBs of 2^32. Even multiplied up through the top partial, that is
//    millionths of a cycle.)
//
// SCATTER adds a fixed random phase offset to each partial. The spectrum is
// unchanged, but the spiky fractal waveform turns into a smooth Shepard
// cloud. Switching between the two lets you hear what phase coherence alone
// does.
//
// All integer arithmetic. The RP2040 has no FPU. This header has no Pico
// dependencies, so tools/render.cpp builds it on a desktop and tests it.

#pragma once
#include <stdint.h>
#include <math.h>

#ifndef __not_in_flash_func
#define __not_in_flash_func(f) f
#endif

namespace weier
{

constexpr int kMaxPartials = 13;
constexpr int kControlPeriod = 16;        // samples per control tick (3 kHz)
constexpr int kControlShift = 4;
constexpr uint32_t kInc20Hz = 1789570;    // 2^32 * 20 / 48000
constexpr uint32_t kNyqLo = 1342177280u;  // 15 kHz: start fading partials out
constexpr uint32_t kNyqHi = 1700091221u;  // 19 kHz: fully gone
constexpr int kLfoShift = 10;             // CV Out 2 reads W 1024x slower

// Window (lowest partial) is 20 Hz * 2^window, clamped to 5 Hz..160 Hz.
constexpr int32_t kWindowMinQ16 = -2 * 65536;
constexpr int32_t kWindowMaxQ16 = 3 * 65536;

class Engine
{
public:
	Engine()
	{
		for (int i = 0; i <= 1024; i++)
			sine_[i] = int16_t(lrint(32767.0 * sin(2.0 * M_PI * i / 1024.0)));
		for (int i = 0; i <= 256; i++)
		{
			exp2_[i] = int32_t(lrint(16384.0 * pow(2.0, i / 256.0)));          // Q14, 1..2
			exp2neg_[i] = int32_t(lrint(32768.0 * pow(2.0, -i / 256.0)));      // Q15, 1..0.5
			fade_[i] = int32_t(lrint(32768.0 * 0.5 * (1.0 - cos(M_PI * i / 256.0))));
		}
		for (int b = 2; b <= 7; b++)
		{
			log2bQ12_[b] = int32_t(lrint(4096.0 * log2((double)b)));
			// Enough partials that, from the lowest window (5 Hz), the top one
			// is already past the anti-alias fade.
			int k = 1;
			double f = 5.0;
			while (f < 19000.0) { f *= b; k++; }
			partials_[b] = k < kMaxPartials ? k : kMaxPartials;
		}
		uint32_t s = 0x9E3779B9u;
		for (int k = 0; k < kMaxPartials; k++) offset_[k] = rng_ = Xorshift(s += 0x6D2B79F5u);

		ApplyRatio(3);
		SetRoughness(2048);
		Reset();
	}

	// --- controls, safe to call every sample --------------------------------

	// Zoom speed in Q32 zoom steps per sample, signed. Positive zooms OUT
	// (pitch rises), negative zooms IN (pitch falls, detail descends).
	void SetZoomRate(int32_t r) { rate_ = r; }

	// b, 2..7. Changing it is declicked: fade out, swap, fade in.
	void SetRatio(int b)
	{
		if (b < 2) b = 2;
		if (b > 7) b = 7;
		pendingRatio_ = b;
	}

	void SetScatter(bool on) { pendingScatter_ = on; }

	// 0..4095 -> fractal dimension D = 1.0 .. 1.95. The graph of W has
	// dimension D = 2 + log(a)/log(b), so a = b^(D-2). Setting D instead of a
	// means the same knob position sounds about equally rough at every ratio.
	void SetRoughness(int32_t x)
	{
		if (x < 0) x = 0;
		if (x > 4095) x = 4095;
		// (2 - D) in Q16: 1.0 down to 0.05
		roughQ16_ = 65536 - ((x * 973) >> 6);   // x * 0.95 * 65536 / 4095
	}

	// Window offset in Q16 octaves from 20 Hz.
	void SetWindow(int32_t octQ16)
	{
		if (octQ16 < kWindowMinQ16) octQ16 = kWindowMinQ16;
		if (octQ16 > kWindowMaxQ16) octQ16 = kWindowMaxQ16;
		window_ = octQ16;
	}

	// Back to x = 0 at the start of a zoom step: every partial at phase 0, the
	// one moment when the function's characteristic spike lines up.
	void Reset()
	{
		z_ = 0;
		P_ = 0;
		lfoP_ = 0;
	}

	// --- audio --------------------------------------------------------------

	// One sample. pm is Audio In 1 (+-2048), a time-shift of the read head.
	// Returns +1 / -1 on the sample a zoom step completes, else 0.
	int __not_in_flash_func(Process)(int32_t pm, int16_t &outCos, int16_t &outSin)
	{
		int wrapped = 0;

		uint32_t zNew = z_ + (uint32_t)rate_;
		if (rate_ > 0 && zNew < z_) { ShiftUp(); wrapped = 1; }
		else if (rate_ < 0 && zNew > z_) { ShiftDown(); wrapped = -1; }
		z_ = zNew;

		uint32_t d0 = BaseIncrement();
		P_ += d0;

		// The lens and the slow function are on different samples, so no
		// one sample pays for both.
		if (++tick_ >= kControlPeriod) tick_ = 0;
		if (tick_ == 0) UpdateLens(d0);
		else if (tick_ == kControlPeriod / 2) UpdateLfo(d0);

		// The input moves the read head in TIME (up to about 5 ms), not in
		// phase. Scaling by d0 keeps the depth continuous across a relabel,
		// because d0 jumps by exactly b there.
		uint32_t q = P_ + ((uint32_t)((int32_t)(d0 >> 8) * pm) << 5);

		int32_t accC = 0, accS = 0;
		const uint32_t b = ratio_;
		const bool sc = scatter_;
		for (int k = 0; k < K_; k++)
		{
			amp_[k] += step_[k];
			uint32_t ph = q + (sc ? offset_[k] : 0u);
			int32_t a = amp_[k];
			accC += a * Sine(ph + 0x40000000u);
			accS += a * Sine(ph);
			q *= b;
		}

		// Global declick gain, and the deferred ratio / scatter changes that
		// can only happen while it is at zero.
		bool pending = pendingRatio_ != ratio_ || pendingScatter_ != scatter_;
		if (pending)
		{
			gain_ -= 32;
			if (gain_ <= 0)
			{
				gain_ = 0;
				if (pendingScatter_ != scatter_) scatter_ = pendingScatter_;
				if (pendingRatio_ != ratio_) ApplyRatio(pendingRatio_);
			}
		}
		else if (gain_ < 32767)
		{
			gain_ += 32;
			if (gain_ > 32767) gain_ = 32767;
		}

		outCos = Clip(((accC >> 15) * gain_) >> 19);
		outSin = Clip(((accS >> 15) * gain_) >> 19);
		return wrapped;
	}

	// --- read-outs for the panel --------------------------------------------

	uint32_t Zoom() const { return z_; }
	int Ratio() const { return ratio_; }
	int Partials() const { return K_; }
	int32_t PartialLevel(int k) const { return k < K_ ? tgt_[k] : 0; }  // Q15
	int32_t Lfo() const { return lfo_; }                                // +-32767
	uint32_t BaseInc() const { return BaseIncrement(); }

private:
	static uint32_t Xorshift(uint32_t x)
	{
		x ^= x << 13; x ^= x >> 17; x ^= x << 5;
		return x;
	}

	static int16_t Clip(int32_t v)
	{
		return int16_t(v > 2047 ? 2047 : (v < -2048 ? -2048 : v));
	}

	int32_t __not_in_flash_func(Sine)(uint32_t ph) const
	{
		uint32_t i = ph >> 22;
		int32_t f = (ph >> 6) & 0xFFFF;
		int32_t s1 = sine_[i], s2 = sine_[i + 1];
		return s1 + (((s2 - s1) * f) >> 16);
	}

	// 2^(-e), e in Q16 >= 0, result Q15.
	int32_t __not_in_flash_func(Exp2Neg)(int32_t e) const
	{
		int32_t ip = e >> 16;
		if (ip > 15) return 0;
		int32_t f = e & 0xFFFF;
		int32_t i = f >> 8, r = f & 0xFF;
		int32_t m = exp2neg_[i] + (((exp2neg_[i + 1] - exp2neg_[i]) * r) >> 8);
		return m >> ip;
	}

	// Read rate of partial 0: 20 Hz * 2^(window + z*log2 b).
	uint32_t __not_in_flash_func(BaseIncrement)() const
	{
		int32_t e = window_ + (int32_t)(((z_ >> 16) * (uint32_t)log2bQ12_[ratio_]) >> 12);
		int32_t ip = e >> 16;
		int32_t f = e & 0xFFFF;
		int32_t i = f >> 8, r = f & 0xFF;
		uint32_t m = (uint32_t)(exp2_[i] + (((exp2_[i + 1] - exp2_[i]) * r) >> 8));
		uint32_t d = ((kInc20Hz >> 7) * m) >> 7;
		return ip >= 0 ? d << ip : d >> -ip;
	}

	// Zoom step completed going OUT: partial k becomes k+1, new bottom fades in.
	void __not_in_flash_func(ShiftUp)()
	{
		for (int k = K_ - 1; k > 0; k--)
		{
			amp_[k] = amp_[k - 1];
			step_[k] = step_[k - 1];
			tgt_[k] = tgt_[k - 1];
			offset_[k] = offset_[k - 1];
		}
		amp_[0] = step_[0] = tgt_[0] = 0;
		offset_[0] = rng_ = Xorshift(rng_);
		P_ /= ratio_;
		lfoP_ /= ratio_;
	}

	// Zoom step completed going IN: partial k becomes k-1, new top fades in.
	void __not_in_flash_func(ShiftDown)()
	{
		for (int k = 0; k < K_ - 1; k++)
		{
			amp_[k] = amp_[k + 1];
			step_[k] = step_[k + 1];
			tgt_[k] = tgt_[k + 1];
			offset_[k] = offset_[k + 1];
		}
		amp_[K_ - 1] = step_[K_ - 1] = tgt_[K_ - 1] = 0;
		offset_[K_ - 1] = rng_ = Xorshift(rng_);
		P_ *= ratio_;
		lfoP_ *= ratio_;
	}

	void __not_in_flash_func(ApplyRatio)(int b)
	{
		ratio_ = b;
		K_ = partials_[b];
		ComputeTargets(BaseIncrement());
		for (int k = 0; k < kMaxPartials; k++)
		{
			amp_[k] = k < K_ ? tgt_[k] : 0;
			step_[k] = 0;
		}
	}

	// The lens: weight of each partial at the current zoom, before and after
	// the anti-alias fade. Fills tgt_ (audio, normalised) and lens_ (raw).
	void __not_in_flash_func(ComputeTargets)(uint32_t d0)
	{
		const int32_t zq = (int32_t)(z_ >> 16);
		const int32_t fz = fade_[zq >> 8] + (((fade_[(zq >> 8) + 1] - fade_[zq >> 8]) * (zq & 0xFF)) >> 8);
		const int32_t slope = (roughQ16_ * log2bQ12_[ratio_]) >> 12;  // -log2(a), Q16
		const uint32_t b = ratio_;

		int32_t sum = 0, lensSum = 0;
		uint32_t inc = d0;
		int32_t raw[kMaxPartials];
		for (int k = 0; k < K_; k++)
		{
			int32_t m = (k << 16) + zq;
			int32_t w = Exp2Neg(((m >> 4) * (slope >> 4)) >> 8);
			if (k == 0) w = (w * fz) >> 15;
			if (k == K_ - 1) w = (w * (32768 - fz)) >> 15;
			lens_[k] = w;
			lensSum += w;

			if (inc >= kNyqHi) w = 0;
			else if (inc > kNyqLo) w = (w * (int32_t)((kNyqHi - inc) >> 13)) / (int32_t)((kNyqHi - kNyqLo) >> 13);
			raw[k] = w;
			sum += w;

			inc = inc > 0xFFFFFFFFu / b ? 0xFFFFFFFFu : inc * b;
		}
		// One division each, then multiplies: this runs inside the audio
		// interrupt and every cycle on this sample counts.
		const int32_t inv = sum > 0 ? (32767 << 14) / sum : 0;           // Q14
		const int32_t lensInv = lensSum > 0 ? (32767 << 14) / lensSum : 0;
		for (int k = 0; k < K_; k++)
		{
			tgt_[k] = (raw[k] * inv) >> 14;
			lens_[k] = (lens_[k] * lensInv) >> 14;
		}
	}

	void __not_in_flash_func(UpdateLens)(uint32_t d0)
	{
		ComputeTargets(d0);
		for (int k = 0; k < K_; k++)
			step_[k] = (tgt_[k] - amp_[k]) >> kControlShift;
	}

	// CV Out 2: the same zooming function, 1024 times slower, with the same
	// lens and no anti-alias fade.
	void __not_in_flash_func(UpdateLfo)(uint32_t d0)
	{
		lfoP_ += (d0 >> kLfoShift) << kControlShift;
		uint32_t q = lfoP_;
		int32_t acc = 0;
		for (int k = 0; k < K_; k++)
		{
			acc += lens_[k] * Sine(q + 0x40000000u);
			q *= ratio_;
		}
		lfo_ = acc >> 15;
	}

	int16_t sine_[1025];
	int32_t exp2_[257], exp2neg_[257], fade_[257];
	int32_t log2bQ12_[8] = {0};
	int partials_[8] = {0};

	int32_t amp_[kMaxPartials] = {0}, step_[kMaxPartials] = {0}, tgt_[kMaxPartials] = {0};
	int32_t lens_[kMaxPartials] = {0};
	uint32_t offset_[kMaxPartials];
	uint32_t rng_ = 1;

	uint32_t z_ = 0, P_ = 0, lfoP_ = 0;
	int32_t rate_ = 0;
	int32_t window_ = 0;
	int32_t roughQ16_ = 32768;
	int32_t lfo_ = 0;
	int ratio_ = 3, pendingRatio_ = 3, K_ = 2;
	bool scatter_ = false, pendingScatter_ = false;
	int32_t gain_ = 32767;
	int tick_ = 0;
};

}  // namespace weier
