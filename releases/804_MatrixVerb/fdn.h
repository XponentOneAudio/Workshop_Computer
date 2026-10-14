// MatrixVerb: the feedback delay network, in fixed point.
//
// Eight delay lines run at 24kHz (half the sample rate, so that each can be
// up to 340ms long in the RAM there is), with a 47-tap half-band filter
// each way between the two rates.  Every sample of the network:
//
//   y[i]  = line i read at its delay (between samples, gliding, modulated)
//   wet  += y[i] x line i's output gain and pan
//   f[i]  = y[i] through line i's decay gain and damping low-pass (Jot's
//           method), then the low cut
//   x[i]  = sum over j of M[i][j] f[j]  +  line i's share of the input
//   line i is written with x[i], through a soft saturation
//
// M is the feedback matrix, already normalised by design.h so that it can't
// amplify anything (spectral norm under 1).  The input runs through a tone
// low-pass, a predelay and four allpass diffusers on its way in.
//
// Safety, from the inside out:
//   1. The matrix is normalised (design.h), and changes to it are crossfaded
//      as a blend of two normalised matrices, which is normalised too.
//   2. Every line is written through a soft saturation, so nothing in the
//      network can grow past full scale, whatever the settings.
//   3. A guard watches the network's level: kept near full scale for a
//      quarter of a second, it pulls every line's feedback down until the
//      level falls, then lets go slowly.
//   4. The wet signal has its DC removed and goes through a look-ahead peak
//      limiter (1.3ms), which turns it down smoothly before a peak arrives,
//      so it never passes the Ceiling and never clips.
//   5. Power-up, Clear and loading a preset mute the wet signal and empty
//      the network, then fade back in.
//   6. The final output is soft clipped above three quarters of full scale.
//
// Nothing here touches the hardware, so test/host_test.cpp builds it on a
// computer too.

#ifndef MATRIXVERB_FDN_H
#define MATRIXVERB_FDN_H

#include "design.h"

namespace mv
{

// Storage, sized to fit the RP2040's RAM alongside the program
static int16_t gLines[kLines][kLineSize];
static int16_t gPre[2][kPredelaySize];
static constexpr int kStages = 4;
static constexpr int kApMax = 352;
static constexpr int kApLen[2][kStages] = {{113, 167, 241, 337}, {127, 179, 229, 349}};
static int16_t gAp[2][kStages][kApMax];
// tanh(x) for x = 0 to 4 in 512 steps, Q15; and one cycle of a sine, Q15
static int16_t gTanh[513];
static int16_t gSine[257];

class Engine
{
public:
	void Init()
	{
		for (int i = 0; i <= 512; i++) gTanh[i] = int16_t(32767.0f * tanhf(float(i) / 128.0f));
		for (int i = 0; i <= 256; i++) gSine[i] = int16_t(32767.0f * sinf(float(i) * (6.2831853f / 256.0f)));
		memset(gLines, 0, sizeof(gLines));
		memset(gPre, 0, sizeof(gPre));
		memset(gAp, 0, sizeof(gAp));
		for (int i = 0; i < kLines; i++) lfo[i] = uint32_t(i) << 29;
		clearTicks = kStartTicks;
	}

	// A new design from core1.  The first one is taken as it is; later ones
	// are glided to.
	void SetDesign(const Design &d)
	{
		dsg = d;
		if (!haveDesign)
		{
			haveDesign = true;
			for (int i = 0; i < kLines; i++)
			{
				g[i] = d.g[i];
				damp[i] = d.damp[i];
				inL[i] = d.inL[i]; inR[i] = d.inR[i];
				outL[i] = d.outL[i]; outR[i] = d.outR[i];
				glide[i] = int32_t(d.len[i]) << 8;
				dl[i] = uint32_t(d.len[i]) << 16;
			}
			memcpy(matCur, d.mat, sizeof(matCur));
			memcpy(matTgt, d.mat, sizeof(matTgt));
			matT = 4096;
			hpA = d.hpA;
			inGain = d.inGain;
			outGain = d.outGain;
			pdCur = d.predelay;
			diffuse = d.diffuse;
			dryGain = d.dryGain;
			wetGain = d.wetGain;
		}
		MatrixTarget(frozen ? d.matFreeze : d.mat);
	}

