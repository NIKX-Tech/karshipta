#!/usr/bin/env python3
"""Tiny verification client for the gateway WebSocket transport (BRIEF M2).

Connects to the gateway, decodes each binary Envelope frame, and prints
WardInfo and WardState lines. Not part of any build; a hand tool for
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
            if kind == "ward_info":
                info = envelope.ward_info
                print(f"info  {info.ward_id}: {info.autopilot} sysid={info.mavlink_system_id}")
            elif kind == "ward_state":
                state = envelope.ward_state
                position = state.position
                # flight is unset for non-flight wards (e.g. a Herald-ingested
                # livestock tag), so armed/in_air only make sense to print
                # when it's actually present.
                armed = state.flight.armed if state.HasField("flight") else "n/a"
                print(
                    f"state {state.ward_id}: alt={position.altitude_rel_m:.1f}m "
                    f"lat={position.latitude_deg:.6f} lon={position.longitude_deg:.6f} "
                    f"battery={state.battery.remaining_pct:.0f}% armed={armed}"
                )
            else:
                print(f"{kind}: {len(frame)} bytes")


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1] if len(sys.argv) > 1 else "ws://localhost:8765"))
