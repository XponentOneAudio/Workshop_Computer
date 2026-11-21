// Renders the example sounds in this folder with the card's own engine
// (../mutagen.h): 48kHz stereo WAVs, Audio Out 1 left and Audio Out 2 right,
// at the card's 12-bit resolution.  The oscillator drones on the card; here
// each note gets a short fade in and out, as a VCA after it would give.
//   g++ -std=c++17 -O2 render.cpp -o render && ./render
#include "../mutagen.h"
#include <stdio.h>
#include <math.h>
#include <vector>
#include <functional>
#include <algorithm>
using namespace mutagen;

static Tables gTables;
static const int kRate = 48000;

static int32_t BV(int pct) {return BipolarValue(pct);}
static int32_t RV(float r) {return ValueForRatio(r);}
static int32_t ZV(int z, int n) {return ValueForZone(z, n);}
static int32_t Wv(int w) {return (w * 4095) / 3;}
// The Main knob for a MIDI note (C1 = 24 at fully left, C4 = 60 at the centre)
static int32_t KnobForNote(float note) {return int32_t(lroundf((note - 24.0f) / 72.0f * 4095.0f));}

struct Note {float start, length, note;};
struct Change {int page, fader; int32_t value;};
struct Scene
{
	const char *file;
	std::vector<Change> changes;
	std::vector<Note> notes;
	float seconds;
	// Knob moves and the like, every control tick: time in seconds
	std::function<void(Engine &, float)> perform;
};

static void WriteWav(const char *name, const std::vector<int16_t> &lr)
{
	FILE *f = fopen(name, "wb");
	auto u32 = [&](uint32_t v) {fwrite(&v, 4, 1, f);};
	auto u16 = [&](uint16_t v) {fwrite(&v, 2, 1, f);};
	uint32_t bytes = uint32_t(lr.size() * 2);
	fwrite("RIFF", 1, 4, f); u32(36 + bytes); fwrite("WAVEfmt ", 1, 8, f);
	u32(16); u16(1); u16(2); u32(kRate); u32(kRate * 4); u16(4); u16(16);
	fwrite("data", 1, 4, f); u32(bytes);
	fwrite(lr.data(), 2, lr.size(), f);
	fclose(f);
}

static void Render(const Scene &s)
{
	static Engine e;
	e.DefaultValues();
	for (auto &c : s.changes) e.values[c.page][c.fader] = uint16_t(c.value);
	e.pitchKnob = s.notes.empty() ? 2048 : KnobForNote(s.notes[0].note);
	e.Begin(&gTables);
	Inputs in;
	int n = int(s.seconds * kRate);
	std::vector<int16_t> out;
	out.reserve(size_t(n) * 2);
	const float fade = 0.006f;
	for (int i = 0; i < n; i++)
	{
		float t = float(i) / kRate;
		float amp = 0;
		for (const Note &nt : s.notes)
			if (t >= nt.start && t < nt.start + nt.length)
			{
				if (i % 16 == 0) e.pitchKnob = KnobForNote(nt.note);
				float a = std::min({1.0f, (t - nt.start) / fade, (nt.start + nt.length - t) / 0.03f});
				amp = std::max(amp, a);
			}
		if (s.perform && i % 16 == 0) s.perform(e, t);
		e.Tick(in);
		out.push_back(int16_t(lroundf(e.out1 * 16 * amp)));
		out.push_back(int16_t(lroundf(e.out2 * 16 * amp)));
	}
	WriteWav(s.file, out);
	printf("%s\n", s.file);
}

// A phrase: notes one after another, each len seconds (gap: a fraction off)
static std::vector<Note> Phrase(std::vector<float> notes, float len, float gap = 0.1f, float start = 0, int repeats = 1)
{
	std::vector<Note> v;
	float t = start;
	for (int r = 0; r < repeats; r++)
		for (float nn : notes) {v.push_back({t, len * (1 - gap), nn}); t += len;}
	return v;
}
static std::vector<Note> Hold(float note, float seconds) {return {{0.0f, seconds, note}};}
static float Tri(float t, float period) {float p = fmodf(t / period, 1.0f); return p < 0.5f ? 2 * p : 2 - 2 * p;}