	void SetFreeze(bool on)
	{
		if (on == frozen) return;
		frozen = on;
		if (haveDesign) MatrixTarget(frozen ? dsg.matFreeze : dsg.mat);
	}
	bool Frozen() const {return frozen;}

	// Empty the network: the wet signal and all feedback are muted for as
	// long as the longest line, then fade back.  Held, it keeps clearing.
	void Clear() {clearTicks = kClearTicks;}
	void SetClearHeld(bool on) {clearHeld = on;}
	bool Clearing() const {return clearTicks > 0 || clearHeld;}

	// A click into the network: into one line (0-7), or through the input
	// vector (-1)
	void Impulse(int line) {impulse = line < -1 ? -1 : (line > 7 ? -1 : line); impulsePending = true;}

	// One sample at 48kHz.  In and out are Q15 (32768 = full scale).
	void Process(int32_t inL48, int32_t inR48, int32_t &outLeft, int32_t &outRight)
	{
		slot = int(sampleCount & 31);
		bool odd = (sampleCount++ & 1) != 0;

		// Control, shared out over the 32 samples so that no one sample has
		// too much to do
		if (slot == 0) ControlGlobal();
		else if ((slot & 3) == 2) ControlLine(slot >> 2);
		else if (slot == 12) ControlMatrix();

		// The input's history at 48kHz, for the decimator (each sample
		// stored twice, so a window of it is always in one piece)
		hinPos = (hinPos + 1) & (kHist - 1);
		hinL[hinPos] = hinL[hinPos + kHist] = int16_t(Clip(inL48, 32767));
		hinR[hinPos] = hinR[hinPos + kHist] = int16_t(Clip(inR48, 32767));

		// The network's work is shared over each pair of samples: the first
		// reads the lines, the second mixes them and writes them back.  The
		// rate changes are shared the same way: the first sample makes the
		// next input frame (from the pair just finished) and the in-between
		// output sample; the second makes the wet frame.
		int32_t wl, wr;
		if (!odd)
		{
			const int16_t *pl = hinL + hinPos + kHist - (kHbTaps - 1);
			const int16_t *pr = hinR + hinPos + kHist - (kHbTaps - 1);
			frameL = Decimate(pl);
			frameR = Decimate(pr);
			ReadLines();
			// The output between the last two frames' centre samples.  A
			// proper interpolation can peak a little between two limited
			// frames, as a true peak can, so it's held to the ceiling too.
			const int16_t *ql = foutL + foutPos + kFrames - (2 * kHbK - 1);
			const int16_t *qr = foutR + foutPos + kFrames - (2 * kHbK - 1);
			int32_t c = ceiling;
			wl = Interpolate(ql);
			wr = Interpolate(qr);
			wl = wl > c ? c : (wl < -c ? -c : wl);
			wr = wr > c ? c : (wr < -c ? -c : wr);
		}
		else
		{
			WriteLines(frameL, frameR);
			// The new wet frame: muted while clearing, DC removed, limited,
			// then into the interpolator's history
			int32_t w0 = Clip((int32_t(int64_t(accL) * outGain >> 12)), 1 << 20);
			int32_t w1 = Clip((int32_t(int64_t(accR) * outGain >> 12)), 1 << 20);
			w0 = int32_t((int64_t(w0) * wetMute) >> 15);
			w1 = int32_t((int64_t(w1) * wetMute) >> 15);
			w0 -= OnePole(dcL, w0, kDcCoef);
			w1 -= OnePole(dcR, w1, kDcCoef);
			Limit(w0, w1);
			foutPos = (foutPos + 1) & (kFrames - 1);
			foutL[foutPos] = foutL[foutPos + kFrames] = int16_t(w0);
			foutR[foutPos] = foutR[foutPos + kFrames] = int16_t(w1);
			// On the frames themselves the interpolation is just the frame,
			// kHbK frames back (the filter's delay)
			wl = foutL[foutPos + kFrames - kHbK];
			wr = foutR[foutPos + kFrames - kHbK];
		}

		// Dry and wet
		outLeft = OutputClip((inL48 * dryGain + wl * wetGain) >> 12);
		outRight = OutputClip((inR48 * dryGain + wr * wetGain) >> 12);

		// Levels
		Peak(peakIn, inL48); Peak(peakIn, inR48);
		Peak(peakOut, outLeft); Peak(peakOut, outRight);
		Peak(peakWet, wl); Peak(peakWet, wr);
	}

