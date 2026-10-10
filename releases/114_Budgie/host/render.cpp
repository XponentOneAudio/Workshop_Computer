// Host build of the voice: renders every template so host/test_parity.py can
// compare it with the Python twin. Not part of the firmware.
//
//   render <out.bin>   writes, per template: int32 sample count, int16 samples
//   render --mods      exercises every Mods field and the queue (sanity only)

#include <cstdio>
#include <cstring>
#include <vector>

#include "../budgie_voice.h"

using namespace budgie;

static std::vector<int16_t> RenderOne(int index, const Mods &m)
{
    Voice v;
    v.Enqueue(index, m);
    std::vector<int16_t> out;
    while (v.Active()) {
        int32_t s = v.Process();
        if (v.Sounding() || s) out.push_back((int16_t)s);
    }
    return out;
}

int main(int argc, char **argv)
{
    InitTables();
    Prepare();
    if (argc == 2 && !strcmp(argv[1], "--mods")) {
        int fails = 0;
        for (int i = 0; i < BUDGIE_N_ELEMENTS; i++) {
            Mods m;
            m.pitch_q16 = 3 * 65536;   // far up: must clamp, not wrap
            m.dur_q8 = 512;
            m.fm_depth_q8 = 512;
            m.harm_q8 = 512;
            m.mix_q8 = 512;
            m.noise_q16 = 65536;
            std::vector<int16_t> y = RenderOne(i, m);
            size_t want = (size_t)budgie_elements[i].dur_ms * 48 * 2;
            if (y.size() + 2 < want || y.size() > want + 2) {
                printf("element %d: %zu samples, want %zu\n", i, y.size(), want);
                fails++;
            }
        }
        // queue: three elements with gaps play back to back
        Voice v;
        Mods m;
        v.Enqueue(0, m, 0);
        v.Enqueue(1, m, 480);
        v.Enqueue(2, m, 480);
        int onsets = 0;
        long n = 0;
        while (v.Active()) {
            v.Process();
            onsets += v.Onset();
            n++;
        }
        if (onsets != 3) { printf("queue: %d onsets\n", onsets); fails++; }
        // Cut(): the current element fades out over kRelease samples instead of
        // stopping dead, then the queued one starts
        {
            Voice c;
            int loud = 0;
            for (int i = 0; i < BUDGIE_N_ELEMENTS; i++)
                if (budgie_elements[i].n_f0 && budgie_elements[i].dur_ms > 100 &&
                    budgie_elements[i].level > budgie_elements[loud].level)
                    loud = i;
            c.Enqueue(loud, Mods());
            int32_t prev = 0, peak = 0;
            for (int i = 0; i < 2000; i++) {
                prev = c.Process();
                if (prev > peak) peak = prev;
            }
            c.Cut();
            c.Enqueue(loud, Mods());
            int starts = 0, before_start = 0;
            for (int i = 0; i < kRelease + 2; i++) {
                int32_t y = c.Process();
                if (c.Onset()) starts++;
                else if (!starts) before_start = y;
            }
            if (starts != 1) { printf("cut: %d starts after release\n", starts); fails++; }
            if (before_start * 20 > peak || -before_start * 20 > peak) {
                printf("cut: %d just before restart (peak %d)\n", before_start, peak);
                fails++;
            }
        }
        // two voices told to share: never two starts in one sample
        {
            Voice a, b;
            for (int i = 0; i < 6; i++) {
                a.Enqueue(i, Mods(), 0);
                b.Enqueue(i, Mods(), 0);
            }
            int both = 0;
            while (a.Active() || b.Active()) {
                a.Process();
                b.Process(!a.Onset());
                both += a.Onset() && b.Onset();
            }
            if (both) { printf("shared starts: %d samples with two starts\n", both); fails++; }
        }
        printf("%s\n", fails ? "FAIL" : "OK");
        return fails != 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: render <out.bin> | --mods\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "wb");
    if (!f) return 1;
    for (int i = 0; i < BUDGIE_N_ELEMENTS; i++) {
        std::vector<int16_t> y = RenderOne(i, Mods());
        int32_t n = (int32_t)y.size();
        fwrite(&n, 4, 1, f);
        fwrite(y.data(), 2, y.size(), f);
    }
    fclose(f);
    return 0;
}
