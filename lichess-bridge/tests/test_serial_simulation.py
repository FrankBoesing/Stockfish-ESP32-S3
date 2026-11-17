import asyncio

from protocol import MessageKind
from serial_client import SimulatedSerialClient


def test_simulated_serial_protocol():
    async def scenario():
        device = SimulatedSerialClient(seed=1)
        await device.connect()
        await device.send("BOTMODE")
        ready = await device.read_message(1)
        assert ready.kind == MessageKind.READY
        await device.send("POSITION", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1")
        ack = await device.read_message(1)
        assert ack.kind == MessageKind.OK_POSITION
        await device.send("GO", "500")
        move = await device.read_message(1)
        assert move.kind == MessageKind.BESTMOVE
        await device.close()
        await device.connect()
        await device.send("BOTMODE")
        ready_again = await device.read_message(1)
        assert ready_again.kind == MessageKind.READY
        await device.close()

    asyncio.run(scenario())
