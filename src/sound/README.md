# Sound port

The speaker output retains Pybricks' board wiring, DAC/timer sequence, square-wave
levels and volume law. Source revision:
[`a7b1b04ae4f8449dd46b3dfb97c420334439fd35`](https://github.com/alepoydes/pybricks-micropython/tree/a7b1b04ae4f8449dd46b3dfb97c420334439fd35),
based on Pybricks v4.1.0b6. The original notices and
[MIT license](../../LICENSES/Pybricks-MIT.txt) are retained.

| Local source | Original source |
| --- | --- |
| `drivers/sound_stm32.c` | `lib/pbio/drv/sound/sound_stm32_hal_dac.c` (F4 path) |
| `boards/lego/spike_prime_f4/sound.dtsi` | `lib/pbio/platform/prime_hub_f4/platform.c`, sound section |
| `src/sound/waveform.c` | `lib/pbio/drv/sound/beep_sampled.c` and volume conversion in `pybricks/common/pb_type_speaker.c` |

The driver exclusively owns DAC1 channel 1, TIM6 and the amplifier. It replaces
the HAL integration with Zephyr DMA, GPIO, pin control, reset and clock APIs, and
uses ST LL register operations for the DAC trigger and timer. The ordinary Zephyr
DAC driver is not also instantiated for that peripheral. The timer source clock
includes APB's timer multiplier; no fixed peripheral clock is assumed.

Unlike the original continuous waveform buffer, Brickphyr uses two refillable
halves and reports DMA/DAC errors. The driver checks refill order, active DMA half
and late callbacks, mutes on detected failure, and joins DMA before releasing its
buffer. A refill that overruns its deadline may already have affected output;
detecting it does not prove sample-perfect playback under arbitrary interrupt
masking. Physical timing must be measured.

Tones retain the original square levels and logarithmic volume conversion. Their
phase advances at the common 48 kHz PCM clock instead of increasing the DAC sample
rate to 128 times the tone frequency. Edge positions are quantized to that clock.
Volume conversion happens once before playback, outside the ISR and IRQ lock.
PCM input is signed 16-bit mono; the DAC consumes its upper 12 bits after conversion
to unsigned samples. No sample queue points into application memory.

The application/terminal owner, bounded queue, draining, error reporting and
cleanup are implemented in `src/hub_sound.c`. Native and future Python/WASM
bindings use that service. See [hardware](../../docs/hardware.md#hub-sound) and
[testing](../../docs/testing.md) for contracts and evidence limits.
