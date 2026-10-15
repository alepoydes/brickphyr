# Bluetooth integration

Use the Zephyr Bluetooth host stack as the selected implementation direction.
The first hub integration is experimental until its controller initialization,
transport, simultaneous roles, and timing are exercised on identified hardware.
The existing Pybricks BTstack integration is evidence about the device, not the
host stack to transplant wholesale.

Management ownership, Pybricks messages and the native Shell stream live in one
shared BLE session implementation. Controller adapters supply connection
references, MTU and notification/disconnect operations; they do not duplicate
the terminal or command dispatcher. F4 uses the Zephyr adapter. Technic's factory
CC2640 owns GAP/GATT itself, so its [adapter](hardware.md#experimental-technic-base)
uses the TI network-processor interface. Physical endpoint acceptance remains pending.

## Experimental Technic controller

The optional [Technic BLE profile](builds.md#experimental-technic-base) starts the
factory CC2640, registers the management services and advertises the Pybricks
identity. Its native Shell and serial-framed SMP reuse the shared transport and
session owner; they do not require USB. The standard SMP BLE service is enabled
by `CONFIG_BRICKPHYR_CC2640_SMP`; it uses Zephyr's packet queue, fragment
reassembly and management handlers. The ATT MTU is fixed at 23 bytes, so larger
SMP packets span several writes or notifications. The legacy
NUS notification characteristic remains for client discovery, while NUS stdin
reports unsupported use.

The `bluetooth` Shell command reports radio state and accepts asynchronous
`on`/`off` requests; shutdown is terminal until reboot. A connection owns the
management session until its disconnect and any accepted operation have drained.
The controller worker bounds command waits and retains the first failure for
diagnosis. The BLE-only profile has no application engine; adding the
[native profile](builds.md#experimental-technic-base) enables RAM application
loading through the shared handlers. Neither profile provides Python, persistent
program storage or port APIs. The native profile also provides shared IMU
observations through the terminal and host monitor. Production-path synthetic checks cover cold startup,
Shell/SMP, fragmented standard SMP requests/replies, notification backpressure,
reconnect and isolation of old output. Disconnect or subscription cancellation
discards incomplete and queued packets; an accepted command finishes while its
old response is discarded. A new subscription cannot inherit that response.
No physical Technic Hub or CC2640 BLE connection has been tested.

## Experimental F4 controller

An optional build initializes the Inventor F4 CC2564C through Zephyr's H:4 driver
and the official TI service pack. Its Pybricks GATT endpoint provides identity,
capabilities, status and explicit unsupported-command errors. Linux pybricksdev
identification, reconnection, return to Shell and USB/BLE exclusion are checked on
one Inventor F4 (PCB revision unknown). Standard SMP BLE clients also provide echo
and settings read/write/save through the same runtime and persistence handlers as
USB. The native interface also provides the [shared interactive Shell](host.md#native-bluetooth-terminal).
The native application API includes experimental LEGO peripheral connections;
see [application LEGO devices](#application-lego-devices) for its limits and evidence.
Program execution through the Pybricks endpoint requires Python.
Adding the [Python profile](builds.md#experimental-python-profile) supplies
experimental program execution through the same endpoint; its current
[client coverage](host.md#implemented-pybricks-subset) is separate from the
physical controller checks above.

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

Host management, including MCUmgr, does not require pairing. Enabling pairing
for application peripherals does not change access to the management services.

The Bluetooth button [indicates the BLE connection](hardware.md#f4-buttons-lighting-and-power),
independently of the USB selection shown by the center button. A connection starts
without a selected profile. Successful Pybricks capability reads, notification
subscriptions or commands select cyan; opening the native terminal or accepting
an SMP request selects yellow. The native terminal retains its indication while
open; otherwise the last accepted profile-specific operation determines it.
Shared Device Information reads do not select a profile. Disconnecting clears
the selection for the next client.

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
and command handlers. The base profile reports no engines, slots or program storage.
The Nordic UART notification characteristic exists because current clients
subscribe to it; legacy stdin is unsupported. Python input/output uses the
Pybricks command/event characteristic.
The Brickphyr exit command `0x80`, written with response to the Pybricks command
characteristic, selects USB Shell after disconnection. Initial subscription
reports BLE ownership. With Python enabled it also reports the common
supervisor's running state and streams program output.

### Pybricks Code

Discovery, connection, program upload, execution and reconnection through unchanged
[Pybricks Code](https://code.pybricks.com) are verified on the Inventor F4 using
Chromium 154 on Linux with experimental web platform features enabled. Close any
USB terminal, click the website's Bluetooth button and select `Brickphyr`. The
connected hub name appears in the status bar. In the Python profile, a Python file
can be uploaded with Run; the website's terminal accepts `input()` and displays
stdout. Checks cover `PrimeHub` and `math` imports, battery voltage, normal return,
Stop while waiting for input, repeat execution and a new run after reconnection.
Disconnecting or closing the browser releases management so USB can reconnect.

The tested stable website supports profile 1.5.0 and warns about the hub's 1.7.0
profile, but completes these operations. [Pybricks Beta](https://beta.pybricks.com)
supports profile 1.7.0 and connects without that warning. Its F4 checks also cover
hub observations, asynchronous tools, REPL input, Ctrl-C interruption, Ctrl-D
soft reset, Stop and subsequent program execution. Use the website's Stop button
to leave its REPL; the native terminal's Ctrl-X Shell return is a separate command.
The unchanged reference `tests/pup/hub/status_light.py` uploads and completes there;
perceived light output still requires operator observation.

Browser-side native/Viper compilation remains unsupported by the tested beta
client: its Run compiler rejects decorators with `SyntaxError: invalid arch`.
The same decorators work when compiled on the hub through its browser REPL,
and through the [matching pybricksdev compiler](upstream.md#python-client-prerequisites).
Full library compatibility remains open. Firmware identity remains Brickphyr;
the website can independently suggest updating firmware because it compares that
version to Pybricks. Do not install different firmware merely to dismiss a warning.

### Controller lifecycle

After closing BLE, open USB Shell and run `bluetooth off` to stop advertising and
disable the controller until `bluetooth on` or reboot. Repeating either command is
idempotent. `bluetooth` reports off, starting, on, stopping or failed; querying it
does not wait for initialization. On means enabled, including while a peer is
connected and advertising is stopped.

Boot initialization, advertising restart, and requested peer disconnection run on
a dedicated Zephyr workqueue below hub and USB service priority. This includes
rejection of an occupied connection and cleanup after terminal output failure.
Controller waits therefore do not occupy the button/power loop, Bluetooth RX
callbacks, or system workqueue. A disconnect request retains its connection
reference until completion; only the actual disconnection event releases the
management session. A failed disconnect retains the first lifecycle error and
requires reboot. After terminal output failure, its subscription cannot reopen
on the closing link; reconnect before starting another terminal session.
Explicit on/off commands wait for their
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
Controller loss during an active BLE session, radio recovery, sustained Bluetooth throughput, pairing,
controller input, and behavior under combined application traffic remain unverified.

The management adapter accepts incoming LE peripheral-role links only. Outgoing
central-role connection events do not acquire or release management, change its
profile, or consume its advertisement. Recycled connection objects restart
advertising only when no management owner and no active advertisement remain.
Pending disconnects retain each distinct connection separately, so a slow HCI
command cannot discard another rejected client's cleanup. These boundaries have
synthetic checks with multiple connection slots. The Bluetooth profile reserves
two slots: one management connection and one application peripheral. The full
Python profile reserves five to also accommodate Classic HubNetwork links.
Simultaneous physical BLE management/peripheral links remain an acceptance item.

The local H:4 close-path correction discards partial framing and releases pending
buffers even when board teardown reports an error. UART-emulator checks cover
reopening after partial RX/TX and teardown failure. This does not make a failed
controller ready or provide automatic recovery of the Bluetooth host stack.

### Application broadcast observation

With `CONFIG_BRICKPHYR_RADIO` and `CONFIG_BT_OBSERVER`,
`messaging.BLERadio(observe_channels=...)` and
the legacy hub `observe_channels` argument request passive scans on the existing
Zephyr radio. A single native observer owns the configured channels; its
application-visible samples are copied from manufacturer data with Pybricks'
company ID and channel byte. Typed data is limited to 26 bytes. `observe()`
returns a decoded scalar or tuple, or `None` when no sample arrived within one
second. `signal_strength()` reports the reference 512 ms filtered RSSI, with
`-128` for no current sample. Repeated equal packets refresh presence. An explicit
close, finalizer or supervised application stop retires the observer; radio HCI
start/stop remains on the existing radio workqueue. This observation does not
acquire another management connection. `BLERadio.version()` returns the loaded TI
service pack's `# Version` value (`12_26.24` in the pinned artifact), cached during
validated controller setup. It requires the radio to be on and sends no new HCI
command. Broadcasting remains unfinished and raises `NotImplementedError`.
Protected ARM checks cover observation values, expiry and lifecycle through a
synthetic scan callback, and check the version against the actual embedded script.
On one Inventor F4, a synthetic Linux advertiser using the reference 30 ms interval
also verifies typed reception, refresh from repeated packets, RSSI, expiry and
version queries through USB and BLE management. The USB check includes ongoing
hub advertising. Management advertising explicitly uses the hub identity so the
legacy scanner selects the same address type at cold start. Actual Pybricks
transmitters, congested radio conditions and sustained-load behavior remain to be
checked.

For a peer broadcasting on channel 7, run this through either application
transport; reception itself does not require a connection to that peer:

```python
from pybricks.messaging import BLERadio
from pybricks.tools import wait

radio = BLERadio(observe_channels=(7,))
while True:
    print(radio.observe(7), radio.signal_strength(7))
    wait(200)
```

### Application LEGO devices

`CONFIG_BRICKPHYR_BLE_PEER` enables one application peripheral through Zephyr's
central role and GATT client. LEGO Wireless Protocol v3 and Xbox HID share this
connection owner. The native `bp_ble_peer_*` lifecycle, `bp_lwp3_*` operations and Python
LEGO device classes share this service, including device setup, command packets
and sensor conversion. Application
loading and terminal commands remain available through either USB or BLE;
an outgoing LEGO link does not acquire computer management or change its profile.

For a Powered Up Remote in discovery mode, run a Python application such as:

```python
from pybricks.pupdevices import Remote
from pybricks.tools import wait

remote = Remote()  # Optional name prefix and scan timeout, default 10000 ms.
for _ in range(20):
    print(remote.buttons.pressed())
    wait(250)
remote.disconnect()
```

`Remote` provides the reference button set, color light, name and
connect/disconnect operations. `LWP3Device` provides raw messages, an optional
pairing request and a bounded notification queue. Messages are 3–20 bytes and
must carry their length in the first byte. `read()` returns the oldest message
or `None`; a full queue drops the oldest message. Notification capacity is
1–255 messages (a Python request of zero uses one). Remote button snapshots are
independent of this queue. Names and name filters are limited to 14 UTF-8 bytes.
Writes, name changes, connection and disconnection work synchronously or through
the reference awaitable interface; construction is blocking.

The same service provides these `pupdevices` profiles:

| Class | Implemented operations |
| --- | --- |
| `TechnicMoveHub` | `drive(speed, steering)`, connection and disconnection |
| `MarioHub` | Mario/Luigi/Peach discovery, `hsv()`, `color()`, `detectable_colors()`, connection and disconnection |
| `DuploTrain` | Both train generations: `drive(power)`, `headlights(color)`, `sound(sound)`, `speed()`, `color()`, connection and disconnection |

Constructors accept the same `name`, `timeout` and `connect` options as `Remote`.
Mario/Technic Move and the newer blue train request pairing; the older black
train and Remote do not. Native callers select the corresponding
`bp_ble_peer_config.lwp3.profile` with protocol `BP_BLE_LWP3`; raw mode instead
uses the caller's `kind` and pairing flag. `bp_lwp3_command()` uses the same
command encoding as Python. `bp_ble_peer_get()` returns connection status and
the protocol's typed snapshot. This replaces the earlier experimental
`bp_lwp3_open/connect/disconnect/get/close` SDK calls; rebuild native applications
against the matching SDK.

Sensor reads raise `OSError(EAGAIN)` until a valid notification arrives after
connection. Native snapshots expose validity flags and monotonic receipt times;
raw mode leaves interpretation to the caller. Reconnection invalidates old
samples. Mario's palette survives reconnection, including an empty palette for
which `color()` returns `None`. The older train uses the reference RGB classifier;
the newer train maps color tags and returns `Color.NONE` for an unknown tag.
Train speed preserves the signed protocol value without conversion to a physical
distance unit.

Technic Move retains the reference suppression of redundant commands: at most
two acknowledged copies of an unchanged drive command within 500 ms of the last
acknowledgment. Failed writes do not count; an ordinary ATT rejection preserves
the connection so the application can retry. Train sound names are `brake`,
`depart`, `water`, `horn` and `steam`; other names raise `ValueError` before sending.
The reference implementation has only one known sound packet for the newer blue
train, so its five names currently produce that same sound.

Discovery shares the existing radio worker and scanner. It temporarily requests
active scanning for the LEGO service, hub kind and name, matching scan responses
by address. Explicit scanning pauses during connection creation; passive broadcast
observation resumes afterwards. Scan timeout is configurable, including `None`
for cancellable indefinite scanning. Connection and pairing each have a five-second
bound; ATT operations use the protocol's 30-second transaction bound.

Closing or finalizing a device releases its claim. A matching, idle connection
can be reused within the same application. Explicit `disconnect()` and supervised
application cleanup end the physical link. Discovery, ATT parameters, outgoing
bytes and notification buffers belong to the kernel, so late callbacks cannot
access an unloaded interpreter. A new claim is rejected as busy until pending
disconnection cleanup finishes, preventing early reuse of its buffers.

The F4 profile enables optional pairing, including LE legacy pairing for older
LEGO devices. Bond keys stay in RAM; the base BLE profile reserves two records,
and the full Python profile reserves five for its additional Classic links.
Unused records can be replaced when full. Pairing without a passkey does not
authenticate the operator. Builds without the required central/GATT service
return an unsupported error before starting device work.

Protected ARM checks use synthetic BLE peers to exercise reference Remote
messages, cached buttons, raw queue overflow, name changes, link reuse,
blocking/awaitable operations, scan cancellation and delayed-write cleanup after
Python stop. Additional cases exercise Technic Move commands and retry suppression,
Mario/Luigi/Peach matching and palettes, both train formats, invalid samples and
reconnection. These establish software behavior only. Physical LEGO device
connection, pairing, sensor/output behavior, radio timing and coexistence with
management and broadcasts remain unverified.

### Application Xbox controller

`iodevices.XboxController` uses the same protected BLE peer and radio worker as
LEGO devices. A program can own one of these peripherals while the computer
retains its USB or BLE management connection. Native callers select
`BP_BLE_XBOX`, set `xbox_deadzone`, read `bp_ble_peer_get().xbox` and send
`bp_xbox_rumble()` through that same service.

The Python interface provides `buttons.pressed()`, both joysticks, triggers,
direction pad, Elite profile, raw `state()`, name, connect/disconnect and rumble.
For a controller in pairing mode:

```python
import errno
from pybricks.iodevices import XboxController
from pybricks.tools import wait

controller = XboxController(joystick_deadzone=10)
print(controller.name())
while True:
    try:
        print(controller.joystick_left(), controller.buttons.pressed())
    except OSError as error:
        if error.args[0] != errno.EAGAIN:
            raise
    wait(100)
```

The current codec accepts BLE firmware-5.x input report 1 with 16-byte payloads,
or 19-byte Elite extensions. It does not implement Classic Bluetooth or older
report layouts. HID Report characteristics are selected by their Report Reference
descriptors, not fixed handles or discovery order; a service with more than eight
Report characteristics is unsupported. The input must support notifications and
the output must support acknowledged writes. The HID Report Map is not parsed;
other layouts require a reviewed codec extension. Pairing uses Zephyr security
and can reuse a RAM bond during the same boot. Persistent bonds remain absent.

Reads raise `OSError(EAGAIN)` until a complete input report arrives, and
`OSError(EPROTO)` after a malformed or unsupported report, until valid data
resumes. Reconnection invalidates earlier input. Snapshots carry monotonic receipt
time. Joysticks use percentages with positive X right and Y up and a square
deadzone; triggers use 0–100%. `state()` retains the reference diagnostic tuple
of twelve integers, with axes centered by subtracting 32767 and no Y inversion.
`profile()` requires the Elite extension and otherwise reports unsupported.

Rumble accepts the reference scalar or four-power tuple, with powers/count clamped
to 0–100 and duration/delay to 0–2500 ms. Zero duration, count or all-zero powers
send nothing. The protocol uses 10 ms ticks; positive durations below one tick
round up to one, other durations and delays round down. Completion acknowledges
the GATT write; the controller executes the effect in the background. Explicit
disconnect and application stop end the link; losing computer management does
not stop an autonomous application.

Protected ARM checks cover synthetic HID discovery, input and rumble through
Python and the native service, including malformed reports, bond reuse and stop
during a pending descriptor read. Physical controller connection, activation,
pairing/reconnection, input and rumble remain unverified. The wire codec uses
licensed SDL/Bluepad32 sources; Pybricks' non-free Xbox source is excluded. See
[licensing](licensing.md#xbox-controller-codec).

### Application hub network

`messaging.HubNetwork` uses Classic BR/EDR L2CAP alongside BLE management.
The full F4 Python profile enables `CONFIG_BRICKPHYR_HUB_NETWORK`, three network
peers, one BLE device and one computer connection. Reduced profiles omit Classic;
constructing `HubNetwork` there reports `OSError(ENOTSUP)`.

The service ports Pybricks' star routing and per-sender queues from
[`pb_type_hub_network.c`](https://github.com/alepoydes/pybricks-micropython/blob/a7b1b04ae4f8449dd46b3dfb97c420334439fd35/pybricks/messaging/pb_type_hub_network.c)
and its MIT-licensed Bluetooth adapter. Zephyr supplies the Classic stack;
BTstack is not linked. Addresses, PSM `0x1001`, the seven-byte routing header and
513-byte message limit match the reference. One coordinator connects to up to
three leaves and forwards messages between them; a leaf cannot start another
star. These are application links, not additional computer management sessions.

```python
from pybricks.messaging import HubNetwork

network = HubNetwork()
print(network.address())
# On the coordinator, substitute the address printed by another brick:
network.connect("00:16:53:12:34:56")
network.send((42, 1.5, True, None, b"data", "hello"))
print(network.inbox())
```

`connect` and `send` also work with the reference awaitable interface. Sending is
best effort, bounded to one second; success is not a delivery acknowledgment.
`inbox(latest=False)` drains a snapshot in receipt order. `latest=True` retains
only the newest message per sender. Each sender gets `inbox_size` bytes, default
1024 and minimum 525; overflow discards that sender's oldest messages. A busy
sender cannot consume another sender's queue. The coordinator allocates one
additional relay queue of the same size. Queues live in the kernel; the native
`bp_network_*` calls copy data and validate application buffers.
Creating inboxes initializes their metadata without clearing the entire backing
allocation under the interrupt lock; readers access only published messages.

Stopping Python or closing its network object cancels pending connection/send
requests and frees its queues. Established network links survive until remote
closure or radio shutdown; a new object gets empty inboxes. A frame already
handed to L2CAP cannot be recalled. Old callbacks retain their connection storage
until actual release and cannot complete a new lease's operation.

Protected ARM checks exercise both network roles, typed and maximum-size frames,
queue overflow/snapshots, forwarding, awaitable operations and late callbacks
through the real service and Python binding with synthetic Bluetooth endpoints.
They do not establish over-the-air interoperability. On one Inventor F4, a
synthetic Linux Classic peer also exercises incoming connection/pairing,
bidirectional 520-byte frames and retained links across Python stop/reload.
These checks pass with either USB or concurrent BLE management. They cover the
hub's leaf role, not physical coordinator routing or another Pybricks hub.

Classic channels require Zephyr security level 2. Pairing uses Just Works and
RAM-only link keys; reboot loses those keys. Pybricks interoperability, physical
coordinator/multi-peer behavior, sustained radio load and peak memory use remain
acceptance work. The additional Classic
Zephyr fixes are listed under [dependency prerequisites](upstream.md#classic-network-prerequisites).

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
