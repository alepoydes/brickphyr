# Bluetooth integration

Use the Zephyr Bluetooth host stack as the selected implementation direction.
The first hub integration is experimental until its controller initialization,
transport, simultaneous roles, and timing are exercised on identified hardware.
The existing Pybricks BTstack integration is evidence about the device, not the
host stack to transplant wholesale.

## Experimental F4 controller

An optional build initializes the Inventor F4 CC2564C through Zephyr's H:4 driver
and the official TI service pack. Its Pybricks GATT endpoint provides identity,
capabilities, status and explicit unsupported-command errors. Linux pybricksdev
identification, reconnection, return to Shell and USB/BLE exclusion are checked on
one Inventor F4 (PCB revision unknown). Standard SMP BLE clients also provide echo
and settings read/write/save through the same runtime and persistence handlers as
USB. The native interface also provides the [shared interactive Shell](host.md#native-bluetooth-terminal).
Program execution, pairing and controller input are not implemented in this profile.

Build this profile from the repository:

```sh
uv run --locked west build -b spike_prime_f4 -d build/bluetooth-controller -t firmware_package -- -DEXTRA_CONF_FILE=boards/spike_prime_f4_bluetooth.conf -DDTC_OVERLAY_FILE=boards/spike_prime_f4_bluetooth.overlay
```

Install `build/bluetooth-controller/zephyr/firmware.zip` with the same
[Pybricks installer](builds.md#experimental-firmware-package). This profile starts
Bluetooth and advertises automatically after boot; no USB command is required.
To inspect its state, open the [USB terminal](host.md#usage) and run:

```text
bluetooth
```

Close the terminal before connecting over BLE. The hub advertises the Pybricks
and SMP service UUIDs and name `Brickphyr`, and resumes advertising after a
connection closes. A live USB owner rejects a BLE contender by disconnecting it;
a live BLE owner rejects USB selection and discards incoming USB data. A rejected
USB port must close and reopen before it can acquire ownership. Closing a host
client may precede the hub's disconnect event: wait for ownership to be released
before opening the other transport. The 31-byte legacy advertising limit puts one
service UUID in advertising and the other beside the name in scan response. PnP
identity remains available through the Device Information GATT service.

The existing pybricksdev BLE client can identify the hub without uploading code:

```sh
uv run --locked --group flash python - <<'PY'
import asyncio
from pybricksdev.ble import find_device
from pybricksdev.connections.pybricks import PybricksHubBLE

async def main():
    hub = PybricksHubBLE(await find_device(name="Brickphyr", timeout=10))
    try:
        await hub.connect()
        print(hub.fw_version, hub.hub_kind, hub.hub_variant)
    finally:
        await hub.disconnect()

asyncio.run(main())
PY
```

The Device Information and Pybricks services reuse the USB endpoint's identity
and command validation. Capabilities report no engines, slots or program storage.
The Nordic UART notification characteristic exists because current clients
subscribe to it; stdin writes are rejected until an execution engine exists.
The Brickphyr exit command `0x80`, written with response to the Pybricks command
characteristic, selects USB Shell after disconnection. Initial subscription
reports BLE ownership and no running program; dynamic application status awaits
the application supervisor.

### Pybricks Code

Discovery, connection and reconnection through [Pybricks Code](https://code.pybricks.com)
are verified on the Inventor F4 using Chromium 154 on Linux with experimental web
platform features enabled. Close any USB terminal, click the website's Bluetooth
button and select `Brickphyr`. The connected hub name appears
in the status bar. Disconnecting or closing the browser releases the management
session so USB can reconnect.

The tested website supports profile 1.5.0 and warns about the hub's 1.7.0 profile,
but completes the connection. This verifies discovery and identification only:
program upload, execution and REPL are not implemented. Do not install different
firmware merely to dismiss the profile warning.

### Controller lifecycle

After closing BLE, open USB Shell and run `bluetooth off` to stop advertising and
disable the controller until `bluetooth on` or reboot. Repeating either command is
idempotent. `bluetooth` reports off, starting, on, stopping or failed; querying it
does not wait for initialization. On means enabled, including while a peer is
connected and advertising is stopped.

Boot initialization and advertising restart run on a dedicated Zephyr workqueue
below hub and USB service priority. Controller waits therefore do not occupy the
button/power loop or system workqueue. Explicit on/off commands wait for their
operation to finish and supersede pending boot work. The first lifecycle failure
retains its operation and error until reboot; later commands do not repeat a
partially completed initialization. The optional profile does not expose Zephyr's
independent `bt` shell, so radio lifecycle has one command path. Radio operations
do not reload or save settings.

The board supplies the required slow clock on TIM8 channel 4 / PC9, starts USART2
at 115200 baud with RTS/CTS, and uses PA2 for controller reset. The 2930-cycle
clock period at 96 MHz matches the Pybricks F4 configuration. The controller setup
checks the TI manufacturer and CC2564C ROM version before applying the pinned
service pack. It disables the documented eHCILL deep-sleep option; standard H:4
does not implement that handshake. No BTstack host code is included; see
[artifact licensing](licensing.md).

The fixed script is validated before its commands run; Zephyr owns command/reply
correlation and its HCI timeout. The local [HCI timeout change](upstream.md#hci-timeout-prerequisite)
returns an error instead of asserting, detaches caller-local state from late
replies, and cancels commands still queued. It does not reset the controller or
establish recovery after a radio fault during a connection. After a failed radio
operation, reboot before trying again; keep the [independent recovery path](recovery.md)
available.

Startup failure with the controller held in reset is checked on the Inventor F4
(PCB revision unknown), using the experimental profile on Zephyr 4.4.2. USB Shell
responds during the HCI wait and after the initialization timeout; subsequent
radio commands preserve the first error. All four buttons still register, and
a long center press disconnects USB with operator-confirmed lights off. See
[power behavior](settings.md#inactivity-shutdown) for button restart with USB attached.
USB DFU entry and normal-image restoration with the Pybricks installer succeed
after this startup fault; USB and BLE terminals work in the restored image.
Controller loss during an active BLE session, radio recovery, Bluetooth throughput, pairing,
controller input, and behavior under combined application traffic remain unverified.

The local H:4 close-path correction discards partial framing and releases pending
buffers even when board teardown reports an error. UART-emulator checks cover
reopening after partial RX/TX and teardown failure. This does not make a failed
controller ready or provide automatic recovery of the Bluetooth host stack.

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
