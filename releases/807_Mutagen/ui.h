// Mutagen: the 8mu's pages, fader pickup and LEDs, with no hardware in it,
// so it can be tested on a PC (test/engine_test.cpp).  The web editor
// (web/index.html) does the same for an 8mu plugged into the computer.

#ifndef MUTAGEN_UI_H
#define MUTAGEN_UI_H

#include "mutagen.h"

namespace mutagen
{

class EightMuUi
{
public:
	// The page the faders edit: 0-3 the first pages of buttons A-D (MUTANT
	// 1, MUTANT 2, OSC, LFO 1), 4-7 their second pages (WARP 1, WARP 2,
	// MOD, LFO 2)
	int page = PageMut1;

	// A button press: the button's first page, or, if one of its pages is
	// already showing, its other one
	static int NextPage(int pg, int button)
	{
		return ButtonOf(pg) == button ? (pg + 4) % kPages : button;
	}

	// An 8mu fader (0-127) as a value (0-4095)
	static int32_t FaderValue(int32_t f) {return (f * 4095) / 127;}

	// Forget where the faders were (on connecting), so none edits anything
	// until it reaches its stored value
	void Reset()
	{
		for (int i = 0; i < kFaders; i++) latched[i] = false;
		for (int b = 0; b < kButtons; b++) prevButton[b] = false;
		lastValid = false;
	}

	void SetPage(int pg)
	{
		if (pg == page) return;
		page = pg;
		for (int i = 0; i < kFaders; i++) latched[i] = false;
	}

	bool Latched(int i) const {return latched[i];}

	// At control rate: the 8mu's faders (0-127) and buttons.  Returns true
	// if a button changed the page.
	bool Update(Engine &e, const int32_t fader[kFaders], const bool button[kButtons])
	{
		bool turned = false;
		for (int b = 0; b < kButtons; b++)
		{
			if (button[b] && !prevButton[b])
			{
				SetPage(NextPage(page, b));
				turned = true;
			}
			prevButton[b] = button[b];
		}

		for (int i = 0; i < kFaders; i++)
		{
			int32_t f = FaderValue(fader[i]);
			volatile uint16_t &stored = e.values[page][i];
			// Something else (a knob, the web editor) changed the value: the
			// fader lets go, and must reach the new value to take it again
			if (latched[i] && stored != written[i]) latched[i] = false;
			// Pickup: a fader takes over its value once it is moved to (or
			// across) the value already stored, so changing page never
			// changes the sound by accident
			if (!latched[i])
			{
				int32_t d = f - stored;
				bool near = d >= -100 && d <= 100;
				bool crossed = lastValid && ((last[i] - stored < 0) != (d < 0));
				int n = ZonesOf(page, i);
				if (n && ZoneOf(f, n) == ZoneOf(stored, n)) near = true;
				if (near || crossed) latched[i] = true;
			}
			if (latched[i])
			{
				stored = uint16_t(f);
				written[i] = uint16_t(f);
			}
			last[i] = f;
		}
		lastValid = true;
		return turned;
	}

	// The 8mu's LEDs, 0-4095 each: each fader's setting (how far from the
	// centre, for one that goes either way; which choice, for one that
	// picks from a list), and for a few, what it is doing now: a mutant's
	// depth and mix after modulation, and each LFO's rate fader pulsing
	// with the LFO.
	void Leds(const Engine &e, int32_t led[kFaders]) const
	{
		for (int i = 0; i < kFaders; i++)
		{
			int32_t v = e.values[page][i];
			int n = ZonesOf(page, i);
			int32_t b;
			if (n && !IsBipolar(page, i)) b = ((ZoneOf(v, n) + 1) * 4095) / n;
			else if (IsBipolar(page, i)) {int32_t q = Bipolar(v); b = (q < 0 ? -q : q) >> 3;}
			else b = v;
			if (page == PageMut1 || page == PageMut2)
			{
				const Stage &st = e.stage[page == PageMut2 ? 1 : 0];
				if (i == MDepth) b = st.ShownDepth() >> 3;
				if (i == MMix) b = st.ShownMix() >> 3;
			}
			if ((page == PageLfo1 || page == PageLfo2) && i == LRate)
				b = (e.lfo[page == PageLfo2 ? 1 : 0].out + 32768) >> 4;
			led[i] = b < 0 ? 0 : (b > 4095 ? 4095 : b);
		}
	}

private:
	bool latched[kFaders] = {};
	int32_t last[kFaders] = {};
	uint16_t written[kFaders] = {};
	bool prevButton[kButtons] = {};
	bool lastValid = false;
};

} // namespace mutagen

#endif
