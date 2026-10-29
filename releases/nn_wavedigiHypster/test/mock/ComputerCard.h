// Host mock of ComputerCard: just the API this card uses, with the panel
// state exposed so tests can drive it.
// Uses the real header's include guard: force-include this (-include) and
// src/main.cpp's own #include "ComputerCard.h" becomes a no-op.
#ifndef COMPUTERCARD_H
#define COMPUTERCARD_H
#include <cstdint>
#define __not_in_flash_func(f) f

class ComputerCard
{
public:
	enum Knob { Main, X, Y };
	enum Switch { Down, Middle, Up };
	enum Input { Audio1, Audio2, CV1, CV2, Pulse1, Pulse2 };
	virtual ~ComputerCard() = default;

	// test side
	int32_t knob[3] = {2048, 2048, 2048};
	Switch sw = Middle;
	int16_t cvIn[2] = {0, 0}, audioIn[2] = {0, 0};
	bool pulseIn[2] = {false, false}, lastPulseIn[2] = {false, false};
	bool connected[6] = {false, false, false, false, false, false};
	int16_t audioOut[2] = {0, 0}, cvOut[2] = {0, 0};
	bool pulseOut[2] = {false, false};
	uint16_t led[6] = {0};
	void Tick()
	{
		ProcessSample();
		lastPulseIn[0] = pulseIn[0];
		lastPulseIn[1] = pulseIn[1];
	}
	void Run() {}

protected:
	virtual void ProcessSample() = 0;
	void EnableNormalisationProbe() {}
	int32_t KnobVal(Knob k) { return knob[k]; }
	Switch SwitchVal() { return sw; }
	int16_t CVIn1() { return cvIn[0]; }
	int16_t CVIn2() { return cvIn[1]; }
	int16_t AudioIn1() { return audioIn[0]; }
	int16_t AudioIn2() { return audioIn[1]; }
	bool PulseIn1RisingEdge() { return pulseIn[0] && !lastPulseIn[0]; }
	bool PulseIn2() { return pulseIn[1]; }
	bool Connected(Input i) { return connected[i]; }
	void AudioOut1(int16_t v) { audioOut[0] = v; }
	void AudioOut2(int16_t v) { audioOut[1] = v; }
	void CVOut1(int16_t v) { cvOut[0] = v; }
	void CVOut2(int16_t v) { cvOut[1] = v; }
	void PulseOut1(bool v) { pulseOut[0] = v; }
	void PulseOut2(bool v) { pulseOut[1] = v; }
	void LedBrightness(uint32_t i, uint16_t v) { led[i] = v; }
	void LedOn(uint32_t i, bool v = true) { led[i] = v ? 4095 : 0; }
};
#endif
