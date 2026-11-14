// 8mu Shaker
//
// A physically modelled shaker for the Music Thing Modular Workshop Computer,
// played by picking up a Music Thing 8mu controller and shaking it.
//
// The 8mu has an accelerometer inside it.  This card reads that, uses it to
// throw a simulated handful of grains around inside a simulated shell, and
// plays the result out of the two audio outputs.  The 8mu's eight faders
// describe the shaker: how big it is, what it is made of, how many grains are
// in it, and so on.
//
//   fader 1  SIZE         small and high  ...  big and low
//   fader 2  MATERIAL     gourd / wood  ...  plastic  ...  metal
//   fader 3  GRAINS       a few beads  ...  about a thousand
//   fader 4  HARDNESS     sand  ...  seeds  ...  steel shot
//   fader 5  DECAY        how long the rattle carries on after a shake
//   fader 6  SENSITIVITY  how hard you have to shake
//   fader 7  TRAVEL       packed tight  ...  a long tube (rainstick)
//   fader 8  SCATTER      fixed pitch  ...  hard hits bend the pitch up  ...
//                         and beyond that, many separate bells or jingles
//
//   button 1   soft shake          button 3   hold to roll (keep rattling)
//   button 2   hard shake          button 4   hold to choke (grip the shell)
//
// THIS FILE is the "glue": it reads the hardware, hands numbers to the model,
// and sends the model's results to the jacks and LEDs.  The sound itself is
// made in Shaker.h, which is where to look to learn how the model works.
//
// HOW A COMPUTER CARD PROGRAM IS ORGANISED
//
// ComputerCard.h supplies a class called ComputerCard which does all the
// hard work of talking to the hardware.  We write our own class that
// "inherits" from it (class ShakerCard : public ComputerCard), which means
// our class gets everything ComputerCard can do - KnobVal(), AudioOut1(),
// LedOn() and so on - for free.
//
// In return we must supply one function, ProcessSample().  ComputerCard
// calls it 48,000 times every second, once for each sample of audio.  Every
// call has about 20 millionths of a second to read the inputs, work out one
// new sample, and write the outputs.  There is no time for anything slow.
//
// The 8mu is a USB device, so the Computer must act as a USB host.  That
// needs a Rev 1.1 or later Computer, and the 8mu plugged into the front
// USB-C socket.

#include "ComputerCard.h"
#include "EightMU.h"
#include "Shaker.h"
#include "Compressor.h"
#include "hardware/clocks.h"

#include <stdint.h>


class ShakerCard : public ComputerCard
{
public:
	// The CONSTRUCTOR: a function with the same name as the class, which
	// runs once when the card object is created (at power-on).
	ShakerCard()
	{
		// Build the shaker's lookup tables.  This is the only slow maths in
		// the whole card and it happens here, before any audio starts.
		shaker.Init();

		// The Computer's USB socket needs a moment to settle after power-on.
		// Then only start the USB host if the socket really is able to act as
		// one ("DFP" = downstream facing port, i.e. host).  On an older
		// board, or when plugged into a laptop, it isn't, and the card
		// carries on without an 8mu.
		sleep_us(150000);
		if (USBPowerState() == DFP)
		{
			// The RP2040 has two processor cores.  Core 0 does the audio;
			// Start() hands the second one, core 1, to the USB code.
			mu.Start();
		}
	}

