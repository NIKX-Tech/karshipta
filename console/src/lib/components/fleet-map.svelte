<script lang="ts">
	import { untrack } from 'svelte';
	import * as maplibregl from 'maplibre-gl';
	import 'maplibre-gl/dist/maplibre-gl.css';
	// maplibre-gl builds its worker URL from a dynamic template string
	// (`./${t}gl-worker.mjs`, not a static import.meta.url literal), which
	// Vite's asset analysis can't follow - the worker chunk silently never
	// gets served (dev, 404 from node_modules/.vite/deps/) or copied (prod
	// build, 404 from _app/immutable/), in both cases with zero console
	// error from maplibre itself: every GeoJSON source (trails, zones,
	// geozones, mission route) just tiles nothing forever, while markers
	// (DOM, not GL) and the base map (loaded independently) keep working,
	// making this look like a data or paint bug instead of a missing
	// worker. A `?url` import fixes serving the worker file itself, but the
	// worker's own code then imports a second chunk
	// ("./maplibre-gl-shared.mjs") via a plain relative specifier resolved
	// against wherever the worker was served from - Vite never sees that
	// import (it's inside an opaque `?url` asset, not a parsed module), so
	// that 404s too. scripts/copy-maplibre-worker.mjs (predev/prebuild)
	// copies both files as-is into static/maplibre-gl, keeping them
	// adjacent at a stable path exactly like node_modules/maplibre-gl/dist,
	// so the worker's own relative import resolves correctly.
	maplibregl.setWorkerUrl('/maplibre-gl/maplibre-gl-worker.mjs');
	import Supercluster from 'supercluster';
	import { WardOrigin } from '$lib/gen/karshipta/v1/common';
	import { ZoneType } from '$lib/gen/karshipta/v1/fleet';
	import { fleet, type DraftWaypoint } from '$lib/fleet-store.svelte';
	import { fleetGroups } from '$lib/fleet-groups/fleet-groups-store.svelte';
	import { geozoneStore } from '$lib/geozones/geozone-store.svelte';
	import { zoneStore } from '$lib/zones/zone-store.svelte';
	import { themeStore } from '$lib/theme.svelte';
	import type { ViewportBounds } from '$lib/geozones/types';

	interface Props {
		/** read once, before this component mounts - changing it afterward does not move the camera */
		centerLat: number;
		/** read once, before this component mounts - changing it afterward does not move the camera */
		centerLon: number;
		/**
		 * Fires on every map click alongside whatever the map already does
		 * with it (goto targeting, waypoint placement) - a generic escape
		 * hatch for a consuming app's own click-to-place flows (e.g. picking
		 * a spawn point for a new ward) rather than a store-level concept
		 * like fleet.gotoArming, which is specifically about commanding an
		 * already-selected ward. The caller decides whether it cares.
		 */
		onMapClick?: (latitudeDeg: number, longitudeDeg: number) => void;
		/**
		 * Shows a crosshair cursor, same as goto-targeting/waypoint-planning
		 * below - a plain prop for the same reason onMapClick is: a consuming
		 * app's own click-to-place flow (e.g. demo-ward placement) isn't a
		 * store-level concept, so it has no other way to tell the map a click
		 * means something right now.
		 */
		crosshair?: boolean;
		/**
		 * Shows a single pending-placement pin at this point - the visual
		 * counterpart to crosshair/onMapClick: without it, a click during a
		 * consuming app's placement flow (e.g. demo-ward placement) has no
		 * visible result and the operator can't tell where they clicked.
		 */
		placementPoint?: { latitudeDeg: number; longitudeDeg: number };
		/**
		 * Owner identity for the selected ward's badge - a plain callback,
		 * not a store-level concept, since "who owns this" only exists for a
		 * multi-tenant consuming app (see karshipta-cloud); the single-operator
		 * OSS console has no such notion and simply never passes this.
		 */
		ownerFor?: (wardId: string) => { username: string; photoUrl: string | null } | undefined;
	}

	const { centerLat, centerLon, onMapClick, crosshair, placementPoint, ownerFor }: Props = $props();

	// City-scale, not street-block: the first thing a viewer needs is "where
	// in the world is this", not individual streets. z15 (the old value)
	// only shows a few blocks - unreadable as a city, and the point of
	// landing on the map is immediately losing your bearings.
	const INITIAL_ZOOM = 12;

	// All free, no API key: CARTO for dark/light - the same pair the rest of
	// the UI's own theme.css tokens use, so the map follows the app-wide
	// theme toggle (themeStore) rather than exposing a second, independent
	// light/dark choice of its own; a light map under a dark app chrome (or
	// the reverse) was a real, reported problem, not a style preference.
	// Esri World Imagery for satellite - the standard no-key choice for
	// this, layered on top as an orthogonal on/off (real photography has no
	// light/dark variant to match the theme against). Esri's tile scheme is
	// z/y/x, not the usual z/x/y - easy to get backwards.
	const DARK_TILES = ['https://basemaps.cartocdn.com/dark_all/{z}/{x}/{y}.png'];
	const LIGHT_TILES = ['https://basemaps.cartocdn.com/light_all/{z}/{x}/{y}.png'];
	const SATELLITE_TILES = [
		'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}'
	];
	// Raw satellite imagery carries no place names, borders, or river labels
	// at all - Esri's own free reference overlay (transparent PNG, same
	// no-key/z-y-x deal as the imagery itself) adds them back, layered on
	// top rather than replacing the imagery. CARTO's dark/light tiles
	// already render labels as part of the tile image, so this only matters
	// for satellite.
	const SATELLITE_LABELS_TILES = [
		'https://server.arcgisonline.com/ArcGIS/rest/services/Reference/World_Boundaries_and_Places/MapServer/tile/{z}/{y}/{x}'
	];
	// One combined attribution covering every basemap this control can
	// switch to, set once at source creation: MapLibre's RasterTileSource
	// has no setAttribution() to go with setTiles(), and rewriting it on
	// every switch isn't worth the complexity this saves - showing all
	// three credits regardless of which is currently active is a common,
	// acceptable tradeoff other map apps make too.
	const BASEMAP_ATTRIBUTION =
		'&copy; OpenStreetMap contributors &copy; CARTO &copy; Esri, Maxar, Earthstar Geographics';
	const SATELLITE_STORAGE_KEY = 'karshipta:satellite';
	function readStoredSatellite(): boolean {
		if (typeof localStorage === 'undefined') return false;
		return localStorage.getItem(SATELLITE_STORAGE_KEY) === 'true';
	}
	let satellite = $state(readStoredSatellite());
	function setSatellite(value: boolean): void {
		satellite = value;
		if (typeof localStorage === 'undefined') return;
		localStorage.setItem(SATELLITE_STORAGE_KEY, String(value));
	}
	const basemapTiles = $derived(
		satellite ? SATELLITE_TILES : themeStore.current === 'light' ? LIGHT_TILES : DARK_TILES
	);

	let showGeozones = $state(true);
	let showZones = $state(true);
	let layersMenuOpen = $state(false);
	let layersMenuEl: HTMLDivElement | undefined;

	interface MarkerHandle {
		marker: maplibregl.Marker;
		body: HTMLElement;
		stem: HTMLElement;
		arrow: SVGSVGElement;
		arrowPath: SVGPathElement;
		badge: HTMLElement;
		connectivityDot: HTMLElement;
		nameEl: HTMLElement;
		telemetryEl: HTMLElement;
		ownerRow: HTMLElement;
		ownerAvatar: HTMLElement;
		ownerNameEl: HTMLElement;
	}

	const EARTH_CIRCUMFERENCE_M = 40_075_016.686;
	/** maplibre zoom is normalized to 512px world tiles */
	const WORLD_TILE_PX = 512;

	let container: HTMLDivElement;
	let map = $state<maplibregl.Map | undefined>(undefined);
	let mapError = $state<string | undefined>(undefined);
	const ROUTE_SOURCE = 'mission-route';
	const SATELLITE_LABELS_SOURCE = 'satellite-labels';
	const GEOZONE_SOURCE = 'geozones';
	const GEOZONE_FILL_LAYER = 'geozones-fill';
	const GEOZONE_LINE_LAYER = 'geozones-line';
	const ZONE_SOURCE = 'zones';
	const ZONE_FILL_LAYER = 'zones-fill';
	const ZONE_LINE_LAYER = 'zones-line';
	const ZONE_DRAFT_SOURCE = 'zone-draft';
	const ZONE_KEEP_IN_COLOR = '#22c55e';
	const ZONE_KEEP_OUT_COLOR = '#ef4444';
	// One color per ward route, cycled by index - a fleet mission plans a
	// SEPARATE route per ward (the actual fix over the old shared-route
	// design), so the map must make that visually obvious: no two routes in
	// one draft/FleetMission ever render the same color. First entry matches
	// the historical solo-route blue, so a single ward's own mission draft
	// looks unchanged. Distinct from --color-accent/--color-critical/etc.:
	// this palette carries no status meaning, only "which ward".
	const ROUTE_COLORS = ['#3b9eff', '#22c55e', '#f97316', '#a78bfa', '#ec4899', '#14b8a6'];
	function routeColorFor(index: number): string {
		return ROUTE_COLORS[index % ROUTE_COLORS.length];
	}
	const TRAIL_SOURCE = 'ward-trails';
	/**
	 * Older points age out so a long-idle tab doesn't grow this forever, but
	 * generously - a few thousand points costs nothing to render, and a trail
	 * that visibly shrinks while an operator is watching reads as broken, not
	 * as intentional cleanup. 3000 is ~10 minutes at the demo engine's 5Hz
	 * tick rate; a real gateway's telemetry rate is typically lower, so this
	 * covers an even longer duration there.
	 */
	const TRAIL_MAX_POINTS = 3000;
	const MEASURE_SOURCE = 'measure-line';
	const EARTH_RADIUS_M = 6_371_000;
	// Pixel radius supercluster groups points within, at any given zoom -
	// matches the library's own documented default, a reasonable "close
	// enough to be visual clutter" threshold at typical marker sizes.
	const CLUSTER_RADIUS_PX = 60;
	// Above this zoom, individual wards are always shown separately even if
	// still nominally close together - deep zoom is exactly when an operator
	// wants to distinguish specific wards, not see them folded into a count.
	const CLUSTER_MAX_ZOOM = 16;

	// Distance measurement: a self-contained map tool, not a store-level
	// concept like goto-arming - nothing outside this component needs to
	// know two points were clicked to measure between them.
	let measuring = $state(false);
	let measurePoints = $state<[number, number][]>([]);
	function toggleMeasuring(): void {
		measuring = !measuring;
		if (!measuring) measurePoints = [];
	}
	function haversineMeters([lon1, lat1]: [number, number], [lon2, lat2]: [number, number]): number {
		const toRad = (deg: number) => (deg * Math.PI) / 180;
		const dLat = toRad(lat2 - lat1);
		const dLon = toRad(lon2 - lon1);
		const a =
			Math.sin(dLat / 2) ** 2 +
			Math.cos(toRad(lat1)) * Math.cos(toRad(lat2)) * Math.sin(dLon / 2) ** 2;
		return 2 * EARTH_RADIUS_M * Math.asin(Math.sqrt(a));
	}
	function formatDistance(meters: number): string {
		return meters < 1000 ? `${meters.toFixed(0)} m` : `${(meters / 1000).toFixed(2)} km`;
	}

	// imperative per-ward marker cache, deliberately not reactive state
	let markers: Record<string, MarkerHandle> = {};
	// One bubble marker per supercluster cluster_id, only populated at zoom
	// levels where two or more nearby wards get grouped - see the sync
	// effect below. Wards absorbed into a cluster keep their `markers`
	// entry removed for as long as they're grouped, the same way any other
	// removed ward's marker is cleaned up.
	let clusterMarkers: Record<number, maplibregl.Marker> = {};
	// Rebuilt every time the sync effect below runs; supercluster's own
	// index build is fast enough (designed for hundreds of thousands of
	// points) that rebuilding on every telemetry tick for a self-hosted
	// deployment's ward count is not a real cost.
	let clusterIndex: Supercluster<{ wardId: string }> | undefined;
	// Numbered pins only for the ward currently being planned (the fleet
	// wizard's activeWardId, or the solo ward draft); other wards' routes
	// still draw as a colored line (see ROUTE_SOURCE below) but without
	// per-waypoint markers, so N routes stay readable instead of cluttering
	// the map with every ward's every waypoint number at once.
	let waypointMarkers: maplibregl.Marker[] = [];
	let zoneVertexMarkers: maplibregl.Marker[] = [];
	let placementMarker: maplibregl.Marker | undefined;
	let measureMarkers: maplibregl.Marker[] = [];
	let measureLabelMarker: maplibregl.Marker | undefined;
	// flown path per ward, oldest-first; client-side only, never persisted
	// or synced - purely a "where has this ward been recently" trail
	let trails: Record<string, [number, number][]> = {};
	let mapLoaded = $state(false);
	// camera state; arrows compensate for bearing, marker elevation for pitch/zoom
	let bearingDeg = $state(0);
	let pitchDeg = $state(0);
	let zoomLevel = $state(INITIAL_ZOOM);

	function markerElement(wardId: string): Omit<MarkerHandle, 'marker'> & {
		element: HTMLElement;
	} {
		// zero-size root pinned to the ground position; the body (arrow + label)
		// is lifted above it by the projected altitude, connected by a stem
		const element = document.createElement('div');
		element.setAttribute('role', 'button');
		element.setAttribute('tabindex', '0');
		element.setAttribute('aria-label', `Ward ${wardId}`);
		element.className = 'relative h-0 w-0 cursor-pointer';
		// The badge (name/telemetry/owner) only ever renders for the selected
		// ward - see the sync effect below - so it starts hidden. Unselected
		// markers show only the ground dot and arrow; a map with several
		// wards on it stays scannable instead of turning into a wall of text.
		element.innerHTML = `
			<span class="bg-accent/50 absolute -top-0.5 -left-0.5 h-1 w-1 rounded-full" data-part="ground"></span>
			<span class="bg-edge absolute left-0 w-px" data-part="stem"></span>
			<div class="absolute" data-part="body">
				<svg width="30" height="30" viewBox="0 0 34 34" class="-translate-x-1/2 -translate-y-1/2">
					<path d="M17 3 L27 29 L17 22 L7 29 Z" fill="#f5a623" stroke="#0a0e12" stroke-width="1.5" stroke-linejoin="round" />
				</svg>
				<div class="border-edge bg-panel/90 text-fg absolute top-3 left-0 hidden -translate-x-1/2 flex-col gap-0.5 rounded-sm border px-2 py-1 font-mono text-[10px] whitespace-nowrap" data-part="badge">
					<div class="flex items-center gap-1.5" data-part="name-row">
						<span class="h-1.5 w-1.5 shrink-0 rounded-full" data-part="connectivity"></span>
						<span data-part="name"></span>
					</div>
					<div data-part="telemetry"></div>
					<div class="hidden items-center gap-1.5" data-part="owner-row">
						<span class="border-edge h-3.5 w-3.5 shrink-0 rounded-full border bg-cover bg-center" data-part="owner-avatar"></span>
						<span class="text-fg-muted" data-part="owner-name"></span>
					</div>
				</div>
			</div>`;
		const body = element.querySelector<HTMLElement>('[data-part="body"]');
		const stem = element.querySelector<HTMLElement>('[data-part="stem"]');
		const arrow = element.querySelector('svg');
		const arrowPath = element.querySelector('path');
		const badge = body?.querySelector<HTMLElement>('[data-part="badge"]') ?? null;
		const connectivityDot = badge?.querySelector<HTMLElement>('[data-part="connectivity"]') ?? null;
		const nameEl = badge?.querySelector<HTMLElement>('[data-part="name"]') ?? null;
		const telemetryEl = badge?.querySelector<HTMLElement>('[data-part="telemetry"]') ?? null;
		const ownerRow = badge?.querySelector<HTMLElement>('[data-part="owner-row"]') ?? null;
		const ownerAvatar = badge?.querySelector<HTMLElement>('[data-part="owner-avatar"]') ?? null;
		const ownerNameEl = badge?.querySelector<HTMLElement>('[data-part="owner-name"]') ?? null;
		if (
			!body ||
			!stem ||
			!(arrow instanceof SVGSVGElement) ||
			!(arrowPath instanceof SVGPathElement) ||
			!badge ||
			!connectivityDot ||
			!nameEl ||
			!telemetryEl ||
			!ownerRow ||
			!ownerAvatar ||
			!ownerNameEl
		) {
			throw new Error('fleet-map: marker template is missing its parts');
		}
		const toggleSelect = (event: Event) => {
			// keep marker clicks from also registering as map clicks (goto targeting)
			event.stopPropagation();
			fleet.select(fleet.selectedWardId === wardId ? undefined : wardId);
		};
		element.addEventListener('click', toggleSelect);
		element.addEventListener('keydown', (event) => {
			if (event.key === 'Enter' || event.key === ' ') {
				event.preventDefault();
				toggleSelect(event);
			}
		});
		return {
			element,
			body,
			stem,
			arrow,
			arrowPath,
			badge,
			connectivityDot,
			nameEl,
			telemetryEl,
			ownerRow,
			ownerAvatar,
			ownerNameEl
		};
	}

	/**
	 * A cluster bubble is deliberately much simpler than an individual ward
	 * marker: a count, nothing else - it represents several wards at once,
	 * so there is no single heading/altitude/battery/owner to show. Clicking
	 * it zooms in to supercluster's own expansion zoom for that cluster,
	 * the standard "cluster click drills in" pattern. `lngLat` is read back
	 * out of the marker itself (via maplibregl.Marker.getLngLat()) at click
	 * time by the caller, not baked into this closure - see the sync effect
	 * below, which always has the current, correct coordinates on hand.
	 */
	function clusterMarkerElement(onExpand: () => void): HTMLElement {
		const element = document.createElement('button');
		element.type = 'button';
		element.className =
			'border-accent bg-accent/25 text-fg flex h-8 w-8 cursor-pointer items-center justify-center rounded-full border-2 font-mono text-xs font-semibold backdrop-blur-sm';
		element.addEventListener('click', (event) => {
			event.stopPropagation();
			onExpand();
		});
		return element;
	}

	// supercluster's cluster_id is only a stable identity within one
	// getClusters() call at a fixed zoom - the same numeric id can end up
	// reused for an unrelated grouping as zoom changes (e.g. mid-easeTo
	// animation), so a cluster marker kept alive across renders under that
	// id must have its displayed count refreshed every run, not just at
	// creation, or it silently shows a stale number.
	function updateClusterMarkerElement(element: HTMLElement, pointCount: number): void {
		element.setAttribute('aria-label', `${pointCount} wards, click to zoom in`);
		element.textContent = pointCount > 99 ? '99+' : String(pointCount);
	}

	$effect(() => {
		// untrack: this effect must run exactly once, on mount, never again.
		// centerLat/centerLon/basemapTiles are only needed to seed the initial
		// style - reading them normally would make Svelte treat every later
		// basemap/theme change as a reason to tear the whole map down and
		// rebuild it from these original values (losing the operator's pan/zoom
		// and snapping back to whatever centerLat/centerLon were at mount,
		// e.g. always Zurich for a caller passing FAKE_FLEET_CENTER). The
		// tile-swap effect further down is the sole thing that should react to
		// basemapTiles changing.
		const { lat, lon, tiles } = untrack(() => ({
			lat: centerLat,
			lon: centerLon,
			tiles: basemapTiles
		}));
		// a map init failure (e.g. no WebGL) must not take the rest of the
		// console down; ward cards keep working without the map
		let created: maplibregl.Map;
		try {
			created = new maplibregl.Map({
				container,
				center: [lon, lat],
				zoom: INITIAL_ZOOM,
				attributionControl: { compact: true },
				style: {
					version: 8,
					sources: {
						basemap: {
							type: 'raster',
							tiles,
							tileSize: 256,
							attribution: BASEMAP_ATTRIBUTION
						}
					},
					layers: [{ id: 'basemap', type: 'raster', source: 'basemap' }]
				}
			});
		} catch (error) {
			console.error('fleet-map: failed to initialize MapLibre', error);
			mapError = error instanceof Error ? error.message : String(error);
			return;
		}
		// Zoom in/out + a compass that resets bearing/pitch to north-up on
		// click - MapLibre's own control, just recolored (see the :global
		// style block below) to match the rest of the UI instead of its
		// default white box. Bottom-right: the top corners are already taken
		// (layers menu left, and a consuming app's own panels tend to hug the
		// top edge), and bottom-right is where most map apps put camera
		// controls anyway.
		created.addControl(new maplibregl.NavigationControl({ visualizePitch: true }), 'bottom-right');
		// Bottom-left: the only corner still empty, and where most map apps
		// put a scale bar - distance is otherwise unjudgeable at a glance,
		// which matters more here than on a general-purpose map (planning a
		// goto, eyeballing how far a ward still has to fly).
		created.addControl(
			new maplibregl.ScaleControl({ maxWidth: 120, unit: 'metric' }),
			'bottom-left'
		);
		created.on('click', (event) => {
			// Drawing a zone boundary takes priority over everything else -
			// the most "modal" of the map's click-driven tools, since a
			// half-drawn polygon left dangling by some other click handler
			// stealing the event would be confusing to recover from.
			if (zoneStore.draft) {
				zoneStore.addVertex(event.lngLat.lat, event.lngLat.lng);
				return;
			}
			// exclusive while active: a click means "measure from/to here",
			// not also arm a goto or override a placement point
			if (measuring) {
				measurePoints =
					measurePoints.length >= 2
						? [[event.lngLat.lng, event.lngLat.lat]]
						: [...measurePoints, [event.lngLat.lng, event.lngLat.lat]];
				return;
			}
			// A fleet-wide mission assignment being planned takes priority over
			// a solo ward's own draft; planRoute() cancels the latter before
			// starting the former, so in practice at most one is ever active.
			if (fleetGroups.missionAssignmentDraft) {
				fleetGroups.addAssignmentWaypoint(event.lngLat.lat, event.lngLat.lng);
			} else if (fleet.missionDraft && fleet.missionDraft.wardId === fleet.selectedWardId) {
				fleet.addWaypoint(event.lngLat.lat, event.lngLat.lng);
			} else {
				fleet.requestGoto(event.lngLat.lat, event.lngLat.lng);
			}
			onMapClick?.(event.lngLat.lat, event.lngLat.lng);
		});
		created.on('move', () => {
			bearingDeg = created.getBearing();
			pitchDeg = created.getPitch();
			zoomLevel = created.getZoom();
		});
		const requestGeozones = () => {
			const bounds = created.getBounds();
			const viewport: ViewportBounds = [
				bounds.getWest(),
				bounds.getSouth(),
				bounds.getEast(),
				bounds.getNorth()
			];
			geozoneStore.requestViewport(viewport);
		};
		created.on('moveend', requestGeozones);
		created.on('load', () => {
			// right above the raw imagery, below every operational overlay
			// (geozones, trails, route, measure) added below - place names must
			// never be the thing blocking a no-fly zone or a flight path.
			// Hidden by default; the tile-swap effect below shows it only when
			// satellite is the active basemap.
			created.addSource(SATELLITE_LABELS_SOURCE, {
				type: 'raster',
				tiles: SATELLITE_LABELS_TILES,
				tileSize: 256
			});
			created.addLayer({
				id: SATELLITE_LABELS_SOURCE,
				type: 'raster',
				source: SATELLITE_LABELS_SOURCE,
				layout: { visibility: 'none' }
			});
			created.addSource(ROUTE_SOURCE, {
				type: 'geojson',
				data: { type: 'FeatureCollection', features: [] }
			});
			created.addLayer({
				id: ROUTE_SOURCE,
				type: 'line',
				source: ROUTE_SOURCE,
				// color/opacity are per-feature properties (see the drawing effect
				// below), not a single static paint value: one FeatureCollection
				// now carries every selected ward's own route at once.
				paint: {
					'line-color': ['get', 'color'],
					'line-width': 2,
					'line-opacity': ['get', 'opacity'],
					'line-dasharray': [2, 1.5]
				}
			});
			// geozones sit under the route/markers: added first, so later layers paint on top
			created.addSource(GEOZONE_SOURCE, {
				type: 'geojson',
				data: { type: 'FeatureCollection', features: [] }
			});
			created.addLayer(
				{
					id: GEOZONE_FILL_LAYER,
					type: 'fill',
					source: GEOZONE_SOURCE,
					paint: {
						'fill-color': [
							'match',
							['get', 'category'],
							'prohibited',
							'#e74c3c',
							'restricted',
							'#f5a623',
							'#3b9eff'
						],
						'fill-opacity': 0.15
					}
				},
				ROUTE_SOURCE
			);
			created.addLayer(
				{
					id: GEOZONE_LINE_LAYER,
					type: 'line',
					source: GEOZONE_SOURCE,
					paint: {
						'line-color': [
							'match',
							['get', 'category'],
							'prohibited',
							'#e74c3c',
							'restricted',
							'#f5a623',
							'#3b9eff'
						],
						'line-width': 1.5,
						'line-opacity': 0.6
					}
				},
				ROUTE_SOURCE
			);
			// flown-path trails: above geozones (so a zone fill doesn't visually
			// bury them), below the mission-route line (a planned route stays
			// the most prominent line on the map) - a real Marker exists on top
			// of both regardless, so the trail never competes with the ward
			// itself for attention.
			created.addSource(TRAIL_SOURCE, {
				type: 'geojson',
				data: { type: 'FeatureCollection', features: [] }
			});
			created.addLayer(
				{
					id: TRAIL_SOURCE,
					type: 'line',
					source: TRAIL_SOURCE,
					layout: { 'line-join': 'round', 'line-cap': 'round' },
					paint: {
						'line-color': ['case', ['get', 'synthetic'], '#a78bfa', '#f5a623'],
						'line-width': 1.5,
						'line-opacity': 0.45
					}
				},
				ROUTE_SOURCE
			);
			// Operator-drawn safety zones: a parallel source/layer pair to
			// geozones (distinct data, distinct concept - fleet-mission-model.md),
			// stacked just below the route line so a planned route stays the
			// most prominent thing on the map even when it crosses a zone.
			created.addSource(ZONE_SOURCE, {
				type: 'geojson',
				data: { type: 'FeatureCollection', features: [] }
			});
			created.addLayer(
				{
					id: ZONE_FILL_LAYER,
					type: 'fill',
					source: ZONE_SOURCE,
					paint: {
						'fill-color': [
							'match',
							['get', 'type'],
							ZoneType.ZONE_TYPE_KEEP_IN,
							ZONE_KEEP_IN_COLOR,
							ZONE_KEEP_OUT_COLOR
						],
						'fill-opacity': 0.15
					}
				},
				ROUTE_SOURCE
			);
			created.addLayer(
				{
					id: ZONE_LINE_LAYER,
					type: 'line',
					source: ZONE_SOURCE,
					paint: {
						'line-color': [
							'match',
							['get', 'type'],
							ZoneType.ZONE_TYPE_KEEP_IN,
							ZONE_KEEP_IN_COLOR,
							ZONE_KEEP_OUT_COLOR
						],
						'line-width': 2
					}
				},
				ROUTE_SOURCE
			);
			// in-progress polygon being drawn: no beforeId, paints on top of
			// everything (same convention as the measurement tool below) since
			// it's the tool the operator is actively using right now
			created.addSource(ZONE_DRAFT_SOURCE, {
				type: 'geojson',
				data: { type: 'FeatureCollection', features: [] }
			});
			created.addLayer({
				id: `${ZONE_DRAFT_SOURCE}-fill`,
				type: 'fill',
				source: ZONE_DRAFT_SOURCE,
				filter: ['==', ['geometry-type'], 'Polygon'],
				paint: { 'fill-color': '#3b9eff', 'fill-opacity': 0.15 }
			});
			created.addLayer({
				id: `${ZONE_DRAFT_SOURCE}-line`,
				type: 'line',
				source: ZONE_DRAFT_SOURCE,
				paint: { 'line-color': '#3b9eff', 'line-width': 2, 'line-dasharray': [2, 1.5] }
			});
			// measurement line: no beforeId, so it paints above everything else
			// (route, geozones, trails) - it's the tool the operator is actively
			// using right now, not background context
			created.addSource(MEASURE_SOURCE, {
				type: 'geojson',
				data: { type: 'Feature', properties: {}, geometry: { type: 'LineString', coordinates: [] } }
			});
			created.addLayer({
				id: MEASURE_SOURCE,
				type: 'line',
				source: MEASURE_SOURCE,
				paint: { 'line-color': '#e8edf2', 'line-width': 2, 'line-dasharray': [3, 2] }
			});
			// MapLibre's own compact-attribution logic (verified directly in its
			// bundled source, not guessed) initializes already expanded: its
			// _updateCompact() adds both the "open" attribute AND the
			// maplibregl-compact-show class on construction. The "open"
			// attribute is a red herring here - _toggleAttribution's own
			// open/close branches both set it unconditionally; the class is
			// what CSS actually keys the visible/hidden state off (see
			// .maplibregl-compact-show .maplibregl-ctrl-attrib-inner in
			// maplibre-gl.css). Removing just that class, matching exactly what
			// _toggleAttribution's own "close" branch does, collapses it and
			// leaves the control's click handler free to reopen it normally -
			// removing the "open" attribute instead (an earlier, wrong attempt)
			// looked like it worked on first paint but didn't survive a click.
			container
				.querySelector('.maplibregl-ctrl-attrib')
				?.classList.remove('maplibregl-compact-show');
			mapLoaded = true;
			requestGeozones();
		});
		map = created;
		return () => {
			markers = {};
			waypointMarkers = [];
			zoneVertexMarkers = [];
			placementMarker = undefined;
			trails = {};
			measureMarkers = [];
			measureLabelMarker = undefined;
			measurePoints = [];
			measuring = false;
			mapLoaded = false;
			created.remove();
			map = undefined;
		};
	});

	// Pan to a ward once when it's selected (sidebar click, marker click,
	// or a /pilots/[username] deep link), not continuously - lastFocusedId
	// guards against re-panning on every subsequent telemetry tick for the
	// same selection, which would fight the operator's own camera control.
	// Reading state.position here (not untracked) is deliberate: a deep
	// link can select a ward before its telemetry has arrived, and this
	// needs to re-fire once position actually shows up for it.
	let lastFocusedWardId: string | undefined;
	$effect(() => {
		const selectedId = fleet.selectedWardId;
		const activeMap = map;
		if (!selectedId || !activeMap) {
			lastFocusedWardId = undefined;
			return;
		}
		if (lastFocusedWardId === selectedId) return;
		const position = fleet.wards[selectedId]?.state?.position;
		if (!position) return;
		lastFocusedWardId = selectedId;
		activeMap.easeTo({
			center: [position.longitudeDeg, position.latitudeDeg],
			duration: 600
		});
	});

	// crosshair cursor while goto targeting, waypoint planning, measuring, or
	// a consuming app's own click-to-place flow (crosshair prop) is active
	$effect(() => {
		if (!map) return;
		const planning = fleet.missionDraft?.wardId === fleet.selectedWardId && fleet.missionDraft;
		map.getCanvas().style.cursor =
			fleet.gotoArming ||
			planning ||
			fleetGroups.missionAssignmentDraft ||
			zoneStore.draft ||
			crosshair ||
			measuring
				? 'crosshair'
				: '';
	});

	// swap the raster tile source in place when the theme or satellite
	// choice changes - cheaper than map.setStyle(), which tears down and
	// rebuilds every source/layer (route, geozones, markers) this component
	// owns, not just the basemap
	$effect(() => {
		const activeMap = map;
		const tiles = basemapTiles;
		const showLabels = satellite;
		if (!activeMap || !mapLoaded) return;
		const source = activeMap.getSource<maplibregl.RasterTileSource>('basemap');
		source?.setTiles(tiles);
		activeMap.setLayoutProperty(
			SATELLITE_LABELS_SOURCE,
			'visibility',
			showLabels ? 'visible' : 'none'
		);
	});

	// no-fly-zone layer visibility, independent of whether zones are loaded
	// at all (geozoneStore.active, gated on the control below even
	// existing) - a viewer who doesn't want the clutter can turn it off
	// without losing the underlying data/fetches
	$effect(() => {
		const activeMap = map;
		const visible = showGeozones;
		if (!activeMap || !mapLoaded) return;
		const visibility = visible ? 'visible' : 'none';
		activeMap.setLayoutProperty(GEOZONE_FILL_LAYER, 'visibility', visibility);
		activeMap.setLayoutProperty(GEOZONE_LINE_LAYER, 'visibility', visibility);
	});

	// close the layers menu on an outside click or Escape, same convention
	// as any other dismissable popover in this app
	$effect(() => {
		if (!layersMenuOpen) return;
		const handlePointerDown = (event: PointerEvent) => {
			if (layersMenuEl && !layersMenuEl.contains(event.target as Node)) layersMenuOpen = false;
		};
		const handleKeydown = (event: KeyboardEvent) => {
			if (event.key === 'Escape') layersMenuOpen = false;
		};
		window.addEventListener('pointerdown', handlePointerDown);
		window.addEventListener('keydown', handleKeydown);
		return () => {
			window.removeEventListener('pointerdown', handlePointerDown);
			window.removeEventListener('keydown', handleKeydown);
		};
	});

	// push loaded airspace zones into their source whenever the store updates
	$effect(() => {
		const activeMap = map;
		if (!activeMap || !mapLoaded) return;
		const source = activeMap.getSource<maplibregl.GeoJSONSource>(GEOZONE_SOURCE);
		source?.setData({
			type: 'FeatureCollection',
			features: geozoneStore.zones.map((zone) => ({
				...zone.polygon,
				properties: { category: zone.category, name: zone.name }
			}))
		});
	});

	// push saved operator-drawn zones into their source whenever the store updates
	$effect(() => {
		const activeMap = map;
		if (!activeMap || !mapLoaded) return;
		const source = activeMap.getSource<maplibregl.GeoJSONSource>(ZONE_SOURCE);
		source?.setData({
			type: 'FeatureCollection',
			features: Object.values(zoneStore.zones)
				.filter((zone) => zone.vertices.length >= 3)
				.map((zone) => {
					const ring = zone.vertices.map((vertex): [number, number] => [
						vertex.longitudeDeg,
						vertex.latitudeDeg
					]);
					ring.push(ring[0]);
					return {
						type: 'Feature',
						properties: { type: zone.type, name: zone.name },
						geometry: { type: 'Polygon', coordinates: [ring] }
					};
				})
		});
	});

	// draw the zone currently being drawn: a growing line until the 3rd
	// vertex, then a fillable closed polygon (the ring is only closed for
	// rendering here - the operator's own vertex list, and what gets sent
	// to CreateZone, never gains that duplicate closing point).
	$effect(() => {
		const activeMap = map;
		if (!activeMap || !mapLoaded) return;
		const draft = zoneStore.draft;
		const vertices = draft?.vertices ?? [];
		const source = activeMap.getSource<maplibregl.GeoJSONSource>(ZONE_DRAFT_SOURCE);
		const coordinates: [number, number][] = vertices.map((vertex) => [
			vertex.longitudeDeg,
			vertex.latitudeDeg
		]);
		if (coordinates.length >= 3) {
			source?.setData({
				type: 'FeatureCollection',
				features: [
					{
						type: 'Feature',
						properties: {},
						geometry: { type: 'Polygon', coordinates: [[...coordinates, coordinates[0]]] }
					}
				]
			});
		} else {
			source?.setData({
				type: 'FeatureCollection',
				features:
					coordinates.length >= 2
						? [
								{
									type: 'Feature',
									properties: {},
									geometry: { type: 'LineString', coordinates }
								}
							]
						: []
			});
		}

		while (zoneVertexMarkers.length > coordinates.length) {
			zoneVertexMarkers.pop()?.remove();
		}
		coordinates.forEach(([lon, lat], index) => {
			let marker = zoneVertexMarkers[index];
			if (!marker) {
				const element = document.createElement('div');
				element.className =
					'border-selected bg-panel text-selected flex h-5 w-5 items-center justify-center rounded-full border font-mono text-[10px]';
				element.setAttribute('aria-label', `Zone vertex ${index + 1}`);
				element.textContent = String(index + 1);
				marker = new maplibregl.Marker({ element }).setLngLat([lon, lat]).addTo(activeMap);
				zoneVertexMarkers[index] = marker;
			}
			marker.setLngLat([lon, lat]);
		});
	});

	// Draw the mission draft of the selected ward, or every ward's own route
	// from a fleet mission draft, as one colored dashed line per ward - the
	// actual visual fix over the old design: N distinct lines prove N
	// distinct routes, not one shared line duplicated onto every marker.
	// Mutually exclusive (planRoute() cancels the ward-scoped draft before
	// starting the fleet-scoped one), so at most one ever has routes at a
	// time. Numbered pins only render for the route currently being edited
	// (activeWardId, or the solo ward draft) - see waypointMarkers' comment.
	$effect(() => {
		const activeMap = map;
		if (!activeMap || !mapLoaded) return;
		const wardDraft =
			fleet.missionDraft && fleet.missionDraft.wardId === fleet.selectedWardId
				? fleet.missionDraft
				: undefined;
		const fleetDraft = fleetGroups.missionAssignmentDraft;

		const features: GeoJSON.Feature<
			GeoJSON.LineString,
			{ wardId: string; color: string; opacity: number }
		>[] = [];
		let activeWaypoints: DraftWaypoint[] = [];
		if (wardDraft) {
			activeWaypoints = wardDraft.waypoints;
			features.push({
				type: 'Feature',
				properties: { wardId: wardDraft.wardId, color: routeColorFor(0), opacity: 1 },
				geometry: {
					type: 'LineString',
					coordinates: wardDraft.waypoints.map((wp) => [wp.longitudeDeg, wp.latitudeDeg])
				}
			});
		} else if (fleetDraft) {
			activeWaypoints = fleetDraft.routes[fleetDraft.activeWardId] ?? [];
			fleetDraft.wardIds.forEach((wardId, index) => {
				features.push({
					type: 'Feature',
					properties: {
						wardId,
						color: routeColorFor(index),
						opacity: wardId === fleetDraft.activeWardId ? 1 : 0.55
					},
					geometry: {
						type: 'LineString',
						coordinates: (fleetDraft.routes[wardId] ?? []).map((wp) => [
							wp.longitudeDeg,
							wp.latitudeDeg
						])
					}
				});
			});
		}

		const source = activeMap.getSource<maplibregl.GeoJSONSource>(ROUTE_SOURCE);
		source?.setData({ type: 'FeatureCollection', features });

		const coordinates = activeWaypoints.map((waypoint) => [
			waypoint.longitudeDeg,
			waypoint.latitudeDeg
		]);
		while (waypointMarkers.length > coordinates.length) {
			waypointMarkers.pop()?.remove();
		}
		coordinates.forEach(([lon, lat], index) => {
			let marker = waypointMarkers[index];
			if (!marker) {
				const element = document.createElement('div');
				element.className =
					'border-selected bg-panel text-selected flex h-5 w-5 items-center justify-center rounded-full border font-mono text-[10px]';
				element.setAttribute('aria-label', `Waypoint ${index + 1}`);
				element.textContent = String(index + 1);
				marker = new maplibregl.Marker({ element }).setLngLat([lon, lat]).addTo(activeMap);
				waypointMarkers[index] = marker;
			}
			marker.setLngLat([lon, lat]);
		});
	});

	// single pending-placement pin, shown while a consuming app's placement
	// flow has a point picked (geolocated default or a click override) and
	// not yet confirmed - distinct styling from both ward arrows (not a
	// real ward yet) and numbered waypoint markers (not a mission)
	$effect(() => {
		const activeMap = map;
		if (!activeMap || !mapLoaded) return;
		if (!placementPoint) {
			placementMarker?.remove();
			placementMarker = undefined;
			return;
		}
		const lngLat: [number, number] = [placementPoint.longitudeDeg, placementPoint.latitudeDeg];
		if (!placementMarker) {
			const element = document.createElement('div');
			element.className =
				'border-accent bg-panel text-accent flex h-6 w-6 items-center justify-center rounded-full border-2';
			element.setAttribute('aria-label', 'Selected spawn point');
			element.innerHTML =
				'<svg width="10" height="10" viewBox="0 0 24 24" fill="currentColor"><circle cx="12" cy="12" r="10"/></svg>';
			placementMarker = new maplibregl.Marker({ element }).setLngLat(lngLat).addTo(activeMap);
		}
		placementMarker.setLngLat(lngLat);
	});

	// draw the measurement tool's points, connecting line, and distance label
	$effect(() => {
		const activeMap = map;
		if (!activeMap || !mapLoaded) return;

		while (measureMarkers.length > measurePoints.length) {
			measureMarkers.pop()?.remove();
		}
		measurePoints.forEach(([lon, lat], index) => {
			let marker = measureMarkers[index];
			if (!marker) {
				const element = document.createElement('div');
				element.className = 'border-fg bg-panel h-2.5 w-2.5 rounded-full border-2';
				element.setAttribute('aria-label', `Measurement point ${index + 1}`);
				marker = new maplibregl.Marker({ element }).setLngLat([lon, lat]).addTo(activeMap);
				measureMarkers[index] = marker;
			}
			marker.setLngLat([lon, lat]);
		});

		const source = activeMap.getSource<maplibregl.GeoJSONSource>(MEASURE_SOURCE);
		source?.setData({
			type: 'Feature',
			properties: {},
			geometry: { type: 'LineString', coordinates: measurePoints }
		});

		if (measurePoints.length === 2) {
			const [a, b] = measurePoints;
			const midpoint: [number, number] = [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2];
			if (!measureLabelMarker) {
				const element = document.createElement('div');
				element.className =
					'border-edge bg-panel/95 text-fg rounded border px-1.5 py-0.5 font-mono text-[10px] whitespace-nowrap';
				measureLabelMarker = new maplibregl.Marker({ element })
					.setLngLat(midpoint)
					.addTo(activeMap);
			}
			measureLabelMarker.setLngLat(midpoint);
			measureLabelMarker.getElement().textContent = formatDistance(haversineMeters(a, b));
		} else {
			measureLabelMarker?.remove();
			measureLabelMarker = undefined;
		}
	});

	// sync one marker per ward from the store, folding nearby wards into a
	// count-bubble cluster marker at low zoom (github issue: console-core
	// map scalability). fleet-map.svelte renders wards as individual
	// maplibregl.Marker DOM objects, not a GeoJSON layer, so MapLibre's own
	// cluster: true does not apply - supercluster (the library MapLibre's
	// clustering is itself built on) does the same grouping client-side.
	$effect(() => {
		if (!map) return;

		const points: Array<GeoJSON.Feature<GeoJSON.Point, { wardId: string }>> = [];
		for (const wardId of fleet.wardIds) {
			const position = fleet.wards[wardId]?.state?.position;
			if (!position) continue;
			points.push({
				type: 'Feature',
				properties: { wardId },
				geometry: { type: 'Point', coordinates: [position.longitudeDeg, position.latitudeDeg] }
			});
		}
		clusterIndex = new Supercluster<{ wardId: string }>({
			radius: CLUSTER_RADIUS_PX,
			maxZoom: CLUSTER_MAX_ZOOM
		}).load(points);
		const clusters = clusterIndex.getClusters([-180, -85, 180, 85], Math.floor(zoomLevel));

		// Every point supercluster returns is either the original leaf
		// (ungrouped - render its full individual marker below) or a
		// synthetic cluster point standing in for two or more wards. Any
		// wardId from the input set that isn't one of the leaves in this
		// result is, by construction, inside some cluster - built in one
		// pass over `clusters` (not by starting from "everything" and
		// deleting entries, which svelte/prefer-svelte-reactivity flags as
		// a suspicious post-construction Set mutation even though this Set
		// never leaves this effect or touches the template).
		const leafWardIds = new Set(
			clusters.flatMap((feature) =>
				'cluster' in feature.properties ? [] : [feature.properties.wardId]
			)
		);
		const clusteredWardIds = new Set(fleet.wardIds.filter((wardId) => !leafWardIds.has(wardId)));
		const activeClusterIds = new Set(
			clusters.flatMap((feature) =>
				'cluster' in feature.properties ? [feature.properties.cluster_id] : []
			)
		);

		for (const feature of clusters) {
			if (!('cluster' in feature.properties)) continue;
			const clusterId = feature.properties.cluster_id;
			const pointCount = feature.properties.point_count;
			const [lon, lat] = feature.geometry.coordinates;
			let clusterMarker = clusterMarkers[clusterId];
			if (!clusterMarker) {
				const index = clusterIndex;
				const element = clusterMarkerElement(() => {
					const activeMap = map;
					if (!activeMap || !index) return;
					let expansionZoom: number;
					try {
						expansionZoom = index.getClusterExpansionZoom(clusterId);
					} catch {
						// stale cluster_id (index rebuilt since this marker was
						// created, a click racing a telemetry update) - harmless
						// to just ignore rather than crash.
						return;
					}
					activeMap.easeTo({ zoom: expansionZoom, center: [lon, lat] });
				});
				clusterMarker = new maplibregl.Marker({ element }).setLngLat([lon, lat]).addTo(map);
				clusterMarkers[clusterId] = clusterMarker;
			}
			clusterMarker.setLngLat([lon, lat]);
			updateClusterMarkerElement(clusterMarker.getElement(), pointCount);
		}
		for (const [clusterIdKey, clusterMarker] of Object.entries(clusterMarkers)) {
			const clusterId = Number(clusterIdKey);
			if (!activeClusterIds.has(clusterId)) {
				clusterMarker.remove();
				delete clusterMarkers[clusterId];
			}
		}

		for (const wardId of fleet.wardIds) {
			const ward = fleet.wards[wardId];
			const state = ward?.state;
			if (!state?.position) continue;
			// flown-path trail: skip appending when the ward hasn't actually
			// moved (idle/disarmed on the ground), so the array doesn't grow
			// for no visual benefit. Kept regardless of clustering - a ward's
			// flown path is independent of whether its marker is currently
			// folded into a cluster bubble.
			const point: [number, number] = [state.position.longitudeDeg, state.position.latitudeDeg];
			const trail = (trails[wardId] ??= []);
			const lastPoint = trail.at(-1);
			if (!lastPoint || lastPoint[0] !== point[0] || lastPoint[1] !== point[1]) {
				trail.push(point);
				if (trail.length > TRAIL_MAX_POINTS) trail.shift();
			}
			if (clusteredWardIds.has(wardId)) {
				// Folded into a cluster bubble this tick - drop any individual
				// marker left over from before it was grouped.
				markers[wardId]?.marker.remove();
				delete markers[wardId];
				continue;
			}
			let handle = markers[wardId];
			if (!handle) {
				const { element, ...parts } = markerElement(wardId);
				// setLngLat must precede addTo: adding projects the position
				const marker = new maplibregl.Marker({ element })
					.setLngLat([state.position.longitudeDeg, state.position.latitudeDeg])
					.addTo(map);
				handle = { marker, ...parts };
				markers[wardId] = handle;
			}
			handle.marker.setLngLat([state.position.longitudeDeg, state.position.latitudeDeg]);
			// heading is relative to true north; subtract the camera bearing so
			// the arrow stays correct when the operator rotates the map
			handle.arrow.style.rotate = `${state.headingDeg - bearingDeg}deg`;
			const selected = fleet.selectedWardId === wardId;
			// Unselected: no badge at all, so the map stays scannable with
			// several wards on it - just the arrow and ground position.
			// The full badge (name, connectivity, altitude, battery, owner)
			// only earns its place once a ward is actually picked.
			handle.badge.classList.toggle('hidden', !selected);
			handle.badge.classList.toggle('flex', selected);
			if (selected) {
				handle.nameEl.textContent = wardId;
				handle.connectivityDot.classList.toggle('bg-accent', state.connected);
				handle.connectivityDot.classList.toggle('animate-pulse', state.connected);
				handle.connectivityDot.classList.toggle('bg-critical', !state.connected);
				const batteryPct = state.battery?.remainingPct;
				const batteryLabel =
					batteryPct === undefined || batteryPct < 0 ? '?' : `${batteryPct.toFixed(0)}%`;
				handle.telemetryEl.textContent = `${state.position.altitudeRelM.toFixed(0)} m \u00b7 ${batteryLabel}`;
				const owner = ownerFor?.(wardId);
				handle.ownerRow.classList.toggle('hidden', !owner);
				handle.ownerRow.classList.toggle('flex', !!owner);
				if (owner) {
					handle.ownerNameEl.textContent = `@${owner.username}`;
					handle.ownerAvatar.style.backgroundImage = owner.photoUrl
						? `url(${JSON.stringify(owner.photoUrl)})`
						: '';
				}
			}
			// lift the body above the ground anchor by the projected altitude:
			// vertical world axis maps to screen-vertical, scaled by sin(pitch)
			const pxPerMeter =
				(WORLD_TILE_PX * Math.pow(2, zoomLevel)) /
				(EARTH_CIRCUMFERENCE_M * Math.cos((state.position.latitudeDeg * Math.PI) / 180));
			const liftPx =
				state.position.altitudeRelM * pxPerMeter * Math.sin((pitchDeg * Math.PI) / 180);
			handle.body.style.top = `${-liftPx}px`;
			handle.stem.style.top = `${-liftPx}px`;
			handle.stem.style.height = `${Math.max(0, liftPx - 8)}px`;
			handle.arrowPath.setAttribute('stroke', selected ? '#3b9eff' : '#0a0e12');
			handle.arrowPath.setAttribute('stroke-width', selected ? '2.5' : '1.5');
			// No autopilot behind this ward at all (see ward-card.svelte): a
			// distinct hue, not a desaturated one, so it doesn't read as
			// disabled/degraded - that's what the opacity fade below already
			// means (link lost), and the two must not look like the same thing.
			const synthetic = ward?.info?.origin === WardOrigin.WARD_ORIGIN_SYNTHETIC;
			handle.arrowPath.setAttribute('fill', synthetic ? '#a78bfa' : '#f5a623');
			handle.badge.classList.toggle('border-selected', selected);
			handle.badge.classList.toggle('border-edge', !selected);
			// link lost: fade the marker so a stale last-known position doesn't
			// read as live
			handle.body.style.opacity = state.connected ? '1' : '0.4';
		}
		for (const wardId of Object.keys(markers)) {
			if (!fleet.wardIds.includes(wardId)) {
				markers[wardId].marker.remove();
				delete markers[wardId];
			}
		}
		for (const wardId of Object.keys(trails)) {
			if (!fleet.wardIds.includes(wardId)) delete trails[wardId];
		}
		const trailSource = map.getSource<maplibregl.GeoJSONSource>(TRAIL_SOURCE);
		trailSource?.setData({
			type: 'FeatureCollection',
			// Accumulation above runs for every ward regardless of selection,
			// so a trail is already there the moment a ward gets selected
			// instead of starting empty - only the render is gated, so an
			// unselected fleet's worth of trails doesn't clutter the map.
			features: Object.entries(trails)
				.filter(([wardId, points]) => wardId === fleet.selectedWardId && points.length > 1)
				.map(([wardId, points]) => ({
					type: 'Feature',
					properties: {
						synthetic: fleet.wards[wardId]?.info?.origin === WardOrigin.WARD_ORIGIN_SYNTHETIC
					},
					geometry: { type: 'LineString', coordinates: points }
				}))
		});
	});
