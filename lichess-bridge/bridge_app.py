"""Application orchestration: serial MCU, Lichess events and Bot game stream."""

from __future__ import annotations

import asyncio
import logging
import time
from typing import Any
from urllib.parse import quote

import chess

from chess_controller import (
    ChessStateError,
    PendingMove,
    board_from_uci_moves,
    calculate_movetime_ms,
    should_start_thinking,
    validate_bestmove,
)
from config import BridgeConfig
from lichess_client import LichessClient, LichessError, LichessRateLimitError
from protocol import MessageKind, ProtocolMessage
from serial_client import SerialConnectionError, SerialTransport

LOG = logging.getLogger(__name__)

# Lichess uses the signed 32-bit maximum as a no-clock placeholder.
LICHESS_NO_CLOCK_SENTINEL_MS = 2_147_483_647


class BridgeApp:
    def __init__(self, config: BridgeConfig, serial: SerialTransport, lichess: LichessClient) -> None:
        self.config = config
        self.serial = serial
        self.lichess = lichess
        self._stop = asyncio.Event()
        self._active_game_id: str | None = None
        self._game_task: asyncio.Task[None] | None = None
        self._username = config.lichess.username
        self._device_name = "unknown"
        self._last_logged_moves: list[str] = []
        self._game_has_clock: bool | None = None

    async def run(self) -> None:
        await self.serial.connect()
        await self.serial.send("BOTMODE")
        ready = await self._wait_serial({MessageKind.READY}, self.config.timeouts.serial_ready_seconds)
        self._device_name = ready.value
        LOG.info("Microcontroller ready: %s", self._device_name)

        await self.serial.send("PING")
        await self._wait_serial({MessageKind.PONG}, self.config.timeouts.serial_command_seconds)
        LOG.info("Serial connection verified.")

        account = await self.lichess.get_account()
        account_name = str(account.get("username", ""))
        if account_name.casefold() != self._username.casefold():
            raise RuntimeError(
                f"Token belongs to {account_name!r}, but the configured username is {self._username!r}."
            )
        if account.get("title") != "BOT":
            raise RuntimeError(
                "The configured Lichess account does not have the BOT title. "
                "The Bot API can only be used with a BOT account."
            )
        LOG.info("Connected to Lichess: BOT @%s", account_name)

        if self.config.opponent.role == "CHALLENGE":
            await self._create_challenge()
        else:
            LOG.info("Role ACCEPT: waiting for a challenge from @%s", self.config.opponent.allowed_username)

        await self._event_loop()

    async def stop(self) -> None:
        self._stop.set()
        if self._game_task is not None:
            self._game_task.cancel()
            try:
                await self._game_task
            except asyncio.CancelledError:
                pass
        try:
            await self.serial.send("STOP")
        except Exception:
            LOG.debug("Could not send STOP during shutdown.", exc_info=True)
        try:
            await self.serial.send("EXITBOT")
        except Exception:
            LOG.debug("Could not send EXITBOT during shutdown.", exc_info=True)
        await self.serial.close()
        await self.lichess.close()

    async def _wait_serial(self, kinds: set[MessageKind], timeout: float) -> ProtocolMessage:
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        while True:
            remaining = deadline - loop.time()
            if remaining <= 0:
                raise TimeoutError(f"No serial response received (expected: {', '.join(k.value for k in kinds)}).")
            message = await self.serial.read_message(remaining)
            if message.kind == MessageKind.INFO:
                LOG.info("MCU: %s", message.value)
                continue
            if message.kind == MessageKind.ERROR:
                if message.code == "SERIAL_READ":
                    raise SerialConnectionError(f"Serial connection lost: {message.value}")
                raise RuntimeError(f"Microcontroller error {message.code}: {message.value}")
            if message.kind == MessageKind.UNKNOWN:
                LOG.warning("Ignoring non-protocol MCU output: %s", message.value)
                continue
            if message.kind in kinds:
                return message
            LOG.debug("Unexpected MCU response: %s", message.raw)

    async def _reconnect_serial(self, *, active_game: bool) -> None:
        """Reopen the serial port and re-handshake without touching Lichess state."""
        LOG.warning("Attempting to reconnect to the serial port.")
        last_error: Exception | None = None
        for attempt in range(1, 4):
            try:
                await self.serial.close()
                await asyncio.sleep(self.config.timeouts.reconnect_seconds)
                await self.serial.connect()
                await self.serial.send("BOTMODE")
                ready = await self._wait_serial(
                    {MessageKind.READY}, self.config.timeouts.serial_ready_seconds
                )
                self._device_name = ready.value
                await self.serial.send("PING")
                await self._wait_serial({MessageKind.PONG}, self.config.timeouts.serial_command_seconds)
                if active_game:
                    await self.serial.send("NEWGAME")
                    await self._wait_serial(
                        {MessageKind.OK_NEWGAME}, self.config.timeouts.serial_command_seconds
                    )
                LOG.info("Serial connection restored: %s", self._device_name)
                return
            except Exception as exc:
                last_error = exc
                LOG.error("Serial reconnection attempt %d/3 failed: %s", attempt, exc)
        raise SerialConnectionError(f"Could not restore the serial connection: {last_error}")

    async def _create_challenge(self) -> None:
        target = self.config.opponent.allowed_username
        result = await self.lichess.create_challenge(
            target,
            rated=self.config.game.rated,
            clock_limit_seconds=self.config.game.initial_time_seconds,
            clock_increment_seconds=self.config.game.increment_seconds,
            variant=self.config.game.variant,
        )
        challenge = result.get("challenge", result) if isinstance(result, dict) else {}
        challenge_id = challenge.get("id", "unknown") if isinstance(challenge, dict) else "unknown"
        url = challenge.get("url") if isinstance(challenge, dict) else None
        LOG.info("Created challenge for @%s (ID: %s).", target, challenge_id)
        if url:
            LOG.info("Challenge URL: %s", url)

    @staticmethod
    def _challenge_opponent(challenge: dict[str, Any]) -> str:
        challenger = challenge.get("challenger") or {}
        if not isinstance(challenger, dict):
            return ""
        return str(challenger.get("id") or challenger.get("name") or challenger.get("username") or "")

    @staticmethod
    def _challenge_variant(challenge: dict[str, Any]) -> str:
        variant = challenge.get("variant") or {}
        if isinstance(variant, dict):
            return str(variant.get("key") or variant.get("name") or "").lower()
        return str(variant).lower()

    def _should_accept_challenge(self, challenge: dict[str, Any]) -> tuple[bool, str]:
        challenger = self._challenge_opponent(challenge)
        if challenger.casefold() != self.config.opponent.allowed_username.casefold():
            return False, f"challenger @{challenger or 'unknown'} is not on the allowlist"
        if self._challenge_variant(challenge) != "standard":
            return False, "only standard chess is allowed"
        if self._active_game_id is not None:
            return False, "a game is already in progress"
        return True, "allowlisted opponent and standard chess"

    async def _event_loop(self) -> None:
        reconnect_delay = self.config.timeouts.reconnect_seconds
        while not self._stop.is_set():
            try:
                async for event in self.lichess.event_stream():
                    if self._stop.is_set():
                        return
                    reconnect_delay = self.config.timeouts.reconnect_seconds
                    await self._handle_event(event)
                if not self._stop.is_set():
                    LOG.warning("Lichess event stream closed; reconnecting.")
                    await asyncio.sleep(reconnect_delay)
                    reconnect_delay = min(60.0, max(1.0, reconnect_delay * 2))
            except asyncio.CancelledError:
                raise
            except LichessRateLimitError as exc:
                LOG.error("Lichess event stream rate-limited: %s; waiting at least 60 seconds.", exc)
                await asyncio.sleep(exc.retry_after_seconds)
            except Exception as exc:
                LOG.error("Lichess event stream interrupted: %s", exc)
                await asyncio.sleep(reconnect_delay)
                reconnect_delay = min(60.0, max(1.0, reconnect_delay * 2))

    async def _handle_event(self, event: dict[str, Any]) -> None:
        event_type = event.get("type")
        if event_type == "challenge":
            challenge = event.get("challenge") or {}
            if not isinstance(challenge, dict):
                return
            challenger = self._challenge_opponent(challenge)
            challenge_id = str(challenge.get("id", ""))
            accepted, reason = self._should_accept_challenge(challenge)
            if self.config.opponent.role != "ACCEPT":
                accepted = False
                reason = "dieser Standort ist im Modus CHALLENGE"
            if accepted:
                LOG.info("Received a challenge from @%s; accepting.", challenger)
                try:
                    await self.lichess.accept_challenge(challenge_id)
                except Exception as exc:
                    LOG.error("Could not accept challenge %s: %s", challenge_id, exc)
            else:
                LOG.info("Declining challenge from @%s: %s", challenger, reason)
                if challenge_id:
                    try:
                        await self.lichess.decline_challenge(challenge_id)
                    except Exception as exc:
                        LOG.warning("Could not decline challenge %s: %s", challenge_id, exc)
        elif event_type == "gameStart":
            game = event.get("game") or {}
            game_id = str(game.get("id", "")) if isinstance(game, dict) else ""
            if not game_id:
                LOG.warning("Received gameStart without a game ID.")
                return
            if self._active_game_id == game_id:
                return
            if self._active_game_id is not None:
                LOG.warning("Ignoring additional game %s; game %s is still active.", game_id, self._active_game_id)
                return
            self._active_game_id = game_id
            self._last_logged_moves = []
            self._game_has_clock = None
            game_url = f"https://lichess.org/{quote(game_id)}"
            LOG.info("Game started: %s", game_url)
            self._game_task = asyncio.create_task(self._run_game(game_id), name=f"lichess-game-{game_id}")
        elif event_type == "gameFinish":
            game = event.get("game") or {}
            game_id = str(game.get("id", "")) if isinstance(game, dict) else ""
            LOG.info("Received game-finished event: %s", game_id or "unknown")
        elif event_type in {"challengeCanceled", "challengeDeclined"}:
            LOG.info("Challenge event: %s", event_type)
        else:
            LOG.debug("Ignoring Lichess event: %s", event_type)

    def _configure_game_time_control(self, game_full: dict[str, Any]) -> bool:
        """Detect a real Lichess clock from gameFull metadata.

        Correspondence games have no classic clock: Lichess omits/nulls clock,
        sets speed to correspondence and may include daysPerTurn. Their
        gameState wtime/btime fields can contain INT32_MAX as a placeholder,
        not a real countdown.
        """
        speed = str(game_full.get("speed", "unknown")).lower()
        days_per_turn = game_full.get("daysPerTurn")
        clock = game_full.get("clock")

        initial = clock.get("initial") if isinstance(clock, dict) else None
        increment = clock.get("increment") if isinstance(clock, dict) else None

        valid_clock = (
            isinstance(initial, int)
            and not isinstance(initial, bool)
            and initial > 0
            and isinstance(increment, int)
            and not isinstance(increment, bool)
            and increment >= 0
        )
        has_classic_clock = (
            speed != "correspondence"
            and days_per_turn is None
            and valid_clock
        )
        self._game_has_clock = has_classic_clock

        if has_classic_clock:
            LOG.info(
                "Lichess time control: %d s initial time + %d s increment.",
                initial // 1000,
                increment // 1000,
            )
        else:
            details = [f"speed={speed}"]
            if days_per_turn is not None:
                details.append(f"daysPerTurn={days_per_turn}")
            if clock is None:
                details.append("clock=null/missing")
            elif not valid_clock:
                details.append(f"clock object is invalid or incomplete: {clock!r}")
            LOG.info(
                "No conventional chess clock detected (%s). "
                "Using the configured search time limit of %d ms for the MCU.",
                ", ".join(details),
                self.config.game.max_move_time_ms,
            )
        return has_classic_clock

    @staticmethod
    def _log_opponent_gone(event: dict[str, Any]) -> None:
        """Report opponent disconnect/reconnect information without claiming a result."""
        gone = event.get("gone")
        claim_in = event.get("claimWinInSeconds")
        if gone is True:
            if isinstance(claim_in, int) and not isinstance(claim_in, bool) and claim_in > 0:
                LOG.warning(
                    "Opponent is disconnected; according to Lichess, a win can be claimed in %d seconds.",
                    claim_in,
                )
            elif claim_in == 0:
                LOG.warning(
                    "Opponent is disconnected; Lichess reports that a win can now be claimed."
                )
            else:
                LOG.warning("Opponent is disconnected; Lichess did not provide a claim-win time.")
        elif gone is False:
            LOG.info("Opponent reconnected.")
        else:
            LOG.warning("Received invalid opponentGone event: %r", event)

    def _log_new_opponent_moves(self, moves_text: str, bot_color: chess.Color) -> None:
        """Log every newly observed opponent move once, in SAN and UCI notation."""
        moves = moves_text.split()
        previous = self._last_logged_moves
        if moves[: len(previous)] != previous:
            LOG.warning("Lichess move list was reset or changed; resetting move-log synchronization.")
            previous = []

        board = chess.Board()
        for index, token in enumerate(moves):
            try:
                move = chess.Move.from_uci(token)
            except ValueError as exc:
                raise RuntimeError(f"Invalid UCI move in game stream: {token!r}") from exc
            if move not in board.legal_moves:
                raise RuntimeError(f"Illegal UCI move in game stream: {token!r}")
            if index >= len(previous) and board.turn != bot_color:
                san = board.san(move)
                move_number = board.fullmove_number
                turn_suffix = "." if board.turn == chess.WHITE else "..."
                LOG.info("Opponent move: %d%s %s (%s).", move_number, turn_suffix, san, token)
            board.push(move)

        self._last_logged_moves = moves

    def _calculate_move_budget(self, state: dict[str, Any], bot_color: chess.Color) -> int:
        """Calculate and log a move budget from Lichess clock fields (milliseconds)."""
        if self._game_has_clock is False:
            LOG.info(
                "Game has no conventional clock: using the configured search time limit of %d ms.",
                self.config.game.max_move_time_ms,
            )
            return self.config.game.max_move_time_ms

        remaining_key = "wtime" if bot_color == chess.WHITE else "btime"
        increment_key = "winc" if bot_color == chess.WHITE else "binc"
        remaining_raw = state.get(remaining_key)
        increment_raw = state.get(increment_key)

        def valid_ms(value: Any) -> int | None:
            if isinstance(value, bool) or not isinstance(value, (int, float)):
                return None
            value = int(value)
            return value if value >= 0 else None

        remaining_ms = valid_ms(remaining_raw)
        increment_ms = valid_ms(increment_raw)

        if remaining_ms == LICHESS_NO_CLOCK_SENTINEL_MS:
            LOG.info(
                "Lichess clock field %s contains the no-clock placeholder %d; "
                "using the configured search time limit of %d ms.",
                remaining_key,
                LICHESS_NO_CLOCK_SENTINEL_MS,
                self.config.game.max_move_time_ms,
            )
            return self.config.game.max_move_time_ms
        if remaining_ms is None:
            LOG.warning(
                "Lichess clock field %s is missing or invalid (%r); using the configured maximum search budget of %d ms. "
                "Check whether the game has a clock.",
                remaining_key,
                remaining_raw,
                self.config.game.max_move_time_ms,
            )
        if increment_ms is None:
            LOG.warning(
                "Lichess increment field %s is missing or invalid (%r); treating the increment as 0 ms for this calculation.",
                increment_key,
                increment_raw,
            )
            increment_ms = 0

        movetime = calculate_movetime_ms(
            remaining_ms,
            increment_ms,
            moves_to_go=self.config.game.moves_to_go,
            safety_margin_ms=self.config.game.safety_margin_ms,
            factor=self.config.game.move_time_factor,
            min_ms=self.config.game.min_move_time_ms,
            max_ms=self.config.game.max_move_time_ms,
        )
        if remaining_ms is not None:
            LOG.info(
                "Lichess clock (%s): %.3f s remaining, increment %.3f s; "
                "search budget %d ms (safety margin %d ms, factor %.2f, max. %d ms).",
                "White" if bot_color == chess.WHITE else "Black",
                remaining_ms / 1000.0,
                increment_ms / 1000.0,
                movetime,
                self.config.game.safety_margin_ms,
                self.config.game.move_time_factor,
                self.config.game.max_move_time_ms,
            )
        else:
            LOG.info("Search budget without usable clock data: %d ms.", movetime)
        return movetime

    async def _run_game(self, game_id: str) -> None:
        pending: PendingMove | None = None
        thinking_moves: str | None = None
        bot_color: chess.Color | None = None
        reconnect_delay = self.config.timeouts.reconnect_seconds
        try:
            try:
                await self.serial.send("NEWGAME")
                await self._wait_serial({MessageKind.OK_NEWGAME}, self.config.timeouts.serial_command_seconds)
            except SerialConnectionError:
                await self._reconnect_serial(active_game=True)
            while not self._stop.is_set():
                try:
                    async for event in self.lichess.game_stream(game_id):
                        if self._stop.is_set():
                            return
                        reconnect_delay = self.config.timeouts.reconnect_seconds
                        event_type = event.get("type")
                        if event_type == "gameFull":
                            state = event.get("state") or {}
                            white = event.get("white") or {}
                            black = event.get("black") or {}
                            if str(white.get("id", "")).casefold() == self._username.casefold():
                                bot_color = chess.WHITE
                            elif str(black.get("id", "")).casefold() == self._username.casefold():
                                bot_color = chess.BLACK
                            else:
                                raise RuntimeError("Our bot is neither White nor Black in gameFull.")
                            LOG.info("Player color: %s", "White" if bot_color else "Black")
                            self._configure_game_time_control(event)
                            pending, thinking_moves = await self._process_game_state(
                                game_id, state, bot_color, pending, thinking_moves
                            )
                        elif event_type == "gameState":
                            if bot_color is None:
                                LOG.debug("Ignoring gameState received before gameFull.")
                                continue
                            pending, thinking_moves = await self._process_game_state(
                                game_id, event, bot_color, pending, thinking_moves
                            )
                        elif event_type == "opponentGone":
                            self._log_opponent_gone(event)
                        elif event_type == "chatLine":
                            LOG.debug("Game chat: %s", event.get("text", ""))
                    LOG.info("Game stream for %s closed.", game_id)
                    break
                except asyncio.CancelledError:
                    raise
                except LichessRateLimitError as exc:
                    LOG.error("Game stream rate-limited: %s; waiting at least 60 seconds.", exc)
                    await asyncio.sleep(exc.retry_after_seconds)
                except Exception as exc:
                    LOG.error("Game stream %s interrupted: %s", game_id, exc)
                    await asyncio.sleep(reconnect_delay)
                    reconnect_delay = min(60.0, max(1.0, reconnect_delay * 2))
            LOG.info("Game finished or stream closed: https://lichess.org/%s", game_id)
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            LOG.exception("Game processing for %s was aborted: %s", game_id, exc)
        finally:
            if self._active_game_id == game_id:
                self._active_game_id = None
            self._game_task = None

    async def _process_game_state(
        self,
        game_id: str,
        state: dict[str, Any],
        bot_color: chess.Color,
        pending: PendingMove | None,
        thinking_moves: str | None,
    ) -> tuple[PendingMove | None, str | None]:
        moves_text = str(state.get("moves", "")).strip()
        status = str(state.get("status", "started"))
        if status != "started":
            winner = state.get("winner")
            if winner == "white":
                result = "1-0"
            elif winner == "black":
                result = "0-1"
            elif status in {"draw", "stalemate", "repetition", "insufficientMaterial", "50move", "variantEnd"}:
                result = "1/2-1/2"
            else:
                result = "no result (e.g. aborted)"
            LOG.info("Game status: %s, result: %s", status, result)
            return None, thinking_moves

        if pending is not None and pending.is_confirmed_by(moves_text):
            LOG.info("Lichess confirmed our move %s.", pending.uci)
            pending = None
        elif pending is not None:
            LOG.debug("Waiting for confirmation of the move already submitted: %s.", pending.uci)
            return pending, thinking_moves

        try:
            board = board_from_uci_moves(moves_text)
        except ChessStateError as exc:
            raise RuntimeError(f"Invalid game state received from Lichess: {exc}") from exc

        self._log_new_opponent_moves(moves_text, bot_color)

        if board.is_game_over(claim_draw=False):
            LOG.info("Position is terminal: %s", board.result(claim_draw=False))
            return pending, thinking_moves

        if board.turn != bot_color:
            LOG.debug("Opponent to move; move list: %s", moves_text or "starting position")
            return pending, thinking_moves

        if not should_start_thinking(
            current_moves=moves_text,
            bot_to_move=(board.turn == bot_color),
            pending=pending,
            thinking_moves=thinking_moves,
        ):
            return pending, thinking_moves

        thinking_moves = moves_text
        movetime = self._calculate_move_budget(state, bot_color)
        LOG.info(
            "MCU calculating move; move number %d, time budget %d ms.",
            board.fullmove_number,
            movetime,
        )

        response: ProtocolMessage | None = None
        for attempt in range(2):
            try:
                await self.serial.send("POSITION", board.fen())
                await self._wait_serial({MessageKind.OK_POSITION}, self.config.timeouts.serial_command_seconds)
                search_started = time.monotonic()
                await self.serial.send("GO", str(movetime))
                response = await self._wait_serial(
                    {MessageKind.BESTMOVE},
                    (movetime / 1000.0) + self.config.timeouts.move_extra_seconds,
                )
                break
            except SerialConnectionError:
                if attempt >= 1:
                    raise
                LOG.warning("Serial connection lost while calculating a move; reinitializing the MCU.")
                await self._reconnect_serial(active_game=True)
            except TimeoutError:
                try:
                    await self.serial.send("STOP")
                except Exception:
                    LOG.warning("Could not send STOP after the timeout.")
                LOG.error("MCU did not return BESTMOVE within the time limit; move will not be submitted.")
                return pending, thinking_moves
        if response is None:
            LOG.error("No usable BESTMOVE response received.")
            return pending, thinking_moves

        try:
            move = validate_bestmove(board, response.value)
        except ChessStateError as exc:
            LOG.error("Rejected BESTMOVE: %s", exc)
            return pending, thinking_moves

        uci = move.uci()
        san = board.san(move)
        elapsed_ms = int((time.monotonic() - search_started) * 1000)
        LOG.info("Calculation finished: %d ms; move %s (%s).", elapsed_ms, uci, san)
        pending = PendingMove(moves_text, uci)
        try:
            await self.lichess.make_move(game_id, uci)
            LOG.info("Move submitted: %s", uci)
        except LichessError as exc:
            LOG.error(
                "Move submission status is unclear or failed (%s). Do not submit the same move again; "
                "waiting for the game stream to resolve the status.", exc
            )
        return pending, thinking_moves
