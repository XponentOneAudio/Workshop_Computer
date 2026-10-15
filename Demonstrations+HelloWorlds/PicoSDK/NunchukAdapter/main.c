/*
  Nunchuk adapter

  Runs on a Raspberry Pi Pico.  Reads a Wii Nunchuk over I2C and presents it
  as a USB MIDI device that speaks the Music Thing Modular 8mu's protocol, so
  that a Workshop System Computer running a card built on EightMU.h (such as
  the ComputerCard 'eightmu' example) sees it as an 8mu:

    Nunchuk joystick X   -> 8mu fader 1 (CC 34)   eightmu card: CV out 1
    Nunchuk joystick Y   -> 8mu fader 2 (CC 35)   eightmu card: CV out 2
    Nunchuk tilt fwd/back-> 8mu Pitch  (CC 42/43) eightmu card: Audio out 1
    Nunchuk tilt L/R     -> 8mu Roll   (CC 44/45) eightmu card: Audio out 2
    Nunchuk up/down      -> 8mu Flip   (CC 48/49)
    Nunchuk Z button     -> 8mu button A (note 36) eightmu card: Pulse out 1
    Nunchuk C button     -> 8mu button B (note 48) eightmu card: Pulse out 2

  8mu faders 3-8, Yaw (the Nunchuk has no gyroscope) and buttons C/D are
  always 0.  LED messages from the Computer are accepted and ignored.

  The 8mu protocol parts implemented are the ones EightMU.h relies on:
  - replying to the identify SysEx (F0 7D 00 00 21 F7) with an identify
    reply carrying the 8mu's device index, without which EightMU ignores
    all CCs and notes;
  - replying to the fader query SysEx (F0 7D 00 00 12 F7) with the current
    fader positions.

  The same messages are just as usable from a DAW or any other USB MIDI
  host, so the adapter also works as a general Nunchuk MIDI controller.

  Wiring (Nunchuk -> Pico):
    3.3V -> 3V3(OUT), pin 36     (never 5V/VBUS)
    GND  -> GND, pin 38
    SDA  -> GP4, pin 6
    SCL  -> GP5, pin 7
*/

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "bsp/board.h"
#include "tusb.h"


////////////////////////////////////////////////////////////////
// Configuration

#define NUNCHUK_I2C          i2c0
#define NUNCHUK_SDA_PIN      4
#define NUNCHUK_SCL_PIN      5
#define NUNCHUK_I2C_HZ       100000
#define NUNCHUK_ADDR         0x52

#define POLL_INTERVAL_US     10000   // read the Nunchuk at 100Hz
#define REINIT_INTERVAL_US   500000  // retry a missing Nunchuk twice a second

// Joystick deflection, in raw units either side of centre, that gives a
// full-scale fader.  Genuine Nunchuks reach roughly +/-100.
#define JOY_RANGE            95

// Accelerometer reading at rest, and the change in reading for 1g.
// These vary a little between Nunchuks.
#define ACCEL_ZERO           512
#define ACCEL_1G             200

// Set to 1 to reverse the direction of an axis
#define INVERT_JOY_X         0
#define INVERT_JOY_Y         0
#define INVERT_PITCH         0
#define INVERT_ROLL          0


////////////////////////////////////////////////////////////////
// 8mu protocol constants (must match EightMU.h)

#define EIGHTMU_FIRST_CC     34      // faders are CC 34-41, motion CC 42-49
#define EIGHTMU_NUM_CCS      16
#define EIGHTMU_NUM_FADERS   8
#define EIGHTMU_DEVICE_INDEX 6       // 8mu's index in the identify reply

// Index into the CC table
enum
{
	CC_FADER1 = 0,
	CC_FADER2 = 1,
	CC_PITCH_POS = 8, CC_PITCH_NEG,
	CC_ROLL_POS, CC_ROLL_NEG,
	CC_YAW_POS, CC_YAW_NEG,
	CC_FLIP_POS, CC_FLIP_NEG,
};

