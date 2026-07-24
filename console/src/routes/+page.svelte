<script lang="ts">
	import { onMount } from 'svelte';
	import { env } from '$env/dynamic/public';
	import { fleet } from '$lib/fleet-store.svelte';
	import { zoneStore } from '$lib/zones/zone-store.svelte';
	import { geozoneStore } from '$lib/geozones/geozone-store.svelte';
	import { themeStore } from '$lib/theme.svelte';
	import { leftRailUi } from '$lib/left-rail-ui.svelte';
	import { locateOrFallback } from '$lib/geolocation';
	import { FakeGateway } from '$lib/fake/fleet-sim';
	import FleetMap from '$lib/components/fleet-map.svelte';
	import LeftRail from '$lib/components/left-rail.svelte';
	import RightPanel from '$lib/components/right-panel.svelte';
	import EmptyState from '$lib/components/empty-state.svelte';
	import LocationPickerBar from '$lib/components/location-picker-bar.svelte';
	import logoMark from '$lib/assets/logo-mark.svg';

	const DEFAULT_FLEET_LABEL = 'Fleet';

	// Overridable display label for "fleet" (e.g. a livestock-tracking deploy
	// might prefer "Herd"); purely cosmetic, does not rename any schema field.
	let fleetLabel = $state(DEFAULT_FLEET_LABEL);

	// Amsterdam: this app's own fallback wherever geolocation is denied or
	// unavailable (the initial map view, a new demo ward's default spot),
	// distinct from FAKE_FLEET_CENTER (Zurich Irchel, PX4 SITL's actual
	// default home) - that constant stays as-is so the fake fleet's own
	// internal simulation still matches the real docker-compose demo; see
	// fleet-sim.ts.
	const DEFAULT_MAP_CENTER = { lat: 52.3676, lon: 4.9041 };

	// The map's initial camera position: centered on the operator's own
	// location by default, falling back to DEFAULT_MAP_CENTER on denial or
	// unavailability. Resolved once, undefined until then - FleetMap reads
	// centerLat/centerLon only at mount (see its own untrack'd creation
	// effect), so it must not render until this settles, rather than
	// mounting with a placeholder and reassigning props later.
	let mapCenterLat = $state<number | undefined>(undefined);
	let mapCenterLon = $state<number | undefined>(undefined);

	// Demo ward placement: geolocation as the default, a map click as the
	// override, matching the pattern already proven in karshipta-cloud.
	// Falls back to DEFAULT_MAP_CENTER, not FAKE_FLEET_CENTER: this is a
	// user-facing "where do we put it if we don't know where you are"
	// default, the same concern the map's own fallback above resolves, not
	// the simulation engine's own Zurich-matching invariant.
	let placing = $state(false);
	let placingLocating = $state(false);
	let placeLat = $state(DEFAULT_MAP_CENTER.lat);
	let placeLon = $state(DEFAULT_MAP_CENTER.lon);

	function startPlacement() {
		placing = true;
		placeLat = DEFAULT_MAP_CENTER.lat;
		placeLon = DEFAULT_MAP_CENTER.lon;
		placingLocating = true;
		void locateOrFallback(DEFAULT_MAP_CENTER).then((point) => {
			placingLocating = false;
			placeLat = point.lat;
			placeLon = point.lon;
		});
	}

	function cancelPlacement() {
		placing = false;
	}

	function confirmPlacement() {
		fleet.addDemoWard({ lat: placeLat, lon: placeLon });
		placing = false;
	}

	function onMapClick(latitudeDeg: number, longitudeDeg: number) {
		if (!placing) return;
		placingLocating = false;
		placeLat = latitudeDeg;
		placeLon = longitudeDeg;
	}

	// Binding the demo engine here, at component init, not inside onMount:
	// onMount only runs after the first render already committed, and by
	// then a persisted demo fleet (see fleet-store.svelte.ts) has already
	// painted the empty-state wizard for a frame before flipping over to
	// the restored wards - a real, reported flash, not a style nit. This
	// runs before that first render instead, so fleet.wardIds already
	// reflects any restored wards the very first time the template reads
	// it. Guarded because this script body also runs during prerendering,
	// where window doesn't exist and starting the demo engine's tick
	// interval would be wrong.
	if (typeof window !== 'undefined') {
		fleet.bindDemoEngine(new FakeGateway((envelope) => fleet.applyEnvelope(envelope, 'demo')));
	}

	// The gateway is opt-in and explicit; PUBLIC_GATEWAY_WS_URL/PUBLIC_READONLY
	// stay as automation overrides (docker, CI) but no longer decide what the
	// console shows by default. onMount, not $effect: feeding the store must
	// not make this block depend on it.
	onMount(() => {
		themeStore.init();
		fleet.readonly = env.PUBLIC_READONLY === 'true';
		fleetLabel = env.PUBLIC_FLEET_LABEL || DEFAULT_FLEET_LABEL;
		geozoneStore.configure(env.PUBLIC_OPENAIP_KEY);
		const gatewayUrl = env.PUBLIC_GATEWAY_WS_URL;
		if (gatewayUrl) fleet.connectGateway(gatewayUrl);
		void locateOrFallback(DEFAULT_MAP_CENTER).then((point) => {
			mapCenterLat = point.lat;
			mapCenterLon = point.lon;
		});
		return () => fleet.teardown();
	});
