// nam_fixed.h - fixed-point Neural Amp Modeler (NAM) inference for the RP2040
//
// Runs the two NAM architectures found on tone3000.com:
//   - "WaveNet" (the A1 standard / lite / feather / nano presets)
//   - "LSTM"
// using only 32-bit integer arithmetic, as the RP2040's Cortex-M0+ cores have
// no FPU. Weights are converted from .nam files by tools/nam2header.py.
//
// Number formats
//   - Signals are Q12: 4096 = 1.0, the NAM full-scale input/output level,
//     except the model's final output, which is Q16 for extra resolution.
//     The exception is the WaveNet residual stream between layers, which can
//     reach large values in some models: its format is chosen per layer by
//     nam2header.py, from the levels seen when running the model in floating
//     point. Converting between these formats is folded into the weights.
//   - Each weight block (a set of weights summed into one accumulator) is an
//     integer scaled by 2^shift. nam2header.py picks each shift to be as large
//     as possible while guaranteeing the 32-bit accumulator can never overflow,
//     given the largest possible input to that block. The bias of each block is
//     pre-scaled and includes the rounding offset for the final shift.
//
// Two-core operation
//   Processing is broken into "units" (WaveNet layers or LSTM layers) that run
//   in order. Run(frame, begin, end) runs units [begin, end), so a model can be
//   split between the two cores as a pipeline: core 1 runs [0, split) on
//   sample n while core 0 runs [split, end) on sample n-1. Each unit's state
//   (its dilation history buffer) is only ever touched by the core running it;
//   everything that crosses the split is carried in the Frame.
//
// This header has no Pico SDK dependencies, so it also builds on a desktop
// computer for testing (see tools/host_check.cpp). On the Computer, the card
// is built to run entirely from RAM, so no __not_in_flash_func is needed.

#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

