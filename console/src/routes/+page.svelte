<script lang="ts">
	import { onMount } from 'svelte';
	import { env } from '$env/dynamic/public';
	import { fleet } from '$lib/fleet-store.svelte';
	import { zoneStore } from '$lib/zones/zone-store.svelte';
	import { geozoneStore } from '$lib/geozones/geozone-store.svelte';
	import { wildfireStore } from '$lib/wildfires/wildfire-store.svelte';
	import { leftRailUi } from '$lib/left-rail-ui.svelte';
	import { rightPanelUi } from '$lib/right-panel-ui.svelte';
	import { isMobile } from '$lib/is-mobile.svelte';
	import { mobileSurfaceUi } from '$lib/mobile-surface-ui.svelte';
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

	// Vondelpark, Amsterdam: this app's own fallback wherever geolocation is
	// denied or unavailable (the initial map view, a new demo ward's default
	// spot). A real, open public park, not just "somewhere in Amsterdam" -
	// an earlier coordinate near Centraal turned out to sit on top of a
	// road. Matches FAKE_FLEET_CENTER (fleet-sim.ts) and docker-compose.yml's
	// PX4_HOME_LAT/LON, so the fake fleet, the real docker-compose demo, and
	// this fallback all place wards in the same spot.
	const DEFAULT_MAP_CENTER = { lat: 52.3579, lon: 4.8686 };

	// The map's initial camera position: centered on the operator's own
	// location by default, falling back to DEFAULT_MAP_CENTER on denial or
	// unavailability. Resolved once, undefined until then - FleetMap reads
	// centerLat/centerLon only at mount (see its own untrack'd creation
	// effect), so it must not render until this settles, rather than
	// mounting with a placeholder and reassigning props later.
	let mapCenterLat = $state<number | undefined>(undefined);
	let mapCenterLon = $state<number | undefined>(undefined);

	// Real rendered header height (it varies with env(safe-area-inset-top),
	// a notched phone's status bar height isn't a constant), exposed as a
	// CSS var so right-panel.svelte's mobile ward-detail card - fixed to
	// the true top of the page the same way the header itself is - can sit
	// just below it instead of guessing a fixed offset that would either
	// gap or overlap depending on the device.
	let headerHeight = $state(0);

	// Demo ward placement: pan-to-center, not click-to-place. The spawn
	// point is always wherever the map is currently centered, shown by a
	// plain CSS crosshair fixed at the center of the visible map area (see
	// the map wrapper's own markup below), never a real map marker pinned
	// to a specific lng/lat. A marker pinned at click time had a real,
	// confirmed bug: an operator who panned anywhere else afterward
	// (including just looking around before placing) left it rendering
	// off in some other, possibly off-screen, part of the map, with no
	// visual tie to where the camera actually was any more. Centering it
	// on the viewport instead makes that structurally impossible - it's
	// always exactly where it looks like it is. The map is left wherever
	// the operator already had it, not reset or geolocated out from under
	// them, the moment they start placing - liveMapCenterLat/
	// liveMapCenterLon (kept in sync via onMapMove below, whether placing
	// or not) seed placeLat/placeLon at that moment and keep tracking it
	// while placing continues. Falls back to DEFAULT_MAP_CENTER, not
	// FAKE_FLEET_CENTER: this is a user-facing "where do we put it if we
	// don't know where you are" default, the same concern the map's own
	// fallback above resolves, not the simulation engine's own
	// Zurich-matching invariant.
	let placing = $state(false);
	let liveMapCenterLat = $state<number | undefined>(undefined);
	let liveMapCenterLon = $state<number | undefined>(undefined);
	let placeLat = $state(DEFAULT_MAP_CENTER.lat);
	let placeLon = $state(DEFAULT_MAP_CENTER.lon);
	// LocationPickerBar's own real rendered height, bound below - the
	// placement crosshair subtracts this from the map area it centers
	// itself in, so it lands in the middle of what's actually visible as
	// map, not the middle of the full viewport including the space this
	// bar covers. No separate fixed offset any more: LocationPickerBar is
	// now a real EdgeSheet docked flush to the bottom edge (see its own
	// comment on why), not a floating card with its own bottom-4 gap.
	let placementBarHeight = $state(0);

	function startPlacement() {
		placing = true;
		placeLat = liveMapCenterLat ?? DEFAULT_MAP_CENTER.lat;
		placeLon = liveMapCenterLon ?? DEFAULT_MAP_CENTER.lon;
		// Mobile only, in effect: desktop never has a drawer/sheet open over
		// the map in the first place. Without this, starting placement from
		// inside the Fleets drawer (its own "+ Add Ward" is exactly how
		// AddWardMenu's "Add demo ward" below gets here) left that drawer
		// open on top of the map - confirmed live as a real bug, the
		// crosshair and Deploy button were both there, both completely
		// unreachable, covered by the still-open sheet underneath them.
		mobileSurfaceUi.close();
	}

	function cancelPlacement() {
		placing = false;
	}

	function confirmPlacement() {
		fleet.addDemoWard({ lat: placeLat, lon: placeLon });
		placing = false;
	}

	// + env(safe-area-inset-bottom): this is the legend's actual offset
	// from the real bottom edge of the screen (viewport-fit=cover in
	// app.html makes the page draw there) - mirrors karshipta-cloud's own
	// +page.svelte identically. No longer selection-dependent: the sheet
	// used to always show at least a peek bar the moment a ward was
	// selected, which this pushed the legend clear of; now the
	// ward-detail card lives at the top instead (see right-panel.svelte's
	// own comment) and the bottom sheet only exists at all once actually
	// expanded, so there's no more persistent bottom chrome to clear
	// just from a selection on its own.
	const mobileChromeBottom = $derived('calc(10px + env(safe-area-inset-bottom, 0px))');

	// Mobile only, and only once a ward is actually selected (the card
	// doesn't render otherwise - see right-panel.svelte): pushes
	// fleet-map.svelte's own top overlays (weather widget, loading toast)
	// down clear of right-panel.svelte's fixed ward-detail card, which
	// would otherwise render right underneath it, confirmed as the direct
	// next consequence of moving that card to the top. rightPanelUi's own
	// topCardHeight is the card's real measured height (its content -
	// WardStatusStrip - varies it slightly by ward state), not a guess.
	const mobileWardCardHeight = $derived(
		isMobile.current && fleet.selectedWardId !== undefined ? rightPanelUi.topCardHeight : 0
	);

	// Mobile only: the real rendered height of whichever bottom sheet is
	// actually open right now - left-rail.svelte's own drawer (Gateway/
	// Fleets/Events), right-panel.svelte's own (ward detail tab content),
	// or ThemeToggle's/UnitsToggle's own small pickers (also left-rail
	// triggers, also EdgeSheet-based - see mobile-surface-ui.svelte.ts's
	// own comment on why these get the same treatment as the other two).
	// mobileSurfaceUi.open already guarantees at most one of the four is
	// ever open at a time, so reading it directly (rather than e.g. taking
	// the max of every height) is exact, not just a reasonable
	// approximation. Fed to fleet-map.svelte below as
	// --mobile-external-sheet-reserve, which it adds to its own
	// locally-driven layers/attribution sheet reserve to shrink its
	// viewport by the total - an open sheet should push the map (and
	// everything positioned against it) up clear of itself, not just
	// cover the bottom of it, confirmed live as a real bug before this.
	// placing takes priority over mobileSurfaceUi.open, not another branch
	// of it: LocationPickerBar's own EdgeSheet isn't coordinated through
	// that singleton at all (it's not a left-rail trigger the way the
	// others are), and startPlacement() already closes whatever else was
	// open the moment placement starts, so the two never actually
	// overlap - but reading it first here still avoids relying on that
	// ordering to avoid a stale reserve from whatever was open a moment
	// before. Confirmed live as a real bug without this: fleet-map.svelte's
	// own weather toggle button, positioned against the map's own
	// (un-shrunk) viewport bottom, rendered directly underneath/behind
	// this sheet once it grew tall enough to reach that corner.
	const mobileExternalSheetReserve = $derived(
		isMobile.current
			? placing
				? placementBarHeight
				: mobileSurfaceUi.open === 'drawer'
					? leftRailUi.bottomSheetHeight
					: mobileSurfaceUi.open === 'sheet'
						? rightPanelUi.bottomSheetHeight
						: mobileSurfaceUi.open === 'theme'
							? mobileSurfaceUi.themeHeight
							: mobileSurfaceUi.open === 'units'
								? mobileSurfaceUi.unitsHeight
								: mobileSurfaceUi.open === 'about'
									? mobileSurfaceUi.aboutHeight
									: mobileSurfaceUi.open === 'add-ward'
										? mobileSurfaceUi.addWardHeight
										: 0
			: 0
	);

	// The one place that decides how much every mobile bottom sheet in the
	// app (left-rail.svelte's drawer, right-panel.svelte's ward sheet,
	// ThemeToggle's/UnitsToggle's/AboutDialog's own, fleet-map.svelte's own
	// layers/attribution sheets) insets from the left/right edge to clear
	// whichever icon rail is actually visible right now - exposed as CSS
	// vars below, read directly by EdgeSheet (and, via the same vars,
	// fleet-map.svelte's hand-rolled sheets) instead of each of those seven
	// call sites computing and hardcoding its own copy of this. Confirmed
	// live as a real, repeated bug otherwise: every sheet had its own
	// static leftRailInset={40}/rightRailInset={40} (or, for right-panel.svelte's
	// own sheet, no leftRailInset at all), none of them reactive to the
	// rail actually toggling visible/hidden while the sheet stayed open -
	// a sheet opened before the hamburger was tapped rendered with the
	// rail painted over its edge once the rail appeared, and a sheet left
	// open while the rail was hidden again kept reserving space for a
	// rail that was no longer there.
	const mobileLeftRailInset = $derived(isMobile.current && leftRailUi.mobileRailVisible ? 40 : 0);
	const mobileRightRailInset = $derived(
		isMobile.current && fleet.selectedWardId !== undefined ? 40 : 0
	);

	// A live resize past sm (not just a fresh load) can leave
	// mobileSurfaceUi.open stale - e.g. open the drawer on mobile, then
	// widen past the breakpoint without it ever explicitly closing.
	$effect(() => {
		if (!isMobile.current) mobileSurfaceUi.close();
	});

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
		fleet.readonly = env.PUBLIC_READONLY === 'true';
		fleetLabel = env.PUBLIC_FLEET_LABEL || DEFAULT_FLEET_LABEL;
		geozoneStore.configure(env.PUBLIC_OPENAIP_KEY);
		// obstacleStore/airportStore need no configure() any more - they're
		// proxied through a consuming app's own backend now (see
		// obstacles/proxiedSource.ts). This reference app is adapter-static
		// with no backend, so /api/obstacles and /api/airports simply 404 and
		// both layers degrade gracefully, same as the Aircraft layer already
		// does here.
		wildfireStore.configure(env.PUBLIC_FIRMS_KEY);
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

