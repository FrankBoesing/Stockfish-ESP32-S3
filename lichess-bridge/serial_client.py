"""Real and simulated serial transports implementing the bridge protocol."""

from __future__ import annotations

import asyncio
import logging
import queue
import random
import threading
from typing import Protocol

import chess

from protocol import MessageKind, ProtocolMessage, format_command, parse_message

LOG = logging.getLogger(__name__)


class SerialConnectionError(RuntimeError):
    """Raised when the physical serial connection is unavailable."""


class SerialTransport(Protocol):
    async def connect(self) -> None: ...
    async def send(self, command: str, argument: str | None = None) -> None: ...
    async def read_message(self, timeout: float) -> ProtocolMessage: ...
    async def close(self) -> None: ...


class SerialClient:
    """pySerial transport with a dedicated reader thread and asyncio queue."""

    def __init__(self, port: str, baudrate: int = 115200) -> None:
        self.port = port
        self.baudrate = baudrate
        self._serial = None
        self._queue: asyncio.Queue[ProtocolMessage] = asyncio.Queue()
        self._loop: asyncio.AbstractEventLoop | None = None
        self._reader_thread: threading.Thread | None = None
        self._stop = threading.Event()

    async def connect(self) -> None:
        try:
            import serial  # type: ignore[import-not-found]
        except ImportError as exc:
            raise RuntimeError("pySerial is missing. Install it with: python -m pip install pyserial") from exc
        self._loop = asyncio.get_running_loop()
        self._queue = asyncio.Queue()
        try:
            self._serial = serial.Serial(self.port, self.baudrate, timeout=0.2, write_timeout=3)
        except Exception as exc:  # pySerial raises platform-specific exception types.
            raise SerialConnectionError(f"Serial port {self.port!r} could not be opened: {exc}") from exc
        self._stop.clear()
        self._reader_thread = threading.Thread(target=self._reader, name="serial-reader", daemon=True)
        self._reader_thread.start()
        LOG.info("Opened serial port: %s @ %d baud", self.port, self.baudrate)

    def _reader(self) -> None:
        assert self._serial is not None
        assert self._loop is not None
        while not self._stop.is_set():
            try:
                raw = self._serial.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                message = parse_message(line)
                if message is not None:
                    self._loop.call_soon_threadsafe(self._queue.put_nowait, message)
            except Exception as exc:
                if not self._stop.is_set():
                    message = ProtocolMessage(MessageKind.ERROR, f"ERROR SERIAL_READ {exc}", str(exc), "SERIAL_READ")
                    self._loop.call_soon_threadsafe(self._queue.put_nowait, message)
                break

    async def send(self, command: str, argument: str | None = None) -> None:
        if self._serial is None or not self._serial.is_open:
            raise SerialConnectionError("Serial port is not open.")
        data = format_command(command, argument)
        try:
            await asyncio.to_thread(self._serial.write, data)
            await asyncio.to_thread(self._serial.flush)
        except Exception as exc:
            raise SerialConnectionError(f"Serial write error: {exc}") from exc

    async def read_message(self, timeout: float) -> ProtocolMessage:
        try:
            return await asyncio.wait_for(self._queue.get(), timeout=timeout)
        except asyncio.TimeoutError as exc:
            raise TimeoutError("Timed out waiting for a serial protocol response.") from exc

    async def close(self) -> None:
        self._stop.set()
        if self._serial is not None:
            try:
                await asyncio.to_thread(self._serial.close)
            except Exception:
                LOG.debug("Error while closing the serial port", exc_info=True)
        if self._reader_thread is not None:
            await asyncio.to_thread(self._reader_thread.join, 1.0)
        self._serial = None


class SimulatedSerialClient:
    """In-process MCU simulator. Returns legal random moves for supplied FENs."""

    def __init__(self, device_name: str = "SIMULATED-CHESS-MCU", seed: int = 7) -> None:
        self.device_name = device_name
        self._queue: asyncio.Queue[ProtocolMessage] = asyncio.Queue()
        self._rng = random.Random(seed)
        self._board = chess.Board()
        self._connected = False

    async def connect(self) -> None:
        self._queue = asyncio.Queue()
        self._connected = True
        LOG.info("Simulated microcontroller enabled: %s", self.device_name)

    async def send(self, command: str, argument: str | None = None) -> None:
        if not self._connected:
            raise RuntimeError("Simulation is not connected.")
        if command == "BOTMODE":
            await self._queue.put(parse_message(f"READY LICHESS-BRIDGE/1 {self.device_name}"))
        elif command == "PING":
            await self._queue.put(parse_message("PONG"))
        elif command == "NEWGAME":
            self._board = chess.Board()
            await self._queue.put(parse_message("OK NEWGAME"))
        elif command == "POSITION":
            try:
                self._board = chess.Board(argument or "")
                if not self._board.is_valid():
                    raise ValueError("FEN does not represent a valid chess position")
                await self._queue.put(parse_message("OK POSITION"))
            except Exception as exc:
                await self._queue.put(parse_message(f"ERROR BAD_FEN {exc}"))
        elif command == "GO":
            legal = list(self._board.legal_moves)
            if not legal:
                await self._queue.put(parse_message("BESTMOVE 0000"))
            else:
                move = self._rng.choice(legal)
                await self._queue.put(parse_message(f"BESTMOVE {move.uci()}"))
        elif command == "STOP":
            await self._queue.put(parse_message("INFO STOP acknowledged"))
        elif command == "EXITBOT":
            self._connected = False
        else:
            await self._queue.put(parse_message(f"ERROR UNKNOWN_COMMAND {command}"))

    async def read_message(self, timeout: float) -> ProtocolMessage:
        try:
            message = await asyncio.wait_for(self._queue.get(), timeout=timeout)
            if message is None:
                raise RuntimeError("Internal simulation error")
            return message
        except asyncio.TimeoutError as exc:
            raise TimeoutError("Timed out in the serial simulation.") from exc

    async def close(self) -> None:
        self._connected = False