namespace nam_fx
{

constexpr int kQ = 12;
constexpr int32_t kOne = 1 << kQ;
constexpr int kMaxChannels = 16;
constexpr int kMaxUnits = 32;
constexpr int kMaxLSTMLayers = 4;
constexpr int kMaxHidden = 16;

enum ModelType : uint8_t { kWaveNet = 0, kLSTM = 1 };
enum Activation : uint8_t { kTanh = 0, kHardtanh = 1, kReLU = 2, kSigmoid = 3 };

// out[r] = (b[r] + sum_c w[r*cols + c] * in[c]) >> shift
struct Dense
{
	const int32_t *w;
	const int32_t *b; // may be null
	uint8_t rows, cols, shift;
};

struct WNLayer
{
	uint16_t dilation;
	uint8_t channels;
	uint8_t gated;
	uint8_t activation;
	uint8_t shift_z, shift_1;
	int8_t res_shift; // residual: output format minus input format, in bits
	// Dilated conv + input mixin, with channels (or 2*channels if gated)
	// outputs. The inputs are [tap t-2d][tap t-d][tap t] (channels each) then
	// [mixin]. Stored in blocks of rows for MacMatrix, with the number of
	// rows (and the biases) padded to an even number.
	const int32_t *wz;
	const int32_t *bz;
	const int32_t *w1; // channels x channels, stored the same way
	const int32_t *b1;
};

struct WNArray
{
	uint8_t first_layer, num_layers;
	Dense rechannel; // input (1 or previous array's channels) -> channels
	Dense head;      // channels -> head_size (= next array's channels, or 1)
};

struct LSTMLayer
{
	uint8_t input_size, hidden, shift;
	// Inputs are [x (Q12) or previous layer's hidden state (Q14)][hidden state (Q14)]
	// Stored in blocks of 4 rows, as for MacMatrix.
	const int32_t *w;  // 4*hidden x (input_size + hidden), gates i, f, g, o
	const int32_t *b;  // 4*hidden
	const int16_t *h0; // initial hidden state, Q14
	const int32_t *c0; // initial cell state, Q14
};

struct Model
{
	const char *name;
	uint8_t type;
	uint8_t num_units; // WaveNet layers or LSTM layers
	uint8_t split;     // suggested split: units [0, split) on core 1
	// WaveNet
	uint8_t num_arrays;
	const WNArray *arrays;
	const WNLayer *layers;
	// LSTM
	const LSTMLayer *lstm;
	Dense lstm_head;
	// Q16 gains mapping the Computer's input (+-2048 = +-6V) to the model's Q12
	// input, and the model's Q16 output to the Computer's output, from the
	// .nam file's input_level_dbu and loudness metadata where available.
	int32_t input_gain_q16, output_gain_q16;
	uint32_t ring_words; // int16 words of dilation history needed
};

// Everything passed from one unit to the next, and across the core split
struct Frame
{
	int32_t x;                    // model input, Q12
	int16_t v[kMaxChannels];      // residual stream (WaveNet) or hidden state (LSTM, Q14)
	int32_t head[kMaxChannels];   // WaveNet head accumulator
	int32_t y;                    // model output, Q16, valid after the last unit
};

////////////////////////////////////////////////////////////////////////////////
// Activations

// tanh(i/128) for i = 0..1024, Q14. Filled in by InitTables().
static int16_t kTanhTable[1025];

static inline void InitTables()
{
	for (int i = 0; i <= 1024; i++) kTanhTable[i] = (int16_t)lrint(tanh(i / 128.0) * 16384);
}

// tanh of a Q14 value, Q14 result, by linear interpolation of kTanhTable
static inline int32_t Tanh14(int32_t x)
{
	uint32_t ax = x < 0 ? -x : x;
	if (ax >= (8u << 14)) return x < 0 ? -16384 : 16384;
	uint32_t i = ax >> 7, frac = ax & 127;
	int32_t a = kTanhTable[i], b = kTanhTable[i + 1];
	int32_t r = a + (((b - a) * (int32_t)frac) >> 7);
	return x < 0 ? -r : r;
}

// sigmoid(x) = (1 + tanh(x/2)) / 2, Q14
static inline int32_t Sigmoid14(int32_t x)
{
	return (16384 + Tanh14(x >> 1)) >> 1;
}

// tanh of a Q12 value, Q12 result
static inline int32_t Tanh(int32_t x)
{
	uint32_t ax = x < 0 ? -x : x;
	if (ax >= (8u << kQ)) return x < 0 ? -kOne : kOne;
	uint32_t i = ax >> 5, frac = ax & 31;
	int32_t a = kTanhTable[i], b = kTanhTable[i + 1];
	int32_t r = (a + (((b - a) * (int32_t)frac) >> 5) + 2) >> 2;
	return x < 0 ? -r : r;
}

// sigmoid, Q12
static inline int32_t Sigmoid(int32_t x)
{
	return (kOne + Tanh(x >> 1)) >> 1;
}

static inline int32_t Activate(uint8_t type, int32_t x)
{
	switch (type)
	{
	case kHardtanh: return x > kOne ? kOne : (x < -kOne ? -kOne : x);
	case kReLU: return x < 0 ? 0 : (x > 32767 ? 32767 : x);
	case kSigmoid: return Sigmoid(x);
	default: return Tanh(x);
	}
}

static inline int16_t Sat16(int32_t x)
{
	return (int16_t)(x > 32767 ? 32767 : (x < -32768 ? -32768 : x));
}

static inline int32_t Clamp(int32_t x, int32_t lim)
{
	return x > lim ? lim : (x < -lim ? -lim : x);
}

////////////////////////////////////////////////////////////////////////////////
// Multiply-accumulate kernels
//
// MacBlock<N>(acc, b, w, in, n): acc[r] = b[r] + sum over c < n of
// w[c*N + r] * in[c], for N = 2 or 4 outputs, advancing w past the weights used. Weight matrices
// are stored as blocks of 4 (then 2) output rows, each block column by column,
// so the kernel streams through the weights with LDM. On the Cortex-M0+, this
// is about 4 cycles per multiply-accumulate, roughly half what the compiler
// manages from C++.

#if defined(__ARM_ARCH_6M__)

template <int N>
static inline void MacBlock(int32_t *acc, const int32_t *b, const int32_t *&w, const int32_t *in, int n);

template <>
inline void MacBlock<4>(int32_t *acc, const int32_t *b, const int32_t *&w, const int32_t *in, int n)
{
	// Accumulators live in high registers, freeing the low registers for LDM
	register int32_t a0 asm("r8") = b[0];
	register int32_t a1 asm("r9") = b[1];
	register int32_t a2 asm("r10") = b[2];
	register int32_t a3 asm("r12") = b[3];
	register int32_t t0 asm("r0");
	register int32_t t1 asm("r1");
	register int32_t t2 asm("r2");
	register int32_t t3 asm("r3");
	const int32_t *end = in + n;
	int32_t x;
	asm volatile(
		// GCC switches to divided syntax for inline assembly on the Cortex-M0+
		".syntax unified\n"
		"1:\n"
		"ldmia %[in]!, {%[x]}\n"
		"ldmia %[w]!, {r0, r1, r2, r3}\n"
		"muls r0, %[x], r0\n"
		"add r8, r0\n"
		"muls r1, %[x], r1\n"
		"add r9, r1\n"
		"muls r2, %[x], r2\n"
		"add r10, r2\n"
		"muls r3, %[x], r3\n"
		"add r12, r3\n"
		"cmp %[in], %[end]\n"
		"bne 1b\n"
		: [in] "+l"(in), [w] "+l"(w), [x] "=&l"(x),
		  "+h"(a0), "+h"(a1), "+h"(a2), "+h"(a3),
		  "=&l"(t0), "=&l"(t1), "=&l"(t2), "=&l"(t3)
		: [end] "l"(end)
		: "cc", "memory");
	acc[0] = a0;
	acc[1] = a1;
	acc[2] = a2;
	acc[3] = a3;
}

template <>
inline void MacBlock<2>(int32_t *acc, const int32_t *b, const int32_t *&w, const int32_t *in, int n)
{
	register int32_t a0 asm("r8") = b[0];
	register int32_t a1 asm("r9") = b[1];
	register int32_t t0 asm("r0");
	register int32_t t1 asm("r1");
	const int32_t *end = in + n;
	int32_t x;
	asm volatile(
		".syntax unified\n"
		"1:\n"
		"ldmia %[in]!, {%[x]}\n"
		"ldmia %[w]!, {r0, r1}\n"
		"muls r0, %[x], r0\n"
		"add r8, r0\n"
		"muls r1, %[x], r1\n"
		"add r9, r1\n"
		"cmp %[in], %[end]\n"
		"bne 1b\n"
		: [in] "+l"(in), [w] "+l"(w), [x] "=&l"(x),
		  "+h"(a0), "+h"(a1), "=&l"(t0), "=&l"(t1)
		: [end] "l"(end)
		: "cc", "memory");
	acc[0] = a0;
	acc[1] = a1;
}

#else

template <int N>
static inline void MacBlock(int32_t *acc, const int32_t *b, const int32_t *&w, const int32_t *in, int n)
{
	for (int r = 0; r < N; r++) acc[r] = b[r];
	for (int c = 0; c < n; c++)
		for (int r = 0; r < N; r++) acc[r] += *w++ * in[c];
}

#endif

// Number of rows once padded for MacBlock (to a multiple of 2)
constexpr int PaddedRows(int r) {return (r + 1) & ~1;}

// acc[0..rows) = b + W * in, with W stored as described above
template <int Rows>
static inline void MacMatrix(int32_t *acc, const int32_t *b, const int32_t *w, const int32_t *in, int n)
{
	constexpr int R = PaddedRows(Rows);
	int r = 0;
	for (; r + 4 <= R; r += 4) MacBlock<4>(acc + r, b + r, w, in, n);
	if (r < R) MacBlock<2>(acc + r, b + r, w, in, n);
}

class Engine
{
public:
	// ring must hold model->ring_words int16 values
	void Init(const Model *model, int16_t *ring)
	{
		if (kTanhTable[1024] == 0) InitTables();
		m = model;
		ringMem = ring;
		uint32_t off = 0;
		if (m->type == kWaveNet)
		{
			for (int l = 0; l < m->num_units; l++)
			{
				const WNLayer &L = m->layers[l];
				ringOffset[l] = off;
				ringLen[l] = 2 * L.dilation + 1;
				off += ringLen[l] * L.channels;
				layerFn[l] = GetLayerFn(L.channels, L.gated);
				int a = 0;
				while (l >= m->arrays[a].first_layer + m->arrays[a].num_layers) a++;
				unitArray[l] = a;
			}
		}
		Reset();
	}

