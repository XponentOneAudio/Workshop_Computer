// Budgie syrinx voice: one element at a time, integer-only in the audio path.
//
// This is the C++ twin of tools/budgie_analysis/budgie_analysis/synth.py
// (render). Keep the two in step: host/test_parity.py checks them against
// each other.
//
// Per sample:
//   tonal  = sum_k w_k sin(k * phase)          harmonics 1..4, muted above 0.45 fs
//            * (1 + d sin(am_phase)) / (1 + d)  amplitude modulation
//   noise  = g * bandpass(uniform noise)       RBJ biquad, 0 dB peak
//   out    = level * env * ((1 - mix) tonal + mix noise)
//
// Pitch is held as log2(phase increment) in Q16 and interpolated between the
// template's breakpoints once per kBlock samples; the increment then ramps
// linearly across the block. Nothing in Process() uses floating point or
// division, so it is safe in the 48 kHz interrupt on the RP2040 (no FPU).
// Start() uses integer division, which the RP2040 does in hardware.
//
// Floats are used only in InitTables() and Prepare(), called once at boot.

#ifndef BUDGIE_VOICE_H
#define BUDGIE_VOICE_H

#include <math.h>
#include <stdint.h>

#include "budgie_data.h"

#ifndef BUDGIE_RAM_FUNC
#define BUDGIE_RAM_FUNC(f) f
#endif

