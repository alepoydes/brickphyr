# SPDX-License-Identifier: Apache-2.0
"""One management connection: shared serial framing or separate BLE characteristics."""

import asyncio
import contextlib
import os

import serial
from bleak import BleakClient, BleakScanner
from smp import header, packet
from smpclient import SMPClient
from smpclient.transport import SMP_CHARACTERISTIC_UUID, SMPTransportDisconnected
from usb.util import dispose_resources

from . import usb

SERVICE = "f3641400-00b0-4240-ba50-05ca45bf8abc"
TX = "f3641401-00b0-4240-ba50-05ca45bf8abc"
RX = "f3641402-00b0-4240-ba50-05ca45bf8abc"


class _Decoder:
    """Separate console bytes; let the SMP library decode length, Base64 and CRC."""

    def __init__(self, output, response):
        self.output, self.response = output, response
        self.limit = 384
        self.buffer = bytearray()
        self.decoder = None
        self.encoded = 0

    def feed(self, data):
        self.buffer.extend(data)
        while self.buffer:
            starts = [
                self.buffer.find(x) for x in (packet.START_DELIMITER, packet.CONTINUE_DELIMITER)
            ]
            start = min((x for x in starts if x >= 0), default=-1)
            if start != 0:
                end = start if start > 0 else len(self.buffer)
                if start < 0 and self.buffer[-1] in (
                    packet.START_DELIMITER[0],
                    packet.CONTINUE_DELIMITER[0],
                ):
                    end -= 1
                if not end:
                    return
                self.output(bytes(self.buffer[:end]))
                del self.buffer[:end]
                continue
            end = self.buffer.find(b"\n")
            if end < 0:
                if len(self.buffer) >= 128:
                    raise ValueError("SMP console line exceeds 127 bytes")
                return
            line = bytes(self.buffer[: end + 1])
            del self.buffer[: end + 1]
            if len(line) > 127:
                raise ValueError("SMP console line exceeds 127 bytes")
            if line.startswith(packet.START_DELIMITER):
                if self.decoder is not None:
                    raise ValueError("New SMP response before the previous one completed")
                self.decoder = packet.decode()
                next(self.decoder)
                self.encoded = 0
            elif self.decoder is None:
                raise ValueError("SMP continuation without a response")
            self.encoded += len(line) - 3
            if self.encoded > 4 * ((self.limit + 4 + 2) // 3):
                raise ValueError("SMP response exceeds the negotiated capacity")
            try:
                self.decoder.send(line)
            except StopIteration as result:
                self.decoder = None
                if len(result.value) > self.limit:
                    raise ValueError("SMP response exceeds the negotiated capacity")
                self.response(result.value)


class ConsoleTransport:
    """Public SMPTransport implementation; human input uses the same write lock."""

    def __init__(self, output, *, ble=None, port=None, serial_number=None, management=True):
        self.ble, self.port, self.serial_number = ble, port, serial_number
        self.output, self.management = output, management
        self.decoder = _Decoder(output, self._response)
        self.closed = asyncio.Event()
        self.error = None
        self.pending = None
        self.writes = asyncio.Lock()
        self.requests = asyncio.Lock()
        self.serial = self.radio = self.hub = None
        self.smp_characteristic = None
        self.smp_buffer = bytearray()
        self.loop = asyncio.get_running_loop()

    @property
    def mtu(self):
        return (
            self.smp_characteristic.max_write_without_response_size
            if self.smp_characteristic
            else 127
        )

    @property
    def max_unencoded_size(self):
        return self.decoder.limit if self.radio is not None else self.decoder.limit - 4

    def initialize(self, smp_server_transport_buffer_size):
        if not 16 <= smp_server_transport_buffer_size <= 65535:
            raise ValueError("Invalid SMP buffer capacity")
        self.decoder.limit = smp_server_transport_buffer_size

    def _fail(self, error):
        if self.error is None:
            self.error = error
        self.closed.set()
        if self.serial is not None and self.serial.is_open:
            self.loop.remove_reader(self.serial.fileno())
        if self.pending is not None and not self.pending.done():
            self.pending.set_result(self.error)

    def _response(self, data):
        if self.pending is None or self.pending.done():
            raise ValueError("Unexpected SMP response")
        self.pending.set_result(data)

    def _feed(self, data):
        if not self.closed.is_set():
            try:
                if self.management and self.radio is None:
                    self.decoder.feed(data)
                else:
                    self.output(data)
            except Exception as error:
                self._fail(error)

    def _feed_smp(self, data):
        if self.closed.is_set():
            return
        try:
            if self.pending is None or self.pending.done():
                raise ValueError("Unexpected SMP response")
            self.smp_buffer.extend(data)
            if len(self.smp_buffer) < header.Header.SIZE:
                return
            size = (
                header.Header.loads(self.smp_buffer[: header.Header.SIZE]).length
                + header.Header.SIZE
            )
            if size > self.decoder.limit or len(self.smp_buffer) > size:
                raise ValueError("SMP response exceeds the negotiated capacity or message length")
            if len(self.smp_buffer) == size:
                self._response(bytes(self.smp_buffer))
                self.smp_buffer.clear()
        except Exception as error:
            self._fail(error)

    def _read_serial(self):
        try:
            data = os.read(self.serial.fileno(), 1024)
            if not data:
                raise SMPTransportDisconnected("USB console disconnected")
            self._feed(data)
        except BlockingIOError:
            pass
        except Exception as error:
            self._fail(error)

    async def connect(self, address="", timeout_s=5):
        if self.serial is not None or self.radio is not None or self.closed.is_set():
            raise RuntimeError("A console transport can only be connected once")
        if self.ble:
            devices = await BleakScanner.discover(timeout=timeout_s)
            matches = [
                d
                for d in devices
                if d.address.casefold() == self.ble.casefold() or d.name == self.ble
            ]
            if len(matches) != 1:
                raise RuntimeError(f"Expected one {self.ble!r} hub, found {len(matches)}")
            established = False

            def disconnected(_):
                # Bleak can report a failed attempt and retry inside connect().
                # Only a loss after that operation succeeds ends our session.
                if established:
                    self._fail(SMPTransportDisconnected("BLE console disconnected"))

            self.radio = BleakClient(
                matches[0],
                timeout=timeout_s,
                disconnected_callback=disconnected,
            )
            await self.radio.connect()
            established = True
            if not self.radio.is_connected:
                disconnected(self.radio)
                raise self.error
            if self.radio.services.get_service(SERVICE) is None:
                raise RuntimeError("This hub does not provide the native Brickphyr terminal")
            await self.radio.start_notify(TX, lambda _, data: self._feed(data))
            if self.management:
                self.smp_characteristic = self.radio.services.get_characteristic(
                    SMP_CHARACTERISTIC_UUID
                )
                if self.smp_characteristic is None:
                    raise RuntimeError("This hub does not provide binary SMP over BLE")
                await self.radio.start_notify(
                    self.smp_characteristic, lambda _, data: self._feed_smp(data)
                )
            if self.closed.is_set():
                raise self.error or SMPTransportDisconnected("Console closed")
        else:
            port = self.port
            if port is None:
                self.hub = usb.device(self.serial_number)
                usb.select(self.hub, "shell")
                port = usb.serial_port(self.hub)
            self.serial = serial.Serial(port, 115200, timeout=0, write_timeout=0, exclusive=True)
            self.loop.add_reader(self.serial.fileno(), self._read_serial)

    async def disconnect(self):
        self.closed.set()
        first = None
        if self.serial is not None:
            try:
                if self.serial.is_open:
                    self.loop.remove_reader(self.serial.fileno())
                self.serial.close()
            except Exception as error:
                first = error
        if self.radio is not None:
            try:
                await self.radio.disconnect()
            except Exception as error:
                first = first or error
        if self.hub is not None:
            try:
                dispose_resources(self.hub)
            except Exception as error:
                first = first or error
        if self.pending is not None and not self.pending.done():
            self.pending.cancel()
        if first:
            raise first

    async def _write_ble(self, characteristic, data, *, response):
        offset = 0
        while offset < len(data):
            if self.closed.is_set():
                raise self.error or SMPTransportDisconnected("Console closed")
            capacity = min(512, characteristic.max_write_without_response_size)
            chunk = data[offset : offset + capacity]
            await self.radio.write_gatt_char(characteristic, chunk, response=response)
            offset += len(chunk)

    async def write(self, data):
        async with self.writes, asyncio.timeout(5):
            if self.radio is not None:
                # Keep acknowledged writes for human input so a full Shell RX
                # buffer is reported instead of silently losing keystrokes.
                await self._write_ble(
                    self.radio.services.get_characteristic(RX), data, response=True
                )
                return
            offset = 0
            while offset < len(data):
                if self.closed.is_set():
                    raise self.error or SMPTransportDisconnected("Console closed")
                try:
                    written = os.write(self.serial.fileno(), data[offset:])
                    if written == 0:
                        raise SMPTransportDisconnected("USB console stopped accepting input")
                    offset += written
                except BlockingIOError:
                    ready = self.loop.create_future()
                    fd = self.serial.fileno()
                    self.loop.add_writer(fd, lambda: not ready.done() and ready.set_result(None))
                    try:
                        await ready
                    finally:
                        self.loop.remove_writer(fd)

    async def send(self, data):
        if not self.management:
            raise RuntimeError("This console was opened without SMP management")
        if self.closed.is_set():
            raise self.error or SMPTransportDisconnected("Console closed")
        if self.pending is not None:
            raise RuntimeError("Only one SMP request may be pending")
        if len(data) > self.max_unencoded_size:
            raise ValueError("SMP request exceeds the negotiated capacity")
        self.pending = self.loop.create_future()
        if self.radio is not None:
            async with self.writes, asyncio.timeout(5):
                await self._write_ble(self.smp_characteristic, data, response=False)
        else:
            await self.write(b"".join(packet.encode(data)))

    async def receive(self):
        if self.pending is None:
            raise RuntimeError("No SMP request is pending")
        result = await self.pending
        self.pending = None
        if isinstance(result, Exception):
            raise result
        return result

    async def send_and_receive(self, data):
        # Input, output and telemetry share one SMP exchange. Cancellation while
        # waiting for admission must not invalidate somebody else's request.
        async with self.requests:
            try:
                await self.send(data)
                return await self.receive()
            except BaseException as error:
                # A timed-out/cancelled request can still receive a late reply.
                # Close admission; this stream must not be reused for another request.
                self._fail(
                    SMPTransportDisconnected("SMP exchange cancelled; reconnect the console")
                    if isinstance(error, asyncio.CancelledError)
                    else error
                )
                raise


@contextlib.asynccontextmanager
async def connection(output, *, management=False, **target):
    transport = ConsoleTransport(output, management=management, **target)
    client = SMPClient(transport, "native-console", timeout_s=5)
    try:
        if management:
            await client.connect()
        else:
            await transport.connect()
        yield transport, client
    except BaseException:
        with contextlib.suppress(Exception):
            await transport.disconnect()
        raise
    else:
        await transport.disconnect()