	void Reset()
	{
		if (m->type == kWaveNet)
		{
			memset(ringMem, 0, m->ring_words * sizeof(int16_t));
			for (int l = 0; l < m->num_units; l++) ringPos[l] = 0;
		}
		else
		{
			for (int l = 0; l < m->num_units; l++)
			{
				const LSTMLayer &L = m->lstm[l];
				for (int i = 0; i < L.input_size; i++) xh[l][i] = 0;
				for (int i = 0; i < L.hidden; i++)
				{
					xh[l][L.input_size + i] = L.h0[i];
					c[l][i] = L.c0[i];
				}
			}
		}
	}

	int NumUnits() const {return m->num_units;}
	const Model *GetModel() const {return m;}

	// Run units [begin, end) on frame f. Set f.x (within +-32767) before
	// running unit 0; f.y holds the output once the last unit has run.
	void Run(Frame &f, int begin, int end)
	{
		if (m->type == kWaveNet)
		{
			for (int l = begin; l < end; l++) RunWaveNetUnit(f, l);
		}
		else
		{
			for (int l = begin; l < end; l++) RunLSTMLayer(f, l);
			if (end == m->num_units) RunLSTMHead(f);
		}
	}

	// Convenience: whole model, one sample
	int32_t Process(int32_t x)
	{
		Frame f;
		f.x = Clamp(x, 32767);
		Run(f, 0, m->num_units);
		return f.y;
	}

private:
	const Model *m = nullptr;
	int16_t *ringMem = nullptr;
	uint32_t ringOffset[kMaxUnits];
	uint16_t ringLen[kMaxUnits];
	uint16_t ringPos[kMaxUnits];
	typedef void (Engine::*LayerFn)(Frame &, int);
	LayerFn layerFn[kMaxUnits];
	uint8_t unitArray[kMaxUnits];

