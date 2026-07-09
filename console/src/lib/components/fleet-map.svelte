<script lang="ts">
	import maplibregl from 'maplibre-gl';
	import 'maplibre-gl/dist/maplibre-gl.css';
	import { fleet } from '$lib/fleet-store.svelte';

	interface Props {
		centerLat: number;
		centerLon: number;
	}

	const { centerLat, centerLon }: Props = $props();

	const INITIAL_ZOOM = 15;

	interface MarkerHandle {
		marker: maplibregl.Marker;
		arrow: SVGSVGElement;
		arrowPath: SVGPathElement;
		label: HTMLElement;
	}

	let container: HTMLDivElement;
	let map = $state<maplibregl.Map | undefined>(undefined);
	let mapError = $state<string | undefined>(undefined);
	const ROUTE_SOURCE = 'mission-route';

	// imperative per-vehicle marker cache, deliberately not reactive state
	let markers: Record<string, MarkerHandle> = {};
	let waypointMarkers: maplibregl.Marker[] = [];
	let mapLoaded = $state(false);

	function markerElement(vehicleId: string): {
		element: HTMLElement;
		arrow: SVGSVGElement;
		arrowPath: SVGPathElement;
		label: HTMLElement;
	} {
		const element = document.createElement('div');
		element.setAttribute('role', 'button');
		element.setAttribute('tabindex', '0');
		element.setAttribute('aria-label', `Vehicle ${vehicleId}`);
		element.className = 'relative cursor-pointer';
		// arrow rotates with heading; the label span below stays upright
		element.innerHTML = `
			<svg width="30" height="30" viewBox="0 0 34 34">
				<path d="M17 3 L27 29 L17 22 L7 29 Z" fill="#f5a623" stroke="#0a0e12" stroke-width="1.5" stroke-linejoin="round" />
			</svg>
			<span class="border-edge bg-panel/90 text-fg absolute top-full left-1/2 -translate-x-1/2 rounded-sm border px-1.5 py-0.5 font-mono text-[10px] whitespace-nowrap">${vehicleId}</span>`;
		const arrow = element.querySelector('svg');
		const arrowPath = element.querySelector('path');
		const label = element.querySelector('span');
		if (!(arrow instanceof SVGSVGElement) || !(arrowPath instanceof SVGPathElement) || !label) {
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
		return { element, arrow, arrowPath, label };
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
		});
		created.on('load', () => {
			created.addSource(ROUTE_SOURCE, {
				type: 'geojson',
				data: { type: 'Feature', geometry: { type: 'LineString', coordinates: [] } }
			});
			created.addLayer({
				id: ROUTE_SOURCE,
				type: 'line',
				source: ROUTE_SOURCE,
				paint: { 'line-color': '#3b9eff', 'line-width': 2, 'line-dasharray': [2, 1.5] }
			});
			mapLoaded = true;
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

	// crosshair cursor while goto targeting or waypoint planning is active
	$effect(() => {
		if (!map) return;
		const planning =
			fleet.missionDraft?.vehicleId === fleet.selectedVehicleId && fleet.missionDraft;
		map.getCanvas().style.cursor = fleet.gotoArming || planning ? 'crosshair' : '';
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
			const state = fleet.vehicles[vehicleId]?.state;
			if (!state?.position) continue;
			let handle = markers[vehicleId];
			if (!handle) {
				const { element, arrow, arrowPath, label } = markerElement(vehicleId);
				// setLngLat must precede addTo: adding projects the position
				const marker = new maplibregl.Marker({ element })
					.setLngLat([state.position.longitudeDeg, state.position.latitudeDeg])
					.addTo(map);
				handle = { marker, arrow, arrowPath, label };
				markers[vehicleId] = handle;
			}
			handle.marker.setLngLat([state.position.longitudeDeg, state.position.latitudeDeg]);
			// heading is relative to true north; map bearing is fixed at 0 for now
			handle.arrow.style.rotate = `${state.headingDeg}deg`;
			const selected = fleet.selectedVehicleId === vehicleId;
			handle.arrowPath.setAttribute('stroke', selected ? '#3b9eff' : '#0a0e12');
			handle.arrowPath.setAttribute('stroke-width', selected ? '2.5' : '1.5');
			handle.label.classList.toggle('border-selected', selected);
			handle.label.classList.toggle('border-edge', !selected);
		}
		for (const vehicleId of Object.keys(markers)) {
			if (!fleet.vehicleIds.includes(vehicleId)) {
				markers[vehicleId].marker.remove();
				delete markers[vehicleId];
			}
		}
	});
</script>

<div bind:this={container} class="h-full w-full" aria-label="Fleet map">
	{#if mapError}
		<p
			role="alert"
			class="border-critical bg-panel text-critical absolute inset-x-0 top-1/2 mx-auto w-fit max-w-lg rounded border px-4 py-2 text-sm"
		>
			Map unavailable: {mapError}
		</p>
	{/if}
</div>
