/*
WorkshopLinkHost

Connects a Workshop Computer card, acting as USB host, to a USB-MIDI
instrument such as the M-VAVE FM1. It provides:

  - plain USB MIDI in both directions (notes, CCs, clock...), which works
    with any class-compliant USB MIDI device, including a stock FM1;
  - a Workshop Link session (see WorkshopLink.h): when the device runs a
    firmware that implements the protocol (fm1_open with the link page),
    the card can show labelled values, bars, indicators, text and an
    oscilloscope trace on the device's screen, and read the device's
    encoders and buttons back.

All of the USB work runs on the second RP2040 core. Everything a card calls
from ProcessSample() is non-blocking and lock-free.

Usage:

	#include "ComputerCard.h"
	#include "WorkshopLinkHost.h"

	class MyCard : public ComputerCard
	{
		WorkshopLinkHost link;
		int knob;
	public:
		MyCard()
		{
			link.SetName("MY CARD");
			link.SetTitle("MY CARD");
			knob = link.AddWidget(WL_W_BAR, 8, 40, 224, 22, WL_C_ACCENT, "MAIN KNOB", 0, 4095);
			link.Start(); // claims core1
		}
		virtual void ProcessSample()
		{
			link.SetValue(knob, KnobVal(Main));
			uint8_t m[3];
			while (int n = link.ReadMIDI(m)) { ... }
		}
	};

It uses TinyUSB's built-in MIDI host class (TinyUSB 0.18 or later; the
Workshop_Computer devcontainer pins 0.20.0), so no extra driver source is
needed. Build (CMake), with this file's directory on the include path:

	target_link_libraries(mycard pico_multicore tinyusb_host tinyusb_board)

and a host-mode tusb_config.h beside the card that sets CFG_TUH_MIDI 1
(copy examples/fm1_link/tusb_config.h).

Threading rules:
  - Layout calls (SetName, SetTitle, AddWidget) only before Start().
  - SetValue may be called from either core. SetText, PushScope, SendMIDI,
    ReadMIDI, EncoderDelta, Buttons should each be called from one core only
    (normally core 0, from ProcessSample or code it calls).
  - SetUserHandler's callback runs on core 1.

Power: when the Computer is a USB host it supplies the device's 5 V, with a
budget of roughly 150 mA. A device that charges a battery from USB can draw
more; use a powered USB hub (hubs are supported) or run the device on its
own power. See the fm1_link example README.

MIT License

Copyright (c) 2026 Workshop Link contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#ifndef WORKSHOP_LINK_HOST_H
#define WORKSHOP_LINK_HOST_H

#include <stdint.h>
#include <string.h>

#include "pico/multicore.h"
#include "pico/time.h"
#include "hardware/sync.h"
#include "bsp/board_api.h"
#include "tusb.h"

#include "WorkshopLink.h"

class WorkshopLinkHost
{
public:
	enum State
	{
		NoDevice,   // nothing (or nothing MIDI) attached
		Connecting, // MIDI device attached, asking it for a link session
		MidiOnly,   // device did not answer: plain USB MIDI still works
		Linked      // device answered: screen and controls available
	};

	static constexpr int MaxWidgets = WL_MAX_WIDGETS;
	static constexpr int Encoders = 7;

	WorkshopLinkHost()
	{
		instance = this;
		strcpy(name, "WORKSHOP");
		title[0] = 0;
	}

	/*** Setup, before Start() ***/

	// Name shown by the device for this card (up to 16 characters).
	void SetName(const char *s) { copy(name, sizeof name, s); }
	// Title of the link page (up to 23 characters).
	void SetTitle(const char *s) { copy(title, sizeof title, s); }
	// Ask the device to open its link page when the session starts.
	void ShowOnConnect(bool show) { showOnConnect = show; }

	// Add a widget; returns its id (0..15) or -1 when full or invalid.
	// x, y, w, h are screen pixels (FM1: 240 x 240, usable y 32..223).
	// min/max are used by WL_W_BAR. Up to two WL_W_SCOPE widgets.
	int AddWidget(wl_widget_type type, int x, int y, int w, int h, wl_color color,
	              const char *label, int16_t min = 0, int16_t max = 0)
	{
		if (started || widgetCount >= MaxWidgets || type >= WL_W_COUNT) return -1;
		if (type == WL_W_SCOPE && scopeCount >= int(WL_MAX_SCOPES)) return -1;
		Widget &wd = widgets[widgetCount];
		wd.type = uint8_t(type);
		wd.x = clamp8(x); wd.y = clamp8(y); wd.w = clamp8(w); wd.h = clamp8(h);
		wd.color = uint8_t(color);
		wd.min = min; wd.max = max;
		copy(wd.label, sizeof wd.label, label);
		wd.scope = type == WL_W_SCOPE ? int8_t(scopeCount++) : int8_t(-1);
		return widgetCount++;
	}

	// Launch the USB host on core 1. Call once, after the layout is set.
	void Start()
	{
		started = true;
		multicore_launch_core1(Core1Entry);
	}

	/*** Runtime, from ProcessSample or anywhere on core 0 ***/

	State GetState() const { return State(state); }
	bool IsLinked() const { return state == Linked; }
	// True while the device is showing the link page.
	bool PageVisible() const { return visible; }

	void SetValue(int id, int16_t v)
	{
		if (unsigned(id) < unsigned(widgetCount)) widgets[id].value = v;
	}

	// Copies up to 23 characters. Uses a sequence counter so core 1 never
	// sends a half-written string.
	void SetText(int id, const char *s)
	{
		if (unsigned(id) >= unsigned(widgetCount)) return;
		Widget &wd = widgets[id];
		wd.textSeq++;          // odd: writing
		__dmb();
		copy(wd.text, sizeof wd.text, s);
		__dmb();
		wd.textSeq++;          // even: stable
	}

	// Feed one point to a scope widget's rolling trace. Call at whatever rate
	// suits the signal (e.g. every 64th audio sample). Samples are 12-bit
	// ComputerCard values (-2048..2047).
	void PushScope(int id, int16_t sample)
	{
		if (unsigned(id) >= unsigned(widgetCount) || widgets[id].scope < 0) return;
		Scope &s = scopes[widgets[id].scope];
		int v = sample >> 4;
		if (v > 127) v = 127;
		if (v < -128) v = -128;
		s.points[s.write % ScopeRing] = int8_t(v);
		__dmb();
		s.write = s.write + 1;
	}

	// Queue a complete short MIDI message (1-3 bytes, no SysEx). Returns
	// false if the queue is full. Sent on the next core 1 pass (~sub-ms).
	bool SendMIDI(const uint8_t *m, int n)
	{
		if (n < 1 || n > 3 || !(m[0] & 0x80) || m[0] == 0xF0 || m[0] == 0xF7) return false;
		uint32_t packed = uint32_t(n) | (uint32_t(m[0]) << 8) |
		                  (uint32_t(n > 1 ? m[1] : 0) << 16) | (uint32_t(n > 2 ? m[2] : 0) << 24);
		return midiOut.Push(packed);
	}
	bool NoteOn(int ch, int note, int vel)
	{
		uint8_t m[3] = {uint8_t(0x90 | (ch & 15)), uint8_t(note & 127), uint8_t(vel & 127)};
		return SendMIDI(m, 3);
	}
	bool NoteOff(int ch, int note)
	{
		uint8_t m[3] = {uint8_t(0x80 | (ch & 15)), uint8_t(note & 127), 0};
		return SendMIDI(m, 3);
	}
	bool ControlChange(int ch, int cc, int v)
	{
		uint8_t m[3] = {uint8_t(0xB0 | (ch & 15)), uint8_t(cc & 127), uint8_t(v & 127)};
		return SendMIDI(m, 3);
	}
	bool Realtime(uint8_t status) { return SendMIDI(&status, 1); } // 0xF8 clock, 0xFA start...

	// Read one received MIDI message (not SysEx). Returns its length 1-3,
	// or 0 when there is nothing to read. Running status is expanded.
	int ReadMIDI(uint8_t m[3])
	{
		uint32_t packed;
		if (!midiIn.Pop(packed)) return 0;
		m[0] = uint8_t(packed >> 8);
		m[1] = uint8_t(packed >> 16);
		m[2] = uint8_t(packed >> 24);
		return int(packed & 3);
	}

	// Detents turned on device encoder i (0..6) since the last call.
	int EncoderDelta(int i)
	{
		if (unsigned(i) >= unsigned(Encoders)) return 0;
		int32_t now = encoderTotal[i];
		int32_t d = now - encoderSeen[i];
		encoderSeen[i] = now;
		return int(d);
	}
	// Device buttons held, as reported by the device (FM1: bits 0..13).
	uint16_t Buttons() const { return buttons; }

	// Open (true) or release (false) the device's link page.
	void RequestShow(bool show) { showRequest = show ? 2 : 1; }

	/*** Extensions ***/

	// Send a card-specific frame (command 0x20-0x3F). Returns false when
	// the queue is full or the payload is too long.
	bool SendUser(uint8_t command, const uint8_t *payload, int n)
	{
		if (command < WL_HOST_USER_FIRST || command > WL_HOST_USER_LAST ||
		    n < 0 || n > int(WL_MAX_PAYLOAD)) return false;
		unsigned w = userWrite;
		if (w - userRead >= UserSlots) return false;
		UserFrame &f = userFrames[w % UserSlots];
		f.command = command; f.length = uint8_t(n);
		if (n) memcpy(f.data, payload, size_t(n));
		__dmb();
		userWrite = w + 1;
		return true;
	}
	// Called on core 1 for device frames 0x60-0x7F.
	typedef void (*UserHandler)(uint8_t command, const uint8_t *payload, int n);
	void SetUserHandler(UserHandler h) { userHandler = h; }

	static WorkshopLinkHost *Instance() { return instance; }

	/*** Called by the TinyUSB callbacks below (core 1) ***/
	void OnMount(uint8_t idx)
	{
		if (midiIdx != NoMidi) return; // first MIDI interface wins
		midiIdx = idx;
		state = Connecting;
		helloTries = 0;
		parser = Parser();
		txHead = txTail = 0;
	}
	void OnUnmount(uint8_t idx)
	{
		if (idx != midiIdx) return;
		midiIdx = NoMidi;
		state = NoDevice;
		visible = false;
		buttons = 0;
	}
	void OnReceive(uint8_t idx, const uint8_t *bytes, uint32_t n)
	{
		if (idx != midiIdx) return;
		for (uint32_t i = 0; i < n; ++i) ParseByte(bytes[i]);
	}

