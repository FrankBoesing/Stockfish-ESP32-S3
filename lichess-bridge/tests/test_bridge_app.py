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
        game=GameConfig(max_move_time_ms=50, min_move_time_ms=1, safety_margin_ms=0),
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
