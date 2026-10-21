// Stand-in for ComputerCard.h so main.cpp can run on a computer, driven by
// test/card_test.cpp: knobs, switch and pulse input are set by the test, and
// every output is captured.  Only the parts main.cpp uses.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#define __not_in_flash_func(x) x
inline void sleep_us(uint64_t) {}
inline uint32_t time_us_32() { return 12345; }
inline void multicore_launch_core1(void (*)()) {}
inline void board_init() {}
inline void tuh_init(int) {}
inline void tud_init(int) {}
inline void tud_task() {}
inline bool tud_midi_available() { return false; }
inline uint32_t tud_midi_stream_read(uint8_t*, uint32_t) { return 0; }
inline uint32_t tud_midi_stream_write(uint8_t, const uint8_t*, uint32_t n) { return n; }
inline bool tud_mounted() { return true; }
inline void set_sys_clock_khz(int, bool) {}

class ComputerCard {
public:
	enum Knob {Main, X, Y};
	enum Switch {Down, Middle, Up};
	enum USBPowerState_t {DFP, UFP, Unsupported};
	virtual ~ComputerCard() {}
	void Run() { for (;;) ProcessSample(); }
	virtual void ProcessSample() = 0;

	// Set by the test
	int32_t knobRaw[3] = {0, 0, 0};
	Switch sw = Up;
	bool pulseIn1 = false, lastPulseIn1 = false;
	USBPowerState_t power = DFP;
	// Captured
	int32_t cv[2] = {0, 0};
	int16_t audio[2] = {0, 0};
	bool pulseOut[2] = {false, false};
	uint16_t led[6] = {};

	// One sample, with the clock input at the given level
	void Sample(bool clock = false)
	{
		lastPulseIn1 = pulseIn1;
		pulseIn1 = clock;
		ProcessSample();
	}

	int32_t KnobVal(Knob k) { return knobRaw[k]; }
	Switch SwitchVal() { return sw; }
	void AudioOut1(int16_t v) { audio[0] = v; }
	void AudioOut2(int16_t v) { audio[1] = v; }
	bool CVOut1Millivolts(int32_t v) { cv[0] = v; return true; }
	bool CVOut2Millivolts(int32_t v) { cv[1] = v; return true; }
	void PulseOut1(bool v) { pulseOut[0] = v; }
	void PulseOut2(bool v) { pulseOut[1] = v; }
	bool PulseIn1RisingEdge() { return pulseIn1 && !lastPulseIn1; }
	void LedBrightness(uint32_t i, uint16_t v) { led[i] = v; }
	void LedOn(uint32_t i, bool v = true) { led[i] = v ? 4095 : 0; }
	USBPowerState_t USBPowerState() { return power; }
};
