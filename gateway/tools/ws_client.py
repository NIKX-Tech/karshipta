#!/usr/bin/env python3
"""Tiny verification client for the gateway WebSocket transport (BRIEF M2).

Connects to the gateway, decodes each binary Envelope frame, and prints
VehicleInfo and VehicleState lines. Not part of any build; a hand tool for
checking the wire without starting the console.

Setup (once):

    pip install websockets protobuf
    protoc -I ../../proto --python_out=gen $(ls ../../proto/karshipta/v1/*.proto)

Run (gateway listening on the default port):

    python3 ws_client.py ws://localhost:8765
"""

import asyncio
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent / "gen"))

try:
    import websockets
    from karshipta.v1 import envelope_pb2
except ImportError as error:
    sys.exit(f"missing dependency ({error}); see the setup lines in this file's docstring")


async def main(url: str) -> None:
    async with websockets.connect(url) as socket:
        print(f"connected to {url}")
        async for frame in socket:
            if isinstance(frame, str):
                print(f"unexpected text frame ignored: {frame[:60]!r}")
                continue
            envelope = envelope_pb2.Envelope()
            envelope.ParseFromString(frame)
            kind = envelope.WhichOneof("payload")
            if kind == "vehicle_info":
                info = envelope.vehicle_info
                print(f"info  {info.vehicle_id}: {info.autopilot} sysid={info.mavlink_system_id}")
            elif kind == "vehicle_state":
                state = envelope.vehicle_state
                position = state.position
                print(
                    f"state {state.vehicle_id}: alt={position.altitude_rel_m:.1f}m "
                    f"lat={position.latitude_deg:.6f} lon={position.longitude_deg:.6f} "
                    f"battery={state.battery.remaining_pct:.0f}% armed={state.armed}"
                )
            else:
                print(f"{kind}: {len(frame)} bytes")


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1] if len(sys.argv) > 1 else "ws://localhost:8765"))
