// Stand-in for EightMU.h: faders and buttons set by the test, LEDs captured.
#pragma once
#include <stdint.h>
class EightMU {
public:
	static constexpr int numFaders = 8;
	static constexpr int numButtons = 4;
	// Set by the test
	bool connected = true;
	int32_t faders[8] = {};
	bool buttons[4] = {};
	// Captured
	int32_t leds[8] = {};
	void Poll() {}
	bool Connected() const { return connected; }
	int32_t Fader(int i) const { return faders[i]; }
	bool Button(int i) const { return buttons[i]; }
	void SetLed(int i, int32_t v) { leds[i] = v; }
};
