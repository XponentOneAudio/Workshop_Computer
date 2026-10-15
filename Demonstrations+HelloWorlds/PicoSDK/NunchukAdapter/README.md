# Nunchuk adapter

Firmware for a Raspberry Pi Pico that lets you use a Wii Nunchuk with the Workshop System Computer.

A Nunchuk talks I2C, not USB, so it can't be plugged into the Computer directly. This firmware runs on a Pico placed in between. The Pico reads the Nunchuk and shows up on USB as a MIDI device that speaks the same protocol as a [Music Thing 8mu](https://www.musicthing.co.uk/8mu/). Cards built on `EightMU.h` therefore work with it unchanged, including the ComputerCard `eightmu` example and the `8mu CV` release.

```
Nunchuk ──I2C──> Pico (this firmware) ──USB MIDI──> Workshop Computer (USB host)
```

## What you need

- A Raspberry Pi Pico (RP2040). A Pico W also works, but its onboard LED will not light.
- A Wii Nunchuk. Genuine and third-party Nunchuks both work.
- One of these two ways to connect the Nunchuk:
  - a "Nunchucky"/"WiiChuck" breakout board that the Nunchuk plugs into, which keeps the Nunchuk intact, or
  - the Nunchuk cable cut and wired to the Pico.
- A USB cable from the Computer's USB socket to the Pico's micro-USB socket, for example a USB-C to micro-B cable.
- Computer hardware Rev 1.1 or later, which is needed for USB host mode.

## Wiring

| Nunchuk | Pico | Pico pin |
|---|---|---|
| 3.3V | 3V3(OUT) | 36 |
| GND | GND | 38 |
| SDA (data) | GP4 | 6 |
| SCL (clock) | GP5 | 7 |

**Do not connect the Nunchuk to 5V or VBUS.** It is a 3.3V device.

If you cut the cable, check each wire with a multimeter against the plug before soldering. On genuine Nunchuks the colours are usually red = 3.3V, white = GND, green = SDA and yellow = SCL, but third-party Nunchuks use any colours at all. A breakout board avoids the guesswork, because its pads are labelled.

The Pico's weak internal pull-ups are enabled on SDA and SCL. Most Nunchuks have their own pull-ups, but if readings are unreliable, add 4.7kΩ resistors from SDA and SCL to 3.3V.

## Controls

When used with the `eightmu` example card:

| Nunchuk | Sent as | `eightmu` card output |
|---|---|---|
| Joystick left/right | 8mu fader 1 (CC 34) | CV Out 1 |
| Joystick up/down | 8mu fader 2 (CC 35) | CV Out 2 |
| Tilt forward/back | 8mu Pitch (CC 42/43) | Audio Out 1 |
| Tilt left/right | 8mu Roll (CC 44/45) | Audio Out 2 |
| Z button (big) | 8mu button A (note 36) | Pulse Out 1 |
| C button (small) | 8mu button B (note 48) | Pulse Out 2 |

The adapter also sends Flip, the up/down axis (CC 48/49). 8mu faders 3-8, Yaw and buttons C/D always read zero, because the Nunchuk has no controls for them.

The joystick's centre is taken from where it rests when the Nunchuk is first detected, so leave it alone while plugging in.

The Pico's LED is lit steadily while a Nunchuk is connected and blinks while it is not. The Nunchuk can be unplugged and plugged back in while everything is running.

The adapter also works with a laptop, DAW or any other USB MIDI host as a general MIDI controller, sending the CCs and notes above on channel 1.

## Installing

1. Hold BOOTSEL on the Pico while plugging it into a computer. It appears as a drive called `RPI-RP2`.
2. Copy `nunchuk_adapter.uf2` onto the drive. The Pico reboots and runs the adapter.
3. Load an `EightMU.h` card onto the Computer, for example the `eightmu` example from `../ComputerCard`.
4. Connect the Pico to the Computer's USB socket.

## Building

This builds with the Pico SDK (2.x) in the usual way:

```
mkdir build && cd build
cmake .. -DPICO_SDK_PATH=/path/to/pico-sdk
make
```

Settings at the top of `main.c` can be changed and rebuilt:

- `INVERT_*` reverses an axis.
- `JOY_RANGE` sets how far the joystick moves for full scale.
- `ACCEL_ZERO` and `ACCEL_1G` set the accelerometer calibration.

## How it works

`EightMU.h` checks that the device it is talking to really is an 8mu before using any of its messages. It sends an identify SysEx (`F0 7D 00 00 21 F7`) and waits for a reply carrying the 8mu's device index. It then asks for the fader positions (`F0 7D 00 00 12 F7`). The adapter answers both messages, then sends the Nunchuk's state as 8mu CCs and notes, sending only values that have changed. Accelerometer axes are sent as pairs of one-sided CCs, as the 8mu does. The Nunchuk is read at 100Hz in its unencrypted mode (`F0 55`, `FB 00` init), which works with both genuine and third-party Nunchuks.