	// "virtual" marks this as our version of a function that ComputerCard
	// declared.  ComputerCard calls it without needing to know what's in it.
	virtual void ProcessSample()
	{
		const bool connected = mu.Connected();

		//--------------------------------------------------------------------
		// 1. The 8mu's faders set the eight parameters of the model
		//--------------------------------------------------------------------
		//
		// mu.Fader() returns 0 to 4064.  ">> 5" divides by 32, giving the
		// 0 to 127 that the model expects.
		//
		// With no 8mu plugged in we leave the parameters alone, so they keep
		// either their defaults (a maraca) or the last fader positions.
		if (connected)
		{
			shaker.params.size        = mu.Fader(0) >> 5;
			shaker.params.material    = mu.Fader(1) >> 5;
			shaker.params.grains      = mu.Fader(2) >> 5;
			shaker.params.hardness    = mu.Fader(3) >> 5;
			shaker.params.decay       = mu.Fader(4) >> 5;
			shaker.params.sensitivity = mu.Fader(5) >> 5;
			shaker.params.travel      = mu.Fader(6) >> 5;
			shaker.params.scatter     = mu.Fader(7) >> 5;
		}

		//--------------------------------------------------------------------
		// 2. The 8mu's accelerometer moves the grains
		//--------------------------------------------------------------------
		//
		// Roll and Pitch say how much of gravity is pulling along each edge
		// of the 8mu, and Yaw how fast it is being spun.  Roll is positive
		// when the right-hand side is lifted - which makes grains slide LEFT -
		// so both tilts are negated to turn "which side is up" into "which
		// way the grains are pushed".
		if (connected)
		{
			shaker.SetMotion(-mu.Roll(), -mu.Pitch(), mu.Yaw());
		}
		else
		{
			shaker.SetMotion(0, 0, 0);
		}

		//--------------------------------------------------------------------
		// 3. Buttons, the switch and the pulse inputs trigger shakes
		//--------------------------------------------------------------------
		//
		// A button is held down for thousands of samples, but we want ONE
		// shake per press.  So we look for the moment it changes from "not
		// pressed" to "pressed" - a "rising edge" - by remembering what it
		// was on the previous sample.
		const bool softButton = mu.Button(0);
		const bool hardButton = mu.Button(1);
		if (softButton && !lastSoftButton) shaker.Trigger(softShake);
		if (hardButton && !lastHardButton) shaker.Trigger(hardShake);
		lastSoftButton = softButton;
		lastHardButton = hardButton;

		// The Computer's own switch, so the card can be played with no 8mu:
		// flick it down for one shake, leave it up for a continuous roll.
		const Switch sw = SwitchVal();
		if (sw == Switch::Down && lastSwitch != Switch::Down) shaker.Trigger(hardShake);
		lastSwitch = sw;

		// Pulse inputs, so a sequencer or clock can play the shaker.
		// ComputerCard does the edge detection for these itself.
		if (PulseIn1RisingEdge()) shaker.Trigger(hardShake);   // accent
		if (PulseIn2RisingEdge()) shaker.Trigger(softShake);   // ghost note

		// "Sustain" keeps the grains rattling at some level without motion.
		// It comes from button 3, the switch being up, or a positive voltage
		// at CV In 2 - whichever is asking for the most.
		int32_t sustain = 0;
		if (mu.Button(2) || sw == Switch::Up) sustain = rollLevel;

		// CVIn2() is -2048 to 2047.  An input with nothing plugged into it
		// does not read zero - it picks up noise - so every input is only
		// used when Connected() says there really is a cable in the jack.
		// Small readings are ignored as well, for the same reason.
		const int32_t shakeCV = Connected(Input::CV2) ? CVIn2() : 0;
		if (shakeCV > 100 && shakeCV * 32 > sustain) sustain = shakeCV * 32;
		shaker.SetSustain(sustain);

		shaker.SetChoke(mu.Button(3));

		//--------------------------------------------------------------------
		// 4. The Computer's knobs and CV In 1
		//--------------------------------------------------------------------
		//
		// X knob: tuning.  KnobVal() is 0 to 4095; subtracting 2048 makes
		// the middle of the knob mean "no change", and the scaling gives
		// about +-2.3 octaves (64 steps per octave).
		int32_t tune = ((KnobVal(Knob::X) - 2048) * 150) >> 11;

		// CV In 1 also shifts the pitch, higher voltage = higher = "smaller".
		// It is roughly one octave per volt but not accurately so: this is a
		// shaker, not an oscillator.
		if (Connected(Input::CV1)) tune += (CVIn1() * 3) >> 4;
		shaker.SetTune(tune);

		// Y knob: stereo width, 0 to 255.
		shaker.SetWidth(KnobVal(Knob::Y) >> 4);

		//--------------------------------------------------------------------
		// 5. Run the model and send its output to the audio jacks
		//--------------------------------------------------------------------
		//
		// Audio In 1 is fed into the shell.  This matters most of all for
		// the "is it connected?" check: a metal shell rings for a long time
		// and is very easily set off, and the faint noise of an empty input
		// is enough to keep it humming quietly for ever.
		const int32_t shellInput = Connected(Input::Audio1) ? AudioIn1() : 0;

		int32_t left, right;
		shaker.Process(shellInput, left, right);

		// Main knob: one knob, two jobs, split at the middle.
		//
		//   Left half:   VOLUME, from silent up to normal level at the middle.
		//   Right half:  volume stays at normal, and the knob instead adds
		//                more and more PARALLEL COMPRESSION (Compressor.h),
		//                which evens out hard and soft hits and brings up
		//                the tails.
		//
		// KnobVal() is 0 to 4095, so the middle is about 2048.
		const int32_t mainKnob = KnobVal(Knob::Main);
		int32_t volume = mainKnob;         // left half: 0..2048
		int32_t compression = 0;
		if (mainKnob > 2048)
		{
			volume = 2048;                              // normal level
			compression = (mainKnob - 2048) * 2;        // right half: 0..4094
		}
		compressor.Process(left, right, compression);

		// The model's output is much bigger than the -2048 to 2047 the
		// hardware accepts; ToOutput scales it down and applies the volume.
		left = ToOutput(left, volume, roundingErrorLeft);
		right = ToOutput(right, volume, roundingErrorRight);

		AudioOut1(left);
		AudioOut2(right);

		//--------------------------------------------------------------------
		// 6. Extra outputs: CVs and pulses for patching into other modules
		//--------------------------------------------------------------------

		// CV Out 1: how hard the shaker is rattling, as a voltage that rises
		// from 0V.  An "envelope follower" for your hand.
		const int32_t energy = shaker.Energy();     // 0 to 131072
		CVOut1(Limit(energy >> 6, 0, 2047));

		// CV Out 2: where the grains are, left to right, as -6V to +6V.
		// Tilt the 8mu slowly and this is a hand-played "slosh" voltage.
		CVOut2(shaker.Position());

		// Pulse Out 1: a 1 ms pulse for every grain collision - random
		// triggers, as dense as the Grains fader.
		// Pulse Out 2: a 5 ms pulse each time the grains hit a wall, which
		// is once per stroke of a shake - the rhythm you are playing.
		//
		// A pulse must stay high for a while to be seen by another module,
		// so each event sets a countdown and the output stays high until
		// the countdown runs out.
		if (shaker.GrainHit()) grainPulseTimer = 48;       // 48 samples = 1 ms
		if (shaker.TakeWallHit()) wallPulseTimer = 240;    // 240 samples = 5 ms
		PulseOut1(grainPulseTimer > 0);
		PulseOut2(wallPulseTimer > 0);
		if (grainPulseTimer > 0) grainPulseTimer--;
		if (wallPulseTimer > 0) wallPulseTimer--;

		//--------------------------------------------------------------------
		// 7. Lights
		//--------------------------------------------------------------------
		//
		// The Computer's six LEDs are numbered like this:
		//
		//     0  1        0: 8mu connected      1: grain collisions
		//     2  3        2: left loudness      3: right loudness
		//     4  5        4: rattle energy      5: wall hit
		//
		// Loudness is shown with a "peak meter": jump up to any new peak at
		// once, otherwise sink slowly, so that very short sounds still give
		// a visible flash.
		const int32_t absLeft = left < 0 ? -left : left;
		const int32_t absRight = right < 0 ? -right : right;
		if (absLeft * 256 > meterLeft) meterLeft = absLeft * 256;
		if (absRight * 256 > meterRight) meterRight = absRight * 256;
		meterLeft -= meterLeft >> 10;
		meterRight -= meterRight >> 10;

		LedOn(0, connected);
		LedOn(1, grainPulseTimer > 0);
		LedBrightness(2, Limit(meterLeft >> 7, 0, 4095));
		LedBrightness(3, Limit(meterRight >> 7, 0, 4095));
		LedBrightness(4, Limit(energy >> 4, 0, 4095));
		LedOn(5, wallPulseTimer > 0);

		// The 8mu's own row of eight LEDs becomes a bar graph of the rattle
		// energy: shake harder and more of them light.
		//
		// This doesn't need doing 48,000 times a second, so a counter makes
		// it happen on only one sample in every 480 (100 times a second).
		if (++ledCounter >= 480)
		{
			ledCounter = 0;

			// The same peak-meter idea, so the bar falls back smoothly.
			if (energy > barLevel) barLevel = energy;
			barLevel -= barLevel >> 4;

			if (connected)
			{
				// Each LED needs twice the energy of the one before it:
				// 600, 1200, 2400 ... 76800.  "600 << i" is 600 doubled
				// i times.
				for (int i = 0; i < EightMU::numLeds; i++)
				{
					if (barLevel > (600 << i)) mu.LedOn(i);
					else mu.LedOff(i);
				}
			}
		}
	}

private:
	// How strong the different kinds of shake are.  65536 is "1.0".
	// "static constexpr" means a fixed value, known when the program is
	// compiled, shared by the whole class - the modern C++ way to name a
	// constant.
	static constexpr int32_t softShake = 20000;
	static constexpr int32_t hardShake = 70000;
	static constexpr int32_t rollLevel = 40000;

