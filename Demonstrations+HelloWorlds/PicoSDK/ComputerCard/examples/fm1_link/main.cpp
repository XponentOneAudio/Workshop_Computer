// FM1 Link example: Workshop Computer <-> M-VAVE FM1 over USB
//
// The Computer is the USB host; plug the FM1 into the Computer's USB-C port
// (see README.md about power). Works in two levels:
//
//  Any FM1 firmware (plain USB MIDI)
//    FM1 -> Computer  notes on MIDI channel 1  -> CV Out 1 (1V/oct), Pulse Out 1 (gate)
//                     velocity                 -> CV Out 2
//                     MIDI clock               -> Pulse Out 2 (16th-note triggers)
//    Computer -> FM1  Pulse In 1 + CV In 1     -> notes on MIDI channel 2 (FM1 track 2)
//                     Knob X                   -> CC 1 on channel 2
//
//  fm1_open firmware with the Workshop Link page (HOME + ARP on the FM1)
//    The FM1 screen shows the knobs, CV inputs, pulse inputs, the last note
//    received from the FM1 and a scope of Audio In 1 (Knob Y = timebase).
//    FM1 encoder 1 transposes the notes the Computer sends.
//
// To get notes out of the FM1, set track 1 to send MIDI over USB on the FM1's
// SEQ 6/8 MIDI TRACK page.
//
// LEDs: top-left = MIDI device attached, top-right = Workshop Link session,
// middle = gate out / clock out, bottom = pulse inputs.

#include "ComputerCard.h"
#include "WorkshopLinkHost.h"

class FM1Link : public ComputerCard
{
public:
	FM1Link()
	{
		link.SetName("FM1 LINK DEMO");
		link.SetTitle("WORKSHOP COMPUTER");
		link.ShowOnConnect(true);

		wMain = link.AddWidget(WL_W_BAR, 8, 36, 72, 26, WL_C_ACCENT, "MAIN", 0, 4095);
		wX = link.AddWidget(WL_W_BAR, 84, 36, 72, 26, WL_C_ACCENT, "X  CC1", 0, 4095);
		wY = link.AddWidget(WL_W_BAR, 160, 36, 72, 26, WL_C_ACCENT, "Y  TIME", 0, 4095);
		wCV1 = link.AddWidget(WL_W_VALUE, 8, 68, 72, 24, WL_C_TEXT, "CV IN 1");
		wCV2 = link.AddWidget(WL_W_VALUE, 84, 68, 72, 24, WL_C_TEXT, "CV IN 2");
		wTranspose = link.AddWidget(WL_W_VALUE, 160, 68, 72, 24, WL_C_AMBER, "TRANSPOSE");
		wP1 = link.AddWidget(WL_W_LED, 8, 98, 72, 12, WL_C_GREEN, "PULSE 1");
		wP2 = link.AddWidget(WL_W_LED, 84, 98, 72, 12, WL_C_GREEN, "PULSE 2");
		wSwitch = link.AddWidget(WL_W_VALUE, 160, 98, 72, 24, WL_C_DIM, "SWITCH");
		wNote = link.AddWidget(WL_W_TEXT, 8, 126, 224, 18, WL_C_GREEN, "FROM FM1");
		wScope = link.AddWidget(WL_W_SCOPE, 8, 148, 224, 72, WL_C_ACCENT, "AUDIO IN 1");

		link.SetText(wNote, "-");
		link.Start();
	}

	virtual void ProcessSample()
	{
		HandleIncomingMIDI();
		SendOutgoingMIDI();
		UpdateScreen();
		UpdateLeds();
	}

private:
	WorkshopLinkHost link;
	int wMain, wX, wY, wCV1, wCV2, wTranspose, wP1, wP2, wSwitch, wNote, wScope;

	// FM1 -> Computer
	int heldNote = -1;
	int clockCount = 0;
	int clockPulse = 0;

	// Computer -> FM1
	int sentNote = -1;
	int transpose = 0;
	int lastCC = -1;
	int ccTimer = 0;

	int scopeCounter = 0;

