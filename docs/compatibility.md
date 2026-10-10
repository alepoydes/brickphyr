# Compatibility

Preserving Pybricks' Python behavior is a design commitment. No Python API is
implemented by Brickphyr yet; a stable target contract must not be confused with
a claim that the port is already complete.

## Reference and scope

The available starting behavioral reference is Pybricks v4.1.0b4, as used in the
existing Inventor development environment. The source reference is recorded in
[hardware](hardware.md). Before implementing compatibility, pin the selected
Pybricks/MicroPython build and publish the covered API version. Changing that
baseline later is an explicit compatibility decision.

Compatibility includes signatures, argument defaults, units, coordinate
conventions, return values, exceptions, calibration behavior, motor stopping,
wait/cancel semantics, and relevant asynchronous behavior. Matching a module or
method name alone is insufficient. Preserve behavior using the reused Pybricks
implementation where practical, not independent reimplementations of each binding.

The contract applies to the APIs available on the given hub and build. Hardware
absence and an unfinished port are different reasons for unavailability and must
be documented separately. Required compatibility deviations are explicit errors
or documented experimental differences, never plausible fake measurements.

## Initial coverage plan

| Area | Validation to add with implementation |
| --- | --- |
| Hub identification, buttons, light/display, power | Actual capabilities and observable API behavior |
| Motors, encoders, control, DriveBase | Units, direction, quantization, stopping, wait/cancel, ownership |
| IMU | Units/axes, filter/sample timing, calibration and persistence |
| Tools, timing, asynchronous operations | Public API semantics under Zephyr execution |
| BLE and controllers | Connection, timeout, input and disconnection behavior |
| Storage and application launch | Explicit differences from Pybricks' program-slot model |

All rows are pending. Refine this into a method-level coverage report when there
are executable compatibility cases; do not hand-maintain a huge empty API catalog.
Test the same small programs on the reference firmware and Brickphyr, retaining
expected tolerances and genuine hardware differences.

## Boundaries that are not automatically compatible

- Python source compatibility does not guarantee `.mpy` bytecode compatibility or
  the native MicroPython dynruntime ABI. Both depend on the selected runtime build.
- The new native SDK is initially unstable and checked at load time. It is not
  the PBIO internal ABI or an unrestricted Zephyr export table.
- Compatible firmware flashing does not guarantee the old program-upload protocol
  can carry new native/WASM applications or larger images unchanged.
- Replacing the scheduler must preserve externally visible waits and cancellation;
  it need not reproduce Pybricks' cooperative internal implementation.
- Hub calibration, pairing data, and user programs are different persistence
  domains. Firmware replacement/restoration needs explicit policies for each.

## Extensions

Keep additional Brickphyr features distinguishable from Pybricks compatibility.
Advertise available engines and capabilities so tools can report unsupported
operations before starting them. Avoid modifying established Python meanings to
expose a new native feature. Extra input devices and transports should extend the
appropriate service, without forcing an unrelated API rewrite.

The reference Pybricks API and implementation remain the source of truth for
compatibility; Brickphyr documents coverage and intentional differences, rather
than copying the entire upstream manual.