int main()
{
	gTables.Build();
	const std::vector<float> bass = {36, 36, 48, 36, 39, 36, 43, 41};
	std::vector<Scene> scenes;
	scenes.push_back(Scene{"01_default_saw_pulse.wav", {}, Phrase({48, 48, 55, 51, 53, 51, 46, 48}, 0.5f, 0.15f, 0, 2), 8, nullptr});
	scenes.push_back(Scene{"02_classic_pwm_square.wav", {{PageOsc, OWave, Wv(3)}, {PageMut1, MDepth, 1400}, {PageMut1, MLfo1, BV(55)},
			{PageLfo1, LRate, 2200}, {PageMut2, MMix, 0}}, Phrase(bass, 0.25f, 0.2f, 0, 4), 8, nullptr});
	scenes.push_back(Scene{"03_pwm_from_two_saws.wav", {{PageOsc, OWave, Wv(2)}, {PageMut1, MMode, ZV(PhaseDiff, kModes)}, {PageMut1, MDepth, 1600},
			{PageMut1, MLfo1, BV(45)}, {PageLfo1, LRate, 1800}, {PageMut1, MLfo2, BV(15)}, {PageMut2, MMix, 0}},
			{{0, 3.9f, 48}, {4, 3.9f, 51}}, 8, nullptr});
	scenes.push_back(Scene{"04_sync_sweep_knob.wav", {{PageOsc, OWave, Wv(2)}, {PageMut1, MMode, ZV(Sync, kModes)}, {PageMut1, MLfo1, 2048},
			{PageMut2, MMix, 0}}, Hold(43, 8), 8, [](Engine &e, float t) {e.values[PageMut1][MDepth] = uint16_t(4095 * Tri(t, 8));}});
	scenes.push_back(Scene{"05_windowed_sync_formants.wav", {{PageOsc, OWave, Wv(0)}, {PageMut1, MMode, ZV(Sync, kModes)}, {PageMut1, MDepth, 2300},
			{PageMut1, MWindow, 4095}, {PageMut1, MLfo1, BV(30)}, {PageLfo1, LRate, 1300}, {PageLfo1, LShape, ZV(LDrift, kShapes)},
			{PageMut2, MMode, ZV(Pulse, kModes)}, {PageMut2, MDepth, 900}, {PageMut2, MMix, 2600}},
			Phrase({45, 48, 52, 50, 48, 45, 43, 45}, 1.0f, 0.05f), 8, nullptr});
	scenes.push_back(Scene{"06_pulse_sync_ratio2.wav", {{PageOsc, OWave, Wv(3)}, {PageMut1, MMode, ZV(Sync, kModes)}, {PageMut1, MRatio, RV(2)},
			{PageMut1, MDepth, 1200}, {PageMut1, MLfo1, 2048}, {PageMut2, MMode, ZV(Pulse, kModes)}, {PageMut2, MDepth, 1800},
			{PageMut2, MLfo1, BV(40)}, {PageLfo1, LRate, 2000}}, Phrase(bass, 0.25f, 0.2f, 0, 4), 8, nullptr});
	scenes.push_back(Scene{"07_mirror_and_squeeze.wav", {{PageOsc, OWave, Wv(1)}, {PageMut1, MMode, ZV(Mirror, kModes)}, {PageMut1, MDepth, 2600},
			{PageMut1, MLfo1, BV(30)}, {PageMut2, MMode, ZV(Squeeze, kModes)}, {PageMut2, MDepth, 1500}, {PageMut2, MLfo2, BV(35)},
			{PageMut2, MMix, 3000}}, Phrase({40, 47, 52, 47}, 1.0f, 0.05f, 0, 2), 8, nullptr});
	scenes.push_back(Scene{"08_warp_bells_ratio3.wav", {{PageOsc, OWave, Wv(0)}, {PageMut1, MMode, ZV(Warp, kModes)}, {PageMut1, MRatio, RV(3)},
			{PageMut1, MDepth, 3000}, {PageMut1, MLfo1, BV(20)}, {PageWarp1, 0, 4095}, {PageWarp1, 1, 0}, {PageWarp1, 2, 3000},
			{PageWarp1, 3, 1000}, {PageWarp1, 4, 4095}, {PageWarp1, 5, 2048}, {PageWarp1, 6, 0}, {PageWarp1, 7, 2500},
			{PageMut2, MMix, 0}}, Phrase({60, 67, 72, 71, 67, 64, 62, 60}, 0.5f, 0.3f, 0, 2), 8, nullptr});
	scenes.push_back(Scene{"09_squeeze_bass_with_sub_R.wav", {{PageOsc, OWave, Wv(2)}, {PageMut1, MMode, ZV(Squeeze, kModes)}, {PageMut1, MDepth, 2400},
			{PageMut1, MLfo1, 2048}, {PageMut2, MMode, ZV(Pulse, kModes)}, {PageMut2, MDepth, 1500}, {PageOsc, ODrive, 1200},
			{PageOsc, OOut2, ZV(Out2Sub1, kOut2Modes)}}, Phrase(bass, 0.25f, 0.3f, 0, 4), 8,
			[](Engine &e, float t) {e.values[PageMut1][MDepth] = uint16_t(1200 + 2800 * Tri(t, 4));}});
	scenes.push_back(Scene{"10_feedback_grit.wav", {{PageOsc, OWave, Wv(3)}, {PageMut1, MDepth, 2000}, {PageMut1, MFeedback, 2200},
			{PageMut2, MDepth, 1300}, {PageMut2, MFeedback, 1200}, {PageMut2, MMix, 2800}, {PageOsc, OBits, 1900}},
			Phrase({36, 36, 39, 41}, 0.5f, 0.2f, 0, 4), 8, nullptr});
	scenes.push_back(Scene{"11_subharmonic_ratio_2_3.wav", {{PageOsc, OWave, Wv(2)}, {PageMut1, MMode, ZV(Pulse, kModes)}, {PageMut1, MRatio, RV(2.0f / 3.0f)},
			{PageMut1, MDepth, 2600}, {PageMut1, MLfo1, BV(25)}, {PageMut2, MMix, 0}}, {{0, 3.9f, 45}, {4, 3.9f, 40}}, 8, nullptr});
		// The ratio knob (switch in the middle) swept up through the whole
		// numbers and back, on a pulse
	scenes.push_back(Scene{"12_ratio_sweep_knob.wav", {{PageOsc, OWave, Wv(2)}, {PageMut1, MMode, ZV(Pulse, kModes)}, {PageMut1, MDepth, 2400},
			{PageMut1, MLfo1, 2048}, {PageMut2, MMix, 0}}, Hold(43, 10), 10,
			[](Engine &e, float t) {e.values[PageMut1][MRatio] = uint16_t(ValueForRatio(1.0f) + (4095 - ValueForRatio(1.0f)) * Tri(t, 10));}});
		// Each mode in turn, depth swept up and down by the X knob
	scenes.push_back(Scene{"13_mode_tour.wav", {{PageOsc, OWave, Wv(2)}, {PageMut1, MLfo1, 2048}, {PageMut2, MMix, 0}}, Hold(48, 12), 12,
			[](Engine &e, float t) {
				int mode = std::min(5, int(t / 2));
				e.values[PageMut1][MMode] = uint16_t(ValueForZone(mode, kModes));
				e.values[PageMut1][MDepth] = uint16_t(4095 * 0.9f * Tri(t, 2));}});
	for (const Scene &s : scenes) Render(s);
	return 0;
}
