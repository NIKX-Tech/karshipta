# Building on Karshipta

Two separate ways to build against Karshipta, depending on what you're
actually trying to do: embed pieces of the console's own UI in another
SvelteKit app, or talk to a gateway directly from your own code, in any
language.

## Embed the console: `@nikx-tech/karshipta-console-core`

The console's map, ward cards, command panel, mission panel, events feed,
and confirm dialog are published as a standalone, reusable npm package -
the same components the full console app is built from, not a separate
simplified API. Useful if you want Karshipta's live fleet view inside your
own SvelteKit app rather than running the standalone console.

```bash
npm install @nikx-tech/karshipta-console-core
```

Full setup (Tailwind v4 peer dependency, GitHub Packages `.npmrc` scope
config, the public component list, a minimal usage example) lives in
[console/README.md](../console/README.md) - this page won't duplicate it.

## Talk to a gateway directly

The gateway speaks one thing: a binary protobuf `Envelope`, one per
WebSocket frame, in both directions. That's the entire wire contract - the
console itself is just one client of it, generated from the same schema.
Anything that can open a WebSocket and decode protobuf can be a Karshipta
client: there is no console-specific handshake or hidden state.

- **Schema**: [`proto/karshipta/v1/`](../proto/karshipta/v1/) is the single
  source of truth. `envelope.proto` lists every message kind in both
  directions in its own top comment; `command.proto`, `telemetry.proto`,
  `fleet.proto` define the payloads themselves. Generate types for your own
  language with `protoc` (the gateway generates C++ at build time, the
  console generates TypeScript via `ts-proto` - see
  `console/package.json`'s `proto:gen` script for the exact invocation to
  copy for another language).
- **Transport**: plain WebSocket by default
  (`ws://<gateway-host>:8765`), or the optional E2E-encrypted relay
  transport (relayly) for a gateway with no reachable inbound port - see
  [gateway/docs/relay-transport.md](../gateway/docs/relay-transport.md).
  Nothing about the wire contract itself changes between the two; only how
  the bytes get there.
- **Reference client**: [`gateway/tools/ws_client.py`](../gateway/tools/ws_client.py)
  is a genuinely minimal example - connect, decode every incoming
  `Envelope`, print `WardInfo`/`WardState` lines. Not part of any build,
  just the fastest way to see real frames without starting the console.
  Run it against a live gateway:

  ```bash
  cd gateway/tools
  pip install websockets protobuf
  protoc -I ../../proto --python_out=gen $(ls ../../proto/karshipta/v1/*.proto)
  python3 ws_client.py ws://localhost:8765
  ```

- **Read-only access**: a connection can be marked a viewer at the
  transport level (see `docs/quickstart.md`'s connection panel notes) -
  telemetry still flows, every state-changing request is rejected with a
  reason instead of being silently dropped. Useful for a client that only
  ever needs to observe.

## Non-MAVLink devices: Herald

If you're integrating a tracker that has no MAVLink autopilot at all
(a livestock GPS tag, a generic tracker, a third-party fleet's own device),
Herald is the ingestion path, not the WebSocket client API above - it's
HTTP/TCP in, WardInfo/WardState out the same WebSocket every other client
reads from. Three ways in, by however close the device already is to
Herald's own shape:

| Path | Protocol | For |
|---|---|---|
| Native | HTTP `POST /herald` | A payload that's already a Herald message |
| Mapped | HTTP `POST /herald/mapped/<source>` | A vendor's own JSON, translated by a declarative YAML field mapping - no code to write |
| GT06 | Raw TCP, port 5023 | Cheap GT06-family GPS trackers, hundreds of models on the market |

See [gateway/docs/herald-ingest.md](../gateway/docs/herald-ingest.md) for
the full ingestion writeup, including the YAML mapping format for the
"mapped" path.
