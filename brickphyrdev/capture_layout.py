# SPDX-License-Identifier: Apache-2.0
"""Capture/2 field order, selection names, wire types and firmware bindings.

A selector of None makes a field mandatory. Optional bits follow their order
within each kind. This data also generates the firmware packing functions.
"""

KINDS = (
    ("imu", "struct bp_imu_sample", (
        (None, "sample_us", "q", "value->sample_us"),
        (None, "read_us", "q", "value->read_us"),
        (None, "sequence", "I", "value->sequence"),
        (None, "missed", "I", "value->missed"),
        (None, "failures", "I", "value->failures"),
        (None, "error", "i", "value->error"),
        ("ax", "ax_um_s2", "i", "value->acceleration[0]"),
        ("ay", "ay_um_s2", "i", "value->acceleration[1]"),
        ("az", "az_um_s2", "i", "value->acceleration[2]"),
        ("gx", "gx_mdps", "i", "value->angular_velocity[0]"),
        ("gy", "gy_mdps", "i", "value->angular_velocity[1]"),
        ("gz", "gz_mdps", "i", "value->angular_velocity[2]"),
    )),
    ("imu.calibrated", "struct bp_imu_state", (
        (None, "sample_us", "q", "value->raw.sample_us"),
        (None, "read_us", "q", "value->raw.read_us"),
        (None, "sequence", "I", "value->raw.sequence"),
        (None, "missed", "I", "value->raw.missed"),
        (None, "failures", "I", "value->raw.failures"),
        (None, "raw_error", "i", "value->raw.error"),
        (None, "epoch", "I", "value->epoch"),
        (None, "error", "i", "value->error"),
        (None, "flags", "I", "(value->ready ? 1 : 0) | (value->stationary ? 2 : 0)"),
        ("ax", "ax_um_s2", "i", "value->acceleration[0]"),
        ("ay", "ay_um_s2", "i", "value->acceleration[1]"),
        ("az", "az_um_s2", "i", "value->acceleration[2]"),
        ("gx", "gx_mdps", "i", "value->angular_velocity[0]"),
        ("gy", "gy_mdps", "i", "value->angular_velocity[1]"),
        ("gz", "gz_mdps", "i", "value->angular_velocity[2]"),
    )),
    ("battery", "struct bp_battery_sample", (
        (None, "sample_ms", "q", "value->sample_ms"),
        (None, "read_ms", "q", "value->read_ms"),
        (None, "sequence", "I", "value->sequence"),
        (None, "failures", "I", "value->failures"),
        (None, "error", "i", "value->error"),
        (None, "flags", "I", "(value->low ? 1 : 0) | (value->critical ? 2 : 0)"),
        ("voltage", "voltage_mv", "I", "value->voltage_mv"),
        ("current", "current_ma", "I", "value->current_ma"),
        ("average", "average_mv", "I", "value->average_mv"),
    )),
    ("encoder", "struct bp_control_port", (
        (None, "sample_ms", "q", "value->sample_ms"),
        (None, "generation", "I", "value->generation"),
        (None, "sequence", "I", "value->sequence"),
        (None, "error", "i", "value->error"),
        ("angle", "angle_mdeg", "q", "value->angle_mdeg"),
    )),
    ("motor", "struct bp_control_port", (
        (None, "applied_ms", "q", "value->applied_ms"),
        (None, "generation", "I", "value->generation"),
        (None, "output_generation", "I", "value->output_generation"),
        (None, "output_error", "i", "value->output_error"),
        (None, "cleanup_error", "i", "value->cleanup_error"),
        (None, "mode", "B", "value->mode"),
        (None, "valid", "B", "value->valid"),
        (None, "pending", "B", "value->pending"),
        (None, "owner", "B", "value->owner"),
        ("duty", "duty_permille", "i", "value->duty_permille"),
        ("requested", "requested_ms", "q", "value->requested_ms"),
        ("request", "request", "I", "value->request"),
        ("completed", "completed", "I", "value->completed"),
        ("request_generation", "request_generation", "I", "value->request_generation"),
    )),
)

SOURCES = ("imu", "imu.calibrated", "battery") + tuple(
    f"{kind}.{port}" for port in "ABCDEF" for kind in ("encoder", "motor")
)
SOURCE_KINDS = (0, 1, 2) + (3, 4) * 6
CONFIG_SIZE = 16 + len(SOURCES)
# These mandatory raw values are also the first fields of imu.calibrated.
IMU_SHARED_FIELDS = 6
DELTA_COUNTERS = frozenset({
    "sequence", "missed", "failures", "epoch", "generation",
    "output_generation", "request", "completed", "request_generation",
})
