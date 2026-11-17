import chess
import pytest

from chess_controller import (
    ChessStateError,
    PendingMove,
    board_from_uci_moves,
    calculate_movetime_ms,
    should_start_thinking,
    validate_bestmove,
)


def test_fen_and_uci_history():
    board = board_from_uci_moves("e2e4 e7e5 g1f3")
    assert board.turn == chess.BLACK
    assert len(board.fen().split()) == 6


def test_reject_illegal_history():
    with pytest.raises(ChessStateError):
        board_from_uci_moves("e2e5")


def test_bestmove_validation():
    board = chess.Board()
    assert validate_bestmove(board, "e2e4") == chess.Move.from_uci("e2e4")
    with pytest.raises(ChessStateError):
        validate_bestmove(board, "e2e5")


def test_promotion_uci_is_validated():
    board = chess.Board("7k/P7/8/8/8/8/8/7K w - - 0 1")
    assert validate_bestmove(board, "a7a8q").promotion == chess.QUEEN


def test_time_budget_is_bounded_and_reserves_margin():
    value = calculate_movetime_ms(30000, 2000, moves_to_go=20, safety_margin_ms=2000, min_ms=100, max_ms=10000)
    assert 100 <= value <= 10000
    assert calculate_movetime_ms(None, None, max_ms=777) == 777
    assert calculate_movetime_ms(500, 0, safety_margin_ms=1800) <= 1


def test_pending_move_confirmation_and_duplicate_guard():
    pending = PendingMove("e2e4", "e7e5")
    assert not pending.is_confirmed_by("e2e4")
    assert pending.is_confirmed_by("e2e4 e7e5")
    assert pending.is_confirmed_by("e2e4 e7e5 g1f3")
    assert should_start_thinking(current_moves="", bot_to_move=True, pending=None, thinking_moves=None)
    assert not should_start_thinking(current_moves="", bot_to_move=True, pending=None, thinking_moves="")
    assert not should_start_thinking(current_moves="", bot_to_move=True, pending=pending, thinking_moves=None)
