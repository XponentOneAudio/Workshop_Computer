// Host simulation of the whole card: main.cpp running against a stand-in for
// ComputerCard, so the panel logic can be tested and heard without hardware.
//
//   card_sim --test         checks triggers, modes and outputs
//   card_sim <tour.wav>     renders a stereo tour of every category (Out 1 left,
//                           Out 2 right)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// --- stand-in for ComputerCard.h -------------------------------------------
#define COMPUTERCARD_H
#define __not_in_flash_func(f) f
inline void set_sys_clock_khz(int, bool) {}

class ComputerCard
{
public:
    enum Knob { Main, X, Y };
    enum Switch { Down, Middle, Up };
    enum Input { Audio1, Audio2, CV1, CV2, Pulse1, Pulse2 };
    virtual ~ComputerCard() {}
    virtual void ProcessSample() = 0;
    void Run() {}
    void EnableNormalisationProbe() {}

    int32_t KnobVal(Knob k) { return knobs[k]; }
    Switch SwitchVal() { return sw; }
    bool SwitchChanged() { return sw != last_sw; }
    bool PulseIn1RisingEdge() { return edge[0]; }
    bool PulseIn2RisingEdge() { return edge[1]; }
    bool Connected(Input i) { return connected[i]; }
    int16_t CVIn1() { return cv[0]; }
    int16_t CVIn2() { return cv[1]; }
    void AudioOut1(int16_t v) { out[0] = v; }
    void AudioOut2(int16_t v) { out[1] = v; }
    void CVOut1(int16_t v) { cvout[0] = v; }
    void CVOut2(int16_t v) { cvout[1] = v; }
    bool CVOut1Millivolts(int32_t mv) { cv1_mv = mv; return false; }
    void PulseOut1(bool v) { pout[0] = v; }
    void PulseOut2(bool v) { pout[1] = v; }
    void LedBrightness(uint32_t i, uint16_t v) { leds[i] = v; }
    void LedOn(uint32_t i, bool v = true) { leds[i] = v ? 4095 : 0; }
    void LedOff(uint32_t i) { leds[i] = 0; }

    // simulation state
    int32_t knobs[3] = {0, 2048, 2048};
    Switch sw = Middle, last_sw = Middle;
    bool edge[2] = {}, connected[6] = {};
    int16_t cv[2] = {};
    int16_t out[2] = {}, cvout[2] = {};
    int32_t cv1_mv = 0;
    bool pout[2] = {};
    uint16_t leds[6] = {};

    void Step()
    {
        ProcessSample();
        last_sw = sw;
        edge[0] = edge[1] = false;
    }
};
// ---------------------------------------------------------------------------

#define main card_main
#include "../main.cpp"
#undef main

static void SetCategory(Budgie &c, int cat) { c.knobs[ComputerCard::Main] = cat * 4096 / 7 + 4096 / 14; }

static void Run(Budgie &c, int samples, std::vector<int16_t> *wav = nullptr)
{
    for (int i = 0; i < samples; i++) {
        c.Step();
        if (wav) {
            wav->push_back(c.out[0]);
            wav->push_back(c.out[1]);
        }
    }
}

static void PressZ(Budgie &c, std::vector<int16_t> *wav = nullptr)
{
    c.sw = ComputerCard::Down;
    Run(c, 2400, wav);  // 50 ms press
    c.sw = ComputerCard::Middle;
}

