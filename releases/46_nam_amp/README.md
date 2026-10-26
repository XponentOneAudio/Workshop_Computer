# NAM Amp

An experimental amp simulator card that plays [Neural Amp Modeler](https://www.neuralampmodeler.com/) (`.nam`) captures, the format shared on [tone3000](https://www.tone3000.com).

**Status: work in progress, not yet tested on hardware.** The firmware builds, and the model maths has been checked against a floating point implementation of NAM on a computer. On-hardware timing is still estimated (see [CPU use](#cpu-use)).

## What runs, and what doesn't

The RP2040 has two 133MHz Cortex-M0+ cores with no floating point or DSP instructions. The card overclocks it to 240MHz (as some other Workshop Computer cards do) and splits each model across both cores. Even so, it is a very small processor for a neural network running 48,000 times a second.

| Model | Cycles per sample | Fits? |
|---|---|---|
| LSTM, hidden size 4 | ~1,800 | Yes, easily |
| LSTM, hidden size 8 | ~3,900 | Yes (~80% of one core) |
| LSTM, hidden size 12 | ~6,600 | No |
| WaveNet "Nano" (the smallest standard WaveNet size) | ~17,700 | No, about 3x too slow |
| WaveNet "Feather" / "Lite" / "Standard" | 30,000 to 100,000 | No |

*Cycle counts are for the engine alone, measured by emulating the compiled Cortex-M0+ code (`tools/cycle_count.py`). At 240MHz the Computer has about 5,000 cycles per sample on each core, with some of core 0 used for the Computer's own input and output.*

So **most tone3000 captures, which are WaveNet models, will not run on this card**. The practical route is a **small LSTM model**:

- NAM can train LSTM models as well as WaveNets, and the NAM plugin plays both. Train one yourself with the [NAM trainer](https://github.com/sdatkinson/neural-amp-modeler), using an LSTM architecture with `num_layers: 1` and `hidden_size` of 8 or less. The training data (a reamped test signal) is the same as for any NAM capture, so you can reuse captures you have made before.
- Some LSTM models are shared on tone3000 too, but at the time of writing they are hard to find there.

The converter tells you whether a model will fit before you build it.

## Controls

| Control | Function |
|---|---|
| Main knob + CV 1 | Drive: input gain, -24dB to +24dB. Noon is the model's calibrated input level |
| Knob X | Output level (noon is about unity) |
| Knob Y + CV 2 | Cabinet: low-pass filter from 1kHz to 12kHz, off fully clockwise |
| Switch Up | Model 1 |
| Switch Middle | Model 2 (or model 1 again, if only one is built in) |
| Switch Down | Bypass while held |
| Pulse In 1 | Rising edge toggles bypass |
| Audio In 1 | Input |
| Audio Out 1 | Model through the cabinet filter |
| Audio Out 2 | Model without the cabinet filter |
| CV Out 1 | Envelope of Audio Out 1 |
| Pulse Out 1 | High while bypassed |

LEDs: top left shows input level and lights fully when the model's input clips; top right shows output level; the middle row shows which model is playing; bottom left shows bypass; bottom right lights if the processor can't keep up with the model.

Many captures are of an amp without a speaker cabinet, which sounds harsh on its own. The cabinet filter is a simple stand-in for a speaker, not an impulse response.

Input level: NAM models expect a particular input level, recorded in the `.nam` file as `input_level_dbu`. With Drive at noon, the card scales its input to match (or treats +/-5V as full scale if the file doesn't say). Eurorack signals are much hotter than a guitar, so you'll often want Drive below noon.

## Putting your own models on the card

You need the [Pico SDK](https://github.com/raspberrypi/pico-sdk) set up to build firmware (the repository's [DevContainer](../../.devcontainer/README.md) has everything), and Python 3 with numpy.

1. Convert one or two `.nam` files into `model_data.h`:

   ```sh
   python3 tools/nam2header.py my_model.nam [second_model.nam] -o model_data.h --verify
   ```

   This reports how much of each core the model will need, and refuses models that are too slow. `--verify` also builds the engine with your computer's C++ compiler, runs it, and compares the result with a floating point version of NAM.

2. Build:

   ```sh
   mkdir build && cd build
   cmake .. && make
   ```

3. Copy `build/nam_amp.uf2` onto the program card.

## How it works

- **Fixed point.** `tools/nam2header.py` converts each model's weights to integers, with scaling chosen so the 32-bit sums can never overflow. Signals are Q12 (4096 = 1.0). The signal path between WaveNet layers gets its own scaling per layer, based on the levels measured when running the model on a test signal. LSTM state uses Q14 for extra precision. The fixed-point error is typically 50 to 90dB below full scale, which is quieter than the effect of one step of the Computer's 12-bit input.
- **Two cores.** The model is split into units (WaveNet layers or LSTM layers) and runs as a pipeline: core 1 runs the first units on each new sample, while core 0, inside `ProcessSample`, finishes the previous sample. This adds one sample of latency. The converter picks the split point to balance the load.
- **Assembly kernels.** The multiply-accumulate loops are short inline-assembly routines (in `nam_fixed.h`). They keep accumulators in the high registers and stream weights with `LDM`, at about 4.5 cycles per multiply-accumulate. That is roughly twice as fast as the compiler manages from C++. The whole card runs from RAM.

Files:

- `main.cpp`: the card (ComputerCard), input/output, cabinet filter, and the two-core pipeline
- `nam_fixed.h`: fixed-point NAM engine (WaveNet and LSTM), no Pico SDK dependencies
- `model_data.h`: generated by `nam2header.py`
- `tools/nam2header.py`: `.nam` converter, CPU estimate, and verification
- `tools/host_check.cpp`: runs the engine on a computer, for `--verify`
- `tools/cycle_count.py`, `tools/cycle_bench.cpp`: measure cycle counts by emulating the Cortex-M0+ code (needs `pip install unicorn capstone pyelftools`)
- `models/`: the demo model

### Supported NAM files

- WaveNet models in the standard ("A1") format: kernel size 3, Tanh, Hardtanh, ReLU or Sigmoid activations, optionally gated, 1 to 16 channels. The newer "A2" features (FiLM, grouped convolutions, bottlenecks, head 1x1) are not supported.
- LSTM models with up to 4 layers and hidden size up to 16 (though only small ones run in real time).
- 48kHz models only, mono.

## Ideas for making WaveNet models fit

- Hand-written assembly for the whole WaveNet layer, not just the multiply-accumulates. Fixed per-layer costs (activations, ring buffers, saturation) are currently more than half the time for a Nano layer.
- ReLU activations instead of Tanh: much cheaper per channel. Others have reported running Nano models on embedded hardware this way, but it needs a model trained with ReLU.
- An RP2350-based card would help a lot: its Cortex-M33 cores have single-cycle 32-bit multiply-accumulate and SIMD instructions.

## Credits and licence

MIT licence. The NAM model format and the reference implementation this card follows are from [NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore) by Steven Atkinson (MIT). The demo model is that project's tiny test model; see [models/README.md](models/README.md).
