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

Default (no env) runs the fake fleet: three simulated multirotors orbiting the PX4 SITL home (47.397742, 8.545594). Set `PUBLIC_GATEWAY_WS_URL=ws://...` to exercise the real transport path instead.

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

- Three vehicle cards (sitl-1/2/3) with live mode, altitude, battery; amber pulse dot when connected.
- Three amber markers orbiting near map center over the dark Carto basemap; positions and arrow rotations change between two screenshots ~6 s apart.
- Selection: clicking a card or marker selects (blue border/ring) and shows the COMMANDS panel.
- Commands (fake fleet answers all of them): Land/RTL/force-disarm require a confirm dialog (Escape cancels without sending); trackers show EXECUTING (amber pulse) then SUCCESS (green) or REJECTED (red, with reason); Arm/Takeoff are disabled while armed/airborne; land then arm then takeoff works; Goto arms crosshair targeting, map click opens a confirm with coordinates; RTL during goto preempts it and the goto tracker settles REJECTED "superseded".
- Missions: Plan mission -> map clicks add numbered blue waypoints on a dashed route (clicks on vehicle markers select instead, keep clear); altitudes editable, waypoints removable; Upload emits a MISSION_UPLOADED event; Start (confirm dialog) flies the items in order with "wp N/M" progress, repeat count adds full extra passes, finish acks the start command SUCCESS and emits MISSION_FINISHED; Pause holds (mode HOLD) and Start resumes; pause with nothing running settles REJECTED; land/RTL/goto during a mission interrupt it (start tracker settles REJECTED "mission interrupted").
- Long legs take real time: waypoints ~30 px apart at the default zoom are ~145 m, about 18 s per leg at 8 m/s. Keep test triangles tight and wait for "finished" with a generous timeout.
- If store state looks impossible (commands vanish, sender unbound) after many HMR edits, restart the dev server before debugging: stale HMR module graphs split the store singleton.
- Events feed (bottom right): landing/rejection events with severity dots and mono timestamps.
- Without WebGL the map shows an inline "Map unavailable" alert but cards keep updating.
- With `PUBLIC_GATEWAY_WS_URL` set and no gateway: no fake vehicles, `transport: websocket error` retries with growing backoff, no crash; sent commands settle TIMEOUT after 10 s.

## Gotchas

- A single `--screenshot` at page load races the first 200 ms telemetry tick; use `--virtual-time-budget` or a real wait.
- The fake fleet is fed from `onMount` in `+page.svelte`; it must never move into an `$effect` (feeding the store from inside an effect that also reads it causes an infinite setup/teardown loop).
