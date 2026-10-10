# Host connections and interfaces

The current firmware implements only a USB CDC ACM diagnostic shell. Pybricks
protocol compatibility, interface selection, Bluetooth, and SMP are not yet
implemented. This document separates required behavior from the USB mechanism
selected for an experiment.

## One management session

Allow one controlling computer session per hub across USB and Bluetooth,
including diagnostics and application transfers. An active USB owner excludes a
second Bluetooth management owner, and conversely. Tools share the current owner
or receive a busy result; another client cannot silently take over the session.
USB power alone does not acquire ownership.

The selected interface determines how requests are interpreted for that session.
Malformed input is an error, not a reason to guess another protocol. Switching
interfaces must preserve application state and the shared settings model. Normal
host disconnection does not stop an autonomous application. Session ownership
also governs [inactivity shutdown](settings.md#inactivity-shutdown).

Input-device connections are governed separately by [Bluetooth](bluetooth.md) and
[applications](applications.md#host-and-input-devices); they are not additional
computer management clients.

## Interface roles

| Interface | Purpose |
| --- | --- |
| Pybricks-compatible | Existing clients discover the hub and use supported Python upload, start/stop, status, and input/output operations |
| Brickphyr management | Zephyr Shell for interactive settings and diagnostics; standard management mechanisms for automation and additional application formats |

Both interfaces use the same firmware services and application supervisor.
Choosing an interface does not replace the operating system or select an execution
engine. A terminal must remain useful independently of Python, native, or WASM
execution. Its application commands use the same lifecycle operations as other
clients; they do not bypass ownership or admission checks.

Prefer SMP/MCUmgr for extended management if its existing operations and clients
fit the actual transfer and settings requirements. Reuse the standard mechanisms
before adding a small application-management command group. Firmware image update
support is not a loader for native or WASM applications. Do not create another
framing or file-transfer protocol merely to rename existing functionality.

Shell and SMP can share a serial channel through Zephyr's existing SMP transport.
This is not automatic multiplexing with the Pybricks USB protocol. Over Bluetooth,
GATT characteristic addresses distinguish services without guessing payloads;
that does not relax the single-management-session rule.

## USB selection experiment

The required outcome is computer-controlled interface selection without buttons
or a hub restart. Buttons may offer the same choice locally, but automated tools
must not depend on them. Bootloader recovery remains a separate operation under
[recovery](recovery.md).

The candidate uses one CDC ACM data channel and device-specific control requests
on USB endpoint 0 to query and select its interface. Zephyr exposes vendor-request
handlers for this purpose. This avoids requiring a second CDC port or an extra
handshake from an unchanged Pybricks client. The initial compatible interface must
be usable by that client without knowing Brickphyr's selection extension.

Accept the mechanism only after a host can select Shell, execute a real command,
select Pybricks, read real identification data, and return to Shell without a
reboot. Verify host-driver access to control requests, rejection of unsupported
selections, ownership during handover, and separation of buffered data between
interfaces. No client may steal an active session through the control endpoint.

Request encoding, host API, session-release detection, and interface reset policy
are still experimental choices. This experiment proves interface selection, not
Python upload or execution. Do not advertise unavailable Pybricks capabilities.

## References

- [Pybricks compatibility](compatibility.md) owns the Python and client contract.
- [Zephyr USB API](https://docs.zephyrproject.org/latest/doxygen/html/group__usbd__api.html)
  supplies control-request handlers.
- [SMP transport](https://docs.zephyrproject.org/latest/services/device_mgmt/smp_transport.html)
  defines Bluetooth and serial framing.
- [MCUmgr handlers](https://docs.zephyrproject.org/latest/services/device_mgmt/mcumgr_handlers.html)
  describes extension groups for operations absent from the standard set.
