"""Parser and message definitions for the serial MCU protocol."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum


class MessageKind(str, Enum):
    READY = "READY"
    PONG = "PONG"
    OK_NEWGAME = "OK_NEWGAME"
    OK_POSITION = "OK_POSITION"
    BESTMOVE = "BESTMOVE"
    INFO = "INFO"
    ERROR = "ERROR"
    UNKNOWN = "UNKNOWN"


@dataclass(frozen=True, slots=True)
class ProtocolMessage:
    kind: MessageKind
    raw: str
    value: str = ""
    code: str = ""


def parse_message(line: str) -> ProtocolMessage | None:
    """Parse one protocol line. Empty lines are ignored; unknown lines are safe."""
    text = line.strip()
    if not text:
        return None

    if text.startswith("READY "):
        # The version and device name are preserved verbatim in value.
        return ProtocolMessage(MessageKind.READY, text, text[6:])
    if text == "PONG":
        return ProtocolMessage(MessageKind.PONG, text)
    if text == "OK NEWGAME":
        return ProtocolMessage(MessageKind.OK_NEWGAME, text)
    if text == "OK POSITION":
        return ProtocolMessage(MessageKind.OK_POSITION, text)
    if text.startswith("BESTMOVE "):
        return ProtocolMessage(MessageKind.BESTMOVE, text, text[9:].strip())
    if text.startswith("INFO "):
        return ProtocolMessage(MessageKind.INFO, text, text[5:])
    if text.startswith("#LOG "):
        return ProtocolMessage(MessageKind.INFO, text, text[5:])
    if text.startswith("ERROR "):
        parts = text.split(maxsplit=2)
        code = parts[1] if len(parts) > 1 else "UNKNOWN"
        detail = parts[2] if len(parts) > 2 else ""
        return ProtocolMessage(MessageKind.ERROR, text, detail, code)
    return ProtocolMessage(MessageKind.UNKNOWN, text, text)


def format_command(command: str, argument: str | None = None) -> bytes:
    """Encode one outgoing command as UTF-8 plus LF."""
    if "\n" in command or "\r" in command:
        raise ValueError("Command name must not contain line breaks")
    if argument is not None and ("\n" in argument or "\r" in argument):
        raise ValueError("Command argument must not contain line breaks")
    line = command if argument is None else f"{command} {argument}"
    return (line + "\n").encode("utf-8")
