# Karshipta console

SvelteKit web dashboard for the Karshipta fleet: live map (MapLibre GL), vehicle status, commands, and missions. `src/lib` is also published as [`@karshipta/console-core`](#consuming-karshiptaconsole-core), the reusable half of this app; `src/routes` is the reference app that consumes it. See [CLAUDE.md](CLAUDE.md) for the conventions and `../docs/architecture.md` for the big picture.

## Develop

```sh
npm install
npm run proto:gen   # generate TypeScript types from ../proto (ts-proto via buf)
npm run dev -- --open
```

The console opens empty: no vehicle appears until you add one from the UI (demo, simulated PX4 SITL, or a real vehicle over a connected gateway). See `docs/console-ux.md` for the onboarding flow, or `docs/quickstart.md` to run a real gateway.

`PUBLIC_GATEWAY_WS_URL` and `PUBLIC_READONLY` are automation overrides (docker, CI), not the default experience:

```sh
PUBLIC_GATEWAY_WS_URL=ws://localhost:8765 npm run dev
```

## Verify

```sh
npm run lint    # prettier + eslint
npm run check   # svelte-check, TypeScript strict
npm run build
```

Generated code in `src/lib/gen/` is never edited by hand; rerun `npm run proto:gen` after any schema change.

## Consuming `@karshipta/console-core`

```sh
npm run package   # svelte-package src/lib -> dist, then publint
```

Publishing runs from this directory (`publishConfig.directory` points npm at `dist`): `npm run package && npm publish`. Not yet published to a real registry; this package needs NIKX npm org credentials neither of us has scripted here yet.

Public surface: `fleet` (the store), `WebSocketTransport`, `FakeGateway` (a self-contained demo engine, handy for any app's dev/demo mode), the generated wire types, and the display components (`FleetMap`, `VehicleCard`, `VehicleDetail`, `CommandPanel`, `MissionPanel`, `EventsFeed`, `ConfirmDialog`). Onboarding UI (empty-state, add-vehicle dialogs, the connection panel) stays app-shell only: it encodes this repo's self-host UX opinions, not a general primitive yet.

**A consuming app must configure Tailwind v4 itself** (`tailwindcss` is a peer dependency): the components ship as plain `.svelte` files using Tailwind utility classes, and Tailwind only compiles classes it can see in your project's scanned sources. Without this, every component renders with zero layout (confirmed by an actual blank-screen failure while writing this, not assumed):

```css
/* src/app.css or equivalent */
@import 'tailwindcss';
@source '../node_modules/@karshipta/console-core/dist';
@import '@karshipta/console-core/theme.css';
```

Minimal usage:

```svelte
<script>
	import { onMount } from 'svelte';
	import 'maplibre-gl/dist/maplibre-gl.css';
	import { fleet, FakeGateway, FleetMap, FAKE_FLEET_CENTER } from '@karshipta/console-core';

	onMount(() => {
		const engine = new FakeGateway((envelope) => fleet.applyEnvelope(envelope, 'demo'));
		fleet.bindDemoEngine(engine);
		fleet.addDemoVehicle();
		return () => fleet.teardown();
	});
</script>

<FleetMap centerLat={FAKE_FLEET_CENTER.lat} centerLon={FAKE_FLEET_CENTER.lon} />
```