</script>

<div class="relative h-full w-full">
	<!--
		MapLibre takes ownership of this div's contents (canvas, its own
		control divs) and paints over anything already inside it, so overlays
		must be siblings here, not children of the bound container below.
	-->
	<div bind:this={container} class="h-full w-full" aria-label="Fleet map"></div>
	{#if mapError}
		<p
			role="alert"
			class="absolute inset-x-0 top-1/2 mx-auto w-fit max-w-lg rounded border border-critical bg-panel px-4 py-2 text-sm text-critical"
		>
			Map unavailable: {mapError}
		</p>
	{/if}

	<!--
		Bottom-right, stacked directly above MapLibre's own NavigationControl
		(zoom/compass, also bottom-right) - one control cluster instead of two
		spatially separate ones, since these are all "controls that change how
		you look at the map" and grouping them reads as more organized than
		scattering them across corners. Measure stacks above map style, in the
		same column, for the same reason.
	-->
	<div class="absolute right-[10px] bottom-[155px] flex flex-col items-end gap-2.5">
		<button
			type="button"
			onclick={toggleMeasuring}
			aria-pressed={measuring}
			aria-label="Measure distance"
			class="flex h-8 w-8 items-center justify-center rounded border {measuring
				? 'border-accent bg-accent/15 text-accent'
				: 'border-edge bg-panel/90 text-fg-muted hover:border-fg-muted hover:text-fg'}"
		>
			<svg
				width="15"
				height="15"
				viewBox="0 0 24 24"
				fill="none"
				stroke="currentColor"
				stroke-width="1.75"
				stroke-linejoin="round"
			>
				<path d="M4 16 L16 4 L20 8 L8 20 Z" />
				<path d="M8.5 15.5 L10.5 13.5" stroke-linecap="round" />
				<path d="M11.5 12.5 L13.5 10.5" stroke-linecap="round" />
				<path d="M14.5 9.5 L16 8" stroke-linecap="round" />
			</svg>
		</button>
		<div bind:this={layersMenuEl} class="relative">
			<button
				type="button"
				onclick={() => (layersMenuOpen = !layersMenuOpen)}
				aria-expanded={layersMenuOpen}
				aria-label="Map layers"
				class="flex h-8 w-8 items-center justify-center rounded border border-edge bg-panel/90 text-fg-muted hover:border-fg-muted hover:text-fg"
			>
				<svg
					width="15"
					height="15"
					viewBox="0 0 24 24"
					fill="none"
					stroke="currentColor"
					stroke-width="1.75"
					stroke-linejoin="round"
				>
					<path d="M12 3 L21 8 L12 13 L3 8 Z" />
					<path d="M3 12 L12 17 L21 12" stroke-linecap="round" />
					<path d="M3 16 L12 21 L21 16" stroke-linecap="round" />
				</svg>
			</button>
			{#if layersMenuOpen}
				<div
					class="absolute top-1/2 right-10 flex w-40 -translate-y-1/2 flex-col gap-3 rounded border border-edge bg-panel/95 p-2.5"
				>
					<div>
						<p class="mb-1.5 text-[9px] font-medium tracking-widest text-fg-muted">MAP STYLE</p>
						<!-- A choice, not an on/off switch - checkmarks against the
					     selected option, not a checkbox, since exactly one is always
					     active. Light/dark isn't offered here: it follows the
					     app-wide theme toggle (themeStore), same as everything else. -->
						<div class="flex flex-col gap-0.5">
							<button
								type="button"
								onclick={() => setSatellite(false)}
								aria-pressed={!satellite}
								class="flex items-center justify-between rounded px-2 py-1 text-left text-[11px] {!satellite
									? 'bg-accent/15 text-accent'
									: 'text-fg-muted hover:bg-white/5 hover:text-fg'}"
							>
								Map
								{#if !satellite}<span aria-hidden="true">&check;</span>{/if}
							</button>
							<button
								type="button"
								onclick={() => setSatellite(true)}
								aria-pressed={satellite}
								class="flex items-center justify-between rounded px-2 py-1 text-left text-[11px] {satellite
									? 'bg-accent/15 text-accent'
									: 'text-fg-muted hover:bg-white/5 hover:text-fg'}"
							>
								Satellite
								{#if satellite}<span aria-hidden="true">&check;</span>{/if}
							</button>
						</div>
					</div>
					{#if geozoneStore.active}
						<div class="border-t border-edge pt-2">
							<label class="flex cursor-pointer items-center gap-2 px-2 text-[11px]">
								<input type="checkbox" bind:checked={showGeozones} class="accent-accent" />
								No-fly zones
							</label>
						</div>
					{/if}
					<div class="border-t border-edge pt-2">
						<label class="flex cursor-pointer items-center gap-2 px-2 text-[11px]">
							<input type="checkbox" bind:checked={showZones} class="accent-accent" />
							Zones
						</label>
					</div>
				</div>
			{/if}
		</div>
	</div>

	{#if zoneStore.zoneIds.length > 0 && showZones}
		<ul
			class="absolute left-4 flex gap-3 rounded border border-edge bg-panel/90 px-2 py-1 {geozoneStore.active &&
			showGeozones
				? 'bottom-16'
				: 'bottom-8'}"
			aria-label="Zone legend"
		>
			<li class="flex items-center gap-1 text-[10px]">
				<span class="h-2 w-2 rounded-full" style="background-color: {ZONE_KEEP_IN_COLOR}"></span>
				Keep in
			</li>
			<li class="flex items-center gap-1 text-[10px]">
				<span class="h-2 w-2 rounded-full" style="background-color: {ZONE_KEEP_OUT_COLOR}"></span>
				Keep out
			</li>
		</ul>
	{/if}

	{#if geozoneStore.active && showGeozones}
		<ul
			class="absolute bottom-8 left-4 flex gap-3 rounded border border-edge bg-panel/90 px-2 py-1"
			aria-label="Airspace zone legend"
		>
			<li class="flex items-center gap-1 text-[10px]">
				<span class="h-2 w-2 rounded-full" style="background-color: #e74c3c"></span>
				Prohibited
			</li>
			<li class="flex items-center gap-1 text-[10px]">
				<span class="h-2 w-2 rounded-full" style="background-color: #f5a623"></span>
				Restricted
			</li>
			<li class="flex items-center gap-1 text-[10px]">
				<span class="h-2 w-2 rounded-full" style="background-color: #3b9eff"></span>
				Other airspace
			</li>
		</ul>
	{/if}
</div>

<style>
	/*
	 * MapLibre's NavigationControl ships hard-coded for its own default
	 * white button background (#333 icon glyphs baked into embedded SVG data
	 * URIs, not recolorable via currentColor). :global() since MapLibre
	 * injects these nodes directly via its own DOM APIs, not through this
	 * component's template - Svelte's scoped-class attribute never reaches
	 * them. filter:invert flips the dark icon light without touching the
	 * vendor's asset; simplest fix that doesn't fork the library's SVGs.
	 */
	:global(.maplibregl-ctrl-group) {
		background: var(--color-panel);
		border: 1px solid var(--color-edge);
		box-shadow: none;
	}
	:global(.maplibregl-ctrl-group button + button) {
		border-top: 1px solid var(--color-edge);
	}
	:global(.maplibregl-ctrl-group button) {
		background: transparent;
	}
	:global(.maplibregl-ctrl-group button:hover) {
		background: rgb(255 255 255 / 0.05);
	}
	:global(.maplibregl-ctrl-icon) {
		filter: invert(1) opacity(0.7);
	}
	/*
	 * The compass gets its own icon instead of MapLibre's (whose default
	 * reads as an abstract wedge, not a compass): a classic needle - north
	 * half in the amber accent, south half muted - which stays meaningful
	 * while MapLibre rotates it with the camera bearing. Ours is drawn in
	 * the design system's own colors already, so the invert filter above
	 * must not apply.
	 */
	/* MapLibre's own rule (.maplibregl-ctrl button.maplibregl-ctrl-compass
	 * .maplibregl-ctrl-icon) is more specific than a plain two-class
	 * selector - !important to actually win regardless, matching this same
	 * file's existing attrib overrides below. */
	:global(.maplibregl-ctrl-compass .maplibregl-ctrl-icon) {
		filter: none !important;
		background-image: url("data:image/svg+xml;charset=utf-8,%3Csvg xmlns='http://www.w3.org/2000/svg' width='29' height='29' viewBox='0 0 29 29'%3E%3Cpath d='M14.5 4.5 L18 14.5 L11 14.5 Z' fill='%23f5a623'/%3E%3Cpath d='M14.5 24.5 L11 14.5 L18 14.5 Z' fill='%238b98a5'/%3E%3C/svg%3E") !important;
	}
	:global(.maplibregl-ctrl-attrib) {
		background: transparent !important;
		color: var(--color-fg-muted) !important;
		font-size: 10px !important;
	}
	/*
	 * MapLibre sizes this container to its own default 24px button
	 * (min-height:20px) and absolutely-positions the button inside it - once
	 * the button became 32px (below) to match this file's other controls, it
	 * started overflowing the container's bottom edge by ~12px, silently
	 * eating most of the container's own 10px margin from the viewport edge
	 * (confirmed directly: computed margin was still 10px, but the button's
	 * actual distance from the bottom of the screen was only 2px). Matching
	 * min-height to the button's real size fixes the margin without touching
	 * the margin rule itself.
	 */
	/* :not(.maplibregl-compact-show), not just .maplibregl-compact: this
	   min-height is only needed collapsed, to stop the enlarged button
	   (below) from overflowing the box MapLibre originally sized for its own
	   24px default. Applying it unconditionally also forced the EXPANDED
	   box 12px taller than its actual text content needs - confirmed
	   directly (measured 21px of empty space below the text vs 9px above
	   it, from content sized to a min-height meant for the collapsed state
	   leaking into the open one) - which is what read as bad/uneven
	   vertical padding. */
	:global(.maplibregl-ctrl-attrib.maplibregl-compact:not(.maplibregl-compact-show)) {
		min-height: 32px !important;
	}
	:global(.maplibregl-ctrl-attrib.maplibregl-compact) {
		/* MapLibre's own 10px margin on every side stacks with the
		   NavigationControl's own bottom margin above it (two independent
		   controls, margins don't collapse in this flex stack), producing a
		   20px gap there while every other gap in this custom stack is 10px -
		   dropping this control's own top margin leaves the 20px as
		   NavigationControl's single 10px margin instead, consistent with
		   the rest of the stack. */
		margin-top: 0 !important;
	}
	:global(.maplibregl-ctrl-attrib a) {
		color: var(--color-fg-muted) !important;
	}
	/* Expanded (copyright text visible): a proper panel like this file's
	   other popovers (layers menu), not a bare row of text with no visual
	   container - more breathing room than MapLibre's own tight 2px
	   top/bottom padding gave it. Right padding is 32px (the button) + 16px
	   real gap, not the earlier 34px (button + a bare 2px) that read as the
	   text running straight into the button. */
	:global(.maplibregl-ctrl-attrib.maplibregl-compact-show) {
		background: var(--color-panel) !important;
		border: 1px solid var(--color-edge) !important;
		border-radius: 0.25rem !important;
		padding: 7px 48px 7px 12px !important;
	}
	/*
	 * The attribution toggle is a native <summary>, styled by MapLibre with
	 * its own semi-transparent white circle and a black "i" baked into the
	 * background-image data URI (not recolorable via currentColor, same
	 * constraint as the compass icon above). A plain `filter: invert(1)` (the
	 * compass fix's usual trick) doesn't work here the way it does there:
	 * this element's OWN background-color is also being overridden below to
	 * match the panel theme, and invert flips that too, turning the intended
	 * dark button light - confirmed directly (computed style showed the
	 * correct dark rgb(20,26,33) background, inverted to a light gray by the
	 * filter). Recoloring the icon directly, like the compass fix already
	 * does for the same reason, avoids the conflict.
	 */
	:global(.maplibregl-ctrl-attrib-button) {
		/* MapLibre's own box is 24x24 with no explicit background-size/position,
		   so the 24x24 icon happened to fill it exactly - adding a border ate
		   into that available box (background defaults to the padding box,
		   inside the border) without anything compensating, which is what
		   pushed the icon off-center. Matching this file's other 32px control
		   buttons (h-8/w-8) and being explicit about size/position fixes both
		   the mismatched size and the centering in one pass. */
		width: 32px !important;
		height: 32px !important;
		/* MapLibre pins this to top:0, which only reads as centered when the
		   container happens to be exactly the button's own height (the
		   collapsed state). Centering it directly means it stays correct
		   regardless of how tall the container ends up - the collapsed 32px
		   box above, or the expanded one sized by its own text content. */
		top: 50% !important;
		transform: translateY(-50%) !important;
		border-radius: 0.25rem !important;
		background-color: var(--color-panel) !important;
		border: 1px solid var(--color-edge) !important;
		/* a plain "i" glyph, not MapLibre's default circled one - the button
		   itself is already the rounded-square container, so a circle baked
		   into the icon on top of that is redundant, matching this file's
		   other stroke-based icons instead (layers, ruler) rather than
		   MapLibre's filled default. Color matches those icons too
		   (--color-fg-muted, the same currentColor they resolve to via
		   text-fg-muted) - an earlier pass brightened it to --color-fg while
		   chasing a visibility fix, making it stand out white against its
		   grayish siblings instead of matching them. */
		background-image: url("data:image/svg+xml;charset=utf-8,%3Csvg xmlns='http://www.w3.org/2000/svg' width='24' height='24' viewBox='0 0 24 24' fill='none' stroke='%238b98a5' stroke-width='2.75' stroke-linecap='round' stroke-linejoin='round'%3E%3Cline x1='12' y1='10.5' x2='12' y2='16'/%3E%3Ccircle cx='12' cy='7' r='1.3' fill='%238b98a5' stroke='none'/%3E%3C/svg%3E") !important;
		background-size: 16px !important;
		background-position: center !important;
		background-repeat: no-repeat !important;
	}
	:global(.maplibregl-ctrl-scale) {
		background: var(--color-panel) !important;
		border-color: var(--color-edge) !important;
		border-top-color: var(--color-edge) !important;
		color: var(--color-fg-muted) !important;
	}
</style>
