"""Small asynchronous client for the official Lichess Bot API."""

from __future__ import annotations

import asyncio
import json
import logging
from urllib.parse import quote
from collections.abc import AsyncIterator
from typing import Any

import httpx

LOG = logging.getLogger(__name__)


class LichessError(RuntimeError):
    pass


class LichessRateLimitError(LichessError):
    def __init__(self, message: str, retry_after_seconds: int = 60) -> None:
        super().__init__(message)
        self.retry_after_seconds = max(60, int(retry_after_seconds))


class LichessClient:
    """Bot API client. Finite REST requests are serialized; streams stay open."""

    def __init__(
        self,
        token: str,
        *,
        base_url: str = "https://lichess.org",
        retries: int = 2,
        transport: httpx.AsyncBaseTransport | None = None,
    ) -> None:
        self.base_url = base_url.rstrip("/")
        self.retries = retries
        self._request_lock = asyncio.Lock()
        self._client = httpx.AsyncClient(
            base_url=self.base_url,
            headers={
                "Authorization": f"Bearer {token}",
                "User-Agent": "Microcontroller-Chess-Lichess-Bridge/0.1 (Python)",
                "Accept": "application/x-ndjson, application/json",
            },
            timeout=httpx.Timeout(connect=10.0, read=20.0, write=10.0, pool=10.0),
            limits=httpx.Limits(max_connections=8, max_keepalive_connections=4),
            transport=transport,
        )

    async def close(self) -> None:
        await self._client.aclose()

    @staticmethod
    def _retry_after(response: httpx.Response) -> int:
        value = response.headers.get("Retry-After", "60")
        try:
            return max(60, int(float(value)))
        except ValueError:
            return 60

    async def _request(self, method: str, path: str, **kwargs: Any) -> httpx.Response:
        # Lichess asks API clients to avoid parallel finite requests. Streaming
        # endpoints are intentionally handled separately in stream_ndjson().
        async with self._request_lock:
            for attempt in range(self.retries + 1):
                try:
                    response = await self._client.request(method, path, **kwargs)
                except httpx.HTTPError as exc:
                    # No automatic POST retry: the server may have processed it
                    # before the connection failed. The caller must reconcile via stream.
                    raise LichessError(f"Network error during {method} {path}: {exc}") from exc

                if response.status_code == 429:
                    delay = self._retry_after(response)
                    LOG.warning("Lichess HTTP 429: pausing for %ss as requested by the rate-limit response.", delay)
                    # Always enforce the cooldown, including after the final retry.
                    await asyncio.sleep(delay)
                    if attempt >= self.retries:
                        raise LichessRateLimitError(
                            f"Lichess returned HTTP 429; retry after {delay}s erneut versuchen.",
                            retry_after_seconds=delay,
                        )
                    continue

                if response.is_error:
                    detail = response.text[:500]
                    raise LichessError(
                        f"Lichess API {method} {path}: HTTP {response.status_code}: {detail}"
                    )
                return response
        raise LichessError("Unexpected request termination")

    async def get_account(self) -> dict[str, Any]:
        response = await self._request("GET", "/api/account")
        return response.json()

    async def create_challenge(
        self,
        username: str,
        *,
        rated: bool,
        clock_limit_seconds: int,
        clock_increment_seconds: int,
        variant: str = "standard",
    ) -> dict[str, Any]:
        response = await self._request(
            "POST",
            f"/api/challenge/{quote(username, safe='')}",
            data={
                "rated": str(rated).lower(),
                "variant": variant,
                "clock.limit": str(clock_limit_seconds),
                "clock.increment": str(clock_increment_seconds),
            },
        )
        return response.json()

    async def accept_challenge(self, challenge_id: str) -> dict[str, Any] | None:
        response = await self._request("POST", f"/api/challenge/{quote(challenge_id, safe='')}/accept")
        if not response.content:
            return None
        try:
            return response.json()
        except json.JSONDecodeError:
            return None

    async def decline_challenge(self, challenge_id: str) -> dict[str, Any] | None:
        response = await self._request("POST", f"/api/challenge/{quote(challenge_id, safe='')}/decline")
        if not response.content:
            return None
        try:
            return response.json()
        except json.JSONDecodeError:
            return None

    async def make_move(self, game_id: str, uci: str) -> None:
        await self._request("POST", f"/api/bot/game/{game_id}/move/{uci}")

    async def stream_ndjson(self, path: str) -> AsyncIterator[dict[str, Any]]:
        """Yield ND-JSON objects from a long-lived stream request."""
        timeout = httpx.Timeout(connect=10.0, read=None, write=10.0, pool=10.0)
        try:
            async with self._client.stream("GET", path, timeout=timeout) as response:
                if response.status_code == 429:
                    delay = self._retry_after(response)
                    raise LichessRateLimitError(
                        f"Lichess stream returned HTTP 429; reopen the connection after at least {delay}s.",
                        retry_after_seconds=delay,
                    )
                if response.is_error:
                    detail = (await response.aread()).decode("utf-8", errors="replace")[:500]
                    raise LichessError(f"Lichess Stream {path}: HTTP {response.status_code}: {detail}")
                async for line in response.aiter_lines():
                    if not line.strip():
                        continue  # Lichess sends blank keep-alive lines.
                    try:
                        value = json.loads(line)
                    except json.JSONDecodeError:
                        LOG.warning("Ignoring invalid ND-JSON line from %s", path)
                        continue
                    if isinstance(value, dict):
                        yield value
        except (httpx.TimeoutException, httpx.TransportError) as exc:
            raise LichessError(f"Stream connection to {path} interrupted: {exc}") from exc

    async def event_stream(self) -> AsyncIterator[dict[str, Any]]:
        async for event in self.stream_ndjson("/api/stream/event"):
            yield event

    async def game_stream(self, game_id: str) -> AsyncIterator[dict[str, Any]]:
        async for event in self.stream_ndjson(f"/api/bot/game/stream/{game_id}"):
            yield event
