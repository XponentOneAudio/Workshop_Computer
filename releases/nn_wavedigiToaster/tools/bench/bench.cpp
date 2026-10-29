// bench.cpp - CPU cost of the model on an RP2040, measured with SysTick.
// Runs the same per-sample work as the card (Process + Control every 8th
// sample), with the knobs moving (worst case) and static, and prints the
// average cycles per sample over USB serial (and UART0) every few seconds.
// Flash it to a Workshop Computer and open the serial port to read it.
// Not included: the ComputerCard framework's own per-sample overhead.

#include <cstdio>
#include "pico/stdlib.h"
#include "hardware/structs/systick.h"
#include "hardware/clocks.h"
#include "toaster_model.h"

using namespace wdt;

static NoiseToaster model;

static uint32_t __not_in_flash_func(Run)(int n, bool moveKnobs, int source, bool sync)
{
	Controls c;
	c.source = source;
	c.sync = sync;
	c.vcfMod = 0.6f;
	c.vcoMod = -0.4f;
	Inputs in;
	Outputs out;
	uint32_t total = 0;
	for (int i = 0; i < n; i++)
	{
		systick_hw->cvr = 0;
		uint32_t t0 = systick_hw->cvr;
		if ((i & 7) == 0)
		{
			if (moveKnobs)
			{
				float x = (float)(i & 4095) / 4095.0f;
				c.cutoff = x; c.reso = 1.0f - x; c.lfoRate = x; c.attack = x; c.release = x; c.pitch = x;
			}
			model.Control(c, model.LastLfo(), model.LastArg());
		}
		in.syncPulse = (i & 512) != 0;
		model.Process(in, out);
		uint32_t t1 = systick_hw->cvr;
		total += (t0 - t1) & 0xFFFFFF;
	}
	return total;
}

int main()
{
	set_sys_clock_khz(192000, true);
	stdio_init_all();
	systick_hw->rvr = 0xFFFFFF;
	systick_hw->csr = 0x5; // enable, processor clock
	const int N = 4096;
	struct { const char *name; bool move; int src; bool sync; } cases[] = {
		{"static knobs, ramp", false, 0, false},
		{"static knobs, square+noise, sync", false, 3, true},
		{"knobs moving every control tick", true, 3, true},
	};
	while (true)
	{
		for (auto &k : cases)
		{
			uint32_t cyc = Run(N, k.move, k.src, k.sync);
			printf("%-36s %5lu cycles/sample (%.1f%% of 192 MHz @ 48 kHz)\n", k.name,
			       (unsigned long)(cyc / N), (double)(cyc / N) * 100.0 / 4000.0);
		}
		printf("\n");
		sleep_ms(3000);
	}
}