	int32_t xh[kMaxLSTMLayers][kMaxHidden * 2];
	int32_t c[kMaxLSTMLayers][kMaxHidden];

	template <typename T>
	static inline void DenseApply(const Dense &d, const T *in, int32_t *out)
	{
		const int32_t *w = d.w;
		for (int r = 0; r < d.rows; r++)
		{
			int32_t acc = d.b ? d.b[r] : 0;
			for (int col = 0; col < d.cols; col++) acc += *w++ * (int32_t)in[col];
			out[r] = acc >> d.shift;
		}
	}

	void RunWaveNetUnit(Frame &f, int l)
	{
		const WNLayer &L = m->layers[l];

		const int a = unitArray[l];
		const WNArray &A = m->arrays[a];

		if (l == A.first_layer)
		{
			int32_t tmp[kMaxChannels];
			if (a == 0)
			{
				int32_t x = f.x;
				DenseApply(A.rechannel, &x, tmp);
				for (int i = 0; i < L.channels; i++) f.head[i] = 0;
			}
			else
			{
				// Head accumulator already holds the previous array's head output
				DenseApply(A.rechannel, f.v, tmp);
			}
			for (int i = 0; i < L.channels; i++) f.v[i] = Sat16(tmp[i]);
		}

		(this->*layerFn[l])(f, l);

		if (l == A.first_layer + A.num_layers - 1)
		{
			int32_t h[kMaxChannels];
			for (int i = 0; i < L.channels; i++) h[i] = Clamp(f.head[i], 1 << 17);
			DenseApply(A.head, h, f.head);
			if (a == m->num_arrays - 1) f.y = f.head[0];
		}
	}

	static LayerFn GetLayerFn(int channels, bool gated)
	{
		switch (channels | (gated << 8))
		{
		case 1: return &Engine::Layer<1, false>;
		case 2: return &Engine::Layer<2, false>;
		case 3: return &Engine::Layer<3, false>;
		case 4: return &Engine::Layer<4, false>;
		case 6: return &Engine::Layer<6, false>;
		case 8: return &Engine::Layer<8, false>;
		case 12: return &Engine::Layer<12, false>;
		case 16: return &Engine::Layer<16, false>;
		case 0x101: return &Engine::Layer<1, true>;
		case 0x102: return &Engine::Layer<2, true>;
		case 0x103: return &Engine::Layer<3, true>;
		case 0x104: return &Engine::Layer<4, true>;
		case 0x106: return &Engine::Layer<6, true>;
		case 0x108: return &Engine::Layer<8, true>;
		default: return &Engine::Layer<1, false>; // rejected by nam2header.py
		}
	}