	// Which of the 32 samples of the control period the last Process() was,
	// so the card can do its own control work on the free ones (16, 20, 24,
	// 28)
	int Slot() const {return slot;}

	// For the LEDs, the CV outputs and the web editor, collected each control
	// period: levels 0-32767, the guard's gain and the limiter's least gain
	// lately (Q15), the delays as they are now (samples)
	int32_t lvIn = 0, lvOut = 0, lvWet = 0, lvTank = 0;
	int32_t guard = 32768;
	int32_t limGain = 32768;
	int32_t LineDelay(int i) const {return int32_t(dl[i] >> 16);}
	bool Guarding() const {return guard < 32000;}

private:
	Design dsg = {};
	bool haveDesign = false;
	bool frozen = false;
	bool clearHeld = false;
	int clearTicks = 0;
	volatile bool impulsePending = false;
	volatile int impulse = -1;

	// Control timing: a tick is one control period, 32 samples (1.5kHz)
	static constexpr int kClearTicks = 560;       // 0.37s: longer than the longest line
	static constexpr int kStartTicks = 900;       // 0.6s at power-up
	uint32_t sampleCount = 0;
	int slot = 0;

	// Changing rate, 48kHz to 24kHz and back: a 47-tap half-band FIR each
	// way, flat to 10kHz and at least 55dB down from 14kHz, so the input
	// doesn't alias into the network and the output leaves no image near
	// the top of the audio range.  Half its taps are zero, and it's
	// symmetric, so each side tap (at odd distances 1, 3 ... 23 from the
	// centre, Q14) is one multiply for a pair of samples.  A Kaiser-windowed
	// sinc, beta 6.
	static constexpr int kHbK = 12;
	static constexpr int kHbTaps = 4 * kHbK - 1;
	static constexpr int16_t kHb[kHbK] = {5188, -1659, 915, -575, 375, -245, 155, -94, 53, -27, 12, -3};
	static constexpr int kHist = 64;          // input samples kept (at least kHbTaps)
	static constexpr int kFrames = 32;        // wet frames kept (at least 2 kHbK)
	int16_t hinL[2 * kHist] = {}, hinR[2 * kHist] = {};
	int16_t foutL[2 * kFrames] = {}, foutR[2 * kFrames] = {};
	int hinPos = 0, foutPos = 0;
	int32_t frameL = 0, frameR = 0;           // the next input frame, Q15

	// 48kHz to 24kHz: the filter centred kHbTaps / 2 samples back, taken
	// once a pair.  p holds the last kHbTaps samples, oldest first.
	static inline int32_t Decimate(const int16_t *p)
	{
		const int16_t *c = p + 2 * kHbK - 1;
		int32_t acc = 8192 * int32_t(*c);
		for (int j = 0; j < kHbK; j++) acc += kHb[j] * (int32_t(c[-(2 * j + 1)]) + int32_t(c[2 * j + 1]));
		return (acc + 8192) >> 14;
	}

	// 24kHz to 48kHz, the sample halfway between frames kHbK and kHbK - 1
	// back (doubled, as half the samples are the zeros put between frames).
	// q holds the last 2 kHbK frames, oldest first.
	static inline int32_t Interpolate(const int16_t *q)
	{
		int32_t acc = 0;
		for (int j = 0; j < kHbK; j++) acc += kHb[j] * (int32_t(q[kHbK - 1 - j]) + int32_t(q[kHbK + j]));
		return (acc + 4096) >> 13;
	}

	// Lines
	int w = 0;                                // write position, shared
	uint32_t dl[kLines] = {};                 // each line's delay now, Q16
	int32_t dInc[kLines] = {};                // its step a network sample
	int32_t glide[kLines] = {};               // its length, glided, Q8
	uint32_t lfo[kLines] = {};
	int32_t lp[kLines] = {};                  // damping filter states, Q19
	int64_t hp[kLines] = {};                  // low cut states, Q35
	int32_t f[kLines] = {};                   // filtered, for the matrix, Q15
	int32_t accL = 0, accR = 0;               // the wet sum, Q15

