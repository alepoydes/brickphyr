# Motor computation adapter

`control.c` ports the Powered Up model tables and default gains from
`lib/pbio/src/motor/servo_settings.c` and the settings initialization in
`lib/pbio/src/servo.c` at Pybricks revision
[`a7b1b04ae4f8449dd46b3dfb97c420334439fd35`](https://github.com/alepoydes/pybricks-micropython/tree/a7b1b04ae4f8449dd46b3dfb97c420334439fd35).
It preserves the [Pybricks MIT](../../LICENSES/Pybricks-MIT.txt) and
[LEGO BSD-3-Clause](../../LICENSES/LEGO-BSD-3-Clause.txt) notices.

The numerical control, trajectory, observer, integrator, differentiator, angle
and integer-math implementations compile directly from the west dependency.
No PBIO scheduler, device driver or PBSYS service runs. The configuration disables
the optional non-free PBSYS telemetry reached through PBIO header dependencies.

The adapter uses millidegrees and the models' 5 ms step. It accepts explicit time
and measured angle and returns a voltage request; a separate prediction call uses
the voltage corresponding to the successful output operation. Instances stay at
fixed addresses because PBIO integrators reference their containing settings.
No application buffer, hardware access, allocation or service lock belongs here.

Long encoder origins stay in rotation-plus-millidegree form. In particular, timed
completion with HOLD starts a zero-relative position hold, avoiding PBIO's
int32 application-unit conversion of the absolute origin. Unsupported models,
out-of-range requests and unsafe arithmetic ranges return errors. A finite timed
move must be shorter than PBIO's 300000 ms forever sentinel; zero requests an
indefinite run. The native boundary does not expose PBIO structures.
Trajectory rejection requires the [PBIO correction](../../docs/upstream.md#motor-trajectory-prerequisite).

The existing port worker owns scheduling, acquisition validity, battery-to-duty
conversion, output, observations and cleanup. See [external ports](../../docs/ports.md).
Synthetic closed-loop and lifecycle tests do not establish physical tuning,
encoder accuracy, interrupt latency or stopping distance. When updating PBIO,
compare the two ported source sections and rerun both numerical and port-service
checks before accepting changed defaults or computation behavior.
