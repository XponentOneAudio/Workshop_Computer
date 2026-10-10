// Cycle benchmark for the voice on an RP2040 (real or emulated).
// Plays a spread of templates on two voices at once, as the card does, and
// reports SysTick cycles per sample over UART. See ../../README.md.

#include <stdio.h>

#include "hardware/structs/systick.h"
#include "pico/stdlib.h"

#define BUDGIE_RAM_FUNC(f) __not_in_flash_func(f)
#include "budgie_voice.h"

using namespace budgie;

static constexpr int kPerCategory = 3;     // templates tried from each category
static constexpr int kMaxSamples = 2400;   // per template (50 ms)

int main()
{
    stdio_init_all();
    systick_hw->rvr = 0xFFFFFF;
    systick_hw->cvr = 0;
    systick_hw->csr = 0x5;

    InitTables();
    Prepare();

    // worst-case modifiers: all harmonics up, full AM path, noise on
    Mods heavy;
    heavy.harm_q8 = 512;
    heavy.mix_q8 = 384;

    uint32_t peak = 0, peak_onset = 0, start_only = 0;
    uint64_t sum = 0;
    uint32_t n = 0;
    volatile int32_t sink = 0;
    Voice v1, v2;
    for (int c = 0; c < BUDGIE_N_CATEGORIES; c++) {
        int first = budgie_category_start[c], count = budgie_category_start[c + 1] - first;
        uint32_t cat_peak = 0;
        for (int k = 0; k < count && k < kPerCategory; k++) {
            int i = first + k * count / kPerCategory;
            v1.Enqueue(i, heavy);
            v2.Enqueue(i, heavy);
            for (int s = 0; s < kMaxSamples && (v1.Active() || v2.Active()); s++) {
                uint32_t t0 = systick_hw->cvr;
                int32_t a = v1.Process();
                uint32_t t1 = systick_hw->cvr;
                int32_t b = v2.Process(!v1.Onset());
                uint32_t dt = (t0 - systick_hw->cvr) & 0xFFFFFF;
                if (v1.Onset()) {
                    uint32_t d1 = (t0 - t1) & 0xFFFFFF;
                    if (d1 > start_only) start_only = d1;
                }
                sink = a + b;
                if (v1.Onset() || v2.Onset()) {
                    if (dt > peak_onset) peak_onset = dt;
                } else {
                    if (dt > peak) peak = dt;
                    if (dt > cat_peak) cat_peak = dt;
                    sum += dt;
                    n++;
                }
            }
            v1.Stop();
            v2.Stop();
        }
        printf("category %c: peak %lu cycles\n", "ABCDEFG"[c], (unsigned long)cat_peak);
    }
    printf("two voices: mean %lu, peak %lu cycles per sample; element start %lu cycles\n",
           (unsigned long)(n ? sum / n : 0), (unsigned long)peak, (unsigned long)peak_onset);
    printf("one voice's start sample alone: %lu cycles\n", (unsigned long)start_only);
    printf("budget at 144 MHz / 48 kHz: 3000 cycles\nBENCH DONE\n");
    (void)sink;
    while (true) tight_loop_contents();
}
