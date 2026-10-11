# IMU processing port

This directory ports Pybricks IMU processing to Brickphyr's acquisition and
Settings service. It retains the original MIT notices; the full license is in
[LICENSES/Pybricks-MIT.txt](../../LICENSES/Pybricks-MIT.txt).

The source revision is
[`a7b1b04ae4f8449dd46b3dfb97c420334439fd35`](https://github.com/alepoydes/pybricks-micropython/tree/a7b1b04ae4f8449dd46b3dfb97c420334439fd35),
based on Pybricks v4.1.0b6.

| Local source | Original source |
| --- | --- |
| `processing.c` | `lib/pbio/src/imu.c` |
| `stationary.c`, `stationary.h` | Stationary detection in `lib/pbio/drv/imu/imu_lsm6ds3tr_c_stm32.c` |

The port retains the calibration, quaternion fusion, heading and stationary
window algorithms. It accepts physical measurements and elapsed time from
Brickphyr instead of calling Pybricks drivers, clocks, motor state or storage.
Settings changes validate before mutation; orientation reads preserve the input
sample and reflect the latest integration. Explicit reset operations let the
service report a new reference after missing data. Stationary differences use
32-bit arithmetic to avoid wrapping across the signed sensor range.

`processing.h` is a private integration boundary, not a new dependency API.
`hub_imu_state.c` serializes processing and configuration, publishes snapshots,
and decides when calibration may learn. Native applications and future runtime
bindings consume the same service. The original Pybricks IMU implementation is
not also compiled into Brickphyr. PBIO geometry helpers and public types are
compiled from the west dependency; see its
[finite-input prerequisite](../../docs/upstream.md#imu-processing-port).

When updating the Pybricks source version, compare both original files and the
geometry helpers, carry applicable fixes into this port, and run the numerical
and acquisition checks described in [testing](../../docs/testing.md).
