---
name: verify
description: Runtime verification recipe for the Karshipta console (SvelteKit web app with MapLibre WebGL map)
---

# Verifying the console

## Launch

```sh
cd console
npm install && npm run proto:gen
npm run dev -- --port 5199
```

Default (no env) opens empty: no ward appears until an onboarding action is taken (see below). Set `PUBLIC_GATEWAY_WS_URL=ws://...` to auto-connect a gateway on load instead (automation override, not the default UX).

## Drive headless

MapLibre needs WebGL. Plain `--disable-gpu` headless Chrome throws "Failed to initialize WebGL"; pass SwiftShader flags:

```sh
"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --headless=new \
  --enable-unsafe-swiftshader --use-angle=swiftshader \
  --window-size=1280,800 --virtual-time-budget=20000 --timeout=30000 \
  --screenshot=out.png http://localhost:5199/
```

For console logs, pageerrors, and timed multi-shot captures, use playwright-core pointed at system Chrome (install it in the scratchpad, not this repo) with the same two SwiftShader args.

## What to check

- **Empty state**: on load, the map shows "No wards yet" with three actions (Add demo ward / Add simulated ward / Connect real ward); the left rail's Gateway tab icon (first of the three) shows a red status dot for DOWN.
- **Demo wards**: "Add demo ward" spawns `demo-1` instantly (no gateway needed), card shows a DEMO badge; click again for `demo-2` etc., each on a different orbit. Card's remove (x) works instantly once the ward is disarmed/landed; disabled with a title while armed or airborne.
- **Gateway connection**: the left rail's Gateway tab (first icon, always shows a status-colored dot: red DOWN / gray CONNECTING / amber pulsing LIVE) is the connection UI, inline - no floating panel anymore. Clicking it (or "Connect real ward"/"Add simulated ward" from the empty state) expands the rail to the WebSocket/Relay form (prefilled `ws://localhost:8765`); Connect shows CONNECTING then LIVE against a running gateway, or stays DOWN retrying with backoff against nothing. Disconnect removes only `source: gateway` wards, demo wards stay. The Fleets tab's header shows the live ward count (moved out of the top bar, which no longer has a WARDS badge or connection button).
- **Add simulated / real ward**: both require a connected gateway (buttons redirect to the connection panel otherwise); the dialog sends `AddWard` and shows Adding... then either closes (ACCEPTED) or shows the rejection reason inline. A second simulated-ward add in one session shows the resource-warning confirm first. The Class dropdown lists every `WardClass` (flight and non-flight), not just flight ones.
- Ward cards show live mode (flight wards only), altitude, battery; amber pulse dot when connected. When `state.connected` is false (link lost, not just gateway disconnected), the whole card and its map marker fade (`opacity-50 grayscale` / marker opacity 0.4), not just the dot. The demo engine never produces this on its own; force it by editing `connected: true` to `false` in `fake/fleet-sim.ts`'s `stateEnvelope` and reverting after.
- Amber markers on the map; positions and arrow rotations change between two screenshots a few seconds apart. Heading applies to every ward (not gated on flight state).
- Selection: clicking a card or marker selects (blue border/ring) and shows the COMMANDS panel. COMMANDS and MISSION only render for a ward with a `flight` field; there is no non-flight demo ward yet to exercise the other side of that gate.
- Commands (the demo engine answers all of them, so these are easiest to drive against a demo ward): Land/RTL/force-disarm require a confirm dialog (Escape cancels without sending); trackers show EXECUTING (amber pulse) then SUCCESS (green) or REJECTED (red, with reason); Arm/Takeoff are disabled while armed/airborne; land then arm then takeoff works; Goto arms crosshair targeting, map click opens a confirm with coordinates; RTL during goto preempts it and the goto tracker settles REJECTED "superseded".
- Missions: Plan mission -> map clicks add numbered blue waypoints on a dashed route (clicks on ward markers select instead, keep clear); altitudes editable, waypoints removable; Upload emits a MISSION_UPLOADED event; Start (confirm dialog) flies the items in order with "wp N/M" progress, repeat count adds full extra passes, finish acks the start command SUCCESS and emits MISSION_FINISHED; Pause holds (mode HOLD) and Start resumes; pause with nothing running settles REJECTED; land/RTL/goto during a mission interrupt it (start tracker settles REJECTED "mission interrupted").
- Long legs take real time: waypoints ~30 px apart at the default zoom are ~145 m, about 18 s per leg at 8 m/s. Keep test triangles tight and wait for "finished" with a generous timeout.
- If store state looks impossible (commands vanish, sender unbound) after many HMR edits, restart the dev server before debugging: stale HMR module graphs split the store singleton.
- Events feed (bottom right): landing/rejection events with severity dots and mono timestamps.
- Without WebGL the map shows an inline "Map unavailable" alert but cards keep updating.
- With `PUBLIC_READONLY=true`: VIEWER badge in the top bar, COMMANDS and MISSION panels absent from the detail panel, telemetry still updating.
- With `PUBLIC_GATEWAY_WS_URL` set and no gateway: no demo wards, top bar auto-attempts connection and stays DOWN, `transport: websocket error` retries with growing backoff, no crash; sent commands settle TIMEOUT after 10 s.
- Without `PUBLIC_OPENAIP_KEY`: no geozone layer, no legend, zero requests to api.core.openaip.net (only the local module URL appears in the network log; that is Vite serving the source, not a real API call).
- With `PUBLIC_OPENAIP_KEY` set (even to a bogus value): legend appears bottom-left of the map; a failed or unexpected response is caught and logged (`geozones: failed to load viewport`), never thrown, and the rest of the console keeps working.

## Gotchas

- A single `--screenshot` at page load races the first 200 ms telemetry tick; use `--virtual-time-budget` or a real wait.
- The demo engine and the (optional, override-only) auto-connect are wired from `onMount` in `+page.svelte`; this must never move into an `$effect` (feeding the store from inside an effect that also reads it causes an infinite setup/teardown loop).
- Every ward in `fleet.wards` carries `source: 'demo' | 'gateway'`; commands and mission uploads route by it (`channelFor` in `fleet-store.svelte.ts`). A ward appearing with no commands working is almost always a channel-routing bug, not a transport bug: check `source` first.
- Overlays that sit on top of the map (legend, error banner) must be siblings of the `bind:this={container}` div in `fleet-map.svelte`, never children of it: MapLibre takes ownership of that div's contents and paints its own canvas over anything already inside it.
- Class fields read from a template (e.g. a store's `active` getter) must be `$state` in Svelte 5, even when private; a plain field silently breaks reactivity with no error, only a UI that never updates.