namespace budgie {

static constexpr int32_t kSampleRate = 48000;
static constexpr int32_t kBlock = 16;            // samples per pitch update
static constexpr int32_t kFade = 24;             // 0.5 ms click guard at each end
static constexpr int32_t kOutScale = 1600;       // full-scale sine -> DAC counts (of 2047)
static constexpr int32_t kSineBits = 12;         // 4096-entry sine table
static constexpr uint32_t kNyquistInc = 1932735283u;  // 0.45 fs as a phase increment
static constexpr int32_t kMaxQueue = 12;
static constexpr int32_t kRelease = 48;         // 1 ms fade when cut short
static constexpr int32_t kMaxLength = 1 << 19;  // ~11 s; keeps position maths in 32 bits

// Matches synth.noise_filter() in the Python twin.
static constexpr int32_t kNoiseMinHz = 200, kNoiseMaxHz = 16000, kNoiseMinBw = 200;
static constexpr int32_t kNoiseQMinQ8 = 77, kNoiseQMaxQ8 = 20 * 256;  // 0.3 .. 20

inline int16_t g_sine[1 << kSineBits];
inline uint32_t g_exp2[257];          // 2^(i/256) in Q30
inline int32_t g_log2_inc_per_hz;     // log2(2^32 / fs) in Q16

inline void InitTables()
{
    for (int i = 0; i < (1 << kSineBits); i++)
        g_sine[i] = (int16_t)lrintf(32767.0f * sinf(6.2831853f * (float)i / (float)(1 << kSineBits)));
    for (int i = 0; i <= 256; i++)
        g_exp2[i] = (uint32_t)llrint(1073741824.0 * pow(2.0, i / 256.0));
    g_log2_inc_per_hz = (int32_t)lrint(65536.0 * (32.0 - log2((double)kSampleRate)));
}

inline int32_t Log2Q16(float x) { return (int32_t)lrintf(65536.0f * log2f(x)); }

// 2^(x / 65536) for x in [0, 32 << 16), as an integer.
static inline uint32_t BUDGIE_RAM_FUNC(Exp2Q16)(int32_t x)
{
    if (x < 0) return 1;
    int32_t ip = x >> 16;
    if (ip > 31) return 0xFFFFFFFFu;
    uint32_t fr = (uint32_t)x & 0xFFFF;
    uint32_t i = fr >> 8, t = fr & 0xFF;
    uint32_t m = g_exp2[i] + (((g_exp2[i + 1] - g_exp2[i]) * t) >> 8);  // Q30; product < 2^30
    return ip >= 30 ? m << (ip - 30) : m >> (30 - ip);
}

// Unity below 1536 DAC counts, then a quadratic knee that reaches full
// scale (2047) with zero slope at 2560. Noise peaks well above its RMS, so
// loud noisy elements need this; a hard clip sounds harsh.
static constexpr int32_t kKnee = 1536;
static inline int32_t BUDGIE_RAM_FUNC(SoftClip)(int32_t x)
{
    int32_t a = x < 0 ? -x : x;
    if (a > kKnee) {
        int32_t e = a - kKnee;
        a = e >= 1024 ? 2047 : kKnee + e - ((e * e) >> 11);
        if (a > 2047) a = 2047;
    }
    return x < 0 ? -a : a;
}

static inline int32_t BUDGIE_RAM_FUNC(Sin)(uint32_t phase)
{
    return g_sine[phase >> (32 - kSineBits)];
}

struct Filter {
    int32_t b0, a1, a2;  // RBJ band-pass, Q14; b1 = 0, b2 = -b0
};

// RBJ band-pass at `centre` Hz with bandwidth `bw`, clamped as in
// synth.noise_filter(). Integer only (two divisions), so it can run when an
// element starts.
inline Filter NoiseFilter(int32_t c, int32_t bw)
{
    if (c < kNoiseMinHz) c = kNoiseMinHz;
    if (c > kNoiseMaxHz) c = kNoiseMaxHz;
    if (bw < kNoiseMinBw) bw = kNoiseMinBw;
    int32_t q_q8 = c * 256 / bw;
    if (q_q8 < kNoiseQMinQ8) q_q8 = kNoiseQMinQ8;
    if (q_q8 > kNoiseQMaxQ8) q_q8 = kNoiseQMaxQ8;
    uint32_t w0 = (uint32_t)c * 89478u;              // phase of c/fs (c < 48 kHz)
    int32_t s = Sin(w0), co = Sin(w0 + 0x40000000u);  // Q15
    int32_t alpha = s * 128 / q_q8;                   // sin/(2Q), Q15
    int32_t inv = (1 << 30) / (32768 + alpha);        // 2^15 / a0, <= 32768
    Filter f;
    f.b0 = (alpha * inv) >> 16;                       // alpha / a0, Q14
    f.a1 = -((co * inv) >> 15);                       // -2 cos / a0, Q14
    f.a2 = ((32768 - alpha) * inv) >> 16;             // (1 - alpha) / a0, Q14
    return f;
}

// Everything about a template that doesn't depend on the knobs, worked out
// once at boot so that starting an element is cheap.
struct Prepared {
    int32_t f0_mean;                       // mean log2(Hz) of the breakpoints, Q16
    int32_t f0_dev[BUDGIE_N_F0_POINTS];    // each breakpoint relative to the mean
    uint32_t f0_t_q12[BUDGIE_N_F0_POINTS]; // breakpoint times, fraction of duration
    uint32_t amp_t_q12[BUDGIE_N_AMP_POINTS];
    int32_t amp_q24[BUDGIE_N_AMP_POINTS];  // 0..1 in Q24
    int32_t hw[BUDGIE_N_HARMONICS];        // harmonic weights summing to 256
    int32_t am_norm;                       // 1 / (1 + depth), Q16 (x256)
    int32_t mix_q8;                        // noise mix, 0..256
    int32_t level_q8;                      // 0..256
    Filter filter;
    int32_t noise_gain_q8;                 // see synth.noise_filter
};
inline Prepared g_prepared[BUDGIE_N_ELEMENTS];

// Harmonic weights with harmonics 2..N scaled by harm_q8, normalised to 256.
inline void HarmonicWeights(const budgie_element_t &e, int32_t harm_q8, int32_t *hw)
{
    int32_t h[BUDGIE_N_HARMONICS], sum = 0;
    for (int k = 0; k < BUDGIE_N_HARMONICS; k++) {
        h[k] = e.harm[k];
        if (k > 0) h[k] = h[k] * harm_q8 >> 8;
        if (h[k] > 255) h[k] = 255;
        sum += h[k];
    }
    for (int k = 0; k < BUDGIE_N_HARMONICS; k++) hw[k] = sum ? h[k] * 256 / sum : 0;
}

inline void Prepare()
{
    for (int i = 0; i < BUDGIE_N_ELEMENTS; i++) {
        const budgie_element_t &e = budgie_elements[i];
        Prepared &p = g_prepared[i];
        int nf = e.n_f0;
        p.f0_mean = 0;
        for (int k = 0; k < nf; k++) p.f0_mean += Log2Q16((float)e.f0_hz[k]);
        if (nf) p.f0_mean /= nf;
        for (int k = 0; k < BUDGIE_N_F0_POINTS; k++) {
            p.f0_dev[k] = k < nf ? Log2Q16((float)e.f0_hz[k]) - p.f0_mean : 0;
            p.f0_t_q12[k] = (uint32_t)e.f0_t[k] * 4096u / 255u;
        }
        for (int k = 0; k < BUDGIE_N_AMP_POINTS; k++) {
            p.amp_t_q12[k] = (uint32_t)e.amp_t[k] * 4096u / 255u;
            p.amp_q24[k] = (int32_t)e.amp[k] * 16777216 / 255;
        }
        HarmonicWeights(e, 256, p.hw);
        int32_t am_d = e.am_rate_hz ? e.am_depth : 0;
        p.am_norm = (256 << 16) / (256 + am_d);
        p.mix_q8 = nf ? (int32_t)e.noise_mix * 256 / 255 : 256;
        p.level_q8 = (int32_t)e.level * 256 / 255;
        p.filter = NoiseFilter(e.noise_centre_hz, e.noise_bw_hz);

        // the filter's noise bandwidth is (pi/2)(c/Q) = (pi/2) bw while Q is
        // inside its limits; reproduce the clamping exactly as the twin does
        float c = (float)e.noise_centre_hz;
        if (c < kNoiseMinHz) c = kNoiseMinHz;
        if (c > kNoiseMaxHz) c = kNoiseMaxHz;
        float bw = e.noise_bw_hz < kNoiseMinBw ? (float)kNoiseMinBw : (float)e.noise_bw_hz;
        float Q = c / bw;
        if (Q < 0.3f) Q = 0.3f;
        if (Q > 20.0f) Q = 20.0f;
        p.noise_gain_q8 = (int32_t)lrintf(256.0f * sqrtf(1.5f * kSampleRate / (3.14159265f * c / Q)));
    }
}

// Changes applied to a template when it starts.
struct Mods {
    int32_t pitch_q16 = 0;       // transpose, log2 Q16 (65536 = one octave up)
    int32_t dur_q8 = 256;        // duration scale, 256 = as recorded
    int32_t level_q8 = 256;      // loudness scale
    int32_t fm_depth_q8 = 256;   // contour excursion around its mean (B, G)
    int32_t harm_q8 = 256;       // harmonics 2..4 scale (C, D)
    int32_t mix_q8 = 256;        // noise mix scale (A)
    int32_t noise_q16 = 0;       // noise centre shift, log2 Q16 (E)
};

class Voice
{
public:
    // Queue an element to start `gap` samples after the previous one ends
    // (or after now, if idle). Returns false if the queue is full.
    bool Enqueue(int index, const Mods &m, int32_t gap = 0)
    {
        if (count_ >= kMaxQueue) return false;
        Pending &p = queue_[(head_ + count_) % kMaxQueue];
        p.index = index;
        p.mods = m;
        p.gap = gap;
        count_++;
        return true;
    }

