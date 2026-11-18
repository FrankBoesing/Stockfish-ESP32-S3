"""Command line entry point for Microcontroller Chess – Lichess Bridge."""

from __future__ import annotations

import argparse
import asyncio
import logging
import random
import sys

import chess

from bridge_app import BridgeApp
from config import ConfigError, load_config
from lichess_client import LichessClient
from serial_client import SerialClient, SimulatedSerialClient


def list_ports() -> int:
    try:
        from serial.tools import list_ports as serial_list_ports  # type: ignore[import-not-found]
    except ImportError:
        print("pySerial is not installed. Install it with: python -m pip install pyserial", file=sys.stderr)
        return 2
    ports = list(serial_list_ports.comports())
    if not ports:
        print("No serial ports found.")
        return 0
    for port in ports:
        description = port.description or "Unknown device"
        print(f"{port.device:24} {description}")
    return 0


async def offline_simulation() -> int:
    """Exercise BOTMODE/PING/POSITION/GO without hardware or a Lichess account."""
    device = SimulatedSerialClient(seed=17)
    await device.connect()
    await device.send("BOTMODE")
    ready = await device.read_message(1.0)
    print(f"MCU: {ready.raw}")
    await device.send("PING")
    pong = await device.read_message(1.0)
    print(f"MCU: {pong.raw}")

    board = chess.Board()
    rng = random.Random(17)
    print("Offline simulation: requesting 8 plies through the serial protocol.")
    for ply in range(8):
        fen = board.fen()
        await device.send("POSITION", fen)
        ack = await device.read_message(1.0)
        if ack.raw != "OK POSITION":
            raise RuntimeError(f"Position was not acknowledged: {ack.raw}")
        await device.send("GO", "100")
        bestmove = await device.read_message(1.0)
        move = chess.Move.from_uci(bestmove.value)
        if move not in board.legal_moves:
            raise RuntimeError(f"Simulation returned an illegal move: {bestmove.value}")
        san = board.san(move)
        board.push(move)
        print(f"{ply + 1:2}: {bestmove.value:5} ({san})  FEN={board.fen()}")
    print("Offline simulation completed successfully.")
    await device.close()
    return 0


async def run_bridge(args: argparse.Namespace) -> int:
    config = load_config(args.config)
    logging.basicConfig(
        level=getattr(logging, config.logging.level),
        format="%(asctime)s %(levelname)-8s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )
    if args.simulate:
        serial = SimulatedSerialClient()
    else:
        if not config.serial.port:
            raise ConfigError("[serial].port is not set. Use --simulate to run without hardware.")
        serial = SerialClient(config.serial.port, config.serial.baudrate)

    client = LichessClient(
        config.token,
        base_url=config.lichess.base_url,
        retries=config.timeouts.api_retries,
    )
    app = BridgeApp(config, serial, client)
    try:
        await app.run()
    except KeyboardInterrupt:
        logging.getLogger(__name__).info("Interrupted by user.")
    finally:
        await app.stop()
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Bridge between a microcontroller chess computer and the Lichess Bot API"
    )
    parser.add_argument("--config", default="config.toml", help="Path to the TOML configuration file (default: config.toml)")
    parser.add_argument("--list-ports", action="store_true", help="List available serial ports")
    parser.add_argument("--validate-config", action="store_true", help="Validate the configuration without opening a connection")
    parser.add_argument("--simulate", action="store_true", help="Simulate the microcontroller; keep the Lichess connection real")
    parser.add_argument("--offline-simulation", action="store_true", help="Run a local protocol simulation without Lichess or hardware")
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    if args.list_ports:
        return list_ports()
    if args.offline_simulation:
        try:
            return asyncio.run(offline_simulation())
        except Exception as exc:
            print(f"Offline simulation failed: {exc}", file=sys.stderr)
            return 1
    try:
        config = load_config(args.config)
        if args.validate_config:
            # Do not require an API token for a static configuration check.
            print(f"Configuration syntax and structure are valid: {args.config}")
            print(f"Role: {config.opponent.role}; Bot: {config.lichess.username}; Opponent: {config.opponent.allowed_username}")
            return 0
        return asyncio.run(run_bridge(args))
    except (ConfigError, RuntimeError, TimeoutError, KeyboardInterrupt) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
