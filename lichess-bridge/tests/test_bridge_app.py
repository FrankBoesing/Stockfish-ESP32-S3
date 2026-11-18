import asyncio

import chess

from bridge_app import BridgeApp
from config import (
    BridgeConfig,
    GameConfig,
    LichessConfig,
    LoggingConfig,
    OpponentConfig,
    SerialConfig,
    TimeoutConfig,
)
from chess_controller import PendingMove
from serial_client import SimulatedSerialClient


class FakeLichess:
    def __init__(self):
        self.moves = []

    async def make_move(self, game_id, uci):
        self.moves.append((game_id, uci))


def make_config():
    return BridgeConfig(
        lichess=LichessConfig("BotA", "TOKEN"),
        opponent=OpponentConfig("BotB", "ACCEPT"),
        serial=SerialConfig("SIM", 115200),
        game=GameConfig(max_move_time_ms=20000, min_move_time_ms=100, safety_margin_ms=1800),
        timeouts=TimeoutConfig(serial_command_seconds=1, move_extra_seconds=1),
        logging=LoggingConfig("ERROR"),
    )


def test_only_allowlisted_standard_challenge_is_accepted():
    app = BridgeApp(make_config(), SimulatedSerialClient(), FakeLichess())
    allowed = {"challenger": {"id": "BotB"}, "variant": {"key": "standard"}}
    other_user = {"challenger": {"id": "NotBotB"}, "variant": {"key": "standard"}}
    other_variant = {"challenger": {"id": "BotB"}, "variant": {"key": "chess960"}}
    assert app._should_accept_challenge(allowed)[0]
    assert not app._should_accept_challenge(other_user)[0]
    assert not app._should_accept_challenge(other_variant)[0]


def test_game_state_submits_only_one_legal_move_for_bot_turn():
    async def scenario():
        device = SimulatedSerialClient(seed=3)
        await device.connect()
        fake_api = FakeLichess()
        app = BridgeApp(make_config(), device, fake_api)
        initial = {"status": "started", "moves": "", "wtime": 60000, "btime": 60000, "winc": 0, "binc": 0}
        pending, thinking = await app._process_game_state("game1", initial, chess.WHITE, None, None)
        assert isinstance(pending, PendingMove)
        assert len(fake_api.moves) == 1
        pending2, thinking2 = await app._process_game_state("game1", initial, chess.WHITE, pending, thinking)
        assert pending2 == pending
        assert len(fake_api.moves) == 1
        await device.close()

    asyncio.run(scenario())



def test_opponent_moves_are_logged_once_in_san_and_uci(caplog):
    import logging

    app = BridgeApp(make_config(), SimulatedSerialClient(), FakeLichess())
    app._log_new_opponent_moves("e2e4", chess.WHITE)
    assert "Gegnerzug" not in caplog.text

    with caplog.at_level(logging.INFO):
        app._log_new_opponent_moves("e2e4 e7e5", chess.WHITE)
        app._log_new_opponent_moves("e2e4 e7e5 g1f3", chess.WHITE)

    assert "Gegnerzug: 1... e5 (e7e5)." in caplog.text
    assert "Gegnerzug: 2... " not in caplog.text
    assert caplog.text.count("Gegnerzug:") == 1


def test_opponent_gone_logs_claim_delay_and_reconnect(caplog):
    import logging

    with caplog.at_level(logging.WARNING):
        BridgeApp._log_opponent_gone({"type": "opponentGone", "gone": True, "claimWinInSeconds": 51})
    assert "in 51 Sekunden" in caplog.text

    caplog.clear()
    with caplog.at_level(logging.INFO):
        BridgeApp._log_opponent_gone({"type": "opponentGone", "gone": False})
    assert "wieder verbunden" in caplog.text


def test_clock_budget_uses_lichess_millisecond_fields(caplog):
    import logging

    app = BridgeApp(make_config(), SimulatedSerialClient(), FakeLichess())
    state = {"wtime": 300000, "btime": 300000, "winc": 3000, "binc": 3000}

    with caplog.at_level(logging.INFO):
        budget = app._calculate_move_budget(state, chess.WHITE)

    assert budget == 10999
    assert "300.000 s verbleibend" in caplog.text
    assert "Inkrement 3.000 s" in caplog.text
    assert "Suchbudget 10999 ms" in caplog.text


def test_missing_clock_is_explicitly_reported(caplog):
    import logging

    app = BridgeApp(make_config(), SimulatedSerialClient(), FakeLichess())
    with caplog.at_level(logging.WARNING):
        budget = app._calculate_move_budget({}, chess.WHITE)
    assert budget == app.config.game.max_move_time_ms
    assert "wtime fehlt/ist ungültig" in caplog.text