	// Keep a value between two limits.
	static int32_t Limit(int32_t v, int32_t lo, int32_t hi)
	{
		return v < lo ? lo : (v > hi ? hi : v);
	}

	// Turn the model's output into a number the hardware can play.
	//
	// The audio outputs have only 4096 steps (12 bits).  That is plenty for
	// a loud sound, but the tail of a long ring is very quiet - only two or
	// three steps tall - and a smooth wave drawn with three steps is a
	// staircase, which sounds rough and buzzy.
	//
	// The cure is called NOISE SHAPING.  Each time we round to a whole step
	// we remember how much we were wrong by, and add that error on to the
	// next sample before rounding it.  Over a few samples the errors cancel
	// out, so the AVERAGE output follows the true value far more finely than
	// one step.  The error does not vanish; it is turned into a very high
	// pitched hiss, where it is hard to hear, instead of a buzz in the
	// middle of the sound.
	//
	// `error` is a reference to the caller's variable (the &), so that what
	// we store in it is still there on the next call.  Left and right each
	// need their own.
	static int32_t ToOutput(int32_t sample, int32_t volume, int32_t &error)
	{
		// Apply the volume, keeping 8 extra bits: `fine` is in 256ths of an
		// output step.  64-bit, as both numbers can be large.
		int32_t fine = int32_t((int64_t(sample) * volume) >> 11);

		// Loud peaks go through the soft clipper.  Fine detail does not
		// matter up there, so it works in whole steps.
		if (fine > (1024 << 8) || fine < -(1024 << 8))
		{
			fine = SoftClip(fine >> 8) * 256;
		}

		// Round down to a whole step, having added the error left over from
		// last time, and keep the new leftover for next time.
		const int32_t total = fine + error;
		const int32_t out = total >> 8;
		error = total - out * 256;

		return Limit(out, -2048, 2047);
	}