	void HandleIncomingMIDI()
	{
		uint8_t m[3];
		while (int n = link.ReadMIDI(m))
		{
			uint8_t kind = m[0] & 0xF0, ch = m[0] & 0x0F;
			if (ch == 0 && n == 3 && (kind == 0x90 || kind == 0x80))
			{
				bool on = kind == 0x90 && m[2] > 0;
				if (on)
				{
					heldNote = m[1];
					CVOut1MIDINote(m[1]);
					CVOut2(int16_t(m[2] * 16)); // velocity 0..127 -> 0..2032
					ShowNote(m[1], m[2]);
				}
				else if (m[1] == heldNote)
				{
					heldNote = -1;
				}
			}
			else if (m[0] == 0xF8)
			{
				if (clockCount == 0) clockPulse = 480; // 10 ms trigger
				clockCount = (clockCount + 1) % 6;      // 24 ppqn -> 16ths
			}
			else if (m[0] == 0xFA)
			{
				clockCount = 0;
			}
		}
		PulseOut1(heldNote >= 0);
		if (clockPulse > 0) clockPulse--;
		PulseOut2(clockPulse > 0);
	}

	void SendOutgoingMIDI()
	{
		transpose += link.EncoderDelta(0);
		if (transpose > 24) transpose = 24;
		if (transpose < -24) transpose = -24;

		if (PulseIn1RisingEdge())
		{
			// CV In 1: about 341 counts per volt; 0 V = middle C.
			int note = 60 + transpose + (CVIn1() * 12 + (CVIn1() >= 0 ? 170 : -170)) / 341;
			if (note < 0) note = 0;
			if (note > 127) note = 127;
			if (sentNote >= 0) link.NoteOff(1, sentNote);
			if (link.NoteOn(1, note, 100)) sentNote = note;
		}
		if (PulseIn1FallingEdge() && sentNote >= 0)
		{
			if (link.NoteOff(1, sentNote)) sentNote = -1;
		}

		// Knob X -> CC 1, at most every 5 ms and only when it changes.
		if (++ccTimer >= 240)
		{
			ccTimer = 0;
			int cc = KnobVal(X) >> 5;
			if (cc != lastCC && link.ControlChange(1, 1, cc)) lastCC = cc;
		}
	}

	void UpdateScreen()
	{
		link.SetValue(wMain, int16_t(KnobVal(Main)));
		link.SetValue(wX, int16_t(KnobVal(X)));
		link.SetValue(wY, int16_t(KnobVal(Y)));
		// Show CV inputs in tenths of a volt, without noise flicker.
		link.SetValue(wCV1, int16_t(CVIn1() * 10 / 341));
		link.SetValue(wCV2, int16_t(CVIn2() * 10 / 341));
		link.SetValue(wTranspose, int16_t(transpose));
		link.SetValue(wP1, PulseIn1());
		link.SetValue(wP2, PulseIn2());
		link.SetValue(wSwitch, int16_t(SwitchVal()));

		// Knob Y sets the scope timebase: one point every 8..256 samples.
		int every = 8 + (KnobVal(Y) >> 4);
		if (++scopeCounter >= every)
		{
			scopeCounter = 0;
			link.PushScope(wScope, AudioIn1());
		}
	}

	void UpdateLeds()
	{
		LedOn(0, link.GetState() != WorkshopLinkHost::NoDevice);
		LedOn(1, link.IsLinked());
		LedOn(2, heldNote >= 0);
		LedOn(3, clockPulse > 0);
		LedOn(4, PulseIn1());
		LedOn(5, PulseIn2());
	}

	void ShowNote(int note, int velocity)
	{
		static const char names[12][3] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
		char s[24];
		int at = 0;
		for (const char *p = names[note % 12]; *p; ++p) s[at++] = *p;
		int octave = note / 12 - 1;
		if (octave < 0) { s[at++] = '-'; octave = -octave; }
		s[at++] = char('0' + octave);
		const char *v = "  VEL ";
		while (*v) s[at++] = *v++;
		if (velocity >= 100) s[at++] = char('0' + velocity / 100);
		if (velocity >= 10) s[at++] = char('0' + velocity / 10 % 10);
		s[at++] = char('0' + velocity % 10);
		s[at] = 0;
		link.SetText(wNote, s);
	}
};

int main()
{
	set_sys_clock_khz(144000, true);
	static FM1Link card;
	card.Run();
}