private:
	static constexpr unsigned ScopeRing = 256; // power of two, >= WL_MAX_SCOPE_POINTS
	static constexpr unsigned TxBytes = 1024;
	static constexpr unsigned UserSlots = 4;
	static constexpr uint32_t HelloRetryUs = 400000;
	static constexpr int HelloTries = 5;
	static constexpr uint32_t HelloIdleRetryUs = 5000000; // keep trying slowly in MidiOnly
	static constexpr uint32_t ValuesUs = 33000;           // ~30 Hz value updates
	static constexpr uint32_t ScopeUs = 50000;            // ~20 Hz traces

	struct Widget
	{
		uint8_t type, x, y, w, h, color;
		int16_t min, max;
		char label[WL_MAX_LABEL + 1];
		volatile int16_t value;
		int32_t sent;              // core 1: last value sent, or Unsent
		char text[WL_MAX_TEXT + 1];
		volatile uint32_t textSeq; // even when stable
		uint32_t textSent;         // core 1: textSeq last sent
		int8_t scope;              // scope slot or -1
	};
	static constexpr int32_t Unsent = 0x7FFFFFFF;

	struct Scope
	{
		int8_t points[ScopeRing];
		volatile uint32_t write;
	};

	struct UserFrame
	{
		uint8_t command, length;
		uint8_t data[WL_MAX_PAYLOAD];
	};

	// Single-producer single-consumer ring of packed MIDI messages.
	template <unsigned N>
	struct Ring
	{
		uint32_t data[N];
		volatile uint32_t read = 0, write = 0;
		bool Push(uint32_t v)
		{
			uint32_t w = write;
			if (w - read >= N) return false;
			data[w % N] = v;
			__dmb();
			write = w + 1;
			return true;
		}
		bool Pop(uint32_t &v)
		{
			uint32_t r = read;
			if (r == write) return false;
			__dmb();
			v = data[r % N];
			__dmb();
			read = r + 1;
			return true;
		}
	};

	// Incremental MIDI byte-stream parser (core 1).
	struct Parser
	{
		uint8_t status = 0, data[2] = {0, 0};
		uint8_t count = 0;
		bool inSysEx = false;
		bool sysExOverflow = false;
		uint16_t sysExLength = 0;
		uint8_t sysEx[WL_MAX_FRAME];
	};

	// Setup
	char name[WL_MAX_NAME + 1];
	char title[WL_MAX_TEXT + 1];
	Widget widgets[MaxWidgets] = {};
	int widgetCount = 0, scopeCount = 0;
	Scope scopes[WL_MAX_SCOPES] = {};
	bool started = false, showOnConnect = false;

	// Shared between cores
	volatile uint8_t state = NoDevice;
	volatile bool visible = false;
	volatile uint16_t buttons = 0;
	volatile int32_t encoderTotal[Encoders] = {};
	int32_t encoderSeen[Encoders] = {};
	volatile uint8_t showRequest = 0; // 0 none, 1 release, 2 show
	Ring<64> midiOut, midiIn;
	UserFrame userFrames[UserSlots];
	volatile uint32_t userRead = 0, userWrite = 0;
	UserHandler userHandler = nullptr;

	// Core 1 only
	static constexpr uint8_t NoMidi = 0xFF;
	uint8_t midiIdx = NoMidi;
	int helloTries = 0;
	uint32_t lastHelloUs = 0, lastSentUs = 0, lastValuesUs = 0, lastScopeUs = 0;
	uint8_t sequence = 0;
	uint8_t deviceMaxWidgets = WL_MAX_WIDGETS, deviceScopePoints = WL_MAX_SCOPE_POINTS;
	Parser parser;
	uint8_t tx[TxBytes];
	unsigned txHead = 0, txTail = 0; // bytes [txTail, txHead) wait for USB

	static WorkshopLinkHost *instance;

	static uint8_t clamp8(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }
	static void copy(char *out, size_t capacity, const char *s)
	{
		size_t n = 0;
		while (s && s[n] && n + 1 < capacity) { out[n] = s[n]; ++n; }
		out[n] = 0;
	}

	static void Core1Entry()
	{
		board_init();
		tusb_init();
		while (true)
		{
			tuh_task();
			instance->Service();
		}
	}

	/*** Transmit (core 1) ***/

	unsigned TxFree() const { return TxBytes - (txHead - txTail); }

	// Append a whole message to the transmit buffer, or nothing.
	bool TxQueue(const uint8_t *data, unsigned n)
	{
		if (n > TxFree()) return false;
		for (unsigned i = 0; i < n; ++i) tx[(txHead + i) % TxBytes] = data[i];
		txHead += n;
		return true;
	}

	bool SendFrame(uint8_t command, const uint8_t *payload, size_t n)
	{
		uint8_t frame[WL_MAX_FRAME];
		size_t len = wl_encode(frame, sizeof frame, command, sequence, payload, n);
		if (!len || !TxQueue(frame, unsigned(len))) return false;
		sequence = uint8_t((sequence + 1) & 0x7F);
		lastSentUs = time_us_32();
		return true;
	}

	// Hand queued bytes to the USB MIDI driver, which may take only part.
	void TxPump()
	{
		if (midiIdx == NoMidi || !tuh_midi_mounted(midiIdx)) return;
		uint8_t chunk[64];
		while (txTail != txHead)
		{
			unsigned n = txHead - txTail;
			if (n > sizeof chunk) n = sizeof chunk;
			for (unsigned i = 0; i < n; ++i) chunk[i] = tx[(txTail + i) % TxBytes];
			uint32_t taken = tuh_midi_stream_write(midiIdx, 0, chunk, n);
			txTail += taken;
			if (taken < n) break;
		}
		tuh_midi_write_flush(midiIdx);
	}

	/*** Session (core 1) ***/

	void SendHello()
	{
		uint8_t p[2 + 1 + WL_MAX_NAME];
		wl_writer w;
		wl_writer_init(&w, p, sizeof p);
		wl_put8(&w, WL_VERSION);
		wl_put8(&w, showOnConnect ? WL_HELLO_SHOW : 0);
		wl_put_str(&w, name, WL_MAX_NAME);
		if (SendFrame(WL_HELLO, p, w.length))
		{
			helloTries++;
			lastHelloUs = time_us_32();
		}
	}

	// Queue the whole layout. Values, texts and traces follow as updates.
	void SendLayout()
	{
		uint8_t p[WL_MAX_PAYLOAD];
		wl_writer w;
		wl_writer_init(&w, p, sizeof p);
		wl_put_str(&w, title, WL_MAX_TEXT);
		SendFrame(WL_LAYOUT, p, w.length);
		int count = widgetCount < deviceMaxWidgets ? widgetCount : deviceMaxWidgets;
		for (int i = 0; i < count; ++i)
		{
			const Widget &wd = widgets[i];
			wl_writer_init(&w, p, sizeof p);
			wl_put8(&w, unsigned(i));
			wl_put8(&w, wd.type); wl_put8(&w, wd.x); wl_put8(&w, wd.y);
			wl_put8(&w, wd.w); wl_put8(&w, wd.h); wl_put8(&w, wd.color);
			wl_put16(&w, wd.min); wl_put16(&w, wd.max);
			wl_put_str(&w, wd.label, WL_MAX_LABEL);
			SendFrame(WL_WIDGET, p, w.length);
		}
		for (int i = 0; i < widgetCount; ++i)
		{
			widgets[i].sent = Unsent;
			widgets[i].textSent = ~0u;
		}
	}

	void SendValues()
	{
		uint8_t p[WL_MAX_VALUES_PER_FRAME * 3];
		wl_writer w;
		wl_writer_init(&w, p, sizeof p);
		int pending[MaxWidgets], n = 0;
		for (int i = 0; i < widgetCount && i < deviceMaxWidgets; ++i)
		{
			Widget &wd = widgets[i];
			if (wd.type == WL_W_LABEL || wd.type == WL_W_TEXT || wd.type == WL_W_SCOPE) continue;
			int16_t v = wd.value;
			if (int32_t(v) == wd.sent) continue;
			wl_put8(&w, unsigned(i));
			wl_put16(&w, v);
			pending[n++] = i;
		}
		if (!n || !SendFrame(WL_VALUES, p, w.length)) return;
		for (int k = 0; k < n; ++k)
		{
			// Record what was packed; a newer value is picked up next time.
			const uint8_t *e = p + 3 * k;
			widgets[pending[k]].sent = int16_t(uint16_t(e[1] | (e[2] << 8)));
		}
	}

	void SendTexts()
	{
		for (int i = 0; i < widgetCount && i < deviceMaxWidgets; ++i)
		{
			Widget &wd = widgets[i];
			if (wd.type != WL_W_TEXT) continue;
			uint32_t seq = wd.textSeq;
			if ((seq & 1) || seq == wd.textSent) continue;
			char copyText[WL_MAX_TEXT + 1];
			__dmb();
			memcpy(copyText, wd.text, sizeof copyText);
			__dmb();
			if (wd.textSeq != seq) continue; // changed while copying; retry later
			uint8_t p[2 + WL_MAX_TEXT];
			wl_writer w;
			wl_writer_init(&w, p, sizeof p);
			wl_put8(&w, unsigned(i));
			wl_put_str(&w, copyText, WL_MAX_TEXT);
			if (SendFrame(WL_TEXT, p, w.length)) wd.textSent = seq;
		}
	}

	void SendScopes()
	{
		for (int i = 0; i < widgetCount && i < deviceMaxWidgets; ++i)
		{
			const Widget &wd = widgets[i];
			if (wd.scope < 0) continue;
			Scope &s = scopes[wd.scope];
			unsigned points = deviceScopePoints < WL_MAX_SCOPE_POINTS ? deviceScopePoints : WL_MAX_SCOPE_POINTS;
			uint32_t end = s.write;
			__dmb();
			// Latest `points` samples, oldest first, in frames of up to 90.
			for (unsigned offset = 0; offset < points; offset += WL_MAX_SCOPE_PER_FRAME)
			{
				unsigned count = points - offset;
				if (count > WL_MAX_SCOPE_PER_FRAME) count = WL_MAX_SCOPE_PER_FRAME;
				uint8_t p[3 + WL_MAX_SCOPE_PER_FRAME];
				p[0] = uint8_t(i); p[1] = uint8_t(offset); p[2] = uint8_t(count);
				for (unsigned k = 0; k < count; ++k)
				{
					uint32_t at = end - points + offset + k;
					p[3 + k] = uint8_t(s.points[at % ScopeRing]);
				}
				if (!SendFrame(WL_SCOPE, p, 3 + count)) return;
			}
		}
	}

	void SendUserFrames()
	{
		while (userRead != userWrite)
		{
			__dmb();
			UserFrame &f = userFrames[userRead % UserSlots];
			if (!SendFrame(f.command, f.data, f.length)) return;
			__dmb();
			userRead = userRead + 1;
		}
	}

	void Service()
	{
		uint32_t now = time_us_32();
		if (midiIdx == NoMidi || !tuh_midi_mounted(midiIdx))
		{
			// Keep the queues from filling with stale messages while unplugged.
			uint32_t discard;
			while (midiOut.Pop(discard)) {}
			return;
		}

		// Plain MIDI first: timing matters more than the screen.
		uint32_t packed;
		while (TxFree() >= 3 && midiOut.Pop(packed))
		{
			uint8_t m[3] = {uint8_t(packed >> 8), uint8_t(packed >> 16), uint8_t(packed >> 24)};
			TxQueue(m, packed & 3);
		}

		if (state == Connecting || state == MidiOnly)
		{
			uint32_t wait = state == Connecting ? HelloRetryUs : HelloIdleRetryUs;
			if (helloTries == 0 || now - lastHelloUs >= wait)
			{
				if (state == Connecting && helloTries >= HelloTries) state = MidiOnly;
				SendHello();
			}
		}
		else if (state == Linked)
		{
			uint8_t show = showRequest;
			if (show)
			{
				uint8_t p = show == 2;
				if (SendFrame(WL_SHOW, &p, 1)) showRequest = 0;
			}
			SendUserFrames();
			if (now - lastValuesUs >= ValuesUs)
			{
				lastValuesUs = now;
				SendValues();
				SendTexts();
			}
			if (now - lastScopeUs >= ScopeUs && TxFree() > TxBytes / 2)
			{
				lastScopeUs = now;
				SendScopes();
			}
			if (now - lastSentUs >= WL_PING_MS * 1000u) SendFrame(WL_PING, nullptr, 0);
		}
		TxPump();
	}

	/*** Receive (core 1) ***/

	void HandleFrame(const uint8_t *m, size_t n)
	{
		uint8_t payload[WL_MAX_PAYLOAD], command;
		int len = wl_decode(m, n, &command, nullptr, payload, sizeof payload);
		if (len < 0) return;
		wl_reader r;
		wl_reader_init(&r, payload, size_t(len));
		switch (command)
		{
		case WL_HELLO_ACK:
		{
			wl_get8(&r); // version (already checked by wl_decode)
			wl_get8(&r); // caps
			wl_get8(&r); wl_get8(&r); // screen size
			unsigned maxWidgets = wl_get8(&r), maxPoints = wl_get8(&r);
			if (!r.ok) return;
			deviceMaxWidgets = uint8_t(maxWidgets < WL_MAX_WIDGETS ? maxWidgets : WL_MAX_WIDGETS);
			deviceScopePoints = uint8_t(maxPoints < WL_MAX_SCOPE_POINTS ? maxPoints : WL_MAX_SCOPE_POINTS);
			state = Linked;
			SendLayout();
			break;
		}
		case WL_EVENT:
		{
			bool vis = wl_get8(&r) != 0;
			unsigned b = unsigned(wl_get16(&r)) & 0xFFFF;
			int8_t d[Encoders];
			for (int i = 0; i < Encoders; ++i) d[i] = int8_t(wl_get8(&r));
			if (!r.ok) return;
			visible = vis;
			buttons = uint16_t(b);
			for (int i = 0; i < Encoders; ++i) encoderTotal[i] = encoderTotal[i] + d[i];
			break;
		}
		case WL_NEED_LAYOUT:
			state = Connecting;
			helloTries = 0;
			break;
		default:
			if (command >= WL_DEVICE_USER_FIRST && userHandler) userHandler(command, payload, len);
			break;
		}
	}

	void Deliver(uint8_t status, uint8_t d1, uint8_t d2, int n)
	{
		midiIn.Push(uint32_t(n) | (uint32_t(status) << 8) | (uint32_t(d1) << 16) | (uint32_t(d2) << 24));
	}

	static int DataBytes(uint8_t status)
	{
		if (status >= 0xF0)
			return status == 0xF1 || status == 0xF3 ? 1 : status == 0xF2 ? 2 : 0;
		uint8_t kind = status & 0xF0;
		return kind == 0xC0 || kind == 0xD0 ? 1 : 2;
	}

	void ParseByte(uint8_t b)
	{
		Parser &p = parser;
		if (b >= 0xF8) { Deliver(b, 0, 0, 1); return; } // realtime, may interrupt anything
		if (b == 0xF0)
		{
			p.inSysEx = true; p.sysExOverflow = false;
			p.sysExLength = 0;
			p.sysEx[p.sysExLength++] = b;
			p.status = 0;
			return;
		}
		if (b == 0xF7)
		{
			if (p.inSysEx && !p.sysExOverflow && p.sysExLength < sizeof p.sysEx)
			{
				p.sysEx[p.sysExLength++] = b;
				if (wl_is_link_frame(p.sysEx, p.sysExLength)) HandleFrame(p.sysEx, p.sysExLength);
			}
			p.inSysEx = false;
			return;
		}
		if (p.inSysEx)
		{
			if (b & 0x80) p.inSysEx = false; // a status byte ends an unterminated SysEx
			else
			{
				if (p.sysExLength < sizeof p.sysEx - 1) p.sysEx[p.sysExLength++] = b;
				else p.sysExOverflow = true; // too long to be ours: ignore it
				return;
			}
		}
		if (b & 0x80)
		{
			p.status = b; p.count = 0;
			if (DataBytes(b) == 0) { Deliver(b, 0, 0, 1); p.status = 0; }
			return;
		}
		if (!p.status) return; // data byte without status
		p.data[p.count++] = b;
		int need = DataBytes(p.status);
		if (p.count >= need)
		{
			Deliver(p.status, p.data[0], need > 1 ? p.data[1] : 0, 1 + need);
			p.count = 0;
			if (p.status >= 0xF0) p.status = 0; // system common has no running status
		}
	}
};

#ifndef WORKSHOPLINKHOST_NOIMPL

WorkshopLinkHost *WorkshopLinkHost::instance = nullptr;

// TinyUSB MIDI host callbacks (core 1, inside tuh_task).
extern "C" void tuh_midi_mount_cb(uint8_t idx, const tuh_midi_mount_cb_t *info)
{
	(void)info;
	if (WorkshopLinkHost::Instance()) WorkshopLinkHost::Instance()->OnMount(idx);
}

extern "C" void tuh_midi_umount_cb(uint8_t idx)
{
	if (WorkshopLinkHost::Instance()) WorkshopLinkHost::Instance()->OnUnmount(idx);
}

extern "C" void tuh_midi_rx_cb(uint8_t idx, uint32_t xferred_bytes)
{
	WorkshopLinkHost *link = WorkshopLinkHost::Instance();
	if (!link || xferred_bytes == 0) return;
	uint8_t cable, buffer[64];
	while (uint32_t n = tuh_midi_stream_read(idx, &cable, buffer, sizeof buffer))
		link->OnReceive(idx, buffer, n);
}

#endif // WORKSHOPLINKHOST_NOIMPL

#endif // WORKSHOP_LINK_HOST_H
