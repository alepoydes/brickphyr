# Motor computation adapter

`control.c` ports the Powered Up model tables and default gains from
`lib/pbio/src/motor/servo_settings.c` and the settings initialization in
`lib/pbio/src/servo.c` at Pybricks revision
[`a7b1b04ae4f8449dd46b3dfb97c420334439fd35`](https://github.com/alepoydes/pybricks-micropython/tree/a7b1b04ae4f8449dd46b3dfb97c420334439fd35).
It preserves the [Pybricks MIT](../../LICENSES/Pybricks-MIT.txt) and
[LEGO BSD-3-Clause](../../LICENSES/LEGO-BSD-3-Clause.txt) notices.
`voltage.h` supplies the model voltage cap to both this controller and the
port service's DC commands, using that same source and revision.

The numerical control, trajectory, observer, integrator, differentiator, angle
and integer-math implementations compile directly from the west dependency.
No PBIO scheduler, device driver or PBSYS service runs. The configuration disables
the optional non-free PBSYS telemetry reached through PBIO header dependencies.

The adapter uses millidegrees and the models' 5 ms step. It accepts explicit time
and measured angle and returns a voltage request; a separate prediction call uses
the voltage corresponding to the successful output operation. Instances stay at
fixed addresses because PBIO integrators reference their containing settings.
No application buffer, hardware access, allocation or service lock belongs here.
`motor_control_observe` adapts load/stall selection from PBIO `servo.c` at the
same revision. It reads the observer after prediction, without advancing it
again. Controller status uses the public stall bit; the PBIO convenience getter
also calculates duration from its global clock, which this adapter does not use.
Motion dispatch also adapts `servo.c`: explicit timed/relative commands, instant
tracking, hold and temporary stall torque limits use the same controller. Smart
relative endpoints and completion decisions remain in the dependency. Custom
precision profiles use the same initialization equations as its servo setup.

Long encoder origins stay in rotation-plus-millidegree form. In particular, timed
completion with HOLD starts a zero-relative position hold, avoiding PBIO's
int32 application-unit conversion of the absolute origin. Instant tracking uses
the public hold transition and a public constant trajectory with a long position.
Unsupported models,
out-of-range requests and unsafe arithmetic ranges return errors. A finite timed
move must be shorter than PBIO's 300000 ms forever sentinel. Zero requests an
indefinite run in the original raw command; the explicit TIMED action completes
immediately, matching the Python method. The native boundary does not expose PBIO structures.
Trajectory rejection requires the [PBIO correction](../../docs/upstream.md#motor-trajectory-prerequisite).

The existing port worker owns scheduling, acquisition validity, battery-to-duty
conversion, output, observations and cleanup. See [external ports](../../docs/ports.md).
Synthetic closed-loop and lifecycle tests do not establish physical tuning,
encoder accuracy, interrupt latency or stopping distance. When updating PBIO,
compare the two ported source sections and rerun both numerical and port-service
checks before accepting changed defaults or computation behavior.

Control settings reuse PBIO's public conversion/validation functions on a copy,
then publish only a fully valid group. The worker applies the snapshot between
frames without restarting control or erasing its command-local torque limit.
`pb_type_control.c` at the same pinned revision supplies the diagnostic trajectory
layout. The port boundary applies direction; the numerical controller and its
private settings remain in physical shaft units.
