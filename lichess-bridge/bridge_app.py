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

    async def run(self) -> None:
        await self.serial.connect()
        await self.serial.send("BOTMODE")
        ready = await self._wait_serial({MessageKind.READY}, self.config.timeouts.serial_ready_seconds)
        self._device_name = ready.value
        LOG.info("Microcontroller bereit: %s", self._device_name)

        await self.serial.send("PING")
        await self._wait_serial({MessageKind.PONG}, self.config.timeouts.serial_command_seconds)
        LOG.info("Serielle Verbindung geprüft.")

        account = await self.lichess.get_account()
        account_name = str(account.get("username", ""))
        if account_name.casefold() != self._username.casefold():
            raise RuntimeError(
                f"Token gehört zu {account_name!r}, konfiguriert ist aber {self._username!r}."
            )
        if account.get("title") != "BOT":
            raise RuntimeError(
                "Das konfigurierte Lichess-Konto trägt nicht den BOT-Titel. "
                "Die Bot API kann nur mit einem Bot-Konto verwendet werden."
            )
        LOG.info("Lichess verbunden: BOT @%s", account_name)

        if self.config.opponent.role == "CHALLENGE":
            await self._create_challenge()
        else:
            LOG.info("Rolle ACCEPT: warte auf eine Challenge von @%s", self.config.opponent.allowed_username)

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
            LOG.debug("STOP konnte beim Beenden nicht gesendet werden.", exc_info=True)
        try:
            await self.serial.send("EXITBOT")
        except Exception:
            LOG.debug("EXITBOT konnte beim Beenden nicht gesendet werden.", exc_info=True)
        await self.serial.close()
        await self.lichess.close()

    async def _wait_serial(self, kinds: set[MessageKind], timeout: float) -> ProtocolMessage:
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        while True:
            remaining = deadline - loop.time()
            if remaining <= 0:
                raise TimeoutError(f"Keine serielle Antwort erhalten (erwartet: {', '.join(k.value for k in kinds)}).")
            message = await self.serial.read_message(remaining)
            if message.kind == MessageKind.INFO:
                LOG.info("MCU: %s", message.value)
                continue
            if message.kind == MessageKind.ERROR:
                if message.code == "SERIAL_READ":
                    raise SerialConnectionError(f"Serielle Verbindung verloren: {message.value}")
                raise RuntimeError(f"Microcontroller-Fehler {message.code}: {message.value}")
            if message.kind == MessageKind.UNKNOWN:
                LOG.warning("Ignoriere nichtprotokollkonforme MCU-Ausgabe: %s", message.value)
                continue
            if message.kind in kinds:
                return message
            LOG.debug("Unerwartete MCU-Antwort: %s", message.raw)

    async def _reconnect_serial(self, *, active_game: bool) -> None:
        """Reopen the serial port and re-handshake without touching Lichess state."""
        LOG.warning("Versuche serielle Verbindung wiederherzustellen.")
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
                LOG.info("Serielle Verbindung wiederhergestellt: %s", self._device_name)
                return
            except Exception as exc:
                last_error = exc
                LOG.error("Serielle Wiederverbindung %d/3 fehlgeschlagen: %s", attempt, exc)
        raise SerialConnectionError(f"Serielle Verbindung konnte nicht wiederhergestellt werden: {last_error}")

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
        LOG.info("Challenge an @%s erstellt (ID: %s).", target, challenge_id)
        if url:
            LOG.info("Challenge-Link: %s", url)

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
            return False, f"nicht freigegebener Herausforderer @{challenger or 'unbekannt'}"
        if self._challenge_variant(challenge) != "standard":
            return False, "nur Standard-Schach ist freigegeben"
        if self._active_game_id is not None:
            return False, "es läuft bereits eine Partie"
        return True, "freigegebener Gegner und Standard-Schach"

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
                    LOG.warning("Lichess Event-Stream wurde geschlossen; verbinde erneut.")
                    await asyncio.sleep(reconnect_delay)
                    reconnect_delay = min(60.0, max(1.0, reconnect_delay * 2))
            except asyncio.CancelledError:
                raise
            except LichessRateLimitError as exc:
                LOG.error("Lichess Event-Stream rate-limited: %s; warte mindestens 60 Sekunden.", exc)
                await asyncio.sleep(exc.retry_after_seconds)
            except Exception as exc:
                LOG.error("Lichess Event-Stream unterbrochen: %s", exc)
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
                LOG.info("Challenge von @%s erhalten; nehme an.", challenger)
                try:
                    await self.lichess.accept_challenge(challenge_id)
                except Exception as exc:
                    LOG.error("Challenge %s konnte nicht angenommen werden: %s", challenge_id, exc)
            else:
                LOG.info("Challenge von @%s wird abgelehnt: %s", challenger, reason)
                if challenge_id:
                    try:
                        await self.lichess.decline_challenge(challenge_id)
                    except Exception as exc:
                        LOG.warning("Challenge %s konnte nicht abgelehnt werden: %s", challenge_id, exc)
        elif event_type == "gameStart":
            game = event.get("game") or {}
            game_id = str(game.get("id", "")) if isinstance(game, dict) else ""
            if not game_id:
                LOG.warning("gameStart ohne Game-ID erhalten.")
                return
            if self._active_game_id == game_id:
                return
            if self._active_game_id is not None:
                LOG.warning("Ignoriere weiteres Spiel %s; %s ist noch aktiv.", game_id, self._active_game_id)
                return
            self._active_game_id = game_id
            game_url = f"https://lichess.org/{quote(game_id)}"
            LOG.info("Partie gestartet: %s", game_url)
            self._game_task = asyncio.create_task(self._run_game(game_id), name=f"lichess-game-{game_id}")
        elif event_type == "gameFinish":
            game = event.get("game") or {}
            game_id = str(game.get("id", "")) if isinstance(game, dict) else ""
            LOG.info("Game-Finish-Ereignis empfangen: %s", game_id or "unbekannt")
        elif event_type in {"challengeCanceled", "challengeDeclined"}:
            LOG.info("Challenge-Ereignis: %s", event_type)
        else:
            LOG.debug("Lichess-Ereignis ignoriert: %s", event_type)

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
                                raise RuntimeError("Eigener Bot ist in gameFull weder Weiß noch Schwarz.")
                            LOG.info("Spielerfarbe: %s", "Weiß" if bot_color else "Schwarz")
                            pending, thinking_moves = await self._process_game_state(
                                game_id, state, bot_color, pending, thinking_moves
                            )
                        elif event_type == "gameState":
                            if bot_color is None:
                                LOG.debug("gameState vor gameFull ignoriert.")
                                continue
                            pending, thinking_moves = await self._process_game_state(
                                game_id, event, bot_color, pending, thinking_moves
                            )
                        elif event_type == "opponentGone":
                            LOG.info("Lichess meldet, dass der Gegner kurzzeitig nicht verbunden ist.")
                        elif event_type == "chatLine":
                            LOG.debug("Game-Chat: %s", event.get("text", ""))
                    LOG.info("Game-Stream für %s wurde geschlossen.", game_id)
                    break
                except asyncio.CancelledError:
                    raise
                except LichessRateLimitError as exc:
                    LOG.error("Game-Stream rate-limited: %s; warte mindestens 60 Sekunden.", exc)
                    await asyncio.sleep(exc.retry_after_seconds)
                except Exception as exc:
                    LOG.error("Game-Stream %s unterbrochen: %s", game_id, exc)
                    await asyncio.sleep(reconnect_delay)
                    reconnect_delay = min(60.0, max(1.0, reconnect_delay * 2))
            LOG.info("Partie beendet oder Stream geschlossen: https://lichess.org/%s", game_id)
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            LOG.exception("Partieverarbeitung %s wurde abgebrochen: %s", game_id, exc)
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
                result = "kein Ergebnis (z. B. abgebrochen)"
            LOG.info("Partiestatus: %s, Ergebnis: %s", status, result)
            return None, thinking_moves

        if pending is not None and pending.is_confirmed_by(moves_text):
            LOG.info("Lichess hat unseren Zug %s bestätigt.", pending.uci)
            pending = None
        elif pending is not None:
            LOG.debug("Warte auf Bestätigung des bereits übertragenen Zuges %s.", pending.uci)
            return pending, thinking_moves

        try:
            board = board_from_uci_moves(moves_text)
        except ChessStateError as exc:
            raise RuntimeError(f"Ungültiger Spielstand aus Lichess: {exc}") from exc

        if board.is_game_over(claim_draw=False):
            LOG.info("Schachstellung ist beendet: %s", board.result(claim_draw=False))
            return pending, thinking_moves

        if board.turn != bot_color:
            LOG.debug("Gegner ist am Zug; Zugfolge: %s", moves_text or "Startstellung")
            return pending, thinking_moves

        if not should_start_thinking(
            current_moves=moves_text,
            bot_to_move=(board.turn == bot_color),
            pending=pending,
            thinking_moves=thinking_moves,
        ):
            return pending, thinking_moves

        thinking_moves = moves_text
        remaining_key = "wtime" if bot_color == chess.WHITE else "btime"
        increment_key = "winc" if bot_color == chess.WHITE else "binc"
        movetime = calculate_movetime_ms(
            state.get(remaining_key),
            state.get(increment_key),
            moves_to_go=self.config.game.moves_to_go,
            safety_margin_ms=self.config.game.safety_margin_ms,
            factor=self.config.game.move_time_factor,
            min_ms=self.config.game.min_move_time_ms,
            max_ms=self.config.game.max_move_time_ms,
        )
        LOG.info("MCU berechnet Zug; Zugnummer %d, Budget %d ms.", board.fullmove_number, movetime)

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
                LOG.warning("Serielle Verbindung während der Zugberechnung verloren; initialisiere MCU neu.")
                await self._reconnect_serial(active_game=True)
            except TimeoutError:
                try:
                    await self.serial.send("STOP")
                except Exception:
                    LOG.warning("STOP konnte nach dem Timeout nicht gesendet werden.")
                LOG.error("MCU hat innerhalb des Zeitlimits keinen BESTMOVE geliefert; Zug wird nicht übertragen.")
                return pending, thinking_moves
        if response is None:
            LOG.error("Keine verwertbare BESTMOVE-Antwort erhalten.")
            return pending, thinking_moves

        try:
            move = validate_bestmove(board, response.value)
        except ChessStateError as exc:
            LOG.error("BESTMOVE verworfen: %s", exc)
            return pending, thinking_moves

        uci = move.uci()
        san = board.san(move)
        elapsed_ms = int((time.monotonic() - search_started) * 1000)
        LOG.info("Berechnung beendet: %d ms; Zug %s (%s).", elapsed_ms, uci, san)
        pending = PendingMove(moves_text, uci)
        try:
            await self.lichess.make_move(game_id, uci)
            LOG.info("Zug übertragen: %s", uci)
        except LichessError as exc:
            LOG.error(
                "Zugübertragung unklar/fehlgeschlagen (%s). Sende denselben Zug nicht erneut; "
                "warte auf den Game-Stream zur Klärung.", exc
            )
        return pending, thinking_moves