<!-- --mobile-sheet-max-height: the real cap on how tall any `position:
     fixed` bottom sheet (EdgeSheet, fleet-map.svelte's own layers/
     attribution sheets) is allowed to render. A sheet's height used to be
     driven only by its own content up to a flat max-h-[80vh], completely
     independent of how much map the formula below leaves visible - since
     a fixed element paints over whatever's beneath it regardless of that
     box's own computed size, a tall sheet just covered most of the map
     even while the map's own height still (correctly) never shrank past
     its 50% floor. Confirmed live as a real bug: a long Configuration tab
     left only a sliver of map visible behind the sheet. Capping the sheet
     itself, not the map, is what actually keeps the map visible -
     overflow-y-auto on the sheet (already there) takes over past this
     height.

     Must also subtract mobileWardCardHeight: the map's own box is pushed
     down by that much (top: var(--mobile-ward-card-height)) before it
     ever claims its own 50% floor, so the card, the map's floor, and the
     sheet's cap all have to share the same total instead of each being
     50% of it independently - otherwise the sheet (anchored to the real
     viewport bottom, with no idea the map started lower than the content
     area's own top) ends up exactly wardCardHeight px taller than the
     space actually left below the map, overlapping its bottom edge (and
     the control buttons anchored to it) by that much. Confirmed live as
     a real bug: a selected ward's Configuration sheet swallowed the
     lower ~110px of the map, which the buttons there sit behind. -->
<div
	class="grid h-dvh grid-cols-[auto_minmax(0,1fr)_auto] grid-rows-[auto_minmax(0,1fr)] bg-ink"
	style="--mobile-chrome-top: {headerHeight}px; --mobile-sheet-max-height: calc((100dvh - var(--mobile-chrome-top)) * 0.5 - {mobileWardCardHeight}px); --mobile-left-rail-inset: {mobileLeftRailInset}px; --mobile-right-rail-inset: {mobileRightRailInset}px;"
>
	<!-- max-md:pt-: this header sits at the true top of the page
	     (viewport-fit=cover in app.html), so without this its content
	     sits under the status bar/notch on a phone - same class of bug
	     as karshipta-cloud's own top bar (see its topbar.svelte). max(),
	     not replace, so it only grows past py-2's own 0.5rem on a device
	     that actually has an inset to clear. max-md:relative z-[46]: an
	     expanded EdgeSheet's own outside-tap catcher is `fixed inset-0`
	     (the full viewport, header included, not just the map) at
	     z-[45] - without this, the hamburger became unclickable the
	     moment its own sheet was open, confirmed live as a real bug
	     (the same reasoning left-rail.svelte/right-panel.svelte's own
	     icon rails already needed this for). -->
	<header
		bind:clientHeight={headerHeight}
		class="col-span-3 col-start-1 row-start-1 flex items-center gap-2 border-b border-edge bg-panel px-4 py-2 max-md:relative max-md:z-[46] max-md:pt-[max(0.5rem,env(safe-area-inset-top,0px))]"
		aria-label="Karshipta"
	>
		{#if isMobile.current}
			<!-- Shows/hides left-rail.svelte's own icon rail on mobile (see
			     leftRailUi.mobileRailVisible's own comment on why it
			     defaults hidden there) - once shown, that rail behaves
			     exactly like the right rail; this button only ever
			     controls whether it's there at all, not what it does.
			     Hiding it also closes its own sheet if open: an orphaned
			     open sheet with no visible trigger left on screen would be
			     confusing, not a useful state to preserve. -->
			<button
				type="button"
				class="justify flex w-4 items-center rounded text-fg-muted hover:bg-white/5 hover:text-fg"
				aria-label="Menu"
				aria-expanded={leftRailUi.mobileRailVisible}
				onclick={() => {
					leftRailUi.toggleMobileRail();
					if (!leftRailUi.mobileRailVisible && mobileSurfaceUi.open === 'drawer') {
						mobileSurfaceUi.close();
					}
				}}
			>
				<svg
					width="18"
					height="18"
					viewBox="0 0 24 24"
					fill="none"
					stroke="currentColor"
					stroke-width="1.75"
					stroke-linecap="round"
					aria-hidden="true"
				>
					<line x1="0" y1="7" x2="14" y2="7" />
					<line x1="0" y1="12" x2="14" y2="12" />
					<line x1="0" y1="17" x2="14" y2="17" />
				</svg>
			</button>
		{/if}
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
		</div>
	</header>

	<LeftRail {fleetLabel} onstartdemoplacement={startPlacement} />

	<!-- col-start-2/row-start-2: explicit, not implicit DOM-order
	     placement - LeftRail renders zero in-flow boxes on mobile (its
	     own rail is hidden there, replaced by the hamburger above), and
	     CSS Grid's auto-placement then shifted this div and RightPanel's
	     own root div one column left each to fill the gap, collapsing
	     this div into column 1's auto-sized (shrink-to-fit) track -
	     confirmed live as a real bug: the map rendered as a sliver with
	     just its background color showing, and RightPanel ended up where
	     this div should be. Explicit placement makes each region's
	     column immune to whether any sibling renders a box at all. -->
	<div
		class="relative col-start-2 row-start-2"
		style="--mobile-chrome-bottom: {mobileChromeBottom}; --mobile-ward-card-height: {mobileWardCardHeight}px; --mobile-external-sheet-reserve: {mobileExternalSheetReserve}px;"
	>
		{#if mapCenterLat !== undefined && mapCenterLon !== undefined}
			<FleetMap
				centerLat={mapCenterLat}
				centerLon={mapCenterLon}
				onMapMove={(view) => {
					liveMapCenterLat = view.lat;
					liveMapCenterLon = view.lon;
					if (placing) {
						placeLat = view.lat;
						placeLon = view.lon;
					}
				}}
				crosshair={placing}
				hideLoadingToast={isMobile.current && mobileSurfaceUi.open === 'drawer'}
				externalSheetOpen={isMobile.current && mobileSurfaceUi.open !== null}
				onLocalSheetOpen={() => mobileSurfaceUi.close()}
			/>
			{#if placing}
				<!-- The spawn point itself: always the exact center of the map
				     area below, never a real map marker - see placeLat/
				     placeLon's own comment above for why. bottom excludes
				     LocationPickerBar's own real footprint (it's a real
				     EdgeSheet docked flush to the bottom edge now, no extra
				     gap to account for) so this centers in what's actually
				     visible as map, not the viewport including the space
				     that bar covers. pointer-events-none: this is a
				     read-only indicator, dragging/panning underneath it
				     must reach the map, not this overlay. -->
				<div
					class="pointer-events-none absolute inset-x-0 top-0 z-30 flex items-center justify-center"
					style="bottom: {placementBarHeight}px"
				>
					<div
						role="img"
						aria-label="Selected spawn point"
						class="flex h-6 w-6 items-center justify-center rounded-full border-2 border-accent bg-panel text-accent"
					>
						<svg width="10" height="10" viewBox="0 0 24 24" fill="currentColor">
							<circle cx="12" cy="12" r="10" />
						</svg>
					</div>
				</div>
			{/if}
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
					class="rounded bg-accent px-3 py-1.5 text-xs font-semibold text-ink disabled:opacity-50"
				>
					Deploy here
				</button>
			{/snippet}
			<LocationPickerBar
				lat={placeLat}
				lon={placeLon}
				locating={false}
				label="Pan the map to move the spawn point"
				oncancel={cancelPlacement}
				bind:height={placementBarHeight}
				{confirm}
			/>
		{/if}
	</div>

	<RightPanel {fleetLabel} />
</div>
