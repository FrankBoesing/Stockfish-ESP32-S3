"""Chess-state helpers, UCI validation, clock budgeting and duplicate protection."""

from __future__ import annotations

from dataclasses import dataclass

import chess


class ChessStateError(ValueError):
    pass


def board_from_uci_moves(moves: str) -> chess.Board:
    """Rebuild a standard chess board from Lichess' space-separated UCI moves."""
    board = chess.Board()
    for index, token in enumerate(moves.split(), start=1):
        try:
            move = chess.Move.from_uci(token)
        except ValueError as exc:
            raise ChessStateError(f"Invalid UCI move at index {index}: {token!r}") from exc
        if move not in board.legal_moves:
            raise ChessStateError(f"Illegal UCI move at index {index}: {token!r}")
        board.push(move)
    return board


def validate_bestmove(board: chess.Board, uci: str) -> chess.Move:
    candidate = uci.strip()
    if candidate == "0000":
        if board.legal_moves.count() == 0:
            raise ChessStateError("BESTMOVE 0000: the position has no legal moves.")
        raise ChessStateError("Microcontroller returned BESTMOVE 0000 even though legal moves exist.")
    try:
        move = chess.Move.from_uci(candidate)
    except ValueError as exc:
        raise ChessStateError(f"Microcontroller returned an invalid UCI format: {candidate!r}") from exc
    if move not in board.legal_moves:
        raise ChessStateError(f"Microcontroller returned an illegal move: {candidate}")
    return move


def calculate_movetime_ms(
    remaining_ms: int | None,
    increment_ms: int | None,
    *,
    moves_to_go: int = 30,
    safety_margin_ms: int = 1800,
    factor: float = 0.85,
    min_ms: int = 100,
    max_ms: int = 20000,
) -> int:
    """Conservative per-move time allocation for an increment clock.

    When clock data is missing, max_ms is returned. With clock data, the
    configured safety margin is reserved before dividing the remaining time.
    """
    if remaining_ms is None:
        return max_ms
    remaining = max(0, int(remaining_ms))
    increment = max(0, int(increment_ms or 0))
    usable = max(0, remaining - max(0, safety_margin_ms))
    estimate = ((usable / max(1, moves_to_go)) + increment) * factor
    # Never request more time than is currently available after the safety margin.
    hard_cap = max(1, usable)
    return max(1, min(max_ms, hard_cap, max(min_ms, int(estimate))))


@dataclass(slots=True)
class PendingMove:
    """A move sent to Lichess whose appearance in the game stream is pending."""
    before_moves: str
    uci: str

    def is_confirmed_by(self, current_moves: str) -> bool:
        expected = f"{self.before_moves} {self.uci}".strip()
        actual = current_moves.strip()
        return actual == expected or actual.startswith(expected + " ")


def should_start_thinking(
    *, current_moves: str, bot_to_move: bool, pending: PendingMove | None, thinking_moves: str | None
) -> bool:
    """Return true only once for a bot-to-move position without a pending move."""
    return bot_to_move and pending is None and thinking_moves != current_moves