static int Test()
{
    int fails = 0;
    auto check = [&](bool ok, const char *what) {
        if (!ok) {
            printf("FAIL: %s\n", what);
            fails++;
        }
    };

    Budgie c;
    Run(c, kBootMute + 10);
    // silent with nothing triggered
    int32_t loud = 0;
    for (int i = 0; i < 4800; i++) {
        c.Step();
        loud |= c.out[0] | c.out[1];
    }
    check(loud == 0, "silent until triggered");

    for (int cat = 0; cat < BUDGIE_N_CATEGORIES; cat++) {
        SetCategory(c, cat);
        Run(c, 480);
        int32_t peak1 = 0, peak2 = 0, onsets = 0, n1 = 0, n2 = 0;
        bool lit_ok = true;
        c.sw = ComputerCard::Down;
        for (int i = 0; i < 48000; i++) {
            if (i == 2400) c.sw = ComputerCard::Middle;
            bool before = c.pout[0];
            c.Step();
            onsets += c.pout[0] && !before;
            if (c.out[0]) n1 = i;
            if (c.out[1]) n2 = i;
            peak1 = c.out[0] > peak1 ? c.out[0] : peak1;
            peak2 = c.out[1] > peak2 ? c.out[1] : peak2;
            if (cat < 6)
                for (int l = 0; l < 6; l++) lit_ok &= (c.leds[l] > 0) == (l == cat);
        }
        char msg[96];
        snprintf(msg, sizeof msg, "category %c: Out 1 sounds", 'A' + cat);
        check(peak1 > 20, msg);
        snprintf(msg, sizeof msg, "category %c: Out 2 sounds", 'A' + cat);
        check(peak2 > 20, msg);
        snprintf(msg, sizeof msg, "category %c: one onset pulse per Z press", 'A' + cat);
        check(onsets == 1, msg);
        snprintf(msg, sizeof msg, "category %c: LED shows the category", 'A' + cat);
        check(lit_ok, msg);
        bool tonal = cat == BUDGIE_B || cat == BUDGIE_C || cat == BUDGIE_D || cat == BUDGIE_G;
        snprintf(msg, sizeof msg, "category %c: syllable (Out 2) outlasts the element (Out 1)", 'A' + cat);
        if (tonal) check(n2 > n1, msg);
    }

    // Pulse In 1 triggers; a patched Pulse In 2 triggers only Out 2
    SetCategory(c, BUDGIE_B);
    c.connected[ComputerCard::Pulse2] = true;
    Run(c, 480);
    c.edge[1] = true;
    int32_t p1 = 0, p2 = 0;
    for (int i = 0; i < 24000; i++) {
        c.Step();
        p1 |= c.out[0];
        p2 |= c.out[1];
    }
    check(p1 == 0 && p2 != 0, "Pulse In 2 (patched) plays Out 2 only");
    c.edge[0] = true;
    p1 = 0;
    for (int i = 0; i < 24000; i++) {
        c.Step();
        p1 |= c.out[0];
    }
    check(p1 != 0, "Pulse In 1 plays Out 1");
    c.connected[ComputerCard::Pulse2] = false;

    // F: Y right of centre makes click trains
    SetCategory(c, BUDGIE_F);
    c.knobs[ComputerCard::Y] = 4095;
    Run(c, 480);
    int trains = 0;
    c.sw = ComputerCard::Down;
    for (int i = 0; i < 24000; i++) {
        if (i == 2400) c.sw = ComputerCard::Middle;
        bool before = c.pout[0];
        c.Step();
        trains += c.pout[0] && !before;
    }
    check(trains >= 6, "F with Y right: a train of clicks");
    c.knobs[ComputerCard::Y] = 2048;

    // Z up plays on its own, repeatedly
    SetCategory(c, BUDGIE_C);
    c.sw = ComputerCard::Up;
    int autos = 0;
    for (int i = 0; i < 4 * 48000; i++) {
        bool before = c.pout[0];
        c.Step();
        autos += c.pout[0] && !before;
    }
    check(autos >= 4, "Z up auto-plays");
    c.sw = ComputerCard::Middle;

    // CV 1 transposes: an octave up should roughly double the tracked pitch CV
    SetCategory(c, BUDGIE_B);
    Run(c, 4800);
    PressZ(c);
    Run(c, 1200);
    int32_t mv0 = c.cv1_mv;
    Run(c, 24000);
    c.cv[0] = 341;  // ~1 V
    PressZ(c);
    Run(c, 1200);
    int32_t mv1 = c.cv1_mv;
    check(mv1 - mv0 > 800 && mv1 - mv0 < 1200, "CV 1 at +1 V raises pitch CV by about 1 V");
    c.cv[0] = 0;

    printf("%s\n", fails ? "FAIL" : "OK");
    return fails != 0;
}

static void WriteWav(const char *path, const std::vector<int16_t> &s)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t data = (uint32_t)s.size() * 2, riff = 36 + data, sr = 48000, br = sr * 4, fmt = 16;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f);
    fwrite(&br, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (int16_t v : s) {
        int16_t x = (int16_t)(v * 16);  // DAC counts -> 16-bit
        fwrite(&x, 2, 1, f);
    }
    fclose(f);
}

static int Tour(const char *path)
{
    // Each category: four variants across X, each pressed twice; then a sweep of Y.
    Budgie c;
    std::vector<int16_t> wav;
    Run(c, kBootMute + 10);
    for (int cat = 0; cat < BUDGIE_N_CATEGORIES; cat++) {
        SetCategory(c, cat);
        c.knobs[ComputerCard::Y] = 2048;
        for (int v = 0; v < 4; v++) {
            c.knobs[ComputerCard::X] = v * 1024 + 512;
            for (int r = 0; r < 2; r++) {
                PressZ(c, &wav);
                Run(c, 21600, &wav);
            }
        }
        for (int y = 0; y < 5; y++) {
            c.knobs[ComputerCard::Y] = y * 1023;
            PressZ(c, &wav);
            Run(c, 21600, &wav);
        }
        Run(c, 24000, &wav);
    }
    WriteWav(path, wav);
    printf("%s: %.1f s\n", path, (double)wav.size() / 2 / 48000);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--test")) return Test();
    if (argc == 2) return Tour(argv[1]);
    fprintf(stderr, "usage: card_sim --test | <tour.wav>\n");
    return 2;
}