	// Coefficients, glided to the design's
	int32_t g[kLines] = {}, gEff[kLines] = {}, damp[kLines] = {};
	int32_t inL[kLines] = {}, inR[kLines] = {}, outL[kLines] = {}, outR[kLines] = {};
	int16_t matCur[64] = {}, matPrev[64] = {}, matTgt[64] = {};
	int32_t matT = 4096;
	static constexpr int32_t kMatStep = 86;   // the matrix crossfades in ~30ms
	int32_t hpA = 171, toneA = 65536;
	int32_t inGain = 0, outGain = 4096, diffuse = 0;
	int32_t drive = 256, invDrive = 65536;
	int32_t dryGain = 4096, wetGain = 4096;
	int32_t ceiling = 23197;

	// Input chain
	int64_t toneL = 0, toneR = 0;
	int pw = 0;                   // predelay write position
	int32_t pdCur = 0;            // predelay now, Q8 samples
	int apPos[2][kStages] = {};

	// Gates, Q15: the input (freeze), the feedback and the wet (clear)
	int32_t inGate = 32768, fbGate = 0, wetMute = 0;

	// Safety
	static constexpr int32_t kGuardLevel = 22937;     // 0.7 of full scale
	int32_t tankEnv = 0;
	int guardOver = 0;
	int32_t limMin = 32768;
	int64_t dcL = 0, dcR = 0;
	static constexpr int32_t kDcCoef = 86;            // ~5Hz at 24kHz, Q16

	// The limiter, at 24kHz: the wet signal delayed by kLook - 1 frames
	// (1.3ms), each frame's gain needed (Q15), the gain held and released,
	// and the last kLook held gains with their sum, for the moving average
	static constexpr int kLook = 32;
	static constexpr int kLimHold = 720;              // 30ms
	int32_t limDelL[kLook] = {}, limDelR[kLook] = {};
	int32_t limNeed[kLook], limBox[kLook];
	int32_t limSum = 32768 * kLook, limHeld = 32768;
	int limPos = 0, limHoldCount = 0;
	bool limInit = false;

	// Peaks over a control period
	int32_t peakIn = 0, peakOut = 0, peakWet = 0, peakX = 0;

	static inline int32_t Clip(int32_t v, int32_t m)
	{
		return v > m ? m : (v < -m - 1 ? -m - 1 : v);
	}
	static inline void Peak(int32_t &p, int32_t v)
	{
		if (v < 0) v = -v;
		if (v > p) p = v;
	}

	// One-pole low-pass, state Q16 of the signal
	static inline int32_t OnePole(int64_t &y, int32_t x, int32_t a)
	{
		y += (((int64_t(x) << 16) - y) * a) >> 16;
		return int32_t((y + 32768) >> 16);
	}

	// tanh, Q15 in (32768 = 1.0) and out
	static inline int32_t Tanh(int32_t v)
	{
		uint32_t a = uint32_t(v < 0 ? -v : v);
		uint32_t i = a >> 8;
		int32_t r;
		if (i >= 512) r = gTanh[512];
		else
		{
			int32_t fr = int32_t(a & 255);
			r = gTanh[i] + (((gTanh[i + 1] - gTanh[i]) * fr) >> 8);
		}
		return v < 0 ? -r : r;
	}

	// Clean to half of full scale, then rounding off into full scale
	static inline int32_t SoftClip(int32_t v)
	{
		int32_t a = v < 0 ? -v : v;
		if (a <= 16384) return v;
		if (a > 0x3FFFFF) a = 0x3FFFFF;
		int32_t r = 16384 + (Tanh((a - 16384) * 2) >> 1);
		return v < 0 ? -r : r;
	}

	// The very last stage, on dry and wet together: clean to three quarters
	// of full scale, then rounding off into full scale
	static inline int32_t OutputClip(int32_t v)
	{
		int32_t a = v < 0 ? -v : v;
		if (a <= 24576) return v;
		if (a > 0x3FFFFF) a = 0x3FFFFF;
		int32_t r = 24576 + (Tanh((a - 24576) * 4) >> 2);
		return v < 0 ? -r : r;
	}

