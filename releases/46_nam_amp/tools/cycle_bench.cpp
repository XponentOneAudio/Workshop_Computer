// Benchmark program for tools/cycle_count.py: runs the NAM engine on noise.
// Built for the Cortex-M0+ without the Pico SDK; see cycle_count.py.

#include MODEL_HEADER

static int16_t ring[NAM_RING_WORDS];
static nam_fx::Engine engine;
static nam_fx::Frame frame;
static uint32_t seed = 1;

extern "C" void setup() {engine.Init(kModels[0], ring);}

extern "C" int num_units() {return kModels[0]->num_units;}

// Run units [begin, end) on n samples of noise
extern "C" void run(int begin, int end, int n)
{
	for (int i = 0; i < n; i++)
	{
		seed = seed * 1664525u + 1013904223u;
		if (begin == 0) frame.x = ((int32_t)(seed >> 16) - 32768) >> 3;
		engine.Run(frame, begin, end);
	}
}

extern "C" void _exit(int) {while (1);}