    void Stop()
    {
        count_ = 0;
        active_ = false;
    }

    // Drop anything queued and fade the current element out over kRelease
    // samples, so a retrigger doesn't click. Queue new elements after this.
    void Cut()
    {
        count_ = 0;
        if (active_ && release_ == 0) release_ = kRelease;
    }

    bool Active() const { return active_ || count_ > 0; }
    bool Sounding() const { return active_; }
    int Category() const { return category_; }
    // true for exactly one sample when an element begins
    bool Onset() const { return onset_; }
    // current pitch, log2(Hz) in Q16; holds its last value in silence
    int32_t PitchLog2() const { return pitch_log2_; }
    // envelope 0..65535
    int32_t Envelope() const { return active_ ? (env_ >> 8) * level_q8_ >> 8 : 0; }

    // One sample, in DAC counts (-2048..2047). Starting an element is the
    // expensive sample; with may_start false a due start waits one sample,
    // so several voices can be kept from starting in the same sample.
    int32_t BUDGIE_RAM_FUNC(Process)(bool may_start = true)
    {
        onset_ = false;
        if (!active_) {
            if (count_ == 0) return 0;
            Pending &p = queue_[head_];
            if (p.gap > 0) {
                p.gap--;
                return 0;
            }
            if (!may_start) return 0;
            Start(p.index, p.mods);
            if (++head_ == kMaxQueue) head_ = 0;
            count_--;
            onset_ = true;
        }

        if (block_left_ == 0) PitchBlock();
        block_left_--;

        int32_t tonal = 0;
        if (nf_) {
            inc_ += dinc_;
            phase_ += inc_;
            int32_t acc = 0;
            uint32_t ph = phase_;
            for (int k = 0; k < BUDGIE_N_HARMONICS; k++, ph += phase_)
                if (hmask_ & (1u << k)) acc += hw_[k] * Sin(ph);
            tonal = acc >> 8;  // Q15
            if (am_d_) {
                am_phase_ += am_inc_;
                int32_t am = ((256 << 15) + am_d_ * Sin(am_phase_)) >> 15;  // 256 +/- d
                tonal = ((tonal * am) >> 8) * am_norm_ >> 16;
            }
        }

        int32_t noise = 0;
        if (mix_) {
            rng_ ^= rng_ << 13;
            rng_ ^= rng_ >> 17;
            rng_ ^= rng_ << 5;
            int32_t x = (int32_t)rng_ >> 16;  // uniform, Q15
            int64_t a = (int64_t)b0_ * (x - x2_) - (int64_t)a1_ * y1_ - (int64_t)a2_ * y2_;
            int32_t y = (int32_t)(a >> 14);
            if (y > (1 << 20)) y = 1 << 20;
            if (y < -(1 << 20)) y = -(1 << 20);
            x2_ = x1_;
            x1_ = x;
            y2_ = y1_;
            y1_ = y;
            noise = (y * ngain_) >> 8;
            if (noise > 65535) noise = 65535;
            if (noise < -65535) noise = -65535;
        }

        int32_t sig = (tonal * (256 - mix_) + noise * mix_) >> 8;  // Q15

        // envelope: linear between breakpoints, held outside them, Q24
        while (seg_a_ < BUDGIE_N_AMP_POINTS - 1 && pos_ >= at_[seg_a_ + 1]) {
            seg_a_++;
            SetEnvSlope();
        }
        if (pos_ < at_[0])
            env_ = av_[0];
        else if (seg_a_ >= BUDGIE_N_AMP_POINTS - 1)
            env_ = av_[BUDGIE_N_AMP_POINTS - 1];
        else
            env_ = av_[seg_a_] + denv_ * (pos_ - at_[seg_a_]);
        int32_t e = env_ >> 8;  // Q16
        int32_t edge = pos_ < n_ - 1 - pos_ ? pos_ : n_ - 1 - pos_;
        if (edge < kFade) e = e * edge / kFade;
        if (release_) {
            e = e * release_ / kRelease;
            if (--release_ == 0) pos_ = n_ - 1;  // ends this sample
        }

        // |sig| < 2^17 and e <= 2^16, so this stays inside 32 bits
        int32_t out = ((sig * (e >> 2)) >> 14) * level_q8_ >> 8;  // Q15
        out = (out * kOutScale) >> 15;

        if (++pos_ >= n_) active_ = false;
        return SoftClip(out);
    }

private:
    struct Pending {
        int index;
        Mods mods;
        int32_t gap;
    };