	// The look-ahead limiter, on one frame of the wet signal at 24kHz.  Each
	// frame's needed gain (ceiling / its peak) goes into a window of the
	// next kLook frames; the gain held is the least in the window, so it
	// comes down before any peak arrives.  A moving average over the same
	// kLook frames turns that into a smooth ramp of 1.3ms, and since every
	// gain it averages is at most what the frame leaving the delay needs,
	// that frame is always at or under the ceiling, with no clipping.  The
	// held gain stays down for 30ms after the last peak (so it doesn't
	// ripple within a low note's cycle), then comes back over about 170ms.
	void Limit(int32_t &l, int32_t &r)
	{
		if (!limInit)
		{
			limInit = true;
			for (int k = 0; k < kLook; k++) limNeed[k] = limBox[k] = 32768;
		}
		int32_t c = ceiling;
		int32_t a = l < 0 ? -l : l, b = r < 0 ? -r : r;
		if (b > a) a = b;
		limNeed[limPos] = a > c ? int32_t((int64_t(c) << 15) / a) : 32768;
		int32_t m = 32768;
		for (int k = 0; k < kLook; k++) if (limNeed[k] < m) m = limNeed[k];
		if (m <= limHeld)
		{
			limHeld = m;
			limHoldCount = kLimHold;
		}
		else if (limHoldCount > 0) limHoldCount--;
		else
		{
			limHeld += ((32768 - limHeld) >> 12) + 1;
			if (limHeld > m) limHeld = m;
		}
		limSum += limHeld - limBox[limPos];
		limBox[limPos] = limHeld;
		int32_t gain = limSum / kLook;
		// The delay: write this frame, read the one kLook - 1 frames back
		limDelL[limPos] = l;
		limDelR[limPos] = r;
		limPos = (limPos + 1) & (kLook - 1);
		l = int32_t((int64_t(limDelL[limPos]) * gain) >> 15);
		r = int32_t((int64_t(limDelR[limPos]) * gain) >> 15);
		// (rounding down can leave a negative sample one past it)
		l = l > c ? c : (l < -c ? -c : l);
		r = r > c ? c : (r < -c ? -c : r);
		if (gain < limMin) limMin = gain;
	}

	// Into a line: drive, the soft clip, and the level brought back down
	inline int16_t Saturate(int32_t x) const
	{
		x = Clip(x, 0x7FFFF);
		int32_t v = SoftClip((x * drive) >> 8);
		return int16_t((v * invDrive) >> 16);
	}

	static inline int32_t Sine(uint32_t phase)
	{
		uint32_t i = phase >> 24;
		int32_t fr = int32_t((phase >> 8) & 0xFFFF);
		return gSine[i] + (((gSine[i + 1] - gSine[i]) * fr) >> 16);
	}

	static inline int32_t Allpass(int16_t *buf, int len, int &pos, int32_t x, int32_t k)
	{
		int32_t z = buf[pos];
		int32_t v = Clip(x - ((z * k) >> 15), 32767);
		buf[pos] = int16_t(v);
		if (++pos >= len) pos = 0;
		return Clip(z + ((v * k) >> 15), 32767);
	}

	void MatrixTarget(const int16_t *m)
	{
		if (memcmp(m, matTgt, sizeof(matTgt)) == 0) return;
		// From wherever the last crossfade has got to
		memcpy(matPrev, matCur, sizeof(matPrev));
		memcpy(matTgt, m, sizeof(matTgt));
		matT = 0;
	}

	//------------------------------------------------------------------------
	// The network, at 24kHz
	//------------------------------------------------------------------------