</script>

<svelte:head>
	<title>Karshipta Console</title>
</svelte:head>

<div class="grid h-dvh grid-cols-[auto_minmax(0,1fr)_auto] grid-rows-[auto_minmax(0,1fr)] bg-ink">
	<header
		class="col-span-3 flex items-center gap-2 border-b border-edge bg-panel px-4 py-2"
		aria-label="Karshipta"
	>
		<img src={logoMark} alt="" class="h-3 w-auto" aria-hidden="true" />
		<span class="font-display text-xs font-medium tracking-[0.25em]">KARSHIPTA</span>
		<div class="ml-auto flex items-center gap-2">
			{#if fleet.readonly}
				<span
					class="rounded border border-edge px-1.5 py-0.5 font-mono text-[10px] text-fg-muted"
					role="status"
					aria-label="Read-only session"
				>
					VIEWER
				</span>
			{/if}
			<button
				class="flex h-6 w-6 items-center justify-center rounded text-fg-muted hover:bg-white/5 hover:text-fg"
				onclick={() => themeStore.toggle()}
				aria-label="Switch to {themeStore.current === 'dark' ? 'light' : 'dark'} theme"
				title="Switch to {themeStore.current === 'dark' ? 'light' : 'dark'} theme"
			>
				{#if themeStore.current === 'dark'}
					<svg
						width="14"
						height="14"
						viewBox="0 0 24 24"
						fill="none"
						stroke="currentColor"
						stroke-width="1.75"
						stroke-linecap="round"
						stroke-linejoin="round"
					>
						<circle cx="12" cy="12" r="4" />
						<path
							d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"
						/>
					</svg>
				{:else}
					<svg width="14" height="14" viewBox="0 0 24 24" fill="currentColor">
						<path d="M20.5 14.5a8.5 8.5 0 1 1-9-13 7 7 0 0 0 9 13Z" />
					</svg>
				{/if}
			</button>
		</div>
	</header>

	<LeftRail {fleetLabel} onstartdemoplacement={startPlacement} />

	<div class="relative">
		{#if mapCenterLat !== undefined && mapCenterLon !== undefined}
			<FleetMap
				centerLat={mapCenterLat}
				centerLon={mapCenterLon}
				{onMapClick}
				crosshair={placing}
				placementPoint={placing ? { latitudeDeg: placeLat, longitudeDeg: placeLon } : undefined}
			/>
		{/if}
		<!-- Zone drawing is the one map-click workflow that doesn't need an
		     existing ward to start (an operator can plan safety areas before
		     ever adding one); without this guard, EmptyState's centered card
		     would sit on top of the canvas and silently eat every vertex
		     click meant for it. -->
		{#if fleet.wardIds.length === 0 && !placing && !zoneStore.draft}
			<EmptyState
				onopenconnection={() => leftRailUi.openGatewayTab()}
				onstartdemoplacement={startPlacement}
			/>
		{/if}
		{#if placing}
			{#snippet confirm()}
				<button
					type="button"
					onclick={confirmPlacement}
					disabled={placingLocating}
					class="rounded bg-accent px-3 py-1.5 text-xs font-semibold text-ink disabled:opacity-50"
				>
					Deploy here
				</button>
			{/snippet}
			<LocationPickerBar
				lat={placeLat}
				lon={placeLon}
				locating={placingLocating}
				oncancel={cancelPlacement}
				{confirm}
			/>
		{/if}
	</div>

	<RightPanel {fleetLabel} />
</div>
