import asyncio

import httpx

from lichess_client import LichessClient


def test_challenge_and_move_use_official_bot_routes():
    seen = []

    async def handler(request: httpx.Request) -> httpx.Response:
        seen.append((request.method, request.url.path, request.content.decode()))
        if request.url.path == "/api/challenge/OtherBot":
            return httpx.Response(200, json={"id": "challenge123", "url": "https://lichess.org/challenge/abc"})
        if request.url.path == "/api/challenge/challenge123/accept":
            return httpx.Response(200, json={"ok": True})
        if request.url.path == "/api/bot/game/stream/game123":
            return httpx.Response(200, content=b'{"type":"gameFull","id":"game123"}\n{"type":"gameState","moves":"e2e4","status":"started"}\n')
        return httpx.Response(200, json={})

    async def scenario():
        client = LichessClient("test-token", transport=httpx.MockTransport(handler))
        await client.create_challenge(
            "OtherBot", rated=False, clock_limit_seconds=300, clock_increment_seconds=2
        )
        await client.accept_challenge("challenge123")
        await client.make_move("game123", "e2e4")
        events = [event async for event in client.game_stream("game123")]
        assert [event["type"] for event in events] == ["gameFull", "gameState"]
        await client.close()

    asyncio.run(scenario())
    assert seen[0][0:2] == ("POST", "/api/challenge/OtherBot")
    assert "clock.limit=300" in seen[0][2]
    assert "clock.increment=2" in seen[0][2]
    assert seen[1][0:2] == ("POST", "/api/challenge/challenge123/accept")
    assert seen[2][0:2] == ("POST", "/api/bot/game/game123/move/e2e4")