#define NOTE_BUTTON_A        36
#define NOTE_BUTTON_B        48


////////////////////////////////////////////////////////////////
// Nunchuk

typedef struct
{
	uint8_t joyX, joyY;       // 0-255, centre ~128
	int16_t accX, accY, accZ; // 10-bit, 0-1023
	bool buttonZ, buttonC;    // true while pressed
} NunchukState;

static bool nunchuk_write(const uint8_t *data, size_t len)
{
	return i2c_write_timeout_us(NUNCHUK_I2C, NUNCHUK_ADDR, data, len, false, 5000) == (int)len;
}

// Initialise the Nunchuk in unencrypted mode, which works for both genuine
// and third-party Nunchuks.
static bool nunchuk_init(void)
{
	static const uint8_t init1[] = {0xF0, 0x55};
	static const uint8_t init2[] = {0xFB, 0x00};

	if (!nunchuk_write(init1, sizeof(init1))) return false;
	sleep_ms(2);
	if (!nunchuk_write(init2, sizeof(init2))) return false;
	sleep_ms(2);
	return true;
}

static bool nunchuk_read(NunchukState *s)
{
	static const uint8_t reg = 0x00;
	uint8_t d[6];

	if (!nunchuk_write(&reg, 1)) return false;
	sleep_us(200); // Nunchuk needs a moment to prepare its data
	if (i2c_read_timeout_us(NUNCHUK_I2C, NUNCHUK_ADDR, d, 6, false, 5000) != 6) return false;

	// An uninitialised or half-connected Nunchuk reads as all 0xFF
	if (d[0] == 0xFF && d[1] == 0xFF && d[2] == 0xFF &&
	    d[3] == 0xFF && d[4] == 0xFF && d[5] == 0xFF) return false;

	s->joyX = d[0];
	s->joyY = d[1];
	s->accX = (int16_t)((d[2] << 2) | ((d[5] >> 2) & 0x03));
	s->accY = (int16_t)((d[3] << 2) | ((d[5] >> 4) & 0x03));
	s->accZ = (int16_t)((d[4] << 2) | ((d[5] >> 6) & 0x03));
	s->buttonZ = !(d[5] & 0x01); // buttons are active low
	s->buttonC = !(d[5] & 0x02);
	return true;
}


////////////////////////////////////////////////////////////////
// Values to send, and what has been sent

static uint8_t ccWanted[EIGHTMU_NUM_CCS];
static int16_t ccSent[EIGHTMU_NUM_CCS]; // -1 = must send
static bool buttonWanted[2];
static int8_t buttonSent[2];            // -1 = must send

static const uint8_t buttonNotes[2] = {NOTE_BUTTON_A, NOTE_BUTTON_B};

static void mark_all_unsent(void)
{
	for (int i = 0; i < EIGHTMU_NUM_CCS; i++) ccSent[i] = -1;
	for (int i = 0; i < 2; i++) buttonSent[i] = -1;
}

