# Karshipta console

SvelteKit web dashboard for the Karshipta fleet: live map (MapLibre GL), ward status, commands, and missions. `src/lib` is also published as [`@nikx-tech/karshipta-console-core`](#consuming-nikx-techkarshipta-console-core), the reusable half of this app; `src/routes` is the reference app that consumes it. See [CLAUDE.md](CLAUDE.md) for the conventions and `../docs/architecture.md` for the big picture.

## Develop

```sh
npm install
npm run proto:gen   # generate TypeScript types from ../proto (ts-proto via buf)
npm run dev -- --open
```

The console opens empty: no ward appears until you add one from the UI (demo, simulated PX4 SITL, or a real ward over a connected gateway). See `docs/console-ux.md` for the onboarding flow, or `docs/quickstart.md` to run a real gateway.

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

## Consuming `@nikx-tech/karshipta-console-core`

```sh
npm run package   # svelte-package src/lib -> dist, then publint
```

Published automatically by `.github/workflows/publish-console-core.yml` to GitHub Packages, from `dev` or `main` alike, whenever `console/package.json`'s version changes (a deliberate bump made in the PR that needs it); a push that doesn't change the version is a silent no-op, not a failure. There is one stable line: `latest`. The library can reach v0.1.0 while the surrounding product is still pre-release; that's normal. GitHub Packages requires the npm scope to match the repo owner (`NIKX-Tech`), which every NIKX product shares, so the product name lives in the package name itself: this will rename to `@karshipta/console-core` (unambiguous under its own scope) once a real `karshipta` npm org exists at OSS launch.

Public surface: `fleet` (the store), `WebSocketTransport`, `FakeGateway` (a self-contained demo engine, handy for any app's dev/demo mode), the generated wire types, and the display components (`FleetMap`, `WardCard`, `WardDetail`, `CommandPanel`, `MissionPanel`, `EventsFeed`, `ConfirmDialog`). Onboarding UI (empty-state, add-ward dialogs, the connection panel) stays app-shell only: it encodes this repo's self-host UX opinions, not a general primitive yet.

**A consuming app must configure Tailwind v4 itself** (`tailwindcss` is a peer dependency): the components ship as plain `.svelte` files using Tailwind utility classes, and Tailwind only compiles classes it can see in your project's scanned sources. Without this, every component renders with zero layout (confirmed by an actual blank-screen failure while writing this, not assumed):

```css
/* src/app.css or equivalent */
@import 'tailwindcss';
@source '../node_modules/@nikx-tech/karshipta-console-core/dist';
@import '@nikx-tech/karshipta-console-core/theme.css';
```

**A consuming app also needs a `.npmrc`** pointing the `@nikx-tech` scope at GitHub Packages, authenticated (GitHub Packages requires auth to read even non-public packages):

```
@nikx-tech:registry=https://npm.pkg.github.com
//npm.pkg.github.com/:_authToken=${GITHUB_PACKAGES_TOKEN}
```

Minimal usage:

```svelte
<script>
	import { onMount } from 'svelte';
	import 'maplibre-gl/dist/maplibre-gl.css';
	import {
		fleet,
		FakeGateway,
		FleetMap,
		FAKE_FLEET_CENTER
	} from '@nikx-tech/karshipta-console-core';

	onMount(() => {
		const engine = new FakeGateway((envelope) => fleet.applyEnvelope(envelope, 'demo'));
		fleet.bindDemoEngine(engine);
		fleet.addDemoWard();
		return () => fleet.teardown();
	});
</script>

<FleetMap centerLat={FAKE_FLEET_CENTER.lat} centerLon={FAKE_FLEET_CENTER.lon} />
```
