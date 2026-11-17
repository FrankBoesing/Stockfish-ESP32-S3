"""TOML configuration loading and validation."""

from __future__ import annotations

import os
import tomllib
from dataclasses import dataclass
from pathlib import Path


class ConfigError(ValueError):
    pass


@dataclass(frozen=True, slots=True)
class LichessConfig:
    username: str
    token_env: str
    base_url: str = "https://lichess.org"


@dataclass(frozen=True, slots=True)
class OpponentConfig:
    allowed_username: str
    role: str


@dataclass(frozen=True, slots=True)
class SerialConfig:
    port: str
    baudrate: int = 115200


@dataclass(frozen=True, slots=True)
class GameConfig:
    variant: str = "standard"
    rated: bool = False
    initial_time_seconds: int = 300
    increment_seconds: int = 3
    moves_to_go: int = 30
    safety_margin_ms: int = 1800
    move_time_factor: float = 0.85
    min_move_time_ms: int = 100
    max_move_time_ms: int = 20000


@dataclass(frozen=True, slots=True)
class TimeoutConfig:
    serial_ready_seconds: float = 12.0
    serial_command_seconds: float = 8.0
    move_extra_seconds: float = 5.0
    reconnect_seconds: float = 3.0
    api_retries: int = 2


@dataclass(frozen=True, slots=True)
class LoggingConfig:
    level: str = "INFO"


@dataclass(frozen=True, slots=True)
class BridgeConfig:
    lichess: LichessConfig
    opponent: OpponentConfig
    serial: SerialConfig
    game: GameConfig
    timeouts: TimeoutConfig
    logging: LoggingConfig

    @property
    def token(self) -> str:
        value = os.getenv(self.lichess.token_env, "").strip()
        if not value:
            raise ConfigError(
                f"Die Umgebungsvariable {self.lichess.token_env!r} ist nicht gesetzt."
            )
        return value


def _section(raw: dict, name: str) -> dict:
    value = raw.get(name, {})
    if not isinstance(value, dict):
        raise ConfigError(f"[{name}] muss ein TOML-Tabellenabschnitt sein.")
    return value


def _required(section: dict, key: str, section_name: str) -> str:
    value = section.get(key)
    if not isinstance(value, str) or not value.strip():
        raise ConfigError(f"[{section_name}].{key} muss als nichtleerer Text angegeben werden.")
    return value.strip()


def load_config(path: str | Path) -> BridgeConfig:
    config_path = Path(path)
    if not config_path.is_file():
        raise ConfigError(
            f"Konfigurationsdatei nicht gefunden: {config_path}. "
            "Kopiere config.example.toml nach config.toml."
        )

    try:
        with config_path.open("rb") as handle:
            raw = tomllib.load(handle)
    except tomllib.TOMLDecodeError as exc:
        raise ConfigError(f"Ungültige TOML-Konfiguration: {exc}") from exc

    if not isinstance(raw, dict):
        raise ConfigError("Die Konfigurationsdatei muss eine TOML-Tabelle enthalten.")
    lichess_raw = _section(raw, "lichess")
    opponent_raw = _section(raw, "opponent")
    serial_raw = _section(raw, "serial")
    game_raw = _section(raw, "game")
    timeout_raw = _section(raw, "timeouts")
    logging_raw = _section(raw, "logging")

    rated_raw = game_raw.get("rated", False)
    if not isinstance(rated_raw, bool):
        raise ConfigError("[game].rated muss true oder false sein.")

    lichess = LichessConfig(
        username=_required(lichess_raw, "username", "lichess"),
        token_env=str(lichess_raw.get("token_env", "LICHESS_TOKEN")).strip(),
        base_url=str(lichess_raw.get("base_url", "https://lichess.org")).rstrip("/"),
    )
    opponent = OpponentConfig(
        allowed_username=_required(opponent_raw, "allowed_username", "opponent"),
        role=str(opponent_raw.get("role", "CHALLENGE")).strip().upper(),
    )
    serial = SerialConfig(
        port=str(serial_raw.get("port", "")).strip(),
        baudrate=int(serial_raw.get("baudrate", 115200)),
    )
    game = GameConfig(
        variant=str(game_raw.get("variant", "standard")).strip().lower(),
        rated=rated_raw,
        initial_time_seconds=int(game_raw.get("initial_time_seconds", 300)),
        increment_seconds=int(game_raw.get("increment_seconds", 3)),
        moves_to_go=int(game_raw.get("moves_to_go", 30)),
        safety_margin_ms=int(game_raw.get("safety_margin_ms", 1800)),
        move_time_factor=float(game_raw.get("move_time_factor", 0.85)),
        min_move_time_ms=int(game_raw.get("min_move_time_ms", 100)),
        max_move_time_ms=int(game_raw.get("max_move_time_ms", 20000)),
    )
    timeouts = TimeoutConfig(
        serial_ready_seconds=float(timeout_raw.get("serial_ready_seconds", 12)),
        serial_command_seconds=float(timeout_raw.get("serial_command_seconds", 8)),
        move_extra_seconds=float(timeout_raw.get("move_extra_seconds", 5)),
        reconnect_seconds=float(timeout_raw.get("reconnect_seconds", 3)),
        api_retries=int(timeout_raw.get("api_retries", 2)),
    )
    logging = LoggingConfig(level=str(logging_raw.get("level", "INFO")).upper())

    if not lichess.token_env:
        raise ConfigError("[lichess].token_env darf nicht leer sein.")
    if not lichess.base_url.startswith(("https://", "http://")):
        raise ConfigError("[lichess].base_url muss eine HTTP(S)-URL sein.")
    if opponent.role not in {"CHALLENGE", "ACCEPT"}:
        raise ConfigError("[opponent].role muss CHALLENGE oder ACCEPT sein.")
    if game.variant != "standard":
        raise ConfigError("Version 1 unterstützt ausschließlich [game].variant = 'standard'.")
    if game.initial_time_seconds < 1 or game.increment_seconds < 0:
        raise ConfigError("Bedenkzeit muss positiv und Inkrement darf nicht negativ sein.")
    if game.moves_to_go < 1:
        raise ConfigError("[game].moves_to_go muss mindestens 1 sein.")
    if game.safety_margin_ms < 0:
        raise ConfigError("[game].safety_margin_ms darf nicht negativ sein.")
    if not 0 < game.move_time_factor <= 1:
        raise ConfigError("[game].move_time_factor muss größer 0 und höchstens 1 sein.")
    if game.min_move_time_ms < 1 or game.max_move_time_ms < game.min_move_time_ms:
        raise ConfigError("Ungültige min_move_time_ms/max_move_time_ms-Konfiguration.")
    if serial.baudrate < 1200:
        raise ConfigError("[serial].baudrate ist unplausibel niedrig.")
    if timeouts.serial_ready_seconds <= 0 or timeouts.serial_command_seconds <= 0:
        raise ConfigError("Serielle Timeouts müssen größer 0 sein.")
    if timeouts.move_extra_seconds < 0 or timeouts.reconnect_seconds < 0:
        raise ConfigError("Move- und Reconnect-Timeouts dürfen nicht negativ sein.")
    if timeouts.api_retries < 0:
        raise ConfigError("[timeouts].api_retries darf nicht negativ sein.")
    if logging.level not in {"DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"}:
        raise ConfigError("[logging].level muss DEBUG, INFO, WARNING, ERROR oder CRITICAL sein.")

    return BridgeConfig(lichess, opponent, serial, game, timeouts, logging)
