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
	import { obstacleStore } from '$lib/obstacles/obstacle-store.svelte';
	import { airportStore } from '$lib/airports/airport-store.svelte';
	import { cityStore } from '$lib/cities/city-store.svelte';
	import { aircraftStore } from '$lib/aircraft/aircraft-store.svelte';
	import { isAircraftEmergency, type AircraftCategory } from '$lib/aircraft/types';
	import {
		AIRCRAFT_ICON_SHAPES,
		ICON_PIXEL_RATIO,
		buildAircraftIconImageData,
		iconIdFor,
		iconShapeForCategory
	} from '$lib/aircraft/icons';
	import { earthquakeStore } from '$lib/earthquakes/earthquake-store.svelte';
	import { wildfireStore } from '$lib/wildfires/wildfire-store.svelte';
	import { weatherStore } from '$lib/weather/weather-store.svelte';
	import { weatherIconShape, weatherConditionLabel } from '$lib/weather/condition';
	import WeatherIcon from '$lib/components/weather-icon.svelte';
	import { zoneStore } from '$lib/zones/zone-store.svelte';
	import { themeStore } from '$lib/theme.svelte';
	import defaultStyleLightPreview from '$lib/assets/map-style-previews/default-light.jpg';
	import defaultStyleDarkPreview from '$lib/assets/map-style-previews/default-dark.jpg';
	import roadmapStylePreview from '$lib/assets/map-style-previews/roadmap.jpg';
	import terrainStylePreview from '$lib/assets/map-style-previews/terrain.jpg';
	import satelliteStylePreview from '$lib/assets/map-style-previews/satellite.jpg';
	import bathymetryStylePreview from '$lib/assets/map-style-previews/bathymetry.jpg';
	import { unitsStore } from '$lib/units/units-store.svelte';
	import {
		formatAltitude,
		formatDistance as formatDistanceUnits,
		formatSpeed,
		formatVehicleSpeed,
		formatTemperature,
		formatPrecipitation,
		formatPressure
	} from '$lib/units/format';
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

	// Below this, don't fetch airspace data at all - airspace boundaries
	// (FIRs, TMAs, prohibited zones) are country/region-scale shapes, not
	// meaningful at a world- or continent-level view, and every fetch below
	// this zoom would just be hitting openaip.ts's 5-degree bbox clamp
	// repeatedly for no visual benefit. 7 is roughly "a country fills the
	// screen" - well above "the whole continent" (where clamping would
	// otherwise fire on almost every pan) and well below the fleet's own
	// working zoom (INITIAL_ZOOM=12), so this never affects normal use.
	const MIN_GEOZONE_ZOOM = 7;

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
	// CARTO Voyager: a colorful roadmap style, free/no-key like dark_all and
	// light_all above (same CDN, same terms) - offered only alongside the
	// light theme (see mapStyle's own comment), since its light background
	// would reintroduce the exact light-map-under-dark-chrome mismatch
	// DARK_TILES/LIGHT_TILES already exist to avoid.
	const ROADMAP_TILES = ['https://basemaps.cartocdn.com/rastertiles/voyager/{z}/{x}/{y}.png'];
	// OpenTopoMap: free/no-key elevation-shaded terrain tiles, community-run
	// (not CARTO/Esri) - three round-robin subdomains, matching their own
	// published usage guidance for spreading load. Its shading has no real
	// dark variant, so - like satellite below - it's offered regardless of
	// the app's current theme rather than paired with just one.
	const TERRAIN_TILES = [
		'https://a.tile.opentopomap.org/{z}/{x}/{y}.png',
		'https://b.tile.opentopomap.org/{z}/{x}/{y}.png',
		'https://c.tile.opentopomap.org/{z}/{x}/{y}.png'
	];
	// GEBCO's WMS, not an XYZ tile server like every other basemap here -
	// {bbox-epsg-3857} is MapLibre's own documented substitution for exactly
	// this case, resolving to each tile's real Web Mercator bbox at request
	// time so a plain WMS GetMap request can stand in for a raster source's
	// usual z/x/y template. Confirmed live with a real bbox. GEBCO_LATEST is
	// shaded relief covering the whole globe (ocean depth and land
	// elevation together), not an ocean-only overlay, so - like Terrain -
	// it works as its own full basemap rather than a layer on top of one.
	const BATHYMETRY_TILES = [
		'https://wms.gebco.net/mapserv?service=WMS&version=1.1.1&request=GetMap&layers=GEBCO_LATEST&styles=&format=image/png&transparent=false&srs=EPSG:3857&bbox={bbox-epsg-3857}&width=256&height=256'
	];
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
	// credits regardless of which is currently active is a common,
	// acceptable tradeoff other map apps make too.
	const BASEMAP_ATTRIBUTION =
		'&copy; OpenStreetMap contributors &copy; CARTO &copy; Esri, Maxar, Earthstar Geographics &copy; OpenTopoMap (CC-BY-SA) GEBCO Compilation Group';

	// 'default' follows the app theme (DARK_TILES/LIGHT_TILES, unchanged
	// behavior); 'roadmap' is light-only (see ROADMAP_TILES's own comment) -
	// picking it while on the dark theme isn't offered in the menu below,
	// and the effect further down falls back to 'default' if the theme
	// flips to dark while it's active. 'terrain'/'satellite' are themeless,
	// same as satellite always was.
	type MapStyle = 'default' | 'roadmap' | 'terrain' | 'satellite' | 'bathymetry';
	const MAP_STYLE_OPTIONS: { id: MapStyle; label: string; preview: string }[] = [
		{ id: 'default', label: 'Map', preview: defaultStyleLightPreview },
		{ id: 'roadmap', label: 'Roadmap', preview: roadmapStylePreview },
		{ id: 'terrain', label: 'Terrain', preview: terrainStylePreview },
		{ id: 'satellite', label: 'Satellite', preview: satelliteStylePreview },
		{ id: 'bathymetry', label: 'Bathymetry', preview: bathymetryStylePreview }
	];
	// 'default' is the one style option that itself follows the app theme
	// (DARK_TILES/LIGHT_TILES - see MapStyle's own comment above), so its
	// preview can't be a single static image like the themeless/light-only
	// options above: showing the light-mode capture while dark theme is
	// active would misrepresent what picking "Map" actually does.
	function stylePreviewFor(option: (typeof MAP_STYLE_OPTIONS)[number]): string {
		if (option.id === 'default') {
			return themeStore.current === 'dark' ? defaultStyleDarkPreview : defaultStyleLightPreview;
		}
		return option.preview;
	}
	const MAP_STYLE_STORAGE_KEY = 'karshipta:mapStyle';
	function readStoredMapStyle(): MapStyle {
		if (typeof localStorage === 'undefined') return 'default';
		const stored = localStorage.getItem(MAP_STYLE_STORAGE_KEY);
		if (
			stored === 'roadmap' ||
			stored === 'terrain' ||
			stored === 'satellite' ||
			stored === 'bathymetry'
		) {
			return stored;
		}
		// Migrates the old boolean satellite flag transparently, so a
		// returning operator's prior choice carries over.
		return localStorage.getItem('karshipta:satellite') === 'true' ? 'satellite' : 'default';
	}
	let mapStyle = $state(readStoredMapStyle());
	function setMapStyle(value: MapStyle): void {
		mapStyle = value;
		if (typeof localStorage === 'undefined') return;
		localStorage.setItem(MAP_STYLE_STORAGE_KEY, value);
	}
	// Roadmap only exists alongside the light theme (see MapStyle's own
	// comment) - if the theme changes out from under an active roadmap
	// selection, fall back rather than silently keep a light-styled map
	// under dark chrome.
	$effect(() => {
		if (mapStyle === 'roadmap' && themeStore.current !== 'light') setMapStyle('default');
	});
	const basemapTiles = $derived(
		mapStyle === 'satellite'
			? SATELLITE_TILES
			: mapStyle === 'terrain'
				? TERRAIN_TILES
				: mapStyle === 'bathymetry'
					? BATHYMETRY_TILES
					: mapStyle === 'roadmap'
						? ROADMAP_TILES
						: themeStore.current === 'light'
							? LIGHT_TILES
							: DARK_TILES
	);

	// Off by default: an operator who configures PUBLIC_OPENAIP_KEY opts
	// into the layer existing at all, but seeing it on every load regardless
	// wasn't the intent - matches "Zones" (showZones) already defaulting to
	// on being the operator's OWN drawn geometry, a different trust level
	// than a third-party overlay.
	let showGeozones = $state(false);
	// Same off-by-default reasoning as showGeozones above.
	let showObstacles = $state(false);
	let showAirports = $state(false);
	// Cities default on, unlike the three OpenAIP layers above: it's a
	// bundled dataset with no key/trust/rate-limit concern (see
	// city-store.svelte.ts's own comment), so there's no reason to make an
	// operator opt in just to see it.
	let showCities = $state(true);
	// Aircraft/earthquakes/wildfires default off like the OpenAIP layers,
	// not on like Cities: real-time third-party traffic and hazard data,
	// same trust-level reasoning as showGeozones/showObstacles/showAirports
	// above, not a static reference dataset.
	let showAircraft = $state(false);
	let showEarthquakes = $state(false);
	let showWildfires = $state(false);
	// Weather defaults on for the same reason as Cities above: Open-Meteo
	// needs no key and carries no rate-limit/trust concern an operator
	// should have to opt into.
	let showWeather = $state(true);
	// Name of the bundled city nearest the map center, for the weather
	// widget's own label - see cityStore.nearestTo. Undefined until the
	// cities dataset has loaded and a location has actually been requested.
	let weatherLocationLabel = $state<string | undefined>(undefined);
	/** Loads the (shared, cached) cities dataset if needed - independent of
	 * whether the Cities layer checkbox itself is on, since the weather
	 * widget's label needs it regardless - then resolves to the nearest
	 * bundled city, falling back to raw coordinates so the widget never
	 * shows a temperature with no indication of where it's for. */
	function updateWeatherLocationLabel(latitudeDeg: number, longitudeDeg: number): void {
		void cityStore.ensureLoaded().then(() => {
			const nearest = cityStore.nearestTo(latitudeDeg, longitudeDeg);
			weatherLocationLabel = nearest
				? nearest.name
				: `${latitudeDeg.toFixed(1)}, ${longitudeDeg.toFixed(1)}`;
		});
	}
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
	const OBSTACLE_SOURCE = 'obstacles';
	const OBSTACLE_LAYER = 'obstacles-points';
	const AIRPORT_SOURCE = 'airports';
	const AIRPORT_LAYER = 'airports-points';
	// Not --color-accent (#f5a623): that's the app's primary/CTA color, used
	// throughout the UI for buttons and active states - reusing it here
	// would make an obstacle marker read as "the same thing" as a primary
	// button. #f97316 is the same orange already in ROUTE_COLORS below,
	// close enough in hue to still land as "caution" without the collision.
	const OBSTACLE_COLOR = '#f97316';
	const AIRPORT_COLOR = '#3b9eff';
	const CITY_SOURCE = 'cities';
	const CITY_LAYER = 'cities-points';
	const CITY_COLOR = '#8b98a5';
	const AIRCRAFT_SOURCE = 'aircraft';
	const AIRCRAFT_LAYER = 'aircraft-points';
	const AIRCRAFT_TRAIL_SOURCE = 'aircraft-trails';
	const AIRCRAFT_TRAIL_LAYER = 'aircraft-trails-line';
	const AIRCRAFT_COLOR = '#14b8a6';
	// Reuses --color-critical's hue rather than reading the CSS custom
	// property directly - MapLibre paint values are plain style-spec
	// values, not CSS, same reasoning as every other *_COLOR constant here
	// being a literal hex instead of var(...).
	const AIRCRAFT_EMERGENCY_COLOR = '#ef4444';
	// Deliberately not reusing any existing semantic token (armed=green,
	// critical=red, synthetic=violet, selected=blue all mean something
	// else already) - a muted slate reads as "notable/distinct" without
	// implying one of those other states.
	const AIRCRAFT_MILITARY_COLOR = '#64748b';
	// Heavier/larger categories render bigger, same "size communicates
	// scale" reasoning as EARTHQUAKE_LAYER's magnitude-interpolated radius
	// below - a 747 and a Cessna reading as the same size marker would
	// hide a real, useful distinction.
	function aircraftIconSize(category: AircraftCategory): number {
		switch (category) {
			case 'heavy':
				return 0.75;
			case 'rotorcraft':
			case 'glider':
				return 0.55;
			case 'light':
				return 0.5;
			case 'uav':
				return 0.46;
			case 'ground':
				return 0.4;
			default:
				return 0.45;
		}
	}
	const EARTHQUAKE_SOURCE = 'earthquakes';
	const EARTHQUAKE_LAYER = 'earthquakes-points';
	const EARTHQUAKE_COLOR = '#ec4899';
	const WILDFIRE_SOURCE = 'wildfires';
	const WILDFIRE_LAYER = 'wildfires-points';
	// Deliberately close to --color-critical (#e74c3c), unlike the other new
	// layers' colors, which were all picked specifically to avoid semantic
	// collisions with existing tokens - fire reading as "red" is such a
	// strong, universal convention that keeping it in the same family is
	// more correct here than avoiding the overlap.
	const WILDFIRE_COLOR = '#dc2626';
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
		return formatDistanceUnits(meters, unitsStore.current);
	}

	// Obstacle/airport names and codes come from OpenAIP, third-party data -
	// escaped before going into Popup.setHTML() (which sets raw innerHTML)
	// the same way any other untrusted string would need to be.
	const HTML_ESCAPES: Record<string, string> = {
		'&': '&amp;',
		'<': '&lt;',
		'>': '&gt;',
		'"': '&quot;',
		"'": '&#39;'
	};
	function escapeHtml(value: string): string {
		return value.replace(/[&<>"']/g, (char) => HTML_ESCAPES[char]);
	}

	function popupRow(label: string, value: string): string {
		return `<div class="flex justify-between gap-3"><span class="text-fg-muted">${label}</span><span>${value}</span></div>`;
	}

	function buildObstaclePopupHtml(properties: Record<string, unknown>): string {
		const name = typeof properties.name === 'string' ? properties.name : 'Obstacle';
		const rows: string[] = [];
		if (typeof properties.heightM === 'number') {
			rows.push(popupRow('Height', formatAltitude(properties.heightM, unitsStore.current)));
		}
		if (typeof properties.elevationM === 'number') {
			rows.push(
				popupRow('Elevation', `${formatAltitude(properties.elevationM, unitsStore.current)} AMSL`)
			);
		}
		if (typeof properties.countryCode === 'string') {
			rows.push(popupRow('Country', escapeHtml(properties.countryCode)));
		}
		let wikiLink = '';
		if (typeof properties.wikipediaUrl === 'string') {
			const href = escapeHtml(properties.wikipediaUrl);
			wikiLink = `<a href="${href}" target="_blank" rel="noreferrer" class="text-accent hover:underline">Wikipedia &#8599;</a>`;
		}
		return `<div class="flex flex-col gap-1 text-xs"><p class="font-medium text-fg">${escapeHtml(name)}</p>${rows.join('')}${wikiLink}</div>`;
	}

	function buildAirportPopupHtml(properties: Record<string, unknown>): string {
		const name = typeof properties.name === 'string' ? properties.name : 'Airport';
		const rows: string[] = [];
		if (typeof properties.icaoCode === 'string') {
			rows.push(popupRow('ICAO', escapeHtml(properties.icaoCode)));
		}
		if (typeof properties.elevationM === 'number') {
			rows.push(
				popupRow('Elevation', `${formatAltitude(properties.elevationM, unitsStore.current)} AMSL`)
			);
		}
		if (typeof properties.countryCode === 'string') {
			rows.push(popupRow('Country', escapeHtml(properties.countryCode)));
		}
		return `<div class="flex flex-col gap-1 text-xs"><p class="font-medium text-fg">${escapeHtml(name)}</p>${rows.join('')}</div>`;
	}

	const KNOTS_TO_MS = 0.514444;

	// Live METAR for the airport popup - confirmed live against a real ICAO
	// code, free, no key. Fetched on demand per popup open, not a viewport
	// layer of its own the way Aircraft/Earthquakes/Wildfires are: METAR is
	// inherently per-airport, and the Airport layer already has exactly one
	// point per airport to hang it off of. wspd/wdir/temp/visib field names
	// and units (wspd in knots, temp in Celsius) confirmed against a real
	// Schiphol report.
	async function fetchMetarHtml(properties: Record<string, unknown>): Promise<string | undefined> {
		const icaoCode = properties.icaoCode;
		if (typeof icaoCode !== 'string') return undefined;
		try {
			const response = await fetch(
				`https://aviationweather.gov/api/data/metar?ids=${encodeURIComponent(icaoCode)}&format=json`
			);
			if (!response.ok) return undefined;
			const body: unknown = await response.json();
			const report = Array.isArray(body) ? body[0] : undefined;
			if (typeof report !== 'object' || report === null) return undefined;
			const data = report as Record<string, unknown>;
			const rows: string[] = [];
			if (typeof data.wdir === 'number' && typeof data.wspd === 'number') {
				rows.push(
					popupRow(
						'Wind',
						`${data.wdir}&deg; ${formatVehicleSpeed(data.wspd * KNOTS_TO_MS, unitsStore.current)}`
					)
				);
			}
			if (typeof data.temp === 'number') {
				rows.push(popupRow('Temp', `${data.temp}&deg;C`));
			}
			if (typeof data.visib === 'string' || typeof data.visib === 'number') {
				rows.push(popupRow('Visibility', `${escapeHtml(String(data.visib))} mi`));
			}
			if (rows.length === 0) return undefined;
			return `<div class="mt-2 flex flex-col gap-1 border-t border-edge pt-2 text-xs">${rows.join('')}</div>`;
		} catch {
			// Same "reassuring, not alarming" philosophy as the viewport
			// layers' own loadError handling - a failed weather enrichment
			// just means the popup stays at its base content, not a visible
			// error state for something this secondary.
			return undefined;
		}
	}

	function buildCityPopupHtml(properties: Record<string, unknown>): string {
		const name = typeof properties.name === 'string' ? properties.name : 'City';
		const rows: string[] = [];
		if (typeof properties.countryCode === 'string') {
			rows.push(popupRow('Country', escapeHtml(properties.countryCode)));
		}
		if (typeof properties.population === 'number') {
			rows.push(popupRow('Population', properties.population.toLocaleString('en-US')));
		}
		return `<div class="flex flex-col gap-1 text-xs"><p class="font-medium text-fg">${escapeHtml(name)}</p>${rows.join('')}</div>`;
	}

	// Raw ADS-B emergency status strings (DO-260B) -> readable labels.
	const AIRCRAFT_EMERGENCY_LABELS: Record<string, string> = {
		general: 'General emergency',
		medical: 'Medical emergency',
		minfuel: 'Minimum fuel',
		nordo: 'Radio failure',
		unlawful: 'Unlawful interference',
		downed: 'Downed aircraft',
		reserved: 'Reserved'
	};

	function buildAircraftPopupHtml(properties: Record<string, unknown>): string {
		const name = typeof properties.callsign === 'string' ? properties.callsign : 'Aircraft';
		const isEmergency = properties.isEmergency === true;
		const isMilitary = properties.isMilitary === true;
		const badges: string[] = [];
		if (isEmergency) {
			badges.push(
				'<span class="rounded bg-critical/15 px-1.5 py-0.5 text-[10px] font-medium text-critical">EMERGENCY</span>'
			);
		}
		if (isMilitary) {
			badges.push(
				'<span class="rounded bg-white/10 px-1.5 py-0.5 text-[10px] font-medium text-fg-muted">MILITARY</span>'
			);
		}
		const rows: string[] = [];
		if (isEmergency && typeof properties.emergency === 'string') {
			const label = AIRCRAFT_EMERGENCY_LABELS[properties.emergency] ?? properties.emergency;
			rows.push(popupRow('Emergency', escapeHtml(label)));
		}
		if (typeof properties.typeDescription === 'string') {
			rows.push(popupRow('Type', escapeHtml(properties.typeDescription)));
		}
		if (typeof properties.operator === 'string') {
			rows.push(popupRow('Operator', escapeHtml(properties.operator)));
		}
		if (typeof properties.registration === 'string') {
			rows.push(popupRow('Registration', escapeHtml(properties.registration)));
		}
		if (typeof properties.icao24 === 'string') {
			rows.push(popupRow('ICAO24', escapeHtml(properties.icao24)));
		}
		if (typeof properties.yearBuilt === 'number') {
			rows.push(popupRow('Built', String(properties.yearBuilt)));
		}
		if (typeof properties.altitudeM === 'number') {
			rows.push(popupRow('Altitude', formatAltitude(properties.altitudeM, unitsStore.current)));
		}
		if (typeof properties.velocityMS === 'number') {
			rows.push(popupRow('Speed', formatVehicleSpeed(properties.velocityMS, unitsStore.current)));
		}
		// Vertical rate stays in formatSpeed's m/s, not formatVehicleSpeed -
		// a variometer-scale rate (a few m/s at most) reads naturally in
		// m/s the same way ward telemetry does, unlike ground speed at
		// aircraft-cruise scale. Under ~1.5 m/s (~300 ft/min) reads as
		// level flight, not a real climb/descent - skip the row rather than
		// showing noisy near-zero values every level cruise segment would
		// otherwise report.
		if (
			typeof properties.verticalRateMS === 'number' &&
			Math.abs(properties.verticalRateMS) >= 1.5
		) {
			const climbing = properties.verticalRateMS > 0;
			rows.push(
				popupRow(
					climbing ? 'Climbing' : 'Descending',
					formatSpeed(Math.abs(properties.verticalRateMS), unitsStore.current)
				)
			);
		}
		if (properties.onGround === true) {
			rows.push(popupRow('Status', 'On ground'));
		}
		const badgeHtml = badges.length > 0 ? `<div class="flex gap-1">${badges.join('')}</div>` : '';
		return `<div class="flex flex-col gap-1 text-xs"><div class="flex items-center justify-between gap-2"><p class="font-medium text-fg">${escapeHtml(name)}</p>${badgeHtml}</div>${rows.join('')}</div>`;
	}

	// adsbdb.com: free, no key, wildcard CORS (confirmed live). Two
	// independent lookups, run in parallel:
	// - /v0/callsign/{callsign} (flightroute): fills a real gap in
	//   airplanes.live's own ownOp field, which is empty even for
	//   obviously commercial flights (confirmed live: "ABY150", a real
	//   Air Arabia flight registered in the UAE, had no operator at all
	//   from airplanes.live, but adsbdb correctly resolves "Air Arabia"
	//   from the callsign's ICAO airline prefix alone). Only called when
	//   operator is already missing - airplanes.live's own field is
	//   preferred when present. Also returns the flight's actual
	//   origin/destination airports, real route data neither feed
	//   otherwise provides. Confirmed live this only covers civil
	//   registrations - a real military aircraft (a Royal Netherlands Air
	//   Force Apache) returned "unknown callsign"/"unknown aircraft" from
	//   both adsbdb endpoints, so this contributes nothing for military
	//   traffic; nothing else here fills that gap either.
	// - /v0/aircraft/{registration}: registered owner's country and
	//   manufacturer, which nothing else here provides at all (not
	//   gated on operator being missing - always worth adding). Also
	//   returns photo URLs, but confirmed live those are dead links
	//   (404 from airport-data.com on every real aircraft tried) - not
	//   used for that reason, not because the fields don't exist.
	async function fetchAircraftEnrichmentHtml(
		properties: Record<string, unknown>
	): Promise<string | undefined> {
		const callsign = properties.callsign;
		const registration = properties.registration;
		const needsAirline = !properties.operator && typeof callsign === 'string' && callsign.trim();
		const hasRegistration = typeof registration === 'string' && registration.trim();
		if (!needsAirline && !hasRegistration) return undefined;

		const [flightrouteResult, aircraftResult] = await Promise.allSettled([
			needsAirline
				? fetch(
						`https://api.adsbdb.com/v0/callsign/${encodeURIComponent((callsign as string).trim())}`
					).then((r) => (r.ok ? r.json() : undefined))
				: Promise.resolve(undefined),
			hasRegistration
				? fetch(
						`https://api.adsbdb.com/v0/aircraft/${encodeURIComponent((registration as string).trim())}`
					).then((r) => (r.ok ? r.json() : undefined))
				: Promise.resolve(undefined)
		]);

		const rows: string[] = [];

		const flightroute =
			flightrouteResult.status === 'fulfilled' &&
			typeof flightrouteResult.value === 'object' &&
			flightrouteResult.value !== null
				? (flightrouteResult.value as { response?: { flightroute?: unknown } }).response
						?.flightroute
				: undefined;
		if (typeof flightroute === 'object' && flightroute !== null) {
			const data = flightroute as Record<string, unknown>;
			const airline =
				typeof data.airline === 'object' && data.airline !== null
					? (data.airline as Record<string, unknown>).name
					: undefined;
			const origin =
				typeof data.origin === 'object' && data.origin !== null
					? (data.origin as Record<string, unknown>).municipality
					: undefined;
			const destination =
				typeof data.destination === 'object' && data.destination !== null
					? (data.destination as Record<string, unknown>).municipality
					: undefined;
			if (typeof airline === 'string' && airline) {
				rows.push(popupRow('Operator', escapeHtml(airline)));
			}
			if (typeof origin === 'string' && typeof destination === 'string' && origin && destination) {
				rows.push(popupRow('Route', `${escapeHtml(origin)} &rarr; ${escapeHtml(destination)}`));
			}
		}

		const aircraft =
			aircraftResult.status === 'fulfilled' &&
			typeof aircraftResult.value === 'object' &&
			aircraftResult.value !== null
				? (aircraftResult.value as { response?: { aircraft?: unknown } }).response?.aircraft
				: undefined;
		if (typeof aircraft === 'object' && aircraft !== null) {
			const data = aircraft as Record<string, unknown>;
			const country = data.registered_owner_country_name;
			const manufacturer = data.manufacturer;
			if (typeof country === 'string' && country) {
				rows.push(popupRow('Country', escapeHtml(country)));
			}
			if (typeof manufacturer === 'string' && manufacturer) {
				rows.push(popupRow('Manufacturer', escapeHtml(manufacturer)));
			}
		}

		if (rows.length === 0) return undefined;
		return `<div class="mt-2 flex flex-col gap-1 border-t border-edge pt-2 text-xs">${rows.join('')}</div>`;
	}

	function buildEarthquakePopupHtml(properties: Record<string, unknown>): string {
		const place = typeof properties.place === 'string' ? properties.place : 'Earthquake';
		const rows: string[] = [];
		if (typeof properties.magnitude === 'number') {
			rows.push(popupRow('Magnitude', properties.magnitude.toFixed(1)));
		}
		if (typeof properties.depthKm === 'number') {
			rows.push(popupRow('Depth', `${properties.depthKm.toFixed(0)} km`));
		}
		if (typeof properties.timeMs === 'number') {
			rows.push(popupRow('When', new Date(properties.timeMs).toLocaleString()));
		}
		return `<div class="flex flex-col gap-1 text-xs"><p class="font-medium text-fg">${escapeHtml(place)}</p>${rows.join('')}</div>`;
	}

	function buildWildfirePopupHtml(properties: Record<string, unknown>): string {
		const rows: string[] = [];
		if (typeof properties.brightnessK === 'number') {
			rows.push(popupRow('Brightness', `${properties.brightnessK.toFixed(0)} K`));
		}
		if (typeof properties.frpMw === 'number') {
			rows.push(popupRow('Radiative power', `${properties.frpMw.toFixed(1)} MW`));
		}
		if (typeof properties.confidence === 'string') {
			rows.push(popupRow('Confidence', escapeHtml(properties.confidence)));
		}
		if (typeof properties.acquiredAtIso === 'string') {
			rows.push(popupRow('Detected', escapeHtml(properties.acquiredAtIso)));
		}
		return `<div class="flex flex-col gap-1 text-xs"><p class="font-medium text-fg">Fire hotspot</p>${rows.join('')}</div>`;
	}

	// Shared hover-popup wiring for a point layer: shows whatever
	// buildHtml() produces from the hovered feature's own properties. Plain
	// mouseleave-closes-immediately doesn't work for a popup with a real
	// link in it (the obstacle popup's Wikipedia link): moving the cursor
	// from the dot toward the link leaves the dot's own hit area first,
	// closing the popup before the click can land. Fixed with a short grace
	// period instead of switching to click-to-open: leaving the dot starts
	// a timer to close, but entering the popup's own DOM element (tracked
	// via its own mouseenter/mouseleave) cancels that timer, so hovering
	// over either the dot or the balloon itself keeps it open - only
	// leaving both actually closes it. One popup instance reused across all
	// three point layers (never more than one open at a time) rather than
	// one per layer.
	const POPUP_CLOSE_GRACE_MS = 150;
	let openPopup: maplibregl.Popup | undefined;
	let closeTimer: ReturnType<typeof setTimeout> | undefined;
	function cancelPopupClose(): void {
		if (closeTimer) clearTimeout(closeTimer);
		closeTimer = undefined;
	}
	function schedulePopupClose(): void {
		cancelPopupClose();
		closeTimer = setTimeout(() => {
			openPopup?.remove();
			openPopup = undefined;
		}, POPUP_CLOSE_GRACE_MS);
	}
	function wirePointLayerPopup(
		targetMap: maplibregl.Map,
		layerId: string,
		buildHtml: (properties: Record<string, unknown>) => string,
		// Optional: for a layer whose popup can show more than what's already
		// in its GeoJSON properties (only the airport layer uses this, for
		// live METAR weather) - runs after the popup is already showing
		// buildHtml()'s own synchronous content, and appends whatever this
		// resolves to once it does. Checked against the popup instance that
		// was open when the fetch started, not just "is a popup open right
		// now": the operator may have moved to a different point (or closed
		// it) before a slow request resolves, and a stale fetch should never
		// overwrite whatever's showing by the time it lands.
		fetchExtra?: (properties: Record<string, unknown>) => Promise<string | undefined>
	): void {
		targetMap.on('mouseenter', layerId, (event) => {
			targetMap.getCanvas().style.cursor = 'pointer';
			cancelPopupClose();
			const feature = event.features?.[0];
			if (!feature || feature.geometry.type !== 'Point') return;
			const [lon, lat] = feature.geometry.coordinates as [number, number];
			const properties = feature.properties ?? {};
			const baseHtml = buildHtml(properties);
			openPopup?.remove();
			const popup = new maplibregl.Popup({ closeButton: false, closeOnClick: false, offset: 10 })
				.setLngLat([lon, lat])
				.setHTML(baseHtml)
				.addTo(targetMap);
			openPopup = popup;
			const popupEl = popup.getElement();
			popupEl.addEventListener('mouseenter', cancelPopupClose);
			popupEl.addEventListener('mouseleave', schedulePopupClose);
			if (fetchExtra) {
				void fetchExtra(properties).then((extraHtml) => {
					if (openPopup !== popup || !extraHtml) return;
					popup.setHTML(baseHtml + extraHtml);
				});
			}
		});
		targetMap.on('mouseleave', layerId, () => {
			targetMap.getCanvas().style.cursor = '';
			schedulePopupClose();
		});
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
		// Safe to call unconditionally on every moveend: each store's own
		// requestViewport() no-ops while inactive (no key configured) or not
		// visible (its own checkbox in the layers menu, see the setVisible()
		// effects below) - this function only adds the zoom-level check on
		// top, since none of the three stores has an opinion on zoom, only on
		// bbox size. All three requests for one moveend still end up
		// serialized through the same openAipRequestGate (see that module's
		// own header comment), so adding obstacles/airports here does not
		// multiply the actual request rate the way three independent
		// per-layer schedules would.
		// Renamed from its original requestOpenAipLayers: now covers every
		// third-party/reference layer, not just the three OpenAIP ones -
		// OpenSky, USGS, and FIRMS each have their own independent service
		// and rate budget (see each store's own comment), not OpenAIP's, but
		// still share this one moveend hookup and the same MIN_GEOZONE_ZOOM
		// floor to avoid firing real network requests at a whole-world zoom.
		const requestThirdPartyLayers = () => {
			const zoom = created.getZoom();
			const bounds = created.getBounds();
			const viewport: ViewportBounds = [
				bounds.getWest(),
				bounds.getSouth(),
				bounds.getEast(),
				bounds.getNorth()
			];
			// Cities are a pure in-memory filter (see city-store.svelte.ts's
			// own comment) with no network cost, so they update at any zoom -
			// unlike every layer below, gated on MIN_GEOZONE_ZOOM since each
			// one is a real network request against a bbox-size-limited API.
			cityStore.setViewport(viewport, zoom);
			// Weather is a real network request too, but a single-point query,
			// not a bbox one - MIN_GEOZONE_ZOOM exists specifically because
			// OpenAIP/etc. reject an oversized bbox, which doesn't apply here,
			// so this isn't gated on it either.
			const center = created.getCenter();
			weatherStore.requestLocation(center.lat, center.lng);
			updateWeatherLocationLabel(center.lat, center.lng);
			// Aircraft is a real network request too, but like weather above,
			// not a raw-bbox one: airplanesLive reduces the viewport to a
			// center point + radius capped at 250nm internally (see
			// boundsToPointRadius in airplaneslive.ts) rather than sending the
			// bbox itself, so there's no "oversized bbox gets rejected" failure
			// mode to guard against by waiting for a minimum zoom - it was
			// previously grouped with the OpenAIP/USGS layers below and so
			// silently didn't load at all until zoomed in past MIN_GEOZONE_ZOOM,
			// which was never the actual constraint for this one.
			aircraftStore.requestViewport(viewport, zoom);
			// Earthquakes (USGS) and Wildfires (FIRMS) also don't need
			// MIN_GEOZONE_ZOOM: unlike OpenAIP's three layers below (confirmed
			// live to reject anything over 5 degrees wide/tall), neither
			// usgs.ts nor firms.ts clamps the bbox at all - both send the real
			// viewport straight through, and both were confirmed live to
			// accept a whole-Europe-sized bbox and return real data, not an
			// error. The zoom floor was blocking them from ever getting the
			// chance to show that at low zoom, for a constraint (oversized
			// bbox rejection) that was never actually true for either of them.
			earthquakeStore.requestViewport(viewport);
			wildfireStore.requestViewport(viewport);
			if (zoom < MIN_GEOZONE_ZOOM) return;
			geozoneStore.requestViewport(viewport);
			obstacleStore.requestViewport(viewport);
			airportStore.requestViewport(viewport);
		};
		created.on('moveend', requestThirdPartyLayers);
		created.on('load', () => {
			// sdf:true registers each shape as a recolorable template - actual
			// per-feature color (normal/military/emergency) and rotation come
			// from the aircraft-points layer's own icon-color/icon-rotate data
			// expressions below, not from these images themselves. Registered
			// once here, not per-feature - see icons.ts's own comment.
			for (const shape of AIRCRAFT_ICON_SHAPES) {
				const imageData = buildAircraftIconImageData(shape);
				if (imageData) {
					created.addImage(iconIdFor(shape), imageData, {
						sdf: true,
						pixelRatio: ICON_PIXEL_RATIO
					});
				}
			}
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
			// obstacles/airports: same reference-data tier as geozones above,
			// point features so a circle layer (not fill/line) is enough.
			created.addSource(OBSTACLE_SOURCE, {
				type: 'geojson',
				data: { type: 'FeatureCollection', features: [] }
			});
			created.addLayer(
				{
					id: OBSTACLE_LAYER,
					type: 'circle',
					source: OBSTACLE_SOURCE,
					paint: {
						'circle-radius': 4,
						'circle-color': OBSTACLE_COLOR,
						'circle-stroke-width': 1.5,
						'circle-stroke-color': '#0a0e12'
					}
				},
				ROUTE_SOURCE
			);
			created.addSource(AIRPORT_SOURCE, {
				type: 'geojson',
				data: { type: 'FeatureCollection', features: [] }
			});
			created.addLayer(
				{
					id: AIRPORT_LAYER,
					type: 'circle',
					source: AIRPORT_SOURCE,
					paint: {
						'circle-radius': 4,
						'circle-color': AIRPORT_COLOR,
						'circle-stroke-width': 1.5,
						'circle-stroke-color': '#0a0e12'
					}
				},
				ROUTE_SOURCE
			);
			created.addSource(CITY_SOURCE, {
				type: 'geojson',
				data: { type: 'FeatureCollection', features: [] }
			});
			created.addLayer(
				{
					id: CITY_LAYER,
					type: 'circle',
					source: CITY_SOURCE,
					paint: {
						'circle-radius': 3,
						'circle-color': CITY_COLOR,
						'circle-stroke-width': 1,
						'circle-stroke-color': '#0a0e12'
					}
				},
				ROUTE_SOURCE
			);
			// Breadcrumb trails, added before the points layer below so they
			// paint underneath it - same ordering reasoning as the ward
			// TRAIL_SOURCE elsewhere in this file.
			created.addSource(AIRCRAFT_TRAIL_SOURCE, {
				type: 'geojson',
				data: { type: 'FeatureCollection', features: [] }
			});
			created.addLayer(
				{
					id: AIRCRAFT_TRAIL_LAYER,
					type: 'line',
					source: AIRCRAFT_TRAIL_SOURCE,
					layout: { 'line-join': 'round', 'line-cap': 'round' },
					paint: {
						'line-color': AIRCRAFT_COLOR,
						'line-width': 1.25,
						'line-opacity': 0.35
					}
				},
				ROUTE_SOURCE
			);
			created.addSource(AIRCRAFT_SOURCE, {
				type: 'geojson',
				data: { type: 'FeatureCollection', features: [] }
			});
			// Symbol layer, not circle like every other point layer here -
			// aircraft are the one layer that needs a per-feature rotated icon
			// (heading) and shape (category: fixed-wing/rotorcraft/glider/UAV/
			// ground vehicle), which a circle layer can't express. Icons are
			// registered as sdf:true templates above (see the addImage loop),
			// recolored per-feature here via icon-color instead of needing a
			// separate colored image per state (normal/military/emergency).
			created.addLayer(
				{
					id: AIRCRAFT_LAYER,
					type: 'symbol',
					source: AIRCRAFT_SOURCE,
					layout: {
						'icon-image': ['get', 'iconId'],
						'icon-rotate': ['coalesce', ['get', 'headingDeg'], 0],
						'icon-rotation-alignment': 'map',
						'icon-allow-overlap': true,
						'icon-ignore-placement': true,
						'icon-size': ['get', 'iconSize']
					},
					paint: {
						'icon-color': [
							'case',
							['==', ['get', 'isEmergency'], true],
							AIRCRAFT_EMERGENCY_COLOR,
							['==', ['get', 'isMilitary'], true],
							AIRCRAFT_MILITARY_COLOR,
							AIRCRAFT_COLOR
						]
					}
				},
				ROUTE_SOURCE
			);
			created.addSource(EARTHQUAKE_SOURCE, {
				type: 'geojson',
				data: { type: 'FeatureCollection', features: [] }
			});
			created.addLayer(
				{
					id: EARTHQUAKE_LAYER,
					type: 'circle',
					source: EARTHQUAKE_SOURCE,
					paint: {
						// Magnitude-scaled radius, unlike every other point layer's
						// fixed size - a M6 and a M2.5 reading identically on the
						// map would hide the one piece of information that matters
						// most about an earthquake.
						'circle-radius': ['interpolate', ['linear'], ['get', 'magnitude'], 2.5, 3, 7, 10],
						'circle-color': EARTHQUAKE_COLOR,
						'circle-stroke-width': 1.5,
						'circle-stroke-color': '#0a0e12'
					}
				},
				ROUTE_SOURCE
			);
			created.addSource(WILDFIRE_SOURCE, {
				type: 'geojson',
				data: { type: 'FeatureCollection', features: [] }
			});
			created.addLayer(
				{
					id: WILDFIRE_LAYER,
					type: 'circle',
					source: WILDFIRE_SOURCE,
					paint: {
						'circle-radius': 4,
						'circle-color': WILDFIRE_COLOR,
						'circle-stroke-width': 1.5,
						'circle-stroke-color': '#0a0e12'
					}
				},
				ROUTE_SOURCE
			);
			wirePointLayerPopup(created, OBSTACLE_LAYER, buildObstaclePopupHtml);
			wirePointLayerPopup(created, AIRPORT_LAYER, buildAirportPopupHtml, fetchMetarHtml);
			wirePointLayerPopup(created, CITY_LAYER, buildCityPopupHtml);
			wirePointLayerPopup(
				created,
				AIRCRAFT_LAYER,
				buildAircraftPopupHtml,
				fetchAircraftEnrichmentHtml
			);
			wirePointLayerPopup(created, EARTHQUAKE_LAYER, buildEarthquakePopupHtml);
			wirePointLayerPopup(created, WILDFIRE_LAYER, buildWildfirePopupHtml);
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
			requestThirdPartyLayers();
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
		const showLabels = mapStyle === 'satellite';
		if (!activeMap || !mapLoaded) return;
		const source = activeMap.getSource<maplibregl.RasterTileSource>('basemap');
		source?.setTiles(tiles);
		activeMap.setLayoutProperty(
			SATELLITE_LABELS_SOURCE,
			'visibility',
			showLabels ? 'visible' : 'none'
		);
	});

	// Tells the store itself whether fetching should be happening at all,
	// not just whether the map layer is painted - geozoneStore.requestViewport()
	// and its own failure-retry loop both gate on this internally now.
	// Previously this effect only ever touched the map layer's CSS
	// visibility, so unchecking "No-fly zones" hid already-fetched data but
	// left requestThirdPartyLayers() (called on every moveend, unconditionally)
	// still firing real network requests in the background indefinitely -
	// a real bug, not a feature: burning through OpenAIP's rate limit even
	// with the layer explicitly turned off. Same reasoning applies to every
	// store below, each against its own independent budget.
	$effect(() => {
		geozoneStore.setVisible(showGeozones);
	});
	$effect(() => {
		obstacleStore.setVisible(showObstacles);
	});
	$effect(() => {
		airportStore.setVisible(showAirports);
	});
	$effect(() => {
		aircraftStore.setVisible(showAircraft);
	});
	$effect(() => {
		earthquakeStore.setVisible(showEarthquakes);
	});
	$effect(() => {
		wildfireStore.setVisible(showWildfires);
	});
	$effect(() => {
		const activeMap = map;
		weatherStore.setVisible(showWeather);
		// Same immediate-fetch reasoning as the Cities effect below: turning
		// Weather on shouldn't wait for the next pan/zoom to show anything.
		if (showWeather && activeMap) {
			const center = activeMap.getCenter();
			weatherStore.requestLocation(center.lat, center.lng);
			updateWeatherLocationLabel(center.lat, center.lng);
		}
	});
	// Loads the bundled dataset the first time Cities is turned on (a no-op
	// on every call after - see ensureLoaded's own comment) and applies the
	// current viewport immediately once loaded - without this, turning
	// Cities on would show nothing until the next pan/zoom happened to
	// trigger requestThirdPartyLayers's own moveend-driven call.
	$effect(() => {
		const activeMap = map;
		if (!showCities || !activeMap) return;
		void cityStore.ensureLoaded().then(() => {
			const bounds = activeMap.getBounds();
			cityStore.setViewport(
				[bounds.getWest(), bounds.getSouth(), bounds.getEast(), bounds.getNorth()],
				activeMap.getZoom()
			);
		});
	});

	// no-fly-zone layer's own paint visibility, independent of whether
	// zones are loaded at all (geozoneStore.active, gated on the control
	// below even existing)
	$effect(() => {
		const activeMap = map;
		const visible = showGeozones;
		if (!activeMap || !mapLoaded) return;
		const visibility = visible ? 'visible' : 'none';
		activeMap.setLayoutProperty(GEOZONE_FILL_LAYER, 'visibility', visibility);
		activeMap.setLayoutProperty(GEOZONE_LINE_LAYER, 'visibility', visibility);
	});

	// obstacle/airport layer visibility - same shape as the geozone effect above.
	$effect(() => {
		const activeMap = map;
		const visible = showObstacles;
		if (!activeMap || !mapLoaded) return;
		activeMap.setLayoutProperty(OBSTACLE_LAYER, 'visibility', visible ? 'visible' : 'none');
	});
	$effect(() => {
		const activeMap = map;
		const visible = showAirports;
		if (!activeMap || !mapLoaded) return;
		activeMap.setLayoutProperty(AIRPORT_LAYER, 'visibility', visible ? 'visible' : 'none');
	});
	$effect(() => {
		const activeMap = map;
		const visible = showCities;
		if (!activeMap || !mapLoaded) return;
		activeMap.setLayoutProperty(CITY_LAYER, 'visibility', visible ? 'visible' : 'none');
	});
	$effect(() => {
		const activeMap = map;
		const visible = showAircraft;
		if (!activeMap || !mapLoaded) return;
		activeMap.setLayoutProperty(AIRCRAFT_LAYER, 'visibility', visible ? 'visible' : 'none');
		activeMap.setLayoutProperty(AIRCRAFT_TRAIL_LAYER, 'visibility', visible ? 'visible' : 'none');
	});
	$effect(() => {
		const activeMap = map;
		const visible = showEarthquakes;
		if (!activeMap || !mapLoaded) return;
		activeMap.setLayoutProperty(EARTHQUAKE_LAYER, 'visibility', visible ? 'visible' : 'none');
	});
	$effect(() => {
		const activeMap = map;
		const visible = showWildfires;
		if (!activeMap || !mapLoaded) return;
		activeMap.setLayoutProperty(WILDFIRE_LAYER, 'visibility', visible ? 'visible' : 'none');
	});

	// Operator-drawn zone layer visibility - same shape as the geozone
	// effect just above, which this one was missing entirely: showZones
	// only ever gated the legend chip's own rendering, never the actual
	// ZONE_FILL_LAYER/ZONE_LINE_LAYER on the map, so the "Zones" checkbox
	// visibly did nothing to the polygons themselves.
	$effect(() => {
		const activeMap = map;
		const visible = showZones;
		if (!activeMap || !mapLoaded) return;
		const visibility = visible ? 'visible' : 'none';
		activeMap.setLayoutProperty(ZONE_FILL_LAYER, 'visibility', visibility);
		activeMap.setLayoutProperty(ZONE_LINE_LAYER, 'visibility', visibility);
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

	// push loaded obstacles/airports into their sources whenever either
	// store updates - same shape as the geozone effect above, points
	// instead of polygons.
	$effect(() => {
		const activeMap = map;
		if (!activeMap || !mapLoaded) return;
		const source = activeMap.getSource<maplibregl.GeoJSONSource>(OBSTACLE_SOURCE);
		source?.setData({
			type: 'FeatureCollection',
			features: obstacleStore.obstacles.map((obstacle) => ({
				type: 'Feature',
				properties: {
					category: obstacle.category,
					name: obstacle.name,
					countryCode: obstacle.countryCode ?? null,
					heightM: obstacle.heightM ?? null,
					elevationM: obstacle.elevationM ?? null,
					wikipediaUrl: obstacle.wikipediaUrl ?? null
				},
				geometry: {
					type: 'Point',
					coordinates: [obstacle.longitudeDeg, obstacle.latitudeDeg]
				}
			}))
		});
	});
	$effect(() => {
		const activeMap = map;
		if (!activeMap || !mapLoaded) return;
		const source = activeMap.getSource<maplibregl.GeoJSONSource>(AIRPORT_SOURCE);
		source?.setData({
			type: 'FeatureCollection',
			features: airportStore.airports.map((airport) => ({
				type: 'Feature',
				properties: {
					category: airport.category,
					name: airport.name,
					icaoCode: airport.icaoCode ?? null,
					countryCode: airport.countryCode ?? null,
					elevationM: airport.elevationM ?? null
				},
				geometry: {
					type: 'Point',
					coordinates: [airport.longitudeDeg, airport.latitudeDeg]
				}
			}))
		});
	});
	$effect(() => {
		const activeMap = map;
		if (!activeMap || !mapLoaded) return;
		const source = activeMap.getSource<maplibregl.GeoJSONSource>(CITY_SOURCE);
		source?.setData({
			type: 'FeatureCollection',
			features: cityStore.visibleCities.map((city) => ({
				type: 'Feature',
				properties: {
					name: city.name,
					countryCode: city.countryCode,
					population: city.population
				},
				geometry: {
					type: 'Point',
					coordinates: [city.longitudeDeg, city.latitudeDeg]
				}
			}))
		});
	});
	$effect(() => {
		const activeMap = map;
		if (!activeMap || !mapLoaded) return;
		const source = activeMap.getSource<maplibregl.GeoJSONSource>(AIRCRAFT_SOURCE);
		source?.setData({
			type: 'FeatureCollection',
			features: aircraftStore.aircraft.map((plane) => ({
				type: 'Feature',
				properties: {
					callsign: plane.callsign ?? null,
					icao24: plane.icao24,
					registration: plane.registration ?? null,
					typeDescription: plane.typeDescription ?? null,
					operator: plane.operator ?? null,
					yearBuilt: plane.yearBuilt ?? null,
					altitudeM: plane.altitudeM ?? null,
					velocityMS: plane.velocityMS ?? null,
					verticalRateMS: plane.verticalRateMS ?? null,
					headingDeg: plane.headingDeg ?? null,
					onGround: plane.onGround,
					isMilitary: plane.isMilitary,
					isEmergency: isAircraftEmergency(plane),
					emergency: plane.emergency ?? null,
					iconId: iconIdFor(iconShapeForCategory(plane.category)),
					iconSize: aircraftIconSize(plane.category)
				},
				geometry: {
					type: 'Point',
					coordinates: [plane.longitudeDeg, plane.latitudeDeg]
				}
			}))
		});
	});
	$effect(() => {
		const activeMap = map;
		if (!activeMap || !mapLoaded) return;
		const source = activeMap.getSource<maplibregl.GeoJSONSource>(AIRCRAFT_TRAIL_SOURCE);
		source?.setData({
			type: 'FeatureCollection',
			features: [...aircraftStore.trails.entries()]
				.filter(([, points]) => points.length > 1)
				.map(([icao24, points]) => ({
					type: 'Feature',
					properties: { icao24 },
					geometry: { type: 'LineString', coordinates: points }
				}))
		});
	});
	$effect(() => {
		const activeMap = map;
		if (!activeMap || !mapLoaded) return;
		const source = activeMap.getSource<maplibregl.GeoJSONSource>(EARTHQUAKE_SOURCE);
		source?.setData({
			type: 'FeatureCollection',
			features: earthquakeStore.earthquakes.map((quake) => ({
				type: 'Feature',
				properties: {
					place: quake.place,
					magnitude: quake.magnitude,
					depthKm: quake.depthKm,
					timeMs: quake.timeMs
				},
				geometry: {
					type: 'Point',
					coordinates: [quake.longitudeDeg, quake.latitudeDeg]
				}
			}))
		});
	});
	$effect(() => {
		const activeMap = map;
		if (!activeMap || !mapLoaded) return;
		const source = activeMap.getSource<maplibregl.GeoJSONSource>(WILDFIRE_SOURCE);
		source?.setData({
			type: 'FeatureCollection',
			features: wildfireStore.hotspots.map((hotspot) => ({
				type: 'Feature',
				properties: {
					brightnessK: hotspot.brightnessK,
					frpMw: hotspot.frpMw ?? null,
					confidence: hotspot.confidence,
					acquiredAtIso: hotspot.acquiredAtIso
				},
				geometry: {
					type: 'Point',
					coordinates: [hotspot.longitudeDeg, hotspot.latitudeDeg]
				}
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
				handle.telemetryEl.textContent = `${formatAltitude(state.position.altitudeRelM, unitsStore.current)} \u00b7 ${batteryLabel}`;
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
	<!-- Both banners share this stack so a simultaneous OpenAIP + aircraft
	     failure doesn't overlap into unreadable stacked text - genuinely
	     independent services (different keys, different rate limits), so
	     each gets its own line rather than being combined into one. -->
	<div
		class="absolute top-3 left-1/2 flex w-fit max-w-md -translate-x-1/2 flex-col items-center gap-1.5"
	>
		{#if (geozoneStore.active && geozoneStore.loadError) || (obstacleStore.active && obstacleStore.loadError) || (airportStore.active && airportStore.loadError)}
			<!-- Reassuring, not alarming: this is almost always OpenAIP's own
			     rate limit (see openaip/request-gate.ts, shared across all three
			     layers), which clears on its own within its cooldown window - not
			     a real, ongoing failure the operator needs to act on. One
			     combined banner rather than up to three stacked ones, since all
			     three layers share the same rate-limited key and so tend to fail
			     together, not independently. Each store's own raw error still
			     goes to console.error and sits in its own title attribute for
			     anyone who wants it. -->
			<p
				role="status"
				title={geozoneStore.loadError ?? obstacleStore.loadError ?? airportStore.loadError}
				class="rounded border border-accent bg-panel px-3 py-1.5 text-xs text-accent"
			>
				Loading airspace data - this can take a few seconds.
			</p>
		{/if}
		{#if showAircraft && (aircraftStore.loading || aircraftStore.loadError)}
			<!-- Same reassuring-banner treatment as the OpenAIP one above, not
			     a separate design - it just wasn't wired up when the aircraft
			     layer first shipped, which read as this layer failing silently
			     with no feedback while OpenAIP's own failures were visible.
			     Unlike the OpenAIP banner (error-only: geozone/obstacle/airport
			     loadError never represents a real in-progress wait, since those
			     are quick single requests), this one also covers
			     aircraftStore.loading - a wide zoomed-out view can now take
			     several genuine seconds (a multi-tile batch, ~1.2s apart per
			     tile to respect airplanes.live's rate limit - see
			     aircraft-store.svelte.ts), and that wait had no visible
			     feedback at all before loading existed: loadError alone never
			     fires on a normal successful fetch, so "still working" and
			     "nothing's happening" looked identical. -->
			<p
				role="status"
				title={aircraftStore.loadError}
				class="rounded border border-accent bg-panel px-3 py-1.5 text-xs text-accent"
			>
				Loading aircraft data - this can take a few seconds.
			</p>
		{/if}
	</div>

	{#if showWeather && weatherStore.conditions}
		{@const conditions = weatherStore.conditions}
		<!-- Top-left: the one corner with nothing else in it (legend/scale
		     live bottom-left, controls bottom-right, error banners top-center).
		     Not a point/popup layer like Aircraft/Earthquakes/etc.: weather is
		     a continuous field, not discrete features, so it's a small
		     always-visible readout for the current map center rather than
		     something you hover a marker for. -->
		<div
			class="absolute top-3 left-3 flex w-48 flex-col gap-1 rounded border border-edge bg-panel/90 px-2.5 py-1.5 text-[10px] text-fg-muted"
			aria-label="Current weather at map center"
		>
			{#if weatherLocationLabel}
				<span class="text-[10px] font-medium text-fg-muted uppercase">
					{weatherLocationLabel}
				</span>
			{/if}
			<div class="flex items-center gap-1.5">
				<span class="text-fg">
					<WeatherIcon
						shape={weatherIconShape(conditions.weatherCode, conditions.isDay)}
						size={22}
					/>
				</span>
				<span class="font-mono text-sm font-semibold text-fg">
					{formatTemperature(conditions.temperatureC, unitsStore.current)}
				</span>
				<span class="truncate">{weatherConditionLabel(conditions.weatherCode)}</span>
			</div>
			{#if conditions.feelsLikeC !== undefined && Math.abs(conditions.feelsLikeC - conditions.temperatureC) >= 1}
				<span>Feels like {formatTemperature(conditions.feelsLikeC, unitsStore.current)}</span>
			{/if}
			<div class="grid grid-cols-2 gap-x-2 gap-y-0.5">
				<span>
					{conditions.windDirectionDeg.toFixed(0)}&deg; {formatVehicleSpeed(
						conditions.windSpeedMS,
						unitsStore.current
					)}
				</span>
				{#if conditions.windGustMS !== undefined && conditions.windGustMS > conditions.windSpeedMS}
					<span>Gusts {formatVehicleSpeed(conditions.windGustMS, unitsStore.current)}</span>
				{/if}
				{#if conditions.cloudCoverPct !== undefined}
					<span>Cloud {conditions.cloudCoverPct.toFixed(0)}%</span>
				{/if}
				{#if conditions.humidityPct !== undefined}
					<span>Humidity {conditions.humidityPct.toFixed(0)}%</span>
				{/if}
				{#if conditions.rainMm}
					<span>Rain {formatPrecipitation(conditions.rainMm, unitsStore.current)}</span>
				{/if}
				{#if conditions.showersMm}
					<span>Showers {formatPrecipitation(conditions.showersMm, unitsStore.current)}</span>
				{/if}
				{#if conditions.snowfallMm}
					<span>Snow {formatPrecipitation(conditions.snowfallMm, unitsStore.current)}</span>
				{/if}
				{#if !conditions.rainMm && !conditions.showersMm && !conditions.snowfallMm}
					<span>
						Precip {formatPrecipitation(conditions.precipitationMm, unitsStore.current)}
					</span>
				{/if}
				{#if conditions.visibilityM !== undefined}
					<span>Vis {formatDistanceUnits(conditions.visibilityM, unitsStore.current)}</span>
				{/if}
				{#if conditions.pressureHpa !== undefined}
					<span>{formatPressure(conditions.pressureHpa, unitsStore.current)}</span>
				{/if}
			</div>
		</div>
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
				<!-- Anchored above and right-aligned to the trigger, same
				     opening direction as ThemeToggle/UnitsToggle in the right
				     rail - and, unlike the old top-1/2/-translate-y-1/2
				     centering this replaced, its height is capped with its own
				     scrollbar instead of being free to grow past the map's
				     top/bottom edges (that used to clip both the map style list
				     and the Zones row at the bottom silently). -->
				<div
					class="absolute right-0 bottom-full z-30 mb-1 flex max-h-[min(90vh,44rem)] w-72 flex-col gap-3 overflow-y-auto rounded border border-edge bg-panel/95 p-2.5"
				>
					<div>
						<p class="mb-1.5 text-[9px] font-medium tracking-widest text-fg-muted">MAP STYLE</p>
						<!-- A choice, not an on/off switch - the selected card gets an
					     accent border and a check badge, not a checkbox, since
					     exactly one is always active. Plain light/dark isn't offered
					     here as its own pair: "Map" follows the app-wide theme
					     toggle (themeStore) rather than exposing a second,
					     independent choice - see basemapTiles's own comment.
					     Roadmap only appears next to the light theme, for the same
					     reason. Preview thumbnails are real captures of each style
					     (see src/lib/assets/map-style-previews/), not swatches -
					     bundled at build time, same asset-import pattern as
					     logo-mark.svg. -->
						<div class="grid grid-cols-2 gap-1.5">
							{#each MAP_STYLE_OPTIONS as option (option.id)}
								{#if option.id !== 'roadmap' || themeStore.current === 'light'}
									<button
										type="button"
										onclick={() => setMapStyle(option.id)}
										aria-pressed={mapStyle === option.id}
										class="relative aspect-[8/5] overflow-hidden rounded border {mapStyle ===
										option.id
											? 'border-accent ring-1 ring-accent'
											: 'border-edge hover:border-fg-muted'}"
									>
										<img
											src={stylePreviewFor(option)}
											alt=""
											class="h-full w-full object-cover"
											loading="lazy"
										/>
										<span
											class="absolute inset-x-0 bottom-0 bg-gradient-to-t from-black/75 to-transparent px-1.5 pt-3 pb-1 text-left text-[10px] font-medium text-white"
										>
											{option.label}
										</span>
										{#if mapStyle === option.id}
											<span
												class="absolute top-1 right-1 flex h-3.5 w-3.5 items-center justify-center rounded-full bg-accent text-white"
												aria-hidden="true"
											>
												<svg
													width="8"
													height="8"
													viewBox="0 0 24 24"
													fill="none"
													stroke="currentColor"
													stroke-width="3"
													stroke-linecap="round"
													stroke-linejoin="round"
												>
													<path d="M20 6 9 17l-5-5" />
												</svg>
											</span>
										{/if}
									</button>
								{/if}
							{/each}
						</div>
					</div>
					<div class="border-t border-edge pt-2">
						<!-- Split into two light categories instead of one flat
					     8-item list - purely a layout grouping (each item's own
					     .active/no-gate rules are unchanged), but it reads as
					     organized rather than a wall of checkboxes, and the
					     2-column grid halves the vertical space either group
					     needs. -->
						{#if geozoneStore.active || obstacleStore.active || airportStore.active}
							<p class="mb-1 text-[9px] font-medium tracking-widest text-fg-muted">AIRSPACE</p>
							<div class="mb-2 grid grid-cols-2 gap-x-1 gap-y-0.5">
								{#if geozoneStore.active}
									<label
										class="flex cursor-pointer items-center gap-1.5 rounded px-1 py-1 text-[11px] hover:bg-white/5"
									>
										<input type="checkbox" bind:checked={showGeozones} class="accent-accent" />
										No-fly zones
									</label>
								{/if}
								{#if obstacleStore.active}
									<label
										class="flex cursor-pointer items-center gap-1.5 rounded px-1 py-1 text-[11px] hover:bg-white/5"
									>
										<input type="checkbox" bind:checked={showObstacles} class="accent-accent" />
										Obstacles
									</label>
								{/if}
								{#if airportStore.active}
									<label
										class="flex cursor-pointer items-center gap-1.5 rounded px-1 py-1 text-[11px] hover:bg-white/5"
									>
										<input type="checkbox" bind:checked={showAirports} class="accent-accent" />
										Airports
									</label>
								{/if}
							</div>
						{/if}
						<p class="mb-1 text-[9px] font-medium tracking-widest text-fg-muted">DATA LAYERS</p>
						<div class="grid grid-cols-2 gap-x-1 gap-y-0.5">
							<!-- No .active gate: cities are a bundled dataset, not an
							     OpenAIP layer, so there's no key-configured state to
							     wait on (see city-store.svelte.ts's own comment). -->
							<label
								class="flex cursor-pointer items-center gap-1.5 rounded px-1 py-1 text-[11px] hover:bg-white/5"
							>
								<input type="checkbox" bind:checked={showCities} class="accent-accent" />
								Cities
							</label>
							<!-- No .active gate: Open-Meteo needs no key. Not a point
							     layer like everything else here - see the weather
							     widget itself, rendered separately below. Kept right
							     after Cities (both no-gate, always-available layers)
							     rather than grouped with the network/hazard feeds
							     below it. -->
							<label
								class="flex cursor-pointer items-center gap-1.5 rounded px-1 py-1 text-[11px] hover:bg-white/5"
							>
								<input type="checkbox" bind:checked={showWeather} class="accent-accent" />
								Weather
							</label>
							<!-- No .active gate either: OpenSky's anonymous tier needs no
							     signup (see aircraft-store.svelte.ts's own comment). -->
							<label
								class="flex cursor-pointer items-center gap-1.5 rounded px-1 py-1 text-[11px] hover:bg-white/5"
							>
								<input type="checkbox" bind:checked={showAircraft} class="accent-accent" />
								Aircraft
							</label>
							<!-- Same again: USGS's feed is fully open. -->
							<label
								class="flex cursor-pointer items-center gap-1.5 rounded px-1 py-1 text-[11px] hover:bg-white/5"
							>
								<input type="checkbox" bind:checked={showEarthquakes} class="accent-accent" />
								Earthquakes
							</label>
							{#if wildfireStore.active}
								<label
									class="flex cursor-pointer items-center gap-1.5 rounded px-1 py-1 text-[11px] hover:bg-white/5"
								>
									<input type="checkbox" bind:checked={showWildfires} class="accent-accent" />
									Wildfires
								</label>
							{/if}
						</div>
					</div>
					<div class="border-t border-edge pt-2">
						<label
							class="flex cursor-pointer items-center gap-1.5 rounded px-1 py-1 text-[11px] hover:bg-white/5"
						>
							<input type="checkbox" bind:checked={showZones} class="accent-accent" />
							Zones
						</label>
					</div>
				</div>
			{/if}
		</div>
	</div>

	{#if (zoneStore.zoneIds.length > 0 && showZones) || (geozoneStore.active && showGeozones) || (obstacleStore.active && showObstacles) || (airportStore.active && showAirports) || showCities || showAircraft || showEarthquakes || (wildfireStore.active && showWildfires)}
		{@const showZoneRow = zoneStore.zoneIds.length > 0 && showZones}
		{@const showGeozoneRow = geozoneStore.active && showGeozones}
		{@const showObstacleRow = obstacleStore.active && showObstacles}
		{@const showAirportRow = airportStore.active && showAirports}
		{@const showCityRow = showCities}
		{@const showAircraftRow = showAircraft}
		{@const showEarthquakeRow = showEarthquakes}
		{@const showWildfireRow = wildfireStore.active && showWildfires}
		<!-- One panel, not two separately-floating boxes: they used to sit at
		     different bottom offsets that had to be kept in sync by hand, and
		     read as visually disconnected even when both were showing. -->
		<div
			class="absolute bottom-8 left-[10px] flex flex-col gap-1.5 border border-edge bg-panel/90 px-2 py-1.5"
		>
			{#if showZoneRow}
				<ul class="flex gap-3" aria-label="Zone legend">
					<li class="flex items-center gap-1 text-[10px]">
						<span class="h-2 w-2 rounded-full" style="background-color: {ZONE_KEEP_IN_COLOR}"
						></span>
						Keep in
					</li>
					<li class="flex items-center gap-1 text-[10px]">
						<span class="h-2 w-2 rounded-full" style="background-color: {ZONE_KEEP_OUT_COLOR}"
						></span>
						Keep out
					</li>
				</ul>
			{/if}
			{#if showGeozoneRow}
				<ul
					class="flex gap-3 {showZoneRow ? 'border-t border-edge pt-1.5' : ''}"
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
			{#if showObstacleRow || showAirportRow || showCityRow || showAircraftRow || showEarthquakeRow || showWildfireRow}
				<ul
					class="flex flex-wrap gap-3 {showZoneRow || showGeozoneRow
						? 'border-t border-edge pt-1.5'
						: ''}"
					aria-label="Point layer legend"
				>
					{#if showObstacleRow}
						<li class="flex items-center gap-1 text-[10px]">
							<span class="h-2 w-2 rounded-full" style="background-color: {OBSTACLE_COLOR}"></span>
							Obstacles
						</li>
					{/if}
					{#if showAirportRow}
						<li class="flex items-center gap-1 text-[10px]">
							<span class="h-2 w-2 rounded-full" style="background-color: {AIRPORT_COLOR}"></span>
							Airports
						</li>
					{/if}
					{#if showCityRow}
						<li class="flex items-center gap-1 text-[10px]">
							<span class="h-2 w-2 rounded-full" style="background-color: {CITY_COLOR}"></span>
							Cities
						</li>
					{/if}
					{#if showAircraftRow}
						<li class="flex items-center gap-1 text-[10px]">
							<span class="h-2 w-2 rounded-full" style="background-color: {AIRCRAFT_COLOR}"></span>
							Aircraft
						</li>
					{/if}
					{#if showEarthquakeRow}
						<li class="flex items-center gap-1 text-[10px]">
							<span class="h-2 w-2 rounded-full" style="background-color: {EARTHQUAKE_COLOR}"
							></span>
							Earthquakes
						</li>
					{/if}
					{#if showWildfireRow}
						<li class="flex items-center gap-1 text-[10px]">
							<span class="h-2 w-2 rounded-full" style="background-color: {WILDFIRE_COLOR}"></span>
							Wildfires
						</li>
					{/if}
				</ul>
			{/if}
		</div>
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
	/*
	 * Same fix, same reason, for the zoom in/out buttons: MapLibre's own
	 * icons here are a filled +/- glyph, a different visual weight than
	 * this file's stroke-based line icons everywhere else (layers, ruler,
	 * attribution's "i") - the invert filter alone made them the right
	 * rough color but not the same style, which is what read as
	 * mismatched. stroke-width 1.75, not the attribution "i"'s 2.75 (fixed
	 * to match, below): 1.75 is this file's own actual dominant weight for
	 * every inline-SVG button icon (layers, ruler), so these two match the
	 * majority instead of the one other vendor-icon override that had
	 * drifted from it.
	 *
	 * These also read as oversized even after the color and stroke-width
	 * fixes: with no explicit background-size, the icon fills the
	 * button's entire native 29px box edge to edge, where the layers/
	 * ruler buttons instead show a small ~15px icon with real padding
	 * around it inside their own box. Button size is left alone (resizing
	 * it to 32px like the attribution "i" was tried and made the whole
	 * control read as bigger overall, not just the glyph inside it, which
	 * wasn't the actual complaint) - only the icon itself is shrunk via
	 * background-size, keeping the same 29px button MapLibre already uses
	 * here.
	 */
	:global(.maplibregl-ctrl-zoom-in .maplibregl-ctrl-icon) {
		filter: none !important;
		background-image: url("data:image/svg+xml;charset=utf-8,%3Csvg xmlns='http://www.w3.org/2000/svg' width='24' height='24' viewBox='0 0 24 24' fill='none' stroke='%238b98a5' stroke-width='1.75' stroke-linecap='round'%3E%3Cline x1='12' y1='6' x2='12' y2='18'/%3E%3Cline x1='6' y1='12' x2='18' y2='12'/%3E%3C/svg%3E") !important;
		background-size: 15px !important;
		background-position: center !important;
		background-repeat: no-repeat !important;
	}
	:global(.maplibregl-ctrl-zoom-out .maplibregl-ctrl-icon) {
		filter: none !important;
		background-image: url("data:image/svg+xml;charset=utf-8,%3Csvg xmlns='http://www.w3.org/2000/svg' width='24' height='24' viewBox='0 0 24 24' fill='none' stroke='%238b98a5' stroke-width='1.75' stroke-linecap='round'%3E%3Cline x1='6' y1='12' x2='18' y2='12'/%3E%3C/svg%3E") !important;
		background-size: 15px !important;
		background-position: center !important;
		background-repeat: no-repeat !important;
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
		   grayish siblings instead of matching them. stroke-width 1.75, not
		   the 2.75 this used to be: 1.75 is this file's own actual dominant
		   weight for every inline-SVG button icon (layers, ruler) - 2.75 had
		   drifted heavier than all of them, reading as visibly thicker than
		   its siblings once actually compared side by side. */
		background-image: url("data:image/svg+xml;charset=utf-8,%3Csvg xmlns='http://www.w3.org/2000/svg' width='24' height='24' viewBox='0 0 24 24' fill='none' stroke='%238b98a5' stroke-width='1.75' stroke-linecap='round' stroke-linejoin='round'%3E%3Cline x1='12' y1='10.5' x2='12' y2='16'/%3E%3Ccircle cx='12' cy='7' r='1.3' fill='%238b98a5' stroke='none'/%3E%3C/svg%3E") !important;
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

	/* Same reasoning as .maplibregl-ctrl-group above: MapLibre's own Popup
	   CSS ships hard-coded for a white content box (#fff background, dark
	   text), which read as washed-out/illegible once the obstacle/airport/
	   city hover popups' own text used the app's --color-fg tokens (light
	   colors, meant for a dark panel) against that white background. Themed
	   the same way as every other MapLibre-injected control, so it follows
	   dark/light theme changes automatically instead of needing its own
	   separate toggle. */
	:global(.maplibregl-popup-content) {
		background: var(--color-panel);
		border: 1px solid var(--color-edge);
		box-shadow: none;
		padding: 8px 10px;
		border-radius: 4px;
		/* MapLibre sets no width of its own - the popup just shrinks to fit
		   whatever HTML it's given, and popupRow's own justify-between rows
		   have no minimum, so a popup with only a couple of short rows (or
		   a long label like "Registration") reads as cramped. A floor, not
		   a fixed width - still grows past this for genuinely wide content
		   (the airport-style METAR rows, an aircraft's Route row). */
		min-width: 200px;
	}
	/* MapLibre's own stylesheet sets the tip's actual visible color via
	   anchor-direction-specific selectors (one border side per direction,
	   the others transparent to form the triangle), which are more
	   specific than a flat .maplibregl-popup-tip override and so win the
	   cascade regardless of Vite's CSS ordering - confirmed live: the
	   generic override above was fully ignored, tip stayed white in dark
	   theme. Matching each of MapLibre's own selectors directly is the
	   only reliable way to actually override this. */
	:global(.maplibregl-popup-anchor-top .maplibregl-popup-tip) {
		border-bottom-color: var(--color-panel);
	}
	:global(.maplibregl-popup-anchor-bottom .maplibregl-popup-tip) {
		border-top-color: var(--color-panel);
	}
	:global(.maplibregl-popup-anchor-left .maplibregl-popup-tip) {
		border-right-color: var(--color-panel);
	}
	:global(.maplibregl-popup-anchor-right .maplibregl-popup-tip) {
		border-left-color: var(--color-panel);
	}
	:global(.maplibregl-popup-close-button) {
		color: var(--color-fg-muted);
	}
	:global(.maplibregl-popup-close-button:hover) {
		background: transparent;
		color: var(--color-fg);
	}
</style>
