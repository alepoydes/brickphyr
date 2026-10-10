# Bluetooth integration

Use the Zephyr Bluetooth host stack as the selected implementation direction.
The first hub integration is experimental until its controller initialization,
transport, simultaneous roles, and timing are exercised on identified hardware.
The existing Pybricks BTstack integration is evidence about the device, not the
host stack to transplant wholesale.

## Boundaries

| Part | Owner / work |
| --- | --- |
| Radio/controller hardware | Already present in the hub; identify exact variant |
| UART/SPI transport, reset, clocks, flow control | Zephyr drivers plus board configuration |
| Vendor controller initialization / service pack | Audit hardware requirements and redistribution terms |
| Host protocols, connection handling, GATT | Zephyr Bluetooth host |
| Pybricks-facing services and controller-specific behavior | Reuse suitable Pybricks logic over the new host |
| Application and desktop behavior | Shared lifecycle and explicit capability protocol |

For the initial Prime F4 source reference, the radio is TI CC2564C. A working
generic HCI UART driver does not establish that its reset, flow control, baud-rate
changes, initialization commands, and vendor patch sequence are correct for this
board. Confirm those details from the platform/device code and radio documentation.
Do not assume a ready-to-use Zephyr CC2564C board integration exists.

## Required behavior

Support a desktop link and a supported controller connection together on capable
hardware. Separate peripheral/central connection work from application computation;
neither a pairing attempt nor a slow receiver may hold the motor service hostage.
Keep all waits bounded or cancellable according to the public operation contract.

Normal telemetry is bounded. Host backpressure must not suspend the application's
control loop. Report disconnects and dropped diagnostics explicitly. Do not filter
legitimate repeated notifications by payload equality or let multiple desktop
clients compete for the same hub's control channel.

Follow the [single management session](host.md#one-management-session) rule across
Bluetooth and USB. Pybricks discovery and client compatibility are defined in
[compatibility](compatibility.md#existing-host-clients); interface selection does
not create a second application supervisor.

Connection policy belongs at the appropriate boundary: a program may choose
autonomy after controller loss, while a manual motor test may require stopping.
Do not hardwire babybot's button assignments or waking rules into the Bluetooth
subsystem. Additional controllers are future device integrations, not a reason
to invent a large input plugin framework now.

## First verification

Bring up and verify [independent recovery](recovery.md) before changing the radio
stack. Then establish controller identification/HCI exchange, advertising and
desktop transfer, controller input, disconnection/cancellation, and combined
traffic under a measured sensor/motor-service workload. Begin with outputs gated
off. Record whole-system latency and peak memory, not only average packet rates.

A failed radio experiment must remain recoverable through the retained boot path.
USB-based recovery on the first target must not depend on the new Bluetooth host.
Other hubs need their own bootloader transport evidence.

## Licensing

Do not carry BTstack's noncommercial dependency into the default distribution.
Using Zephyr's host avoids that particular dependency, but does not establish
redistribution rights for TI controller initialization binaries. Resolve the
actual included service pack and its license separately. See [licensing](licensing.md).

Use the Bluetooth APIs in the chosen Zephyr release and the corresponding
Pybricks platform sources. Pin them during implementation; current upstream
documentation alone is not a validated version combination.