	// The audio outputs can only go from -2048 to 2047.  Simply chopping off
	// anything bigger ("hard clipping") sounds harsh.  This lets the signal
	// through untouched up to half scale, and above that squeezes it ever
	// more gently towards the limit, so loud peaks are rounded off instead.
	static int32_t SoftClip(int32_t v)
	{
		const int32_t knee = 1024;          // where the squeezing starts
		const int32_t room = 2047 - knee;   // how much space is left above it

		const bool negative = v < 0;
		if (negative) v = -v;               // work on the size, restore sign later

		if (v > knee)
		{
			// `over` is how far past the knee we are.  over/(over+room)
			// creeps towards 1 but never reaches it, however big `over` is.
			int32_t over = v - knee;
			if (over > 1000000) over = 1000000;   // keeps the multiply in range
			v = knee + (room * over) / (over + room);
		}
		return negative ? -v : v;
	}

	// These are "member variables": they belong to the card object and keep
	// their values between one call of ProcessSample() and the next.
	Shaker shaker;   // the physical model (Shaker.h)
	Compressor compressor;   // output dynamics (Compressor.h)
	EightMU mu;      // the connection to the 8mu controller (EightMU.h)

	bool lastSoftButton = false;
	bool lastHardButton = false;
	Switch lastSwitch = Switch::Middle;

	int32_t grainPulseTimer = 0;
	int32_t wallPulseTimer = 0;

	int32_t roundingErrorLeft = 0;    // see ToOutput()
	int32_t roundingErrorRight = 0;

	int32_t meterLeft = 0;
	int32_t meterRight = 0;
	int32_t barLevel = 0;
	int32_t ledCounter = 0;
};


// Every C++ program starts at main().
int main()
{
	// Run the processor a little faster than standard (200 MHz rather than
	// 125 MHz) to leave plenty of time for the audio maths.
	set_sys_clock_khz(200000, true);

	// "static" here keeps the card in permanent memory rather than on the
	// stack - the small scratch area functions use for their temporary
	// variables - because the shaker's lookup tables make it fairly large.
	static ShakerCard card;

	// Ask ComputerCard to work out which input jacks have cables in them,
	// which is what makes Connected() work.  (It does this by sending a
	// recognisable pattern down the path that an EMPTY jack is wired to,
	// and looking for that pattern arriving at each input.)
	card.EnableNormalisationProbe();

	// Run() starts the audio and never returns: from here on, everything
	// happens inside ProcessSample().
	card.Run();
}