    // Begin an element. Runs inside the audio interrupt, so the knob-free
    // work is already done by Prepare(); what is left is mostly multiplies.
    void Start(int index, const Mods &m)
    {
        const budgie_element_t &e = budgie_elements[index];
        const Prepared &pr = g_prepared[index];
        category_ = e.category;
        n_ = (int32_t)e.dur_ms * (kSampleRate / 1000) * m.dur_q8 >> 8;
        if (n_ < 2) n_ = 2;
        if (n_ > kMaxLength) n_ = kMaxLength;
        pos_ = 0;
        release_ = 0;

        nf_ = e.n_f0;
        mix_ = pr.mix_q8;
        if (nf_) {
            // pitch breakpoints as log2(phase increment), contour scaled about its mean
            int32_t base = pr.f0_mean + m.pitch_q16 + g_log2_inc_per_hz;
            for (int k = 0; k < nf_; k++) {
                ft_[k] = (int32_t)((pr.f0_t_q12[k] * (uint32_t)n_) >> 12);
                fl_[k] = base + (m.fm_depth_q8 == 256 ? pr.f0_dev[k]
                                                      : (pr.f0_dev[k] >> 4) * m.fm_depth_q8 >> 4);
            }
            seg_f_ = 0;
            SetPitchSlope();
            phase_ = 0;
            inc_ = Exp2Q16(PitchAt(0));
            if (inc_ > kNyquistInc) inc_ = kNyquistInc;
            dinc_ = 0;

            if (m.harm_q8 == 256) {
                for (int k = 0; k < BUDGIE_N_HARMONICS; k++) hw_[k] = pr.hw[k];
            } else {
                HarmonicWeights(e, m.harm_q8, hw_);
            }

            am_d_ = e.am_rate_hz ? e.am_depth : 0;
            am_inc_ = (uint32_t)e.am_rate_hz * 89478u;  // 2^32 / 48000
            am_phase_ = 0;
            am_norm_ = pr.am_norm;
            mix_ = mix_ * m.mix_q8 >> 8;
            if (mix_ > 256) mix_ = 256;
        }
        block_left_ = 0;

        if (mix_) {
            if (m.noise_q16) {
                uint32_t c = (uint32_t)e.noise_centre_hz * (Exp2Q16(m.noise_q16 + (16 << 16)) >> 8) >> 8;
                Filter f = NoiseFilter((int32_t)c, e.noise_bw_hz);
                b0_ = f.b0, a1_ = f.a1, a2_ = f.a2;
            } else {
                b0_ = pr.filter.b0, a1_ = pr.filter.a1, a2_ = pr.filter.a2;
            }
            x1_ = x2_ = y1_ = y2_ = 0;
            ngain_ = pr.noise_gain_q8;  // bandwidth is unchanged by a centre shift
        }

        for (int k = 0; k < BUDGIE_N_AMP_POINTS; k++) {
            at_[k] = (int32_t)((pr.amp_t_q12[k] * (uint32_t)n_) >> 12);
            av_[k] = pr.amp_q24[k];
        }
        seg_a_ = 0;
        SetEnvSlope();

        level_q8_ = pr.level_q8 * m.level_q8 >> 8;
        active_ = true;
    }