	template <int C, bool Gated>
	void Layer(Frame &f, int l)
	{
		const WNLayer &L = m->layers[l];
		int16_t *ring = ringMem + ringOffset[l];
		const int len = ringLen[l];
		const int d = L.dilation;

		// Store this sample's input, and find the inputs d and 2d samples ago
		int pos = ringPos[l];
		int16_t *cur = ring + pos * C;
		for (int i = 0; i < C; i++) cur[i] = f.v[i];
		int p1 = pos - d;
		if (p1 < 0) p1 += len;
		int p0 = p1 - d;
		if (p0 < 0) p0 += len;
		const int16_t *x1 = ring + p1 * C;
		const int16_t *x0 = ring + p0 * C;
		if (++pos == len) pos = 0;
		ringPos[l] = pos;

		// Dilated conv and input mixin
		constexpr int R = Gated ? 2 * C : C;
		int32_t in[3 * C + 1];
		for (int i = 0; i < C; i++)
		{
			in[i] = x0[i];
			in[C + i] = x1[i];
			in[2 * C + i] = cur[i];
		}
		in[3 * C] = f.x;
		int32_t acc[PaddedRows(R)];
		MacMatrix<R>(acc, L.bz, L.wz, in, 3 * C + 1);

		int32_t act[C];
		const int sz = L.shift_z;
		const bool tanhAct = L.activation == kTanh;
		for (int o = 0; o < C; o++)
		{
			int32_t z = acc[o] >> sz;
			int32_t a = tanhAct ? Tanh(z) : Activate(L.activation, z);
			if (Gated) a = (a * Sigmoid(acc[o + C] >> sz)) >> kQ;
			act[o] = a;
			f.head[o] += a;
		}

		// 1x1
		int32_t acc1[PaddedRows(C)];
		MacMatrix<C>(acc1, L.b1, L.w1, act, C);

		// Residual connection
		const int rs = L.res_shift, s1 = L.shift_1;
		for (int o = 0; o < C; o++)
		{
			int32_t res = rs >= 0 ? cur[o] << rs : cur[o] >> -rs;
			f.v[o] = Sat16(res + (acc1[o] >> s1));
		}
	}

	void RunLSTMLayer(Frame &f, int l)
	{
		const LSTMLayer &L = m->lstm[l];
		int32_t *v = xh[l];
		const int H = L.hidden, n = L.input_size + H;
		if (l == 0)
			v[0] = Clamp(f.x, 32767);
		else
			for (int i = 0; i < L.input_size; i++) v[i] = f.v[i];

		// Gates, Q14
		int32_t g[4 * kMaxHidden];
		const int32_t *w = L.w;
		for (int r = 0; r < 4 * H; r += 4) MacBlock<4>(g + r, L.b + r, w, v, n);
		const int shift = L.shift;

		// Cell state is Q14, hidden state Q14 (so at most 16384)
		int32_t *cl = c[l];
		for (int i = 0; i < H; i++)
		{
			int32_t ig = Sigmoid14(g[i] >> shift);
			int32_t fg = Sigmoid14(g[H + i] >> shift);
			int32_t gg = Tanh14(g[2 * H + i] >> shift);
			int32_t og = Sigmoid14(g[3 * H + i] >> shift);
			// f * c, split in two to stay within 32 bits
			int32_t ci = cl[i];
			int32_t fc = fg * (ci >> 14) + ((fg * (ci & 0x3fff)) >> 14);
			int32_t cn = Clamp(fc + ((ig * gg) >> 14), 1 << 30);
			cl[i] = cn;
			int32_t h = (og * Tanh14(cn)) >> 14;
			v[L.input_size + i] = h;
			f.v[i] = (int16_t)h;
		}
	}

	void RunLSTMHead(Frame &f)
	{
		int32_t y;
		DenseApply(m->lstm_head, f.v, &y);
		f.y = y;
	}
};

} // namespace nam_fx