	void ReadLines()
	{
		static constexpr uint32_t kMaskQ = (uint32_t(kLineSize) << 16) - 1;
		uint32_t wq = uint32_t(w) << 16;
		int32_t sl = 0, sr = 0;
		for (int i = 0; i < kLines; i++)
		{
			uint32_t rp = (wq - dl[i]) & kMaskQ;
			uint32_t i0 = rp >> 16, i1 = (i0 + 1) & (kLineSize - 1);
			int32_t fr = int32_t((rp >> 2) & 0x3FFF);
			int32_t s0 = gLines[i][i0], s1 = gLines[i][i1];
			int32_t y = s0 + (((s1 - s0) * fr) >> 14);
			sl += y * outL[i];
			sr += y * outR[i];
			// Decay and damping: lp = g y, low-passed (Q19)
			int32_t u = (y * gEff[i]) >> 11;
			lp[i] += ((u - lp[i]) * damp[i]) >> 10;
			// Low cut, its state with 16 more bits so that even at 10Hz it
			// follows the smallest signal rather than sticking (which would
			// leave a little DC circulating for ever)
			hp[i] += ((int64_t(lp[i]) << 16) - hp[i]) * hpA >> 16;
			f[i] = (lp[i] - int32_t((hp[i] + 32768) >> 16)) >> 4;
		}
		accL = sl >> 14;
		accR = sr >> 14;
	}

	void WriteLines(int32_t frL, int32_t frR)
	{
		// The input: level, freeze gate, tone, predelay, diffusion
		int32_t gin = (inGain * inGate) >> 15;
		int32_t xl = Clip((frL * gin) >> 12, 32767);
		int32_t xr = Clip((frR * gin) >> 12, 32767);
		if (toneA < 65536)
		{
			xl = OnePole(toneL, xl, toneA);
			xr = OnePole(toneR, xr, toneA);
		}
		gPre[0][pw] = int16_t(xl);
		gPre[1][pw] = int16_t(xr);
		{
			static constexpr uint32_t kMaskQ = (uint32_t(kPredelaySize) << 8) - 1;
			uint32_t rp = ((uint32_t(pw) << 8) - uint32_t(pdCur)) & kMaskQ;
			uint32_t i0 = rp >> 8, i1 = (i0 + 1) & (kPredelaySize - 1);
			int32_t fr = int32_t(rp & 255);
			xl = gPre[0][i0] + (((gPre[0][i1] - gPre[0][i0]) * fr) >> 8);
			xr = gPre[1][i0] + (((gPre[1][i1] - gPre[1][i0]) * fr) >> 8);
		}
		if (++pw >= kPredelaySize) pw = 0;
		if (diffuse > 0)
		{
			for (int s = 0; s < kStages; s++)
			{
				xl = Allpass(gAp[0][s], kApLen[0][s], apPos[0][s], xl, diffuse);
				xr = Allpass(gAp[1][s], kApLen[1][s], apPos[1][s], xr, diffuse);
			}
		}

		// A click, if one was asked for
		int32_t imp = 0;
		int impLine = -2;
		if (impulsePending)
		{
			impulsePending = false;
			impLine = impulse;
			imp = 16384;
		}

		// The matrix, and back into the lines
		for (int i = 0; i < kLines; i++)
		{
			const int16_t *row = matCur + i * kLines;
			int32_t acc = 0;
			for (int j = 0; j < kLines; j++) acc += (row[j] * f[j]) >> 3;
			int32_t x = (acc >> 11) + ((xl * inL[i] + xr * inR[i]) >> 14);
			if (impLine == i) x += imp;
			else if (impLine == -1) x += (imp * (inL[i] + inR[i])) >> 14;
			Peak(peakX, x);
			gLines[i][w] = Saturate(x);
			dl[i] += uint32_t(dInc[i]);
		}
		w = (w + 1) & (kLineSize - 1);
	}

	//------------------------------------------------------------------------
	// Control, every 32 samples
	//------------------------------------------------------------------------