    void SetEnvSlope()
    {
        denv_ = 0;
        if (seg_a_ < BUDGIE_N_AMP_POINTS - 1) {
            int32_t span = at_[seg_a_ + 1] - at_[seg_a_];
            if (span > 0) denv_ = (av_[seg_a_ + 1] - av_[seg_a_]) / span;
        }
    }

    // Pitch at sample `pos` (log2 increment, Q16). pos only moves forward.
    int32_t PitchAt(int32_t pos)
    {
        while (seg_f_ < nf_ - 1 && pos >= ft_[seg_f_ + 1]) {
            seg_f_++;
            SetPitchSlope();
        }
        if (pos <= ft_[0] || nf_ == 1) return fl_[0];
        if (seg_f_ >= nf_ - 1) return fl_[nf_ - 1];
        // |slope| * (pos - start) <= |difference| << 8, so this fits in 32 bits
        return fl_[seg_f_] + ((fslope_q8_ * (pos - ft_[seg_f_])) >> 8);
    }

    void SetPitchSlope()
    {
        fslope_q8_ = 0;
        if (seg_f_ < nf_ - 1) {
            int32_t span = ft_[seg_f_ + 1] - ft_[seg_f_];
            if (span > 0) fslope_q8_ = ((fl_[seg_f_ + 1] - fl_[seg_f_]) << 8) / span;
        }
    }

    void PitchBlock()
    {
        block_left_ = kBlock;
        if (!nf_) return;
        int32_t l = PitchAt(pos_ + kBlock);
        pitch_log2_ = l - g_log2_inc_per_hz;
        uint32_t target = Exp2Q16(l);
        if (target > kNyquistInc) target = kNyquistInc;
        dinc_ = ((int32_t)target - (int32_t)inc_) / kBlock;
        hmask_ = 0;
        for (int k = 0; k < BUDGIE_N_HARMONICS; k++)
            if (target < kNyquistInc / (uint32_t)(k + 1)) hmask_ |= 1u << k;
    }

    Pending queue_[kMaxQueue];
    int head_ = 0, count_ = 0;

    bool active_ = false, onset_ = false;
    int category_ = 0;
    int32_t n_ = 0, pos_ = 0, release_ = 0;

    int nf_ = 0, seg_f_ = 0;
    int32_t ft_[BUDGIE_N_F0_POINTS] = {}, fl_[BUDGIE_N_F0_POINTS] = {};
    uint32_t phase_ = 0, inc_ = 0;
    int32_t dinc_ = 0, block_left_ = 0, pitch_log2_ = 10 << 16, fslope_q8_ = 0;
    uint32_t hmask_ = 0;
    int32_t hw_[BUDGIE_N_HARMONICS] = {};

    uint32_t am_phase_ = 0, am_inc_ = 0;
    int32_t am_d_ = 0, am_norm_ = 0;

    int32_t mix_ = 0;
    uint32_t rng_ = 0x9E3779B9u;
    int32_t b0_ = 0, a1_ = 0, a2_ = 0, x1_ = 0, x2_ = 0, y1_ = 0, y2_ = 0, ngain_ = 0;

    int32_t at_[BUDGIE_N_AMP_POINTS] = {}, av_[BUDGIE_N_AMP_POINTS] = {};
    int seg_a_ = 0;
    int32_t env_ = 0, denv_ = 0, level_q8_ = 0;
};

}  // namespace budgie

#endif  // BUDGIE_VOICE_H
