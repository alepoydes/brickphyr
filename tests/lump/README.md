# LUMP fixtures

`captures.h` retains the device-to-hub metadata packets from Pybricks
[`lib/pbio/test/src/test_lump.c`](https://github.com/alepoydes/pybricks-micropython/blob/a7b1b04ae4f8449dd46b3dfb97c420334439fd35/lib/pbio/test/src/test_lump.c),
under its [MIT license](../../LICENSES/Pybricks-MIT.txt). The selected arrays are
`msg0`–`msg82` from `test_boost_color_distance_sensor`, `msg0`–`msg33` from
`test_boost_interactive_motor`, and `msg2`–`msg54` from `test_technic_large_motor`.
They are concatenated without changing bytes, ending at the device's ACK.
These are upstream fixtures, not Brickphyr hardware captures.

Tests feed them through the production parser and UART service. Additional
synthetic malformed frames check bounds and rejection; synthetic serial timing
does not verify electrical baud switching, ISR cost or physical device behavior.

The sensor peers in `main.c` construct metadata from the pinned `lego/device.h`
mode formats. Their SPIKE color/ultrasonic/force, BOOST color/distance and WeDo
tilt/infrared readings are deliberately synthetic. They check meaningful units,
settling, invalid calibration, mode ownership, ignored selections, power polarity
and cleanup failure through the actual UART/GPIO service, Shell and SMP. They
are not captures or evidence of physical sensor accuracy.

Motor cases use the upstream BOOST and Technic handshakes plus synthetic metadata
for the remaining known absolute motors. Synthetic angle sequences check units,
turn counting, signed-counter rollover, invalid values and reconnect/reset through
the same service. Shell and SMP must expose the same angle, and encoder selection
must leave both motor outputs inactive even when supply flags are advertised.

A synthetic private-ID peer advertises versions, units, ranges, mappings, flags
and opaque INFO fields, then sends all four numeric wire formats. The actual
`port` Shell command must escape device strings, retain unavailable fields and
show the same bytes and values as the native snapshot without SELECT, ownership
or H-bridge changes. Boundary cases include 16 modes, 32-byte opaque payloads,
duplicate or short metadata, overlong units and reconnects without optional fields.

The mode-memory check fills every configured port with sixteen complete modes,
then reconnects with shorter payloads and verifies reclamation. The four-port
variant uses `../ports/four.overlay` and the corresponding devicetree-derived
heap minimum, without overriding heap size. Run `lump_capacity::test_mode_memory_lifetime`
and `lump::test_mode_payload_allocation_failure` for that capacity boundary;
the six-port fixture owns the full protocol suite.

The ARM capacity variant keeps four real UART link allocations live while the
same parser check interleaves maximum metadata growth, then frees both. It uses
the 25 KiB devicetree-derived minimum with small heap headers and no libc malloc
arena. Its independent UART emulators provide link state, not simultaneous wire
negotiation or a physical timing test. Build separately before the short check:

```sh
uv run --locked west build -b mps2/an386 -s tests/lump -d build/lump-arm-capacity -o=-j4 -- \
  -DUART_CAPACITY_ONLY=ON -DEXTRA_CONF_FILE="capacity.conf;$PWD/tests/arm.conf" \
  '-DDTC_OVERLAY_FILE=app.overlay;capacity.overlay;../ports/four.overlay'
uv run --locked pytest -q -x 'tests/test_native.py::test_arm_firmware[lump-arm-capacity-PROJECT EXECUTION SUCCESSFUL]'
```