	// Gates, globals, the guard and the levels
	void ControlGlobal()
	{
		// Clear: feedback and wet muted (fast) for as long as the longest
		// line, so the network empties, then back (slowly)
		if (clearHeld) clearTicks = kClearTicks;
		bool clearing = clearTicks > 0;
		if (clearTicks > 0) clearTicks--;
		Ramp(fbGate, clearing ? 0 : 32768, clearing ? 2 : 5);
		Ramp(wetMute, clearing ? 0 : 32768, clearing ? 2 : 8);
		Ramp(inGate, frozen ? 0 : 32768, 4);

		// Frozen, the low cut drops to just a DC blocker, so the lows stay
		hpA = frozen && dsg.hpA > 171 ? 171 : dsg.hpA;
		toneA = dsg.toneA;
		drive = dsg.drive;
		invDrive = dsg.invDrive;
		ceiling = dsg.ceiling;
		Ramp(inGain, dsg.inGain, 4);
		Ramp(outGain, dsg.outGain, 4);
		Ramp(diffuse, dsg.diffuse, 4);
		Ramp(dryGain, dsg.dryGain, 4);
		Ramp(wetGain, dsg.wetGain, 4);
		// Predelay glides, at most 1/8 sample a sample
		int32_t pd = dsg.predelay - pdCur;
		int32_t step = pd >> 5;
		if (step == 0) step = pd;
		if (step > 1024) step = 1024;
		if (step < -1024) step = -1024;
		pdCur += step;

		// The guard: the network kept near full scale for a quarter of a
		// second pulls the feedback down (about 50dB a second) until it
		// falls; then the feedback comes back over a second or so
		int32_t px = peakX > 32767 ? 32767 : peakX;
		tankEnv = px > tankEnv ? px : tankEnv - (tankEnv >> 6) - 1;
		if (tankEnv < 0) tankEnv = 0;
		if (tankEnv > kGuardLevel)
		{
			if (guardOver < 375) guardOver++;
			else guard -= (guard >> 8) + 1;
			if (guard < 2048) guard = 2048;
		}
		else
		{
			guardOver = 0;
			guard += (32768 - guard) >> 10;
			if (guard > 32760) guard = 32768;
		}

		// Levels for the LEDs, CV and editor
		lvIn = Fall(lvIn, peakIn);
		lvOut = Fall(lvOut, peakOut);
		lvWet = Fall(lvWet, peakWet);
		lvTank = tankEnv;
		limGain = limMin < limGain ? limMin : limGain + ((32768 - limGain) >> 7);
		peakIn = peakOut = peakWet = peakX = 0;
		limMin = 32768;
	}

	static inline void Ramp(int32_t &v, int32_t target, int shift)
	{
		int32_t d = target - v;
		int32_t s = d >> shift;
		if (s == 0) s = d;
		v += s;
	}

	static int32_t Fall(int32_t lv, int32_t peak)
	{
		if (peak > 32767) peak = 32767;
		if (peak > lv) return peak;
		return lv - (lv >> 6) - 1 < 0 ? 0 : lv - (lv >> 6) - 1;
	}

	// One line: its coefficients, its length's glide and its modulation
	void ControlLine(int i)
	{
		Ramp(g[i], frozen ? 32768 : dsg.g[i], 3);
		// The guard and the clear gate both act on the feedback
		gEff[i] = (((g[i] * fbGate) >> 15) * guard) >> 15;
		Ramp(damp[i], frozen ? 1024 : dsg.damp[i], 3);
		Ramp(inL[i], dsg.inL[i], 3);
		Ramp(inR[i], dsg.inR[i], 3);
		Ramp(outL[i], dsg.outL[i], 3);
		Ramp(outR[i], dsg.outR[i], 3);

		// Length: a one-pole of about 40ms, at most 4 samples a tick (a
		// quarter of the network's speed), so changes bend the pitch
		// rather than click
		int32_t delta = (int32_t(dsg.len[i]) << 8) - glide[i];
		int32_t step = delta >> 6;
		if (step == 0) step = delta;
		if (step > 1024) step = 1024;
		if (step < -1024) step = -1024;
		glide[i] += step;
		lfo[i] += dsg.modInc[i];
		int32_t q8 = glide[i] + ((Sine(lfo[i]) * dsg.modDepth) >> 15);
		if (q8 < (kMinLen << 8)) q8 = kMinLen << 8;
		if (q8 > ((kLineSize - 4) << 8)) q8 = (kLineSize - 4) << 8;
		// Reach it over the 16 network samples until this line's next turn
		dInc[i] = int32_t((uint32_t(q8) << 8) - dl[i]) >> 4;
	}

	void ControlMatrix()
	{
		if (matT >= 4096) return;
		matT += kMatStep;
		if (matT > 4096) matT = 4096;
		for (int k = 0; k < 64; k++)
		{
			matCur[k] = int16_t(matPrev[k] + (((matTgt[k] - matPrev[k]) * matT) >> 12));
		}
	}
};

} // namespace mv

#endif
