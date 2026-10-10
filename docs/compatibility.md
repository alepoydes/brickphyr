# Compatibility

Preserving Pybricks' Python behavior is a design commitment. No Python API is
implemented by Brickphyr yet; a stable target contract must not be confused with
a claim that the port is already complete.

## Reference and scope

The starting behavioral reference is Pybricks v4.1.0b4. Its source revision is
recorded in [hardware](hardware.md). Before implementing compatibility, pin the
selected Pybricks/MicroPython build and publish the covered API version. Changing
that baseline later is an explicit compatibility decision.

Compatibility includes signatures, argument defaults, units, coordinate
conventions, return values, exceptions, calibration behavior, motor stopping,
wait/cancel semantics, and relevant asynchronous behavior. Matching a module or
method name alone is insufficient. Preserve behavior using the reused Pybricks
implementation where practical, not independent reimplementations of each binding.

Compatibility covers supported, useful operation; it does not require reproducing
bugs, deadlocks, or a loss of system control. Document deliberate corrections and
test the resulting behavior rather than treating every upstream failure as a
contract to preserve.

The contract applies to the APIs available on the given hub and build. Hardware
absence and an unfinished port are different reasons for unavailability and must
be documented separately. Required compatibility deviations are explicit errors
or documented experimental differences, never plausible fake measurements.

## Multitasking and system responsiveness

Support Pybricks `async`, `await`, `multitask`, and `run_task`, including normal
completion, waiting, and cancellation. Zephyr services must continue independently
of a Python program yielding. Reusing these Python interfaces does not require
reusing PBIO's cooperative scheduling for the whole firmware or mapping every
coroutine to a kernel thread.

A non-yielding or faulty application must remain stoppable while management and
essential device services continue. If a coroutine cannot be terminated safely
in isolation, terminate its application and report the cause. Do not resume a
damaged runtime merely to preserve a connection. Verify this property separately
for Python, native, and WASM under the [application contract](applications.md).

## Existing host clients

Retain a Pybricks-compatible interface so existing Pybricks Code and host tools can
discover the hub, upload supported Python programs, start/stop them, and exchange
status and input/output. Python source compatibility alone does not provide this
client compatibility. Validate discovery, protocol versions, capabilities, program
packaging, and `.mpy` format against a specified client/runtime combination.

Once Bluetooth is implemented and enabled, an available hub must advertise for
host connection without requiring a user program. A connected client must see
only implemented capabilities; an identification reply is not proof that Python
execution or every built-in Pybricks tool works. Firmware installation and
restoration have their own [recovery contract](recovery.md).

The [host interface](host.md) defines session ownership and the proposed USB
selection experiment. Native/WASM delivery and management may use standard
mechanisms beyond the unchanged Pybricks client's capabilities.

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

Extensions may expose additional hardware, such as the hub's Bluetooth-button
LED. Select names and capability reporting when implementing the feature; define
ownership relative to system connection indications. Do not change an existing
call's meaning to introduce the extension.

The reference Pybricks API and implementation remain the source of truth for
compatibility; Brickphyr documents coverage and intentional differences, rather
than copying the entire upstream manual.
