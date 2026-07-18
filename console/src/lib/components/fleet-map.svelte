<script lang="ts">
	import maplibregl from 'maplibre-gl';
	import 'maplibre-gl/dist/maplibre-gl.css';
	import { VehicleOrigin } from '$lib/gen/karshipta/v1/common';
	import { fleet } from '$lib/fleet-store.svelte';
	import { geozoneStore } from '$lib/geozones/geozone-store.svelte';
	import type { ViewportBounds } from '$lib/geozones/types';

	interface Props {
		centerLat: number;
		centerLon: number;
		/**
		 * Fires on every map click alongside whatever the map already does
		 * with it (goto targeting, waypoint placement) - a generic escape
		 * hatch for a consuming app's own click-to-place flows (e.g. picking
		 * a spawn point for a new vehicle) rather than a store-level concept
		 * like fleet.gotoArming, which is specifically about commanding an
		 * already-selected vehicle. The caller decides whether it cares.
		 */
		onMapClick?: (latitudeDeg: number, longitudeDeg: number) => void;
		/**
		 * Owner identity for the selected vehicle's badge - a plain callback,
		 * not a store-level concept, since "who owns this" only exists for a
		 * multi-tenant consuming app (see karshipta-cloud); the single-operator
		 * OSS console has no such notion and simply never passes this.
		 */
		ownerFor?: (vehicleId: string) => { username: string; photoUrl: string | null } | undefined;
	}

	const { centerLat, centerLon, onMapClick, ownerFor }: Props = $props();

	// City-scale, not street-block: the first thing a viewer needs is "where
	// in the world is this", not individual streets. z15 (the old value)
	// only shows a few blocks - unreadable as a city, and the point of
	// landing on the map is immediately losing your bearings.
	const INITIAL_ZOOM = 11;

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
	const GEOZONE_SOURCE = 'geozones';
	const GEOZONE_FILL_LAYER = 'geozones-fill';
	const GEOZONE_LINE_LAYER = 'geozones-line';

	// imperative per-vehicle marker cache, deliberately not reactive state
	let markers: Record<string, MarkerHandle> = {};
	let waypointMarkers: maplibregl.Marker[] = [];
	let mapLoaded = $state(false);
	// camera state; arrows compensate for bearing, marker elevation for pitch/zoom
	let bearingDeg = $state(0);
	let pitchDeg = $state(0);
	let zoomLevel = $state(INITIAL_ZOOM);

	function markerElement(vehicleId: string): Omit<MarkerHandle, 'marker'> & {
		element: HTMLElement;
	} {
		// zero-size root pinned to the ground position; the body (arrow + label)
		// is lifted above it by the projected altitude, connected by a stem
		const element = document.createElement('div');
		element.setAttribute('role', 'button');
		element.setAttribute('tabindex', '0');
		element.setAttribute('aria-label', `Vehicle ${vehicleId}`);
		element.className = 'relative h-0 w-0 cursor-pointer';
		// The badge (name/telemetry/owner) only ever renders for the selected
		// vehicle - see the sync effect below - so it starts hidden. Unselected
		// markers show only the ground dot and arrow; a map with several
		// vehicles on it stays scannable instead of turning into a wall of text.
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
			fleet.select(fleet.selectedVehicleId === vehicleId ? undefined : vehicleId);
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

	$effect(() => {
		// a map init failure (e.g. no WebGL) must not take the rest of the
		// console down; vehicle cards keep working without the map
		let created: maplibregl.Map;
		try {
			created = new maplibregl.Map({
				container,
				center: [centerLon, centerLat],
				zoom: INITIAL_ZOOM,
				attributionControl: { compact: true },
				style: {
					version: 8,
					sources: {
						basemap: {
							type: 'raster',
							tiles: ['https://basemaps.cartocdn.com/dark_all/{z}/{x}/{y}.png'],
							tileSize: 256,
							attribution: '&copy; OpenStreetMap contributors &copy; CARTO'
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
		created.on('click', (event) => {
			if (fleet.missionDraft && fleet.missionDraft.vehicleId === fleet.selectedVehicleId) {
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
			created.addSource(ROUTE_SOURCE, {
				type: 'geojson',
				data: { type: 'Feature', properties: {}, geometry: { type: 'LineString', coordinates: [] } }
			});
			created.addLayer({
				id: ROUTE_SOURCE,
				type: 'line',
				source: ROUTE_SOURCE,
				paint: { 'line-color': '#3b9eff', 'line-width': 2, 'line-dasharray': [2, 1.5] }
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
			mapLoaded = true;
			requestGeozones();
		});
		map = created;
		return () => {
			markers = {};
			waypointMarkers = [];
			mapLoaded = false;
			created.remove();
			map = undefined;
		};
	});

	// Pan to a vehicle once when it's selected (sidebar click, marker click,
	// or a /pilots/[username] deep link), not continuously - lastFocusedId
	// guards against re-panning on every subsequent telemetry tick for the
	// same selection, which would fight the operator's own camera control.
	// Reading state.position here (not untracked) is deliberate: a deep
	// link can select a vehicle before its telemetry has arrived, and this
	// needs to re-fire once position actually shows up for it.
	let lastFocusedVehicleId: string | undefined;
	$effect(() => {
		const selectedId = fleet.selectedVehicleId;
		const activeMap = map;
		if (!selectedId || !activeMap) {
			lastFocusedVehicleId = undefined;
			return;
		}
		if (lastFocusedVehicleId === selectedId) return;
		const position = fleet.vehicles[selectedId]?.state?.position;
		if (!position) return;
		lastFocusedVehicleId = selectedId;
		activeMap.easeTo({
			center: [position.longitudeDeg, position.latitudeDeg],
			duration: 600
		});
	});

	// crosshair cursor while goto targeting or waypoint planning is active
	$effect(() => {
		if (!map) return;
		const planning =
			fleet.missionDraft?.vehicleId === fleet.selectedVehicleId && fleet.missionDraft;
		map.getCanvas().style.cursor = fleet.gotoArming || planning ? 'crosshair' : '';
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

	// draw the mission draft of the selected vehicle: dashed blue route + numbered points
	$effect(() => {
		const activeMap = map;
		if (!activeMap || !mapLoaded) return;
		const draft =
			fleet.missionDraft && fleet.missionDraft.vehicleId === fleet.selectedVehicleId
				? fleet.missionDraft
				: undefined;
		const coordinates = (draft?.waypoints ?? []).map((waypoint) => [
			waypoint.longitudeDeg,
			waypoint.latitudeDeg
		]);
		const source = activeMap.getSource<maplibregl.GeoJSONSource>(ROUTE_SOURCE);
		source?.setData({
			type: 'Feature',
			properties: {},
			geometry: { type: 'LineString', coordinates }
		});

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

	// sync one marker per vehicle from the store
	$effect(() => {
		if (!map) return;
		for (const vehicleId of fleet.vehicleIds) {
			const vehicle = fleet.vehicles[vehicleId];
			const state = vehicle?.state;
			if (!state?.position) continue;
			let handle = markers[vehicleId];
			if (!handle) {
				const { element, ...parts } = markerElement(vehicleId);
				// setLngLat must precede addTo: adding projects the position
				const marker = new maplibregl.Marker({ element })
					.setLngLat([state.position.longitudeDeg, state.position.latitudeDeg])
					.addTo(map);
				handle = { marker, ...parts };
				markers[vehicleId] = handle;
			}
			handle.marker.setLngLat([state.position.longitudeDeg, state.position.latitudeDeg]);
			// heading is relative to true north; subtract the camera bearing so
			// the arrow stays correct when the operator rotates the map
			handle.arrow.style.rotate = `${state.headingDeg - bearingDeg}deg`;
			const selected = fleet.selectedVehicleId === vehicleId;
			// Unselected: no badge at all, so the map stays scannable with
			// several vehicles on it - just the arrow and ground position.
			// The full badge (name, connectivity, altitude, battery, owner)
			// only earns its place once a vehicle is actually picked.
			handle.badge.classList.toggle('hidden', !selected);
			handle.badge.classList.toggle('flex', selected);
			if (selected) {
				handle.nameEl.textContent = vehicleId;
				handle.connectivityDot.classList.toggle('bg-accent', state.connected);
				handle.connectivityDot.classList.toggle('animate-pulse', state.connected);
				handle.connectivityDot.classList.toggle('bg-critical', !state.connected);
				const batteryPct = state.battery?.remainingPct;
				const batteryLabel =
					batteryPct === undefined || batteryPct < 0 ? '?' : `${batteryPct.toFixed(0)}%`;
				handle.telemetryEl.textContent = `${state.position.altitudeRelM.toFixed(0)} m \u00b7 ${batteryLabel}`;
				const owner = ownerFor?.(vehicleId);
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
			// No autopilot behind this vehicle at all (see vehicle-card.svelte): a
			// distinct hue, not a desaturated one, so it doesn't read as
			// disabled/degraded - that's what the opacity fade below already
			// means (link lost), and the two must not look like the same thing.
			const synthetic = vehicle?.info?.origin === VehicleOrigin.VEHICLE_ORIGIN_SYNTHETIC;
			handle.arrowPath.setAttribute('fill', synthetic ? '#a78bfa' : '#f5a623');
			handle.badge.classList.toggle('border-selected', selected);
			handle.badge.classList.toggle('border-edge', !selected);
			// link lost: fade the marker so a stale last-known position doesn't
			// read as live
			handle.body.style.opacity = state.connected ? '1' : '0.4';
		}
		for (const vehicleId of Object.keys(markers)) {
			if (!fleet.vehicleIds.includes(vehicleId)) {
				markers[vehicleId].marker.remove();
				delete markers[vehicleId];
			}
		}
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
			class="border-critical bg-panel text-critical absolute inset-x-0 top-1/2 mx-auto w-fit max-w-lg rounded border px-4 py-2 text-sm"
		>
			Map unavailable: {mapError}
		</p>
	{/if}
	{#if geozoneStore.active}
		<ul
			class="border-edge bg-panel/90 absolute bottom-8 left-4 flex gap-3 rounded border px-2 py-1"
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