static int clamp(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

// Joystick, raw 0-255, to fader 0-127, relative to the centre seen at power-up
static uint8_t joy_to_fader(int raw, int centre, bool invert)
{
	int offset = raw - centre;
	if (invert) offset = -offset;
	return (uint8_t)clamp(64 + offset * 64 / JOY_RANGE, 0, 127);
}

// Signed axis, -127 to 127, to the 8mu's pair of one-sided CCs
static void set_axis(int posIndex, int value)
{
	value = clamp(value, -127, 127);
	ccWanted[posIndex] = (uint8_t)(value > 0 ? value : 0);
	ccWanted[posIndex + 1] = (uint8_t)(value < 0 ? -value : 0);
}

// Smoothed accelerometer readings, in raw units scaled by 16
static int32_t accSmooth[3];
static bool accSmoothValid = false;

static int smooth_axis(int i, int raw)
{
	if (!accSmoothValid) accSmooth[i] = raw << 4;
	accSmooth[i] += ((raw << 4) - accSmooth[i]) >> 2;
	return accSmooth[i] >> 4;
}

// Accelerometer, raw 0-1023, to -127 (-1g) to 127 (+1g)
static int accel_to_axis(int raw)
{
	return (raw - ACCEL_ZERO) * 127 / ACCEL_1G;
}

static void update_from_nunchuk(const NunchukState *s, int joyCentreX, int joyCentreY)
{
	ccWanted[CC_FADER1] = joy_to_fader(s->joyX, joyCentreX, INVERT_JOY_X);
	ccWanted[CC_FADER2] = joy_to_fader(s->joyY, joyCentreY, INVERT_JOY_Y);

	int ax = smooth_axis(0, s->accX);
	int ay = smooth_axis(1, s->accY);
	int az = smooth_axis(2, s->accZ);
	accSmoothValid = true;

	// Nunchuk Y axis tips forwards/backwards, X tips left/right.
	// The 8mu's Flip is -1g lying flat, the right way up.
	int pitch = accel_to_axis(ay);
	int roll = accel_to_axis(ax);
	set_axis(CC_PITCH_POS, INVERT_PITCH ? -pitch : pitch);
	set_axis(CC_ROLL_POS, INVERT_ROLL ? -roll : roll);
	set_axis(CC_FLIP_POS, -accel_to_axis(az));

	buttonWanted[0] = s->buttonZ;
	buttonWanted[1] = s->buttonC;
}

// Send whatever has changed.  Anything that doesn't fit in the USB buffer
// stays marked as unsent and goes out next time.
static void send_changes(void)
{
	if (!tud_midi_mounted()) return;

	for (int i = 0; i < 2; i++)
	{
		if (buttonSent[i] == (int8_t)buttonWanted[i]) continue;

		uint8_t msg[3];
		if (buttonWanted[i])
		{
			msg[0] = 0x90; msg[1] = buttonNotes[i]; msg[2] = 127;
		}
		else
		{
			msg[0] = 0x80; msg[1] = buttonNotes[i]; msg[2] = 0;
		}
		if (tud_midi_stream_write(0, msg, 3) != 3) return;
		buttonSent[i] = (int8_t)buttonWanted[i];
	}

	for (int i = 0; i < EIGHTMU_NUM_CCS; i++)
	{
		if (ccSent[i] == ccWanted[i]) continue;

		uint8_t msg[3] = {0xB0, (uint8_t)(EIGHTMU_FIRST_CC + i), ccWanted[i]};
		if (tud_midi_stream_write(0, msg, 3) != 3) return;
		ccSent[i] = ccWanted[i];
	}
}


////////////////////////////////////////////////////////////////
// Incoming SysEx from the Computer

static void send_identify_reply(void)
{
	// Device index, then firmware version major/minor/point, then one
	// spare byte: EightMU wants at least 10 bytes before the F7.
	const uint8_t msg[] = {0xF0, 0x7D, 0x00, 0x00, 0x20,
	                       EIGHTMU_DEVICE_INDEX, 1, 0, 0, 0, 0xF7};
	tud_midi_stream_write(0, msg, sizeof(msg));

	// The Computer ignores CCs until it has seen this reply, so make sure
	// everything is sent again after it.
	mark_all_unsent();
}

static void send_fader_reply(void)
{
	uint8_t msg[5 + EIGHTMU_NUM_FADERS + 1] = {0xF0, 0x7D, 0x00, 0x00, 0x12};
	for (int i = 0; i < EIGHTMU_NUM_FADERS; i++)
	{
		msg[5 + i] = ccWanted[i];
	}
	msg[sizeof(msg) - 1] = 0xF7;
	tud_midi_stream_write(0, msg, sizeof(msg));
}

static uint8_t sysex[16];
static int sysexLen = -1; // -1 = not in a SysEx message

static void handle_sysex(void)
{
	if (sysexLen < 5) return;
	if (sysex[0] != 0xF0 || sysex[1] != 0x7D || sysex[2] != 0x00 || sysex[3] != 0x00) return;

	switch (sysex[4])
	{
	case 0x21: send_identify_reply(); break;
	case 0x12: send_fader_reply(); break;
	default: break; // LED blink on/off etc.: nothing to do
	}
}

static void receive_midi(void)
{
	uint8_t buffer[64];

	while (tud_midi_available())
	{
		uint32_t n = tud_midi_stream_read(buffer, sizeof(buffer));
		for (uint32_t i = 0; i < n; i++)
		{
			uint8_t b = buffer[i];

			if (b >= 0xF8) continue; // real-time, can appear anywhere

			if (b == 0xF0)
			{
				sysex[0] = b;
				sysexLen = 1;
			}
			else if (b == 0xF7)
			{
				if (sysexLen > 0) handle_sysex();
				sysexLen = -1;
			}
			else if (b & 0x80)
			{
				sysexLen = -1; // any other status byte ends a SysEx
			}
			else if (sysexLen >= 0 && sysexLen < (int)sizeof(sysex))
			{
				sysex[sysexLen++] = b;
			}
			// Channel messages (LED note on/off) are ignored
		}
	}
}


////////////////////////////////////////////////////////////////

#ifdef PICO_DEFAULT_LED_PIN
static void led_put(bool on) {gpio_put(PICO_DEFAULT_LED_PIN, on);}
#else
static void led_put(bool on) {(void)on;}
#endif

int main(void)
{
	board_init();

#ifdef PICO_DEFAULT_LED_PIN
	gpio_init(PICO_DEFAULT_LED_PIN);
	gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#endif

	i2c_init(NUNCHUK_I2C, NUNCHUK_I2C_HZ);
	gpio_set_function(NUNCHUK_SDA_PIN, GPIO_FUNC_I2C);
	gpio_set_function(NUNCHUK_SCL_PIN, GPIO_FUNC_I2C);
	// Weak internal pull-ups; most Nunchuks have their own as well
	gpio_pull_up(NUNCHUK_SDA_PIN);
	gpio_pull_up(NUNCHUK_SCL_PIN);

	// Faders at centre, everything else at rest, until the Nunchuk is read
	ccWanted[CC_FADER1] = 64;
	ccWanted[CC_FADER2] = 64;
	mark_all_unsent();

	tusb_init();

	bool nunchukOk = false;
	int joyCentreX = 128, joyCentreY = 128;
	bool haveCentre = false;
	uint32_t lastPollUs = time_us_32();
	uint32_t lastInitUs = 0;
	bool everInit = false;

	while (1)
	{
		tud_task();
		receive_midi();

		uint32_t now = time_us_32();
		if (now - lastPollUs >= POLL_INTERVAL_US)
		{
			lastPollUs = now;

			if (!nunchukOk && (!everInit || now - lastInitUs >= REINIT_INTERVAL_US))
			{
				everInit = true;
				lastInitUs = now;
				nunchukOk = nunchuk_init();
				haveCentre = false;
				accSmoothValid = false;
			}

			NunchukState s;
			if (nunchukOk && nunchuk_read(&s))
			{
				// Take the joystick position when first connected as its
				// centre, if it is plausibly near the middle.
				if (!haveCentre)
				{
					joyCentreX = abs(s.joyX - 128) < 40 ? s.joyX : 128;
					joyCentreY = abs(s.joyY - 128) < 40 ? s.joyY : 128;
					haveCentre = true;
				}
				update_from_nunchuk(&s, joyCentreX, joyCentreY);
			}
			else if (nunchukOk)
			{
				// Nunchuk unplugged: release buttons, hold everything else
				nunchukOk = false;
				buttonWanted[0] = buttonWanted[1] = false;
			}

			send_changes();

			// LED steady while a Nunchuk is connected, blinking while not
			led_put(nunchukOk || ((now >> 18) & 1));
		}
	}
}
