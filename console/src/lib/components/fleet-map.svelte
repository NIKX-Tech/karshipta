<script module lang="ts">
	import * as maplibreglModule from 'maplibre-gl';
	// Both calls below move here together, in their original relative order,
	// because that order plausibly matters: the RTL plugin's own worker
	// broadcast may depend on the worker URL already being configured. The
	// very first draft of this fix moved only setRTLTextPlugin into this
	// module block, which runs before the instance script below ever does -
	// that silently reversed the two calls' order (setWorkerUrl used to run
	// first, every time, in the old single-script version), a real risk
	// caught by re-checking this fix rather than trusting it, not something
	// confirmed as the cause of any specific incident. Keeping both calls
	// together here removes the question entirely instead of leaving it
	// open.
	//
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
	// Guarded on document, not just wrapped in a function called from
	// onMount: <script module> body code runs the moment this module is
	// first imported, which happens during SvelteKit's SSR/prerender
	// analysis too (any page that statically imports FleetMap pulls this
	// in, whether or not FleetMap ever actually renders there). Confirmed
	// live in a consumer app's own `npm run build`: maplibre-gl's
	// setRTLTextPlugin calls into code that reads `document`, which
	// doesn't exist in that Node analysis pass, crashing the build outright
	// - the same class of environment this file's own worker/RTL setup
	// needs a real browser for in the first place.
	if (typeof document !== 'undefined') {
		maplibreglModule.setWorkerUrl('/maplibre-gl/maplibre-gl-worker.mjs');
		// MapLibre's own doc comment on setRTLTextPlugin is explicit: "If the
		// status is loaded and the plugin is requested again, an error will be
		// thrown" - this is a once-per-page registration, not a per-map-instance
		// one. Calling it from the component's own instance script (as this did
		// before) re-requested it on every single FleetMap mount - every repeat
		// visit to a page that mounts this component, not just a rare edge case -
		// confirmed live as the root cause of an intermittent blank map: the
		// second registration corrupts the first one's still-in-flight (lazy:
		// true defers the real fetch) worker broadcast, throwing deep inside
		// MapLibre's own internals ("Cannot read properties of undefined
		// (reading 'off')" in a control's onRemove, several frames below
		// anything this app's own code calls directly) and leaving the map
		// mid-setup. <script module> runs exactly once per page load regardless
		// of how many times this component itself mounts/unmounts, which is
		// what this call actually needs - the getRTLTextPluginStatus() check is
		// a second, defensive layer on top of that, not a replacement for it.
		if (maplibreglModule.getRTLTextPluginStatus() === 'unavailable') {
			// OpenFreeMap's vector styles label Arabic/Hebrew/etc. place names
			// via glyphs, not baked-in raster text like CARTO's old tiles -
			// without this plugin maplibre draws each glyph in isolated form and
			// left-to-right storage order instead of shaping/joining letters and
			// reordering right-to-left, so RTL labels render as disconnected,
			// backwards characters. lazy=true: only fetched the first time a
			// tile actually needs RTL shaping, not on every map load.
			void maplibreglModule.setRTLTextPlugin('/maplibre-gl/mapbox-gl-rtl-text.js', true);
		}
	}
</script>

<script lang="ts">
	import { untrack } from 'svelte';
	import * as maplibregl from 'maplibre-gl';
	import 'maplibre-gl/dist/maplibre-gl.css';
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
	import { isAircraftEmergency, type Aircraft, type AircraftCategory } from '$lib/aircraft/types';
	import {
		aircraftIconMarkup,
		iconShapeForCategory,
		type AircraftIconShape
	} from '$lib/aircraft/icons';
	import { earthquakeStore } from '$lib/earthquakes/earthquake-store.svelte';
	import { wildfireStore } from '$lib/wildfires/wildfire-store.svelte';
	import { weatherStore } from '$lib/weather/weather-store.svelte';
	import { weatherIconShape, weatherConditionLabel } from '$lib/weather/condition';
	import WeatherIcon from '$lib/components/weather-icon.svelte';
	import SheetHeader from '$lib/components/ui/sheet-header.svelte';
	import { zoneStore } from '$lib/zones/zone-store.svelte';
	import { themeStore } from '$lib/theme.svelte';
	import {
		loadBasemapLayers,
		type VectorBasemap
	} from '$lib/assets/basemap-styles/load-basemap-layers';
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
		 * Read once, same as centerLat/centerLon. Defaults to INITIAL_ZOOM
		 * (city-scale) when omitted, matching every existing caller's
		 * current behavior unchanged. A consuming app that resolves its own
		 * fallback center server-side (e.g. a country-scale view for a
		 * not-yet-located guest) needs a wider starting zoom than
		 * INITIAL_ZOOM to actually show that scale.
		 */
		initialZoom?: number;
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
		/**
		 * Fires after the camera settles from a pan, zoom, or flyTo (bound to
		 * moveend, not move - the latter fires every animation frame, far too
		 * often for a consuming app that wants to persist the result, e.g.
		 * remembering an operator's last view across a page reload). Never
		 * fires for the initial camera placement itself, only for movement
		 * after that - centerLat/centerLon/initialZoom already cover the
		 * starting point.
		 */
		onMapMove?: (view: { lat: number; lon: number; zoom: number }) => void;
		/**
		 * Friendly name for the selected ward's marker badge, in place of
		 * the raw wardId - same "a consumer-owned concept this library has
		 * no notion of" reasoning as ownerFor above. Falls back to wardId
		 * when absent or when this callback itself returns undefined for a
		 * given ward (no name set for it).
		 */
		nameFor?: (wardId: string) => string | undefined;
		/**
		 * Hides the airspace/aircraft loading toast - a plain prop for the
		 * same reason crosshair/placementPoint are: a consuming app's own
		 * mobile shell state (e.g. "a full-screen drawer is covering the
		 * map right now") isn't a concept this library has any notion of,
		 * and mobile-surface-ui.svelte.ts is deliberately never shared via
		 * the package barrel (every consumer keeps its own). Without this,
		 * the toast stayed visible, dimmed by the drawer's own backdrop -
		 * confirmed live as a real bug: it's a map-only indicator with no
		 * reason to show through chrome that's actively covering the map
		 * it describes.
		 */
		hideLoadingToast?: boolean;
		/**
		 * True while a consuming app's own mobile bottom sheet (ward
		 * detail, the fleet/gateway drawer) is open - closes this file's
		 * own layers/attribution sheets when it turns true. Without this,
		 * a consuming app's sheet and this file's own could both be open
		 * at once (two independent toggle states, nothing coordinating
		 * between them), stacking two `fixed inset-x-0 bottom-0` sheets on
		 * top of each other and leaving the map-shrink math - which only
		 * ever accounts for one local + one external reserve, not two
		 * locals at once - visibly wrong. Same reasoning as
		 * hideLoadingToast's own comment: a consuming app's mobile shell
		 * state isn't a concept this library has any notion of on its own.
		 */
		externalSheetOpen?: boolean;
		/**
		 * Fires the moment this file's own layers or attribution sheet
		 * opens, so a consuming app can close its own ward-detail/fleet
		 * sheet in response - the other half of externalSheetOpen's own
		 * coordination, going the opposite direction.
		 */
		onLocalSheetOpen?: () => void;
	}

	const {
		centerLat,
		centerLon,
		initialZoom,
		onMapClick,
		crosshair,
		placementPoint,
		ownerFor,
		onMapMove,
		nameFor,
		hideLoadingToast = false,
		externalSheetOpen = false,
		onLocalSheetOpen
	}: Props = $props();

	// City-scale, not street-block: the first thing a viewer needs is "where
	// in the world is this", not individual streets. z15 (the old value)
	// only shows a few blocks - unreadable as a city, and the point of
	// landing on the map is immediately losing your bearings.
	const INITIAL_ZOOM = 12;

	// Closer than INITIAL_ZOOM (city-scale) on purpose - the "locate me"
	// button's whole point is "show exactly where I am", not just which
	// city, so it earns a tighter zoom than the initial landing view does.
	const LOCATE_ZOOM = 14;

	// Selecting a ward or an aircraft (not just "locate me") earns the
	// tightest zoom of the three - this is "look at this one thing", not
	// "where am I". Math.max'd against the camera's current zoom at the
	// call site, never forced down to this value: switching between two
	// selections while already zoomed in past this shouldn't zoom back out.
	const SELECTED_ENTITY_ZOOM = 16;

	// Tilting the camera on selection is what actually makes the existing
	// altitude-lift rendering (the marker's "stem" below, in the marker
	// position effect) visible - at pitch 0 sin(pitch) is 0, so the stem
	// never had any height to show even though the formula already existed.
	// Shared by wards and aircraft (both use the same ground+stem+body
	// marker shape - see aircraftMarkerElement's own comment on why). 45:
	// steep enough that a real stem reads as a stem, shallow enough to stay
	// legible and not disorient; MapLibre's own default maxPitch is 60.
	const SELECTED_ENTITY_PITCH_DEG = 45;

	// eventData tag for the selected-ward/selected-aircraft follow
	// effects' own recurring recenter (see either effect's own comment on
	// why) - a plain object identity check (the moveend handlers below
	// compare the received eventData's own isFollowTick field, not this
	// reference) since MapLibre clones/merges eventData into each fired
	// event rather than passing this exact object through.
	const SELECTION_FOLLOW_TICK_EVENT_DATA = { isFollowTick: true };

	// Below this, don't fetch airspace data at all - airspace boundaries
	// (FIRs, TMAs, prohibited zones) are country/region-scale shapes, not
	// meaningful at a world- or continent-level view, and every fetch below
	// this zoom would just be hitting openaip.ts's 5-degree bbox clamp
	// repeatedly for no visual benefit. 7 is roughly "a country fills the
	// screen" - well above "the whole continent" (where clamping would
	// otherwise fire on almost every pan) and well below the fleet's own
	// working zoom (INITIAL_ZOOM=12), so this never affects normal use.
	const MIN_GEOZONE_ZOOM = 7;

	// CARTO's free legacy tile CDN (basemaps.cartocdn.com) started baking
	// an "API KEY REQUIRED" watermark into every tile image while still
	// returning HTTP 200 - confirmed live, and not caught by any error
	// handling since nothing about the response looks like a failure.
	// OpenFreeMap (openfreemap.org) is the free/no-key replacement, but it
	// serves vector styles, not raster PNG tiles like every other basemap
	// here - hence the different shape below (a vector source + a vendored
	// layer set per style, added/removed the same additive way every other
	// overlay in this file works, rather than raster tile URLs swapped via
	// setTiles()). See src/lib/assets/basemap-styles/README.md for exactly
	// what's vendored and why.
	const OPENFREEMAP_VECTOR_SOURCE = 'openmaptiles';
	const OPENFREEMAP_TILES_URL = 'https://tiles.openfreemap.org/planet';
	const OPENFREEMAP_SPRITE_URL = 'https://tiles.openfreemap.org/sprites/ofm_f384/ofm';
	const OPENFREEMAP_GLYPHS_URL = 'https://tiles.openfreemap.org/fonts/{fontstack}/{range}.pbf';
	// 'default' follows the app theme (dark/light, same pairing CARTO used
	// to provide) so the map follows the app-wide theme toggle (themeStore)
	// rather than exposing a second, independent light/dark choice of its
	// own; a light map under a dark app chrome (or the reverse) was a real,
	// reported problem, not a style preference. 'roadmap' (OpenFreeMap's
	// Liberty style) is offered only alongside the light theme (see
	// mapStyle's own comment below), since its light background would
	// reintroduce that same mismatch.
	// Esri World Imagery for satellite - the standard no-key choice for
	// this, layered on top as an orthogonal on/off (real photography has no
	// light/dark variant to match the theme against). Esri's tile scheme is
	// z/y/x, not the usual z/x/y - easy to get backwards.
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
	// switch to, set once at source creation on the raster 'basemap' source
	// and read directly by the custom attribution button/popup below (see
	// attributionControl: false's own comment - this stopped being
	// MapLibre's own native control's job). Showing all credits regardless
	// of which basemap is currently active is a common, acceptable
	// tradeoff other map apps make too - not worth rewriting on every
	// switch. Plain "©" characters, not "&copy;" entities: this used
	// to feed MapLibre's own control, which renders via innerHTML and
	// needed them; now it's plain Svelte template text (not {@html}, which
	// eslint's svelte/no-at-html-tags flags even for a string this file
	// itself fully controls), so a literal character displays correctly
	// without needing HTML entity decoding at all.
	// One entry per credit, not one run-on string - confirmed live as a
	// real bug otherwise: six separate credits joined with nothing but
	// plain spaces read as one ambiguous sentence (is "OpenFreeMap" its
	// own credit or attached to "OpenStreetMap contributors"? the string
	// alone can't say), and rendering that single long string inside the
	// desktop popup's own max-w-xs box wrapped unpredictably mid-phrase,
	// giving the popup a dynamic height that changed with wherever the
	// text happened to break. Rendered one per line below (both the
	// desktop popup and the mobile sheet), each kept to a single line via
	// whitespace-nowrap - a fixed, predictable height (one line per
	// credit) instead of wrapped prose, and each credit now reads
	// unambiguously on its own. BASEMAP_ATTRIBUTION (the joined string)
	// still exists for MapLibre's own source `attribution` field, which
	// expects one string, not an array - joined from the same array so
	// there's one source of truth, not two copies that could drift apart.
	const BASEMAP_ATTRIBUTION_LINES = [
		'© OpenStreetMap contributors · OpenFreeMap',
		'© OpenMapTiles',
		'© Esri, Maxar, Earthstar Geographics',
		'© OpenTopoMap (CC-BY-SA) · GEBCO Compilation Group'
	];
	const BASEMAP_ATTRIBUTION = BASEMAP_ATTRIBUTION_LINES.join(' ');

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
	// undefined for 'default'/'roadmap': those are vector-backed now (see
	// activeVectorStyle below), not a raster tile source at all.
	const activeRasterTiles = $derived(
		mapStyle === 'satellite'
			? SATELLITE_TILES
			: mapStyle === 'terrain'
				? TERRAIN_TILES
				: mapStyle === 'bathymetry'
					? BATHYMETRY_TILES
					: undefined
	);
	const activeVectorStyle = $derived<VectorBasemap | undefined>(
		mapStyle === 'roadmap'
			? 'roadmap'
			: mapStyle === 'default'
				? themeStore.current === 'light'
					? 'light'
					: 'dark'
				: undefined
	);

	// Off by default: an operator who configures PUBLIC_OPENAIP_KEY opts
	// into the layer existing at all, but seeing it on every load regardless
	// wasn't the intent - matches "Zones" (showZones) already defaulting to
	// on being the operator's OWN drawn geometry, a different trust level
	// than a third-party overlay.
	let showGeozones = $state(false);
	// Default on, unlike showGeozones above: real collision hazards for
	// low-altitude flight are directly relevant to this app's own purpose
	// (a drone/ward C2 platform), not just general reference context the
	// way No-fly zones' regulatory boundaries are - same "no key needed to
	// be useful, silently no-ops without one" reasoning as showAirports
	// below. Only safe to default on now that obstacle-store.svelte.ts has
	// zoom-based thinning (OBSTACLE_MIN_ZOOM) - unlike Airports/Aircraft,
	// OpenAIP doesn't sparsely cap obstacles the way it does other layers,
	// so a zoomed-out city view could otherwise return the full 200-result
	// page with nothing thinning it.
	let showObstacles = $state(true);
	// Default on, same reasoning as Cities/Weather below: no key required to
	// see something, and with an empty fleet (the common first-run state)
	// an all-black map reads as broken rather than "nothing connected yet".
	// If PUBLIC_OPENAIP_KEY is unset this silently shows nothing (see
	// airportStore's own no-op-without-a-key behavior) rather than erroring,
	// so defaulting it on costs nothing for a deployment without the key.
	let showAirports = $state(true);
	// Cities default on, unlike the three OpenAIP layers above: it's a
	// bundled dataset with no key/trust/rate-limit concern (see
	// city-store.svelte.ts's own comment), so there's no reason to make an
	// operator opt in just to see it.
	let showCities = $state(true);
	// Default on, unlike showGeozones/showObstacles/showAirports: no key
	// needed, and an empty fleet with every layer off used to render as a
	// plain black map on first load. (api.airplanes.live itself stopped
	// serving requests in 2026-08 - its own API repo is now archived. Its
	// documented replacement, api.adsb.one, turned out to have its own
	// infra problems on top of a Cloudflare bot-challenge - see
	// proxiedSource.ts's own header comment on why aircraft now go through
	// this app's own same-origin proxy route to adsb.lol instead of a
	// direct third-party fetch. AircraftProxyAccessError covers any future
	// repeat of this failure mode.)
	let showAircraft = $state(true);
	let showEarthquakes = $state(true);
	let showWildfires = $state(true);
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
	let attributionMenuOpen = $state(false);
	let attributionMenuEl: HTMLDivElement | undefined;
	// Tap-to-select for aircraft (mobile's own equivalent of desktop's
	// hover popup, which touch has no equivalent gesture for at all) -
	// requested directly, following the exact same local-sheet shape
	// layers/attribution already use below, not a new pattern. Local
	// state, not a fleet-store-style singleton: nothing outside this file
	// needs to know which aircraft is selected, unlike fleet.selectedWardId
	// (right-panel.svelte's whole tab system depends on that one).
	let selectedAircraftIcao24 = $state<string | undefined>(undefined);

	// Mobile bottom sheet heights, real measured values (content varies -
	// the layers menu is much taller than the attribution credits line),
	// fed into the map container's own height below so an open sheet
	// pushes the visible map up instead of just covering the bottom of
	// it - confirmed as the wanted behavior directly, not a guess.
	// Mutually exclusive by construction (toggleLayersMenu/
	// toggleAttributionMenu/selectAircraft below close one another first,
	// and also call onLocalSheetOpen to close a consuming app's own sheet
	// too - see externalSheetOpen's own comment): any two of these five
	// mobile sheets open at once would all be `fixed inset-x-0 bottom-0`,
	// stacking on top of each other at the same spot, and this reserve
	// math only has room to account for one at a time regardless.
	let layersMenuMobileHeight = $state(0);
	let attributionMenuMobileHeight = $state(0);
	let aircraftSheetMobileHeight = $state(0);
	const mobileLocalSheetReserve = $derived(
		layersMenuOpen
			? layersMenuMobileHeight
			: attributionMenuOpen
				? attributionMenuMobileHeight
				: selectedAircraftIcao24 !== undefined
					? aircraftSheetMobileHeight
					: 0
	);

	// externalSheetOpen flipping true (a consuming app's own ward-detail/
	// fleet sheet just opened) closes these the same direction -
	// confirmed live as a real bug otherwise: opening the ward sheet,
	// then tapping the layers button, left both open and stacked, the
	// map-shrink math only ever subtracting one reserve while two sheets
	// physically covered the screen.
	$effect(() => {
		if (externalSheetOpen) {
			layersMenuOpen = false;
			attributionMenuOpen = false;
			selectedAircraftIcao24 = undefined;
		}
	});

	function toggleLayersMenu() {
		if (!layersMenuOpen) {
			attributionMenuOpen = false;
			selectedAircraftIcao24 = undefined;
			onLocalSheetOpen?.();
		}
		layersMenuOpen = !layersMenuOpen;
	}
	function toggleAttributionMenu() {
		if (!attributionMenuOpen) {
			layersMenuOpen = false;
			selectedAircraftIcao24 = undefined;
			onLocalSheetOpen?.();
		}
		attributionMenuOpen = !attributionMenuOpen;
	}
	// Toggles like the other two local sheets (tapping the same aircraft
	// again deselects), but also clears any selected ward - only one
	// "selected map entity" at a time, the same way opening this closes
	// layers/attribution and vice versa, so there's never two competing
	// info panels up at once.
	function selectAircraft(icao24: string) {
		if (selectedAircraftIcao24 === icao24) {
			selectedAircraftIcao24 = undefined;
			return;
		}
		layersMenuOpen = false;
		attributionMenuOpen = false;
		fleet.select(undefined);
		onLocalSheetOpen?.();
		selectedAircraftIcao24 = icao24;
	}

	// Mobile only, in spirit - gated by CSS breakpoint (max-md:), not a
	// JS isMobile check: this component has no such singleton available
	// (it's the shared library component; a consuming app's own
	// is-mobile.svelte.ts is deliberately per-app, never exported from
	// the barrel - see that file's own comment). Both booleans default
	// false and only ever flip true via their own button below, so
	// desktop - where that button never renders at all (hidden
	// max-md:flex) - never has a way to set them.
	let weatherExpandedMobile = $state(false);
	let legendExpandedMobile = $state(false);
	let weatherMobileEl = $state<HTMLDivElement | undefined>(undefined);
	let legendMobileEl = $state<HTMLDivElement | undefined>(undefined);

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

	// `icon` is the shape-defining <svg> - rotation/color update via plain
	// CSS, matching the ward marker's own arrow. `body`/`stem` mirror
	// MarkerHandle's own ground+stem+body split (see aircraftMarkerElement's
	// own comment on why aircraft got the same treatment). displayLng/
	// displayLat/displayHeadingDeg are the currently-shown values, distinct
	// from the store's latest fix - see animateAircraftMarker's own comment.
	interface AircraftMarkerHandle {
		marker: maplibregl.Marker;
		body: HTMLElement;
		stem: HTMLElement;
		icon: SVGSVGElement;
		displayLng: number;
		displayLat: number;
		displayHeadingDeg: number;
		animFrame: number | undefined;
	}

	const EARTH_CIRCUMFERENCE_M = 40_075_016.686;
	/** maplibre zoom is normalized to 512px world tiles */
	const WORLD_TILE_PX = 512;

	let container: HTMLDivElement;
	let map = $state<maplibregl.Map | undefined>(undefined);
	let mapError = $state<string | undefined>(undefined);
	// Imperative handles, not $state - only ever read from inside effects,
	// same as markers/aircraftMarkers below.
	let scaleControl: maplibregl.ScaleControl | undefined;
	let scaleBarEl: HTMLElement | undefined;
	// True only once scaleControl.onAdd() has actually run - confirmed live
	// this matters, not just belt-and-suspenders: scaleControl itself
	// becomes non-null (in the mount effect below) well before onAdd ever
	// runs (that's deferred to this action, gated on mapLoaded), so a plain
	// `scaleControl?.` truthiness check elsewhere is not enough to know the
	// control is actually usable yet - calling setUnit() on a ScaleControl
	// before onAdd has set its internal _map/_container crashes inside
	// MapLibre's own code. $state (not a plain variable) specifically so
	// the unit-sync effect below can react to this flipping true on its
	// own, independent of whatever order Svelte happens to run this
	// action's update() versus that effect in.
	let scaleControlReady = $state(false);

	// Inserts the scale control's DOM element into this node - a Svelte
	// action rather than bind:this + appendChild, since manipulating a
	// bind:this ref directly trips svelte/no-dom-manipulating (Svelte's own
	// reconciliation could get confused by an untracked child). Keyed off
	// `mapLoaded` rather than scaleControl/map themselves: those are plain
	// variables, not $state, so an action's `update` wouldn't re-fire on
	// their change - mapLoaded flipping true is just the reactive trigger
	// to re-check them.
	function scaleBarSlot(node: HTMLDivElement, loaded: boolean) {
		function tryInsert(isLoaded: boolean) {
			if (!isLoaded || !scaleControl || !map || scaleBarEl) return;
			scaleBarEl = scaleControl.onAdd(map);
			node.appendChild(scaleBarEl);
			scaleControlReady = true;
		}
		tryInsert(loaded);
		return {
			update: tryInsert,
			destroy() {
				// Not scaleControl?.onRemove() here too: the main map effect
				// (see its own cleanup below, paired with the `new
				// ScaleControl` that creates this instance) already owns that
				// call. This action's node and that effect are torn down
				// together on every unmount (the legend panel this is nested
				// in is never conditionally rendered), so calling onRemove()
				// from both was a real, confirmed-live double-teardown bug:
				// MapLibre's own onRemove() clears the control's internal
				// _map reference, so the second call crashed inside it
				// ("Cannot read properties of undefined (reading 'off')").
				// Only this action's own local bookkeeping belongs here.
				scaleBarEl = undefined;
				scaleControlReady = false;
			}
		};
	}
	const showWardsRow = $derived(fleet.wardIds.length > 0);
	const showZoneRow = $derived(zoneStore.zoneIds.length > 0 && showZones);
	const showGeozoneRow = $derived(geozoneStore.active && showGeozones);
	const showObstacleRow = $derived(showObstacles);
	const showAirportRow = $derived(showAirports);
	const showCityRow = $derived(showCities);
	const showAircraftRow = $derived(showAircraft);
	const showEarthquakeRow = $derived(showEarthquakes);
	const showWildfireRow = $derived(wildfireStore.active && showWildfires);
	// Every data store a toggleable layer reads now carries its own
	// genuine in-flight-fetch signal (see each store's own `loading`
	// field doc comment) - this picks the noun for whichever one(s) are
	// actually loading right now, gated the same way each layer's own
	// visibility already is (the *Row flags above), so the toast never
	// claims a hidden layer is loading. Order here is just the order
	// the labels would read in if more than one were ever true at once.
	const loadingServices = $derived(
		(
			[
				[showAircraftRow && aircraftStore.loading, 'aircraft'],
				[showObstacleRow && obstacleStore.loading, 'obstacle'],
				[showAirportRow && airportStore.loading, 'airport'],
				[showGeozoneRow && geozoneStore.loading, 'airspace'],
				[showWildfireRow && wildfireStore.loading, 'wildfire'],
				[showEarthquakeRow && earthquakeStore.loading, 'earthquake']
			] as const
		)
			.filter(([active]) => active)
			.map(([, label]) => label)
	);
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
	const AIRCRAFT_TRAIL_SOURCE = 'aircraft-trails';
	const AIRCRAFT_TRAIL_LAYER = 'aircraft-trails-line';
	const AIRCRAFT_COLOR = '#14b8a6';
	// Reuses --color-critical's hue rather than reading the CSS custom
	// property directly - kept as a literal hex like every other *_COLOR
	// constant here, now genuinely a CSS color (marker `color` style) since
	// aircraft moved to DOM markers, not a MapLibre style-spec value
	// anymore - still not var(...) so this file doesn't need a live
	// binding to the current theme just to pick a color.
	const AIRCRAFT_EMERGENCY_COLOR = '#ef4444';
	// First attempt here was a muted slate (#64748b), deliberately avoiding
	// every existing semantic token - confirmed live to be a real problem,
	// not just a design nitpick: too low-contrast against light basemaps
	// (Roadmap especially) to read as a distinct marker at all. This is a
	// real khaki/olive - the actual color military materiel is drab in -
	// which reads as "military" by association and has enough saturation
	// to stay visible on both light and dark basemaps, without being a
	// bold/alarming color the way red or bright orange would be.
	const AIRCRAFT_MILITARY_COLOR = '#8a9a5b';
	// Altitude reads as brightness, not just the popup's own number:
	// ground-level traffic renders at a still-clearly-visible baseline,
	// cruise-altitude traffic renders brighter. Emergency aircraft are
	// deliberately exempt (see the color-assignment call site) - dimming
	// the one color that's supposed to always read as urgent because a
	// distressed aircraft happens to be low (often exactly when it matters
	// most, mid-descent) would undermine the whole point of that color.
	// ~FL390: a reasonable ceiling for "this is about as high as traffic
	// normally gets", not a hard operational limit - taller-flying traffic
	// just clamps to full intensity rather than going any brighter.
	const AIRCRAFT_ALTITUDE_CEILING_M = 12_000;
	// "Visual equivalent" altitude (meters) fed into the same lift formula
	// ward markers use, at full altitudeFraction (1.0) - see the sync
	// effect's own comment on why this isn't aircraft's literal altitude.
	// sqrt(altitudeFraction), not altitudeFraction directly: a ward's own
	// altitudeRelM tops out around 200m (drone flight ceilings), and a
	// *linear* compression needed either an absurdly high REF_M (defeating
	// the point of compressing at all) or left genuinely-high real traffic
	// (a regional flight at a few thousand meters, altitudeFraction still
	// well under 1) reading as no higher than a nearby drone - confirmed
	// live to look wrong, traffic that's actually kilometers up should
	// never look close to ward altitude. sqrt rises steeply at first (any
	// real aircraft clears drone range almost immediately after leaving
	// the ground) and flattens toward the ceiling, rather than a straight
	// line through both.
	const AIRCRAFT_LIFT_REF_M = 2_000;
	// Belt-and-suspenders cap on the pixel lift itself, independent of the
	// altitude compression above - without it, a tight zoom + steep pitch
	// could still push AIRCRAFT_LIFT_REF_M's own projection far enough off
	// the visible map to feel broken rather than "way up there".
	const AIRCRAFT_MAX_LIFT_PX = 600;
	// HSL hue/saturation of AIRCRAFT_COLOR/AIRCRAFT_MILITARY_COLOR above -
	// computed once (python: colorsys.rgb_to_hls on the hex values) rather
	// than converting on every render tick for every aircraft. Saturation
	// stays fixed regardless of altitude (see scaledAircraftColor's own
	// comment on why only lightness moves) - only lightness has a "full"
	// value here.
	const AIRCRAFT_HUE_DEG = 173;
	const AIRCRAFT_SATURATION_PCT = 80;
	const AIRCRAFT_FULL_LIGHTNESS_PCT = 55;
	const AIRCRAFT_MILITARY_HUE_DEG = 75;
	const AIRCRAFT_MILITARY_SATURATION_PCT = 40;
	const AIRCRAFT_MILITARY_FULL_LIGHTNESS_PCT = 58;
	// Floor, not zero - confirmed live that varying *saturation* toward a
	// floor (the original design here) washed a marker's hue out toward
	// gray at low altitude, reading as "this marker vanished" against a
	// near-black basemap rather than "this one's just lower than that
	// one". Saturation now stays fixed at its full value everywhere (see
	// above); this is lightness's own floor, chosen well above the
	// basemap's own near-black tone so nothing ever gets unreadably dim,
	// only relatively dimmer.
	const AIRCRAFT_COLOR_MIN_LIGHTNESS_PCT = 42;

	function aircraftAltitudeFraction(plane: Aircraft): number {
		if (plane.onGround || plane.altitudeM === undefined) return 0;
		return Math.min(1, Math.max(0, plane.altitudeM / AIRCRAFT_ALTITUDE_CEILING_M));
	}

	function scaledAircraftColor(
		hueDeg: number,
		saturationPct: number,
		fullLightnessPct: number,
		altitudeFraction: number
	): string {
		const l =
			AIRCRAFT_COLOR_MIN_LIGHTNESS_PCT +
			(fullLightnessPct - AIRCRAFT_COLOR_MIN_LIGHTNESS_PCT) * altitudeFraction;
		return `hsl(${hueDeg}, ${saturationPct}%, ${l}%)`;
	}

	// Heavier/larger categories render bigger, same "size communicates
	// scale" reasoning as EARTHQUAKE_LAYER's magnitude-interpolated radius
	// below - a 747 and a Cessna reading as the same size marker would
	// hide a real, useful distinction. Plain CSS pixels now (a DOM marker's
	// actual rendered size), not a multiplier against a fixed base icon
	// the way MapLibre's icon-size worked.
	function aircraftIconSizePx(category: AircraftCategory): number {
		switch (category) {
			case 'heavy':
				return 18;
			case 'rotorcraft':
			case 'glider':
				return 16;
			case 'light':
				return 14;
			case 'uav':
				return 14;
			case 'ground':
				return 12;
			default:
				return 13;
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

	// Own plain getCurrentPosition call rather than geolocation.ts's
	// locateOrFallback - that helper is for a silent best-effort default at
	// initial load; this is an explicit request, so failure should do
	// nothing visible rather than jump to an unrelated fallback point.
	const LOCATE_TIMEOUT_MS = 8000;
	let locating = $state(false);
	function locateMe(): void {
		if (!map || locating) return;
		if (typeof navigator === 'undefined' || !('geolocation' in navigator)) return;
		locating = true;
		navigator.geolocation.getCurrentPosition(
			(position) => {
				locating = false;
				map?.flyTo({
					center: [position.coords.longitude, position.coords.latitude],
					zoom: Math.max(map?.getZoom() ?? 0, LOCATE_ZOOM)
				});
			},
			() => {
				locating = false;
			},
			{ timeout: LOCATE_TIMEOUT_MS }
		);
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
		// Only rendered when true - most airports are neither, and a blank
		// "Access: none" row for the common case would just be noise.
		const accessNotes: string[] = [];
		if (properties.isPrivate === true) accessNotes.push('Private');
		if (properties.requiresPpr === true) accessNotes.push('PPR required');
		if (accessNotes.length > 0) {
			rows.push(popupRow('Access', escapeHtml(accessNotes.join(', '))));
		}
		return `<div class="flex flex-col gap-1 text-xs"><p class="font-medium text-fg">${escapeHtml(name)}</p>${rows.join('')}</div>`;
	}

	const KNOTS_TO_MS = 0.514444;

	// Raw METAR flight-category strings -> the same VFR/MVFR/IFR/LIFR
	// terminology real aviation weather briefings use, not a paraphrase -
	// see the FLIGHT_CATEGORY_LABELS reasoning: worth spelling out in full
	// since the four-letter code alone means nothing to a non-pilot
	// operator, but the underlying category itself is real, standard
	// aviation terminology, not this app inventing a severity scale.
	const FLIGHT_CATEGORY_LABELS: Record<string, string> = {
		VFR: 'VFR (visual)',
		MVFR: 'Marginal VFR',
		IFR: 'IFR (instrument)',
		LIFR: 'Low IFR'
	};

	// Live METAR for the airport popup - confirmed live against a real ICAO
	// code, free, no key. Fetched on demand per popup open, not a viewport
	// layer of its own the way Aircraft/Earthquakes/Wildfires are: METAR is
	// inherently per-airport, and the Airport layer already has exactly one
	// point per airport to hang it off of. wspd/wdir/temp/visib/altim/fltCat
	// field names and units (wspd in knots, temp/altim in Celsius/hPa)
	// confirmed against a real Schiphol report.
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
			// Flight category first - the single field a pilot-facing METAR
			// display leads with, since it's the one-glance answer to "is it
			// safe-looking VMC or not", derived by NOAA itself from the same
			// visibility/ceiling data reported below rather than this app
			// guessing at a threshold.
			if (typeof data.fltCat === 'string' && data.fltCat) {
				rows.push(
					popupRow(
						'Flight category',
						FLIGHT_CATEGORY_LABELS[data.fltCat] ?? escapeHtml(data.fltCat)
					)
				);
			}
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
			if (typeof data.altim === 'number') {
				rows.push(popupRow('Pressure', formatPressure(data.altim, unitsStore.current)));
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

	// Same field shape the old GeoJSON-feature properties used, kept as
	// its own function now that a plane's popup content is built on
	// demand (at hover time, from the live aircraft object) rather than
	// baked into a GeoJSON feature ahead of time - see aircraftMarkerElement.
	function aircraftPopupProperties(plane: Aircraft): Record<string, unknown> {
		return {
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
			emergency: plane.emergency ?? null
		};
	}

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
	// - /v0/callsign/{callsign} (flightroute): fills a real gap in the
	//   feed's own ownOp field, which is empty even for obviously
	//   commercial flights (confirmed live against api.airplanes.live,
	//   since replaced - see proxiedSource.ts's own header comment - same
	//   underlying feed shape though - "ABY150", a real Air Arabia
	//   flight registered in the UAE, had no operator at all from the
	//   feed, but adsbdb correctly resolves "Air Arabia" from the
	//   callsign's ICAO airline prefix alone). Only called when operator
	//   is already missing - the feed's own field is preferred when
	//   present. Also returns the flight's actual
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
		const badges: string[] = [];
		if (properties.tsunamiWarning === true) {
			badges.push(
				'<span class="rounded bg-critical/15 px-1.5 py-0.5 text-[10px] font-medium text-critical">TSUNAMI</span>'
			);
		}
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
		// USGS's own PAGER estimated-impact color ('green'/'yellow'/'orange'/
		// 'red') is already a valid CSS color keyword - used directly rather
		// than mapped through this app's own semantic tokens, since it's
		// USGS's real classification, not a severity scale this app is
		// inventing. Most events never get one at all (PAGER only runs when
		// real human impact is likely), so this only shows when present.
		if (typeof properties.alertLevel === 'string' && properties.alertLevel) {
			const level = properties.alertLevel;
			rows.push(
				popupRow(
					'PAGER alert',
					`<span style="color: ${escapeHtml(level)}">${escapeHtml(level)}</span>`
				)
			);
		}
		const badgeHtml = badges.length > 0 ? `<div class="flex gap-1">${badges.join('')}</div>` : '';
		let detailsLink = '';
		if (typeof properties.detailsUrl === 'string') {
			const href = escapeHtml(properties.detailsUrl);
			detailsLink = `<a href="${href}" target="_blank" rel="noreferrer" class="text-accent hover:underline">USGS details &#8599;</a>`;
		}
		return `<div class="flex flex-col gap-1 text-xs"><div class="flex items-center justify-between gap-2"><p class="font-medium text-fg">${escapeHtml(place)}</p>${badgeHtml}</div>${rows.join('')}${detailsLink}</div>`;
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
		// A daytime detection carries a real caveat VIIRS itself documents
		// (solar reflection can inflate the thermal signal vs. a nighttime
		// read) - worth surfacing, not just trivia.
		if (typeof properties.isNightDetection === 'boolean') {
			rows.push(popupRow('Detected at', properties.isNightDetection ? 'Night' : 'Day'));
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

	// Which aircraft (icao24) currently shows a trail - only the one being
	// hovered, not every aircraft in view at once. Confirmed live this was
	// a real problem, not just a style nitpick: with a dense traffic area
	// (Dubai/UAE), a trail per aircraft on top of an already-dense cluster
	// of markers reads as pure noise. Mirrors the ward marker's own trail
	// effect below, which already only draws a trail for fleet.selectedWardId
	// rather than every ward - aircraft have no "selected" state (see
	// aircraftMarkerElement's own comment on why hover, not selection, is
	// the right interaction model here), so hover fills that role instead.
	// Same grace-period shape as the popup close timer, kept as its own
	// timer rather than piggybacking on schedulePopupClose/cancelPopupClose
	// directly - those are shared across every point layer's popup, not
	// aircraft-specific, and entangling them would make a non-aircraft
	// layer's popup timing accidentally depend on aircraft state.
	let hoveredAircraftIcao24 = $state<string | undefined>(undefined);
	let aircraftTrailClearTimer: ReturnType<typeof setTimeout> | undefined;
	function cancelAircraftTrailClear(): void {
		if (aircraftTrailClearTimer) clearTimeout(aircraftTrailClearTimer);
		aircraftTrailClearTimer = undefined;
	}
	function scheduleAircraftTrailClear(): void {
		cancelAircraftTrailClear();
		aircraftTrailClearTimer = setTimeout(() => {
			hoveredAircraftIcao24 = undefined;
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
	// same pattern, keyed by icao24 - see the sync effect below
	let aircraftMarkers: Record<string, AircraftMarkerHandle> = {};
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
	let clusterIndex:
		Supercluster<{ wardId: string; connected: boolean }, { anyOnline: boolean }> | undefined;
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
	let zoomLevel = $state(untrack(() => initialZoom ?? INITIAL_ZOOM));

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
				<div class="border-edge bg-panel/90 text-fg absolute bottom-14 left-0 hidden -translate-x-1/2 flex-col gap-0.5 rounded-sm border px-2 py-1 font-mono text-[10px] whitespace-nowrap" data-part="badge">
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
			// Only one "selected map entity" at a time - selecting a ward
			// closes any aircraft info sheet, mirroring selectAircraft's own
			// fleet.select(undefined) in the other direction.
			selectedAircraftIcao24 = undefined;
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

	// Wrapping <div> (not the <svg> itself) is the actual Marker element -
	// mirrors markerElement()'s own root/body split, and gives
	// mouseenter/mouseleave something to bind to that isn't fighting SVG's
	// own event model inside a maplibregl.Marker. Popup content is looked
	// up fresh from aircraftStore.aircraft by icao24 on every hover rather
	// than baked in at creation time - a marker is created once and then
	// only repositioned/recolored/resized by the sync effect below, so a
	// closure captured at creation would go stale the moment the aircraft
	// moves.
	// Zero-size root pinned at the ground position, body lifted above it by
	// the projected altitude and connected by a stem - the exact same
	// ground+stem+body split markerElement (wards) already uses, so a
	// pitched-camera view reads "higher = lifted more" consistently for
	// every marker on the map, not just wards. See the sync effect below
	// for why the lift itself uses a compressed altitude rather than
	// aircraft's literal (much larger) real-world altitude.
	function aircraftMarkerElement(
		targetMap: maplibregl.Map,
		icao24: string,
		shape: AircraftIconShape,
		sizePx: number
	): { element: HTMLElement; body: HTMLElement; stem: HTMLElement; icon: SVGSVGElement } {
		const element = document.createElement('div');
		element.className = 'relative h-0 w-0';
		element.setAttribute('role', 'img');
		element.setAttribute('aria-label', `Aircraft ${icao24}`);
		const markup = aircraftIconMarkup(shape);
		element.innerHTML = `
			<span class="bg-edge absolute left-0 w-px" data-part="stem"></span>
			<div class="absolute cursor-pointer" data-part="body">
				<svg width="${sizePx}" height="${sizePx}" viewBox="${markup.viewBox}" fill="currentColor" stroke="#0a0e12" stroke-width="1" stroke-linejoin="round" class="-translate-x-1/2 -translate-y-1/2">${markup.innerHtml}</svg>
			</div>`;
		const body = element.querySelector<HTMLElement>('[data-part="body"]');
		const stem = element.querySelector<HTMLElement>('[data-part="stem"]');
		const icon = element.querySelector('svg');
		if (!body || !stem || !(icon instanceof SVGSVGElement)) {
			throw new Error('fleet-map: aircraft marker template is missing its parts');
		}
		body.addEventListener('mouseenter', () => {
			targetMap.getCanvas().style.cursor = 'pointer';
			cancelPopupClose();
			cancelAircraftTrailClear();
			hoveredAircraftIcao24 = icao24;
			const plane = aircraftStore.aircraft.find((candidate) => candidate.icao24 === icao24);
			if (!plane) return;
			const properties = aircraftPopupProperties(plane);
			const baseHtml = buildAircraftPopupHtml(properties);
			openPopup?.remove();
			const popup = new maplibregl.Popup({
				closeButton: false,
				closeOnClick: false,
				offset: sizePx / 2 + 6
			})
				.setLngLat([plane.longitudeDeg, plane.latitudeDeg])
				.setHTML(baseHtml)
				.addTo(targetMap);
			openPopup = popup;
			const popupEl = popup.getElement();
			popupEl.addEventListener('mouseenter', () => {
				cancelPopupClose();
				cancelAircraftTrailClear();
			});
			popupEl.addEventListener('mouseleave', () => {
				schedulePopupClose();
				scheduleAircraftTrailClear();
			});
			void fetchAircraftEnrichmentHtml(properties).then((extraHtml) => {
				if (openPopup !== popup || !extraHtml) return;
				popup.setHTML(baseHtml + extraHtml);
			});
		});
		body.addEventListener('mouseleave', () => {
			targetMap.getCanvas().style.cursor = '';
			schedulePopupClose();
			scheduleAircraftTrailClear();
		});
		// Tap-to-select: mobile's own equivalent of the hover popup above,
		// which touch has no gesture for at all - requested directly. Wired
		// unconditionally (not CSS/isMobile-gated): on desktop this just
		// also selects the aircraft in the background (selectedAircraftIcao24
		// changes), but the info sheet that state drives is max-md:-only
		// (see its own markup below), so a desktop click has no visible
		// effect beyond what hovering already showed - no regression there,
		// and no isMobile check needed since this component has none
		// available anyway (see every other mobile-only toggle's own
		// comment on that).
		body.addEventListener('click', (event) => {
			event.stopPropagation();
			selectAircraft(icao24);
		});
		return { element, body, stem, icon };
	}

	// Confirmed live that snapping straight to each new fix (a plain
	// setLngLat per refresh) read as choppy/unprofessional - real traffic
	// displays (FlightRadar24 and similar) glide between fixes rather than
	// teleporting, which is what this recreates: eases displayLng/
	// displayLat/displayHeadingDeg from wherever the marker currently is to
	// the new fix over AIRCRAFT_ANIMATION_MS, driven by requestAnimationFrame
	// rather than a CSS transition - MapLibre's own Marker already rewrites
	// the DOM element's transform on every map paint (pan/zoom/rotate), so a
	// CSS transition on that same property would fight the camera instead of
	// the data. Duration is well under aircraft-store.svelte.ts's own
	// REFRESH_INTERVAL_MS so an animation always finishes before the next
	// fix arrives, rather than chaining into a permanently-lagging tween.
	const AIRCRAFT_ANIMATION_MS = 1200;

	function easeOutCubic(t: number): number {
		return 1 - Math.pow(1 - t, 3);
	}

	// Shortest signed turn from `fromDeg` to `toDeg`, e.g. 350 -> 10 yields
	// +20, not -340 - without this, an aircraft crossing the 0/360 wrap
	// would visibly spin the long way around instead of turning a few
	// degrees.
	function shortestHeadingDeltaDeg(fromDeg: number, toDeg: number): number {
		return ((((toDeg - fromDeg + 540) % 360) + 360) % 360) - 180;
	}

	function animateAircraftMarker(
		handle: AircraftMarkerHandle,
		targetLng: number,
		targetLat: number,
		targetHeadingDeg: number
	): void {
		if (handle.animFrame !== undefined) cancelAnimationFrame(handle.animFrame);
		const startLng = handle.displayLng;
		const startLat = handle.displayLat;
		const startHeadingDeg = handle.displayHeadingDeg;
		const headingDeltaDeg = shortestHeadingDeltaDeg(startHeadingDeg, targetHeadingDeg);
		const startTimeMs = performance.now();
		const step = (nowMs: number) => {
			const t = Math.min(1, (nowMs - startTimeMs) / AIRCRAFT_ANIMATION_MS);
			const eased = easeOutCubic(t);
			handle.displayLng = startLng + (targetLng - startLng) * eased;
			handle.displayLat = startLat + (targetLat - startLat) * eased;
			handle.displayHeadingDeg = startHeadingDeg + headingDeltaDeg * eased;
			handle.marker.setLngLat([handle.displayLng, handle.displayLat]);
			handle.icon.style.rotate = `${handle.displayHeadingDeg - bearingDeg}deg`;
			if (t < 1) {
				handle.animFrame = requestAnimationFrame(step);
			} else {
				handle.animFrame = undefined;
			}
		};
		handle.animFrame = requestAnimationFrame(step);
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
	// anyOnline mirrors markerElement's own sync effect, which fades an
	// individual disconnected ward's body to 0.4 opacity (see
	// handle.body.style.opacity below) - a cluster bubble standing in for
	// several wards needs the same tell once none of them are actually
	// live, or it keeps reading as "something active in here" at a zoom
	// level where the individual markers' own fade is invisible (folded
	// into the bubble). Full strength the moment at least one member is
	// online, same as the bubble's one open question ("is anything here
	// worth zooming into") being answered yes.
	function updateClusterMarkerElement(
		element: HTMLElement,
		pointCount: number,
		anyOnline: boolean
	): void {
		element.setAttribute('aria-label', `${pointCount} wards, click to zoom in`);
		element.textContent = pointCount > 99 ? '99+' : String(pointCount);
		element.style.opacity = anyOnline ? '1' : '0.4';
	}

	$effect(() => {
		// untrack: this effect must run exactly once, on mount, never again.
		// centerLat/centerLon/the initial basemap choice are only needed to
		// seed the initial style - reading them normally would make Svelte
		// treat every later basemap/theme change as a reason to tear the whole
		// map down and rebuild it from these original values (losing the
		// operator's pan/zoom and snapping back to whatever centerLat/centerLon
		// were at mount, e.g. always Zurich for a caller passing
		// FAKE_FLEET_CENTER). The tile-swap effect further down is the sole
		// thing that should react to activeRasterTiles/activeVectorStyle
		// changing.
		const { lat, lon, zoom, tiles, rasterVisible } = untrack(() => ({
			lat: centerLat,
			lon: centerLon,
			zoom: initialZoom ?? INITIAL_ZOOM,
			// TERRAIN_TILES is an arbitrary, harmless placeholder when the
			// restored mapStyle is actually a vector style - rasterVisible
			// below keeps this layer hidden in that case, and the tile-swap
			// effect corrects `tiles` for real on its very first run regardless.
			tiles: activeRasterTiles ?? TERRAIN_TILES,
			rasterVisible: activeRasterTiles !== undefined
		}));
		// a map init failure (e.g. no WebGL) must not take the rest of the
		// console down; ward cards keep working without the map
		let created: maplibregl.Map;
		try {
			created = new maplibregl.Map({
				container,
				center: [lon, lat],
				zoom,
				// Replaced by a custom button + popup below (same pattern as
				// the layers menu) instead of MapLibre's own native compact
				// control - its own expand-in-place behavior had no
				// viewport-aware repositioning and clipped on a narrow phone,
				// confirmed live. BASEMAP_ATTRIBUTION (below) still carries
				// the actual credit text either way.
				attributionControl: false,
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
					layers: [
						{
							id: 'basemap',
							type: 'raster',
							source: 'basemap',
							// Hidden at mount when the restored mapStyle is actually a
							// vector style - avoids a flash of the placeholder
							// TERRAIN_TILES fallback before the vector layers land.
							layout: { visibility: rasterVisible ? 'visible' : 'none' }
						}
					]
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
		// Distance is otherwise unjudgeable at a glance, which matters more
		// here than on a general-purpose map (planning a goto, eyeballing
		// how far a ward still has to fly) - hence a real scale bar, not
		// just a nice-to-have. Deliberately NOT addControl()'d into
		// MapLibre's own bottom-left corner box: this control's DOM element
		// gets inserted directly into the legend panel instead (see the
		// effect below, which calls onAdd itself), so it only ever exists
		// in that one place - addControl-then-move-it-afterward left a real
		// gap where it could still be found in MapLibre's own corner
		// container. Initial unit doesn't matter (see the separate effect
		// that sets the real one immediately and on every later change) -
		// MapLibre's own ScaleControl requires one at construction
		// regardless.
		scaleControl = new maplibregl.ScaleControl({ maxWidth: 120, unit: unitsStore.current });
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
			if (fleetGroups.missionAssignmentDraft) {
				fleetGroups.addAssignmentWaypoint(event.lngLat.lat, event.lngLat.lng);
			} else {
				// Read before requestGoto/requestSetHome run, since each
				// clears its own arming flag the instant it consumes this
				// click - this is the only way to tell afterward whether
				// that's what just happened. A ward is typically selected
				// specifically to arm one of these from its own Control tab,
				// so the tap that sends it there must not also deselect it.
				const wasTargeting = fleet.gotoArming || fleet.setHomeArming;
				fleet.requestGoto(event.lngLat.lat, event.lngLat.lng);
				fleet.requestSetHome(event.lngLat.lat, event.lngLat.lng);
				// Tapping empty map area deselects the current ward -
				// requested directly as the standard, more discoverable
				// complement to tapping the same marker again (which still
				// works independently, via each marker's own
				// stopPropagation'd click handler never reaching here at
				// all). Deliberately NOT done on a pan/zoom (see
				// followSuspended's own comment above) - those stay
				// non-destructive, only an actual tap elsewhere dismisses.
				if (!wasTargeting) {
					if (fleet.selectedWardId !== undefined) fleet.select(undefined);
					// Same deselect-on-empty-tap treatment as the ward above -
					// each aircraft marker's own click handler (selectAircraft's
					// caller) stopPropagation's too, so this only ever fires for
					// a genuine empty-map tap.
					if (selectedAircraftIcao24 !== undefined) selectedAircraftIcao24 = undefined;
				}
			}
			onMapClick?.(event.lngLat.lat, event.lngLat.lng);
		});
		// Pausing the selected-ward follow effect (below) the moment the
		// operator drags or pinch-zooms themselves - originalEvent is only
		// set for a real user gesture, never for one of this file's own
		// programmatic easeTo calls (confirmed: none of them pass eventData
		// with an originalEvent), so this can't misfire from the follow
		// effect's own moves, including its SELECTION_FOLLOW_TICK_EVENT_DATA-
		// tagged ones. See followSuspended's own comment for the bug this
		// fixes.
		created.on('dragstart', (event) => {
			if (event.originalEvent) followSuspended = true;
		});
		created.on('zoomstart', (event) => {
			if (event.originalEvent) followSuspended = true;
		});
		created.on('move', () => {
			bearingDeg = created.getBearing();
			pitchDeg = created.getPitch();
			zoomLevel = created.getZoom();
		});
		created.on('moveend', () => {
			const center = created.getCenter();
			onMapMove?.({ lat: center.lat, lon: center.lng, zoom: created.getZoom() });
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
			// not a raw-bbox one: proxiedSource reduces the viewport to a
			// center point + radius capped at 250nm internally (see
			// boundsToPointRadius in proxiedSource.ts) rather than sending the
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
			obstacleStore.requestViewport(viewport, zoom);
			airportStore.requestViewport(viewport);
		};
		// isFollowTick: skips this whole refresh for the selected-ward
		// follow effect's own recurring recenter (see SELECTION_FOLLOW_TICK_EVENT_DATA's
		// own comment) - every one of the fetches above is debounced or
		// rate-gated, and a flying ward's own ~200ms-apart recenters fired
		// moveend far more often than any of them could ever get a quiet
		// gap to actually complete in, confirmed live as a real bug
		// (weather specifically: zero successful fetches for the entire
		// time a ward stayed selected). A genuine "jump to a new area"
		// move (selecting a ward for the first time, locate-me, a cluster
		// expansion) carries no such tag and still refreshes normally.
		created.on('moveend', (event: maplibregl.MapLibreEvent & { isFollowTick?: boolean }) => {
			if (event.isFollowTick) return;
			requestThirdPartyLayers();
		});
		created.on('load', () => {
			// OpenFreeMap's vector basemap (see loadBasemapLayers's own
			// comment on why this is additive rather than map.setStyle()).
			// Added once, unconditionally: all three vendored variants
			// (dark/light/roadmap) share this one source and these same
			// sprite/glyphs URLs, and the tile-swap effect below is what
			// actually adds/removes each variant's own layers on top of it.
			// sprite/glyphs are whole-style properties with no per-source
			// equivalent - setSprite()/setGlyphs() are the additive runtime
			// setters for them, the same reason setTiles() exists for a raster
			// source instead of a full style replace.
			created.addSource(OPENFREEMAP_VECTOR_SOURCE, {
				type: 'vector',
				url: OPENFREEMAP_TILES_URL
			});
			created.setSprite(OPENFREEMAP_SPRITE_URL);
			created.setGlyphs(OPENFREEMAP_GLYPHS_URL);
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
			// Aircraft points themselves are DOM maplibregl.Marker elements now
			// (see aircraftMarkerElement and the sync effect below), not a GL
			// layer here - the same technique ward markers already use, and for
			// the same reason: a MapLibre symbol layer rasterizes each icon to
			// a fixed-resolution texture and GPU-scales/rotates it, which lost
			// fine detail (the plane shape's thin fuselage specifically,
			// confirmed live) that a real SVG element rendered natively by the
			// browser doesn't. Only the trail source/layer above stays as GL -
			// a thin line has no such detail to lose, and a GeoJSON line layer
			// is the right tool for many short paths regardless.
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
			// Not tracked via addControl (see its own comment above), so
			// map.remove() below won't know to clean it up on its own -
			// remove it explicitly first, and reset scaleBarEl so a
			// remounted instance re-inserts a fresh one rather than thinking
			// it already has (which would otherwise reference a torn-down
			// node forever after). This effect is the sole owner of this
			// call - see scaleBarSlot's own destroy() above for why it no
			// longer also calls onRemove() itself.
			scaleControl?.onRemove();
			scaleControl = undefined;
			scaleBarEl = undefined;
			scaleControlReady = false;
			created.remove();
			map = undefined;
		};
	});

	// MapLibre only redraws its canvas at a new size when told to - a
	// container that shrinks (a bottom sheet opening, see
	// mobileLocalSheetReserve's own comment above) doesn't trigger that on
	// its own, confirmed live: the canvas stayed the old size, cropped
	// rather than actually resized, until resize() was called. A
	// ResizeObserver, not a $effect keyed on mobileLocalSheetReserve: this
	// also catches --mobile-external-sheet-reserve changing (pure CSS, a
	// $effect can't see that at all) and plain window/orientation resizes
	// alongside it, one mechanism for every cause instead of enumerating
	// each one.
	$effect(() => {
		if (!map || !container) return;
		const activeMap = map;
		const observer = new ResizeObserver(() => activeMap.resize());
		observer.observe(container);
		return () => observer.disconnect();
	});

	// Keep a selected ward centered for as long as it stays selected, not
	// just on the moment it's picked - a ward in flight keeps moving, and
	// an operator watching one specific ward wants the camera to follow
	// it, not just glance at where it was when selected. lastFocusedWardId
	// still distinguishes "just selected" (gets the zoom-in/tilt-in flight,
	// and only ever zooms in, never back out, past whatever the operator's
	// own zoom already was) from "already selected, this is just the next
	// telemetry tick" (center only - an operator who's deliberately zoomed/
	// panned/pitched further while following shouldn't have that fought on
	// every tick, only the center itself needs to keep tracking). Reading
	// state.position here (not untracked) is what makes both cases work: a
	// deep link can select a ward before its telemetry has arrived (first
	// read fires once position shows up), and a flying ward's own position
	// changing is the whole reason this effect needs to keep re-firing.
	// The pitch is what turns the altitude-lift marker rendering (see the
	// position effect's own "lift the body" comment) from a dormant formula
	// into an actually visible stem - confirmed live, at pitch 0 nothing
	// above the ground anchor ever showed regardless of a ward's real
	// altitude.
	// focusingIn: true for the duration of the initial fly-in ease (zoom +
	// pitch + center), false once it actually settles. Follow-ticks below
	// are skipped entirely while this is true - confirmed live as a real
	// bug otherwise: the sim ticks at 5Hz (TICK_HZ, fleet-sim.ts, ~200ms
	// apart), well inside the fly-in's own 600ms duration, so a follow-tick
	// almost always landed mid-animation and immediately re-eased with no
	// zoom/pitch specified. MapLibre's easeTo takes "unspecified" to mean
	// "keep whatever the camera's current value is right now", which
	// mid-animation is some partial interpolated zoom/pitch, not the real
	// target - that partial value then became the new "current", and the
	// fly-in never actually reached SELECTED_ENTITY_ZOOM/PITCH_DEG at all.
	// Letting the fly-in finish untouched, then handing off to follow-ticks
	// once it's done, is what actually gets both: the animated zoom-in/
	// tilt-in on selection, and a camera that keeps tracking afterward.
	let lastFocusedWardId: string | undefined;
	let focusingIn = false;
	// True the moment the operator drags or pinch-zooms the map themselves
	// while a ward is selected - set by the dragstart/zoomstart listeners
	// below (near the map's own 'click' handler), reset only by a fresh
	// selection (justSelected below). Follow-ticks skip themselves
	// entirely while this is true. Without it, every follow-tick (every
	// ~200ms, see that branch's own comment) forced the center straight
	// back to the ward regardless of what the operator had just done to
	// the camera - confirmed live as a real bug, panning away from a
	// selected, moving ward felt "rubber-banded": the map snapped back
	// within a couple hundred milliseconds no matter how hard or how far
	// you dragged, never actually letting you look anywhere else. The
	// zoom/pitch half of this same problem was already handled (see the
	// follow-tick's own comment on why it never re-specifies zoom/pitch);
	// this is the other half, for the center/pan itself, which that fix
	// never actually covered.
	let followSuspended = false;
	$effect(() => {
		const selectedId = fleet.selectedWardId;
		const activeMap = map;
		if (!selectedId) {
			// Deselecting flattens the camera back out - an operator
			// shouldn't be left staring at a tilted map once nothing is
			// actually selected to justify the altitude view. Center/zoom
			// are left alone; only the pitch this effect itself introduced
			// needs undoing.
			if (activeMap && lastFocusedWardId !== undefined) {
				activeMap.easeTo({ pitch: 0, duration: 600 });
			}
			lastFocusedWardId = undefined;
			focusingIn = false;
			return;
		}
		if (!activeMap) return;
		const position = fleet.wards[selectedId]?.state?.position;
		if (!position) return;
		const justSelected = lastFocusedWardId !== selectedId;
		lastFocusedWardId = selectedId;
		if (justSelected) {
			focusingIn = true;
			followSuspended = false;
			activeMap.easeTo({
				center: [position.longitudeDeg, position.latitudeDeg],
				zoom: Math.max(activeMap.getZoom(), SELECTED_ENTITY_ZOOM),
				pitch: SELECTED_ENTITY_PITCH_DEG,
				duration: 600
			});
			activeMap.once('moveend', () => {
				focusingIn = false;
			});
			return;
		}
		if (focusingIn || followSuspended) return;
		// Short duration, not the fly-in's 600ms: this re-fires on every
		// sim tick (~200ms apart), so a duration close to that spacing keeps
		// consecutive eases smoothly retargeting rather than visibly
		// snapping - confirmed live, a longer duration here left the camera
		// chronically lagging a moving ward instead of tracking it. No
		// zoom/pitch here deliberately - an operator who's since zoomed,
		// panned, or pitched further while following shouldn't have that
		// fought on every tick, only the center itself needs to keep
		// tracking.
		// eventData (the 2nd argument) tags every event this transition
		// fires, including the 'moveend' it ends in - the moveend handlers
		// below read SELECTION_FOLLOW_TICK_EVENT_DATA back off that event to
		// skip their own viewport-refresh work specifically for this
		// recentering, not genuine navigation. Without this, a flying,
		// selected ward's own re-centers (every ~200ms, following the sim's
		// own tick rate) fired 'moveend' far too often for any of those
		// debounced/gated fetches (weather, cities, aircraft, obstacles,
		// airports, geozones, wildfires, earthquakes - every one of them
		// hangs off the same moveend) to ever get the quiet gap they
		// needed to actually fire - confirmed live as a real bug, weather
		// (1.5s debounce) never completed a single fetch for the entire
		// time a ward stayed selected, worst case (freshly restored
		// selection on reload, follow starting immediately) never fetching
		// at all.
		activeMap.easeTo(
			{
				center: [position.longitudeDeg, position.latitudeDeg],
				duration: 200
			},
			SELECTION_FOLLOW_TICK_EVENT_DATA
		);
	});

	// Selected-aircraft equivalent of the ward follow effect directly
	// above - same shape entirely (justSelected fly-in with zoom/pitch,
	// then center-only follow-ticks once settled), requested directly:
	// "all this selection and behavior on airplanes/aircraft as well".
	// Deliberately a SEPARATE effect with its own lastFocused/focusingIn
	// pair rather than merged into the one above: selectAircraft()/
	// toggleSelect() already guarantee a ward and an aircraft are never
	// both selected at once (see either one's own comment), so there's
	// no risk of the two fighting each other, and keeping them apart
	// avoids a riskier single-effect rewrite for what's otherwise a
	// contained addition. followSuspended IS shared (declared above, set
	// by the same dragstart/zoomstart listeners) - an operator taking
	// control of the camera should pause auto-follow regardless of
	// whether a ward or an aircraft is what's being followed.
	let lastFocusedAircraftIcao24: string | undefined;
	let aircraftFocusingIn = false;
	$effect(() => {
		const selectedIcao24 = selectedAircraftIcao24;
		const activeMap = map;
		if (!selectedIcao24) {
			if (activeMap && lastFocusedAircraftIcao24 !== undefined) {
				activeMap.easeTo({ pitch: 0, duration: 600 });
			}
			lastFocusedAircraftIcao24 = undefined;
			aircraftFocusingIn = false;
			return;
		}
		if (!activeMap) return;
		const plane = aircraftStore.aircraft.find((candidate) => candidate.icao24 === selectedIcao24);
		if (!plane) return;
		const justSelected = lastFocusedAircraftIcao24 !== selectedIcao24;
		lastFocusedAircraftIcao24 = selectedIcao24;
		if (justSelected) {
			aircraftFocusingIn = true;
			followSuspended = false;
			activeMap.easeTo({
				center: [plane.longitudeDeg, plane.latitudeDeg],
				zoom: Math.max(activeMap.getZoom(), SELECTED_ENTITY_ZOOM),
				pitch: SELECTED_ENTITY_PITCH_DEG,
				duration: 600
			});
			activeMap.once('moveend', () => {
				aircraftFocusingIn = false;
			});
			return;
		}
		if (aircraftFocusingIn || followSuspended) return;
		// Real feed updates land far less often than the ward sim's 5Hz
		// tick, but this still needs the same SELECTION_FOLLOW_TICK_EVENT_DATA
		// tag - an aircraft's own recenter is just as capable of firing
		// moveend often enough to starve weather/cities/etc. of the quiet
		// gap they need (see the ward follow-tick's own comment on the bug
		// this prevents), regardless of the exact cadence.
		activeMap.easeTo(
			{
				center: [plane.longitudeDeg, plane.latitudeDeg],
				duration: 600
			},
			SELECTION_FOLLOW_TICK_EVENT_DATA
		);
	});

	// crosshair cursor while goto targeting, set-home targeting, fleet
	// mission route planning, measuring, or a consuming app's own
	// click-to-place flow (crosshair prop) is active
	$effect(() => {
		if (!map) return;
		map.getCanvas().style.cursor =
			fleet.gotoArming ||
			fleet.setHomeArming ||
			fleetGroups.missionAssignmentDraft ||
			zoneStore.draft ||
			crosshair ||
			measuring
				? 'crosshair'
				: '';
	});

	// One vector basemap's layers are resident at a time, removed and
	// re-added on every switch - the same "one thing active, swapped in
	// place" shape as the raster setTiles() swap below, and avoids needing
	// to namespace dark/light/roadmap's own layer ids (they reuse generic
	// OpenMapTiles ids like "water"/"building" across all three, which only
	// matters if more than one set could ever coexist). Plain module-scope
	// state, not $state - purely internal bookkeeping for the effect below
	// to clean up after itself, never read by anything else.
	let activeVectorLayerIds: string[] = [];

	// swap the active basemap in place when the theme/style/satellite
	// choice changes - cheaper than map.setStyle(), which tears down and
	// rebuilds every source/layer (route, geozones, markers) this component
	// owns, not just the basemap. Raster styles (terrain/satellite/
	// bathymetry) still swap via setTiles(); vector styles (default/roadmap,
	// now OpenFreeMap-backed) add/remove their own layer set instead, since
	// a vector source has no equivalent single "which tiles" knob.
	$effect(() => {
		const activeMap = map;
		const rasterTiles = activeRasterTiles;
		const showLabels = mapStyle === 'satellite';
		if (!activeMap || !mapLoaded) return;

		if (rasterTiles) {
			activeMap.getSource<maplibregl.RasterTileSource>('basemap')?.setTiles(rasterTiles);
		}
		activeMap.setLayoutProperty('basemap', 'visibility', rasterTiles ? 'visible' : 'none');

		activeMap.setLayoutProperty(
			SATELLITE_LABELS_SOURCE,
			'visibility',
			showLabels ? 'visible' : 'none'
		);
	});

	// Separate from the raster/label effect above: this one has an async
	// gap (loadBasemapLayers fetching a not-yet-cached style's JSON), so it
	// can't share a single synchronous effect body with work that has to
	// run immediately. Old layers stay on screen until the new style's are
	// actually ready to swap in, rather than removing them first and
	// leaving a blank basemap for the duration of the fetch.
	$effect(() => {
		const activeMap = map;
		const vectorBasemap = activeVectorStyle;
		if (!activeMap || !mapLoaded) return;

		if (!vectorBasemap) {
			for (const id of activeVectorLayerIds) activeMap.removeLayer(id);
			activeVectorLayerIds = [];
			return;
		}

		void loadBasemapLayers(vectorBasemap).then((layers) => {
			// The map may have been torn down, or a newer style requested,
			// while this style's JSON was still loading - applying a stale
			// response here would stomp whatever the operator has selected
			// since.
			if (map !== activeMap || activeVectorStyle !== vectorBasemap) return;
			for (const id of activeVectorLayerIds) activeMap.removeLayer(id);
			activeVectorLayerIds = layers.map((layer) => {
				activeMap.addLayer(layer, SATELLITE_LABELS_SOURCE);
				return layer.id;
			});
		});
	});

	// Keeps the scale bar's own unit in sync with the operator's
	// metric/imperial choice - MapLibre's ScaleControl only reads its unit
	// once at construction otherwise, so without this it would stay
	// permanently metric regardless of the Units toggle everything else in
	// this app already respects. Gated on scaleControlReady (see its own
	// comment): confirmed live that calling setUnit before onAdd has run
	// crashes inside MapLibre's own code, not just a theoretical race - a
	// consuming app with more components/effects around FleetMap than this
	// reference app's own page shifted the timing enough to actually hit it.
	$effect(() => {
		if (!scaleControlReady) return;
		scaleControl?.setUnit(unitsStore.current);
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
		// Marker visibility itself (not just the trail line layer) is
		// handled inside the marker sync effect below, which removes every
		// marker outright when hidden - DOM markers don't have MapLibre's
		// own layer-visibility toggle to lean on the way a GL layer did.
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

	// Same convention for the attribution button below.
	$effect(() => {
		if (!attributionMenuOpen) return;
		const handlePointerDown = (event: PointerEvent) => {
			if (attributionMenuEl && !attributionMenuEl.contains(event.target as Node)) {
				attributionMenuOpen = false;
			}
		};
		const handleKeydown = (event: KeyboardEvent) => {
			if (event.key === 'Escape') attributionMenuOpen = false;
		};
		window.addEventListener('pointerdown', handlePointerDown);
		window.addEventListener('keydown', handleKeydown);
		return () => {
			window.removeEventListener('pointerdown', handlePointerDown);
			window.removeEventListener('keydown', handleKeydown);
		};
	});

	// Same outside-click/Escape convention as the layers menu above, for
	// the mobile weather/legend buttons' own expanded card.
	$effect(() => {
		if (!weatherExpandedMobile) return;
		const handlePointerDown = (event: PointerEvent) => {
			if (weatherMobileEl && !weatherMobileEl.contains(event.target as Node)) {
				weatherExpandedMobile = false;
			}
		};
		const handleKeydown = (event: KeyboardEvent) => {
			if (event.key === 'Escape') weatherExpandedMobile = false;
		};
		window.addEventListener('pointerdown', handlePointerDown);
		window.addEventListener('keydown', handleKeydown);
		return () => {
			window.removeEventListener('pointerdown', handlePointerDown);
			window.removeEventListener('keydown', handleKeydown);
		};
	});
	$effect(() => {
		if (!legendExpandedMobile) return;
		const handlePointerDown = (event: PointerEvent) => {
			if (legendMobileEl && !legendMobileEl.contains(event.target as Node)) {
				legendExpandedMobile = false;
			}
		};
		const handleKeydown = (event: KeyboardEvent) => {
			if (event.key === 'Escape') legendExpandedMobile = false;
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
					elevationM: airport.elevationM ?? null,
					isPrivate: airport.isPrivate,
					requiresPpr: airport.requiresPpr
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
	function removeAircraftMarker(icao24: string): void {
		const handle = aircraftMarkers[icao24];
		if (!handle) return;
		if (handle.animFrame !== undefined) cancelAnimationFrame(handle.animFrame);
		handle.marker.remove();
		delete aircraftMarkers[icao24];
	}

	// DOM markers, not a GL source/layer - see AircraftMarkerHandle and
	// aircraftMarkerElement above. Deliberately does NOT depend on
	// bearingDeg - that's handled by its own effect below, so rotating the
	// camera never restarts an in-flight position animation for data that
	// hasn't actually changed. Colors/position/heading update via
	// animateAircraftMarker rather than a direct setLngLat/rotate snap - see
	// that function's own comment.
	$effect(() => {
		const activeMap = map;
		if (!activeMap || !mapLoaded) return;
		if (!showAircraft) {
			for (const icao24 of Object.keys(aircraftMarkers)) removeAircraftMarker(icao24);
			return;
		}
		const seenIcao24s: Record<string, true> = {};
		for (const plane of aircraftStore.aircraft) {
			seenIcao24s[plane.icao24] = true;
			const headingDeg = plane.headingDeg ?? 0;
			let handle = aircraftMarkers[plane.icao24];
			if (!handle) {
				// A military fixed-wing aircraft gets a genuinely distinct
				// fighter-jet silhouette, not just the civilian shape recolored -
				// there is no data to tell a transport from a fighter apart, so
				// one shape covers both, but it must not read as "the same
				// traffic, different color" the way khaki-vs-teal alone did.
				// Military rotorcraft keep the ordinary helicopter shape (see
				// icons.ts's own comment) - already a real, non-civilian-specific
				// silhouette.
				const isMilitaryFixedWing =
					plane.isMilitary && (plane.category === 'light' || plane.category === 'heavy');
				const shape = isMilitaryFixedWing ? 'fighter' : iconShapeForCategory(plane.category);
				const sizePx = aircraftIconSizePx(plane.category);
				const { element, body, stem, icon } = aircraftMarkerElement(
					activeMap,
					plane.icao24,
					shape,
					sizePx
				);
				const marker = new maplibregl.Marker({ element })
					.setLngLat([plane.longitudeDeg, plane.latitudeDeg])
					.addTo(activeMap);
				// First appearance snaps straight in, no animation - there is no
				// "previous position" to glide from, and animating in from (0,0)
				// would look worse, not better.
				icon.style.rotate = `${headingDeg - bearingDeg}deg`;
				handle = {
					marker,
					body,
					stem,
					icon,
					displayLng: plane.longitudeDeg,
					displayLat: plane.latitudeDeg,
					displayHeadingDeg: headingDeg,
					animFrame: undefined
				};
				aircraftMarkers[plane.icao24] = handle;
			} else {
				animateAircraftMarker(handle, plane.longitudeDeg, plane.latitudeDeg, headingDeg);
			}
			const altitudeFraction = aircraftAltitudeFraction(plane);
			handle.icon.style.color = isAircraftEmergency(plane)
				? AIRCRAFT_EMERGENCY_COLOR
				: plane.isMilitary
					? scaledAircraftColor(
							AIRCRAFT_MILITARY_HUE_DEG,
							AIRCRAFT_MILITARY_SATURATION_PCT,
							AIRCRAFT_MILITARY_FULL_LIGHTNESS_PCT,
							altitudeFraction
						)
					: scaledAircraftColor(
							AIRCRAFT_HUE_DEG,
							AIRCRAFT_SATURATION_PCT,
							AIRCRAFT_FULL_LIGHTNESS_PCT,
							altitudeFraction
						);
			// Same selected-blue/thicker-stroke treatment ward markers use
			// (see the ward marker sync effect's own arrowPath styling) -
			// the only visual cue a tapped aircraft is actually selected,
			// since unlike a ward there's no name badge appearing below it.
			const aircraftSelected = selectedAircraftIcao24 === plane.icao24;
			handle.icon.setAttribute('stroke', aircraftSelected ? '#3b9eff' : '#0a0e12');
			handle.icon.setAttribute('stroke-width', aircraftSelected ? '2' : '1');
			// lift the body above the ground anchor by the projected altitude,
			// same formula as ward markers (see markerElement's own comment) -
			// but fed a compressed "visual equivalent" altitude, not aircraft's
			// literal real-world one (see AIRCRAFT_LIFT_REF_M's own comment on
			// why sqrt, not altitudeFraction directly). This keeps the same
			// zoom/pitch-responsive physical projection wards use, just applied
			// to a compressed altitude instead - deliberately not a physically
			// accurate height, just a consistent, readable "higher = lifted
			// more" cue that stays clearly separated from ward altitude, capped
			// at AIRCRAFT_MAX_LIFT_PX so an extreme zoom/pitch combination can't
			// push it absurdly far off the visible map.
			const pxPerMeter =
				(WORLD_TILE_PX * Math.pow(2, zoomLevel)) /
				(EARTH_CIRCUMFERENCE_M * Math.cos((plane.latitudeDeg * Math.PI) / 180));
			const liftPx = Math.min(
				AIRCRAFT_MAX_LIFT_PX,
				Math.sqrt(altitudeFraction) *
					AIRCRAFT_LIFT_REF_M *
					pxPerMeter *
					Math.sin((pitchDeg * Math.PI) / 180)
			);
			handle.body.style.top = `${-liftPx}px`;
			handle.stem.style.top = `${-liftPx}px`;
			handle.stem.style.height = `${Math.max(0, liftPx - aircraftIconSizePx(plane.category) / 2)}px`;
		}
		for (const icao24 of Object.keys(aircraftMarkers)) {
			if (!seenIcao24s[icao24]) removeAircraftMarker(icao24);
		}
	});
	// Camera-bearing compensation only - see the effect above's own comment
	// on why this is split out. Applies instantly (not animated): this
	// should track the camera 1:1 like the ward marker's own arrow does,
	// not lag behind a drag.
	$effect(() => {
		const activeBearingDeg = bearingDeg;
		for (const handle of Object.values(aircraftMarkers)) {
			handle.icon.style.rotate = `${handle.displayHeadingDeg - activeBearingDeg}deg`;
		}
	});
	$effect(() => {
		const activeMap = map;
		if (!activeMap || !mapLoaded) return;
		const source = activeMap.getSource<maplibregl.GeoJSONSource>(AIRCRAFT_TRAIL_SOURCE);
		// Only the hovered aircraft's own trail, matching the ward trail
		// effect further below (which only draws fleet.selectedWardId's
		// trail, never every ward's at once) - see hoveredAircraftIcao24's
		// own comment on why a dense traffic area made "every aircraft's
		// trail, always" real, confirmed-live clutter.
		const hovered = hoveredAircraftIcao24;
		source?.setData({
			type: 'FeatureCollection',
			features: [...aircraftStore.trails.entries()]
				.filter(([icao24, points]) => icao24 === hovered && points.length > 1)
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
					timeMs: quake.timeMs,
					alertLevel: quake.alertLevel ?? null,
					tsunamiWarning: quake.tsunamiWarning,
					detailsUrl: quake.detailsUrl ?? null
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
					acquiredAtIso: hotspot.acquiredAtIso,
					isNightDetection: hotspot.isNightDetection ?? null
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
				element.setAttribute('role', 'img');
				element.setAttribute('aria-label', `Zone vertex ${index + 1}`);
				element.textContent = String(index + 1);
				marker = new maplibregl.Marker({ element }).setLngLat([lon, lat]).addTo(activeMap);
				zoneVertexMarkers[index] = marker;
			}
			marker.setLngLat([lon, lat]);
		});
	});

	// Draw every ward's own route from a fleet mission draft, as one colored
	// dashed line per ward - the actual visual fix over the old design: N
	// distinct lines prove N distinct routes, not one shared line
	// duplicated onto every marker. Numbered pins only render for the
	// route currently being edited (activeWardId) - see waypointMarkers'
	// comment.
	$effect(() => {
		const activeMap = map;
		if (!activeMap || !mapLoaded) return;
		const fleetDraft = fleetGroups.missionAssignmentDraft;

		const features: GeoJSON.Feature<
			GeoJSON.LineString,
			{ wardId: string; color: string; opacity: number }
		>[] = [];
		let activeWaypoints: DraftWaypoint[] = [];
		if (fleetDraft) {
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
				element.setAttribute('role', 'img');
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
			element.setAttribute('role', 'img');
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
				element.setAttribute('role', 'img');
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

		const points: Array<GeoJSON.Feature<GeoJSON.Point, { wardId: string; connected: boolean }>> =
			[];
		for (const wardId of fleet.wardIds) {
			const position = fleet.wards[wardId]?.state?.position;
			if (!position) continue;
			points.push({
				type: 'Feature',
				properties: { wardId, connected: fleet.wards[wardId]?.state?.connected ?? false },
				geometry: { type: 'Point', coordinates: [position.longitudeDeg, position.latitudeDeg] }
			});
		}
		clusterIndex = new Supercluster<{ wardId: string; connected: boolean }, { anyOnline: boolean }>(
			{
				radius: CLUSTER_RADIUS_PX,
				maxZoom: CLUSTER_MAX_ZOOM,
				// Aggregates "does this cluster have anything live in it" the same
				// way supercluster aggregates point_count itself - see
				// updateClusterMarkerElement's own comment on why the bubble needs
				// this at all.
				map: (props) => ({ anyOnline: props.connected }),
				reduce: (accumulated, props) => {
					accumulated.anyOnline ||= props.anyOnline;
				}
			}
		).load(points);
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
			const anyOnline = feature.properties.anyOnline;
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
			updateClusterMarkerElement(clusterMarker.getElement(), pointCount, anyOnline);
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
			// (0, 0) - "Null Island" - is a fallback, never a genuine fix: a
			// real ward's state envelope falls invalid/NaN GPS fields back to
			// 0 rather than dropping the frame (see karshipta-cloud's
			// toLiveWardState), which used to crash the whole snapshot instead
			// of just this one ward's position. Plotting that fallback reads
			// as a ward in the middle of the ocean - confirmed live. Remove
			// any existing marker instead; the ward stays visible in the
			// fleet list regardless, this only affects the map layer.
			const hasNoRealPosition =
				state.position.latitudeDeg === 0 && state.position.longitudeDeg === 0;
			if (hasNoRealPosition) {
				markers[wardId]?.marker.remove();
				delete markers[wardId];
				continue;
			}
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
				handle.nameEl.textContent = nameFor?.(wardId) ?? wardId;
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
					// No placeholder circle when there's no photo - an empty
					// bordered dot next to the name reads as a broken image,
					// not "no avatar set".
					handle.ownerAvatar.classList.toggle('hidden', !owner.photoUrl);
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

<!-- height, not padding-bottom: padding is still inside the content box
     clientHeight measures, so a padded-in map container would just keep
     rendering its canvas into the padded-away area - confirmed live, it
     has to actually shrink. On THIS wrapper, not just the map canvas
     below: every overlay in this file (the button column, weather,
     legend, toast) is positioned relative to or inside this same div,
     not the canvas - shrinking only the canvas left them anchored to
     this div's own (unchanged) full height, so they stayed in their
     original spots floating over the sheet instead of moving with the
     map, confirmed live as a real bug. mobileLocalSheetReserve covers
     this file's own layers/attribution sheets below;
     --mobile-external-sheet-reserve is set by a consuming app's own
     bottom sheets (right-panel.svelte's ward detail, left-rail.svelte's
     own) via +page.svelte - see that file's own comment. top/height both
     also subtract --mobile-ward-card-height: that card is `position:
     fixed`, so it was never reducing this div's own box at all -
     unlike a bottom sheet, it just overlaid the top of the map without
     this div ever being told to get out of its way, confirmed live as
     a real bug (the 50% floor below was computed against the full
     height, then the card ate into the result from the other edge
     unaccounted for, leaving noticeably less than 50% actually
     visible). max(50%, ...): the map never shrinks past half the
     available height no matter how tall a sheet's own content gets -
     confirmed as the wanted floor directly, not a guess. A sheet whose
     own content exceeds the remaining space past that floor scrolls
     internally rather than pushing the map smaller still (every sheet's
     own --mobile-sheet-max-height cap, +page.svelte, is set to the same
     50% so the two never disagree; overflow-y-auto takes over past it).
     transition: smooths the push instead of snapping instantly.
     The ResizeObserver below (on the map canvas, which inherits this
     div's new size via its own plain h-full) is what actually makes
     MapLibre redraw at the new size - a container that merely changes
     size doesn't trigger that on its own. -->
<div
	class="relative w-full transition-[height,top] duration-200 ease-out"
	style="top: var(--mobile-ward-card-height, 0px); height: max(50%, calc(100% - var(--mobile-ward-card-height, 0px) - {mobileLocalSheetReserve}px - var(--mobile-external-sheet-reserve, 0px)));"
>
	<!--
		MapLibre takes ownership of this div's contents (canvas, its own
		control divs) and paints over anything already inside it, so overlays
		must be siblings here, not children of the bound container below.
	-->
	<div bind:this={container} role="region" class="h-full w-full" aria-label="Fleet map"></div>
	{#if mapError}
		<p
			role="alert"
			class="absolute inset-x-0 top-1/2 mx-auto w-fit max-w-lg rounded border border-critical bg-panel px-4 py-2 text-sm text-critical"
		>
			Map unavailable: {mapError}
		</p>
	{/if}
	<!-- Shared top-left group: weather first (anchored to the corner),
	     the loading toast beside it - both live here together rather than
	     at opposite corners, confirmed as the wanted layout directly.
	     items-start: top-aligns them regardless of which is taller at any
	     given moment (the weather card grows with its own content; the
	     toast is a single line or a small spinner badge). max-md:top-
	     [...var(--mobile-ward-card-height)]: a consuming app's own fixed
	     ward-detail card (shown once a ward is selected - see
	     karshipta-cloud/console's own +page.svelte/right-panel.svelte)
	     can cover this corner otherwise; 0px whenever no such card exists
	     or none is currently showing. -->
	<!-- max-md:z-[46]: same reasoning as the button column's own comment
	     above - without it, a consuming app's own expanded EdgeSheet
	     catcher silently ate every tap meant for the weather/toast
	     buttons here too. -->
	<div
		class="absolute bottom-[10px] left-3 flex items-end gap-2 max-md:bottom-[var(--mobile-chrome-bottom,10px)] max-md:z-[46]"
	>
		{#if showWeather && weatherStore.conditions}
			{@const conditions = weatherStore.conditions}
			<!-- Not a point/popup layer like Aircraft/Earthquakes/etc.:
			     weather is a continuous field, not discrete features, so
			     it's a small always-visible readout for the current map
			     center rather than something you hover a marker for. Same
			     dimmed-until-hovered treatment as the legend panel - a
			     passive readout, not something that needs to compete with
			     the map at full strength until the operator actually looks
			     at it. Below max-md, this card starts collapsed into a
			     button (same h-8 w-8/border/bg-panel treatment as the
			     bottom-right control column) rather than the full card
			     permanently eating this corner - gated by the CSS
			     breakpoint itself plus a plain boolean, not a JS isMobile
			     check: this shared component has no such singleton
			     available (see weatherExpandedMobile's own comment).
			     Desktop is untouched either way - max-md:hidden never
			     applies there, so the card always shows regardless of the
			     boolean. -->
			{#if !weatherExpandedMobile}
				<button
					type="button"
					onclick={() => (weatherExpandedMobile = true)}
					aria-label="Show weather"
					aria-expanded="false"
					class="hidden h-8 w-8 shrink-0 items-center justify-center rounded border border-edge bg-panel/90 text-fg-muted hover:border-fg-muted hover:text-fg max-md:flex"
				>
					<WeatherIcon
						shape={weatherIconShape(conditions.weatherCode, conditions.isDay)}
						size={17}
					/>
				</button>
			{/if}
			<div
				bind:this={weatherMobileEl}
				role="group"
				class="flex w-48 shrink-0 flex-col gap-1 rounded border border-edge bg-panel/90 px-2.5 py-1.5 text-[10px] text-fg-muted opacity-50 transition-opacity duration-300 ease-out hover:opacity-100 max-md:w-42 {weatherExpandedMobile
					? 'max-md:opacity-100'
					: 'max-md:hidden'}"
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
		<!-- One banner, not one per service: confirmed live that stacking a
		     separate "Loading airspace data" and "Loading aircraft data" box
		     read as visual clutter even though both were true simultaneously
		     often enough (OpenAIP and adsb.lol are genuinely independent
		     services with their own keys/rate limits, so either can be loading
		     or erroring without the other). The message itself says which,
		     rather than always showing a generic "Loading map data" that would
		     lose that distinction. max-w-[130px] on mobile: this div sits in
		     the map's own container, which is narrower than the full
		     viewport by both icon rails' width - confirmed live, a vw-based
		     cap measured against the whole screen instead still ran too
		     wide for it. 130px holds against the worst case (both rails
		     visible, nothing expanded - the map's container width never
		     gets narrower than that on this viewport, since EdgeSheet
		     overlays rather than resizing it). -->
		{#if !hideLoadingToast && loadingServices.length > 0}
			<!-- Shown only while a fetch is genuinely in flight
			     (loadingServices, derived above from every layer's own
			     `loading` field), not while something has merely errored
			     and is waiting on its own cooldown to retry - requested directly, after a static
			     "warning" icon stood in for that second case: an operator
			     can't do anything about a background retry they're not
			     told the specifics of, so the honest options are an
			     animation while real work is happening, or nothing at all
			     otherwise, not a third "something might be wrong" state
			     with no action attached to it. Airspace errors are almost
			     always OpenAIP's own shared rate limit (see
			     openaip/request-gate.ts), which clears on its own - every
			     store's own raw error still goes to console.error for
			     anyone who wants it, just not to a persistent visible
			     badge the operator has no use for. -->
			<!-- max-[999.98px]: below this, the full sentence doesn't fit
			     anywhere worth putting it (this corner is already capped to
			     130px wide below the md breakpoint - see this div's own
			     comment), so the toast itself collapses to a plain spinner
			     badge, same h-8 w-8 footprint as the control column's own
			     buttons. role="status" (a live region) and the text itself
			     both stay in the DOM regardless - sr-only, not removed -
			     so a screen reader still announces what's loading; only
			     the visual presentation changes. 999.98px, not 1000:
			     mirrors is-mobile.svelte.ts's own 767.98 convention for
			     an exact max-width breakpoint (max-width is CSS-inclusive,
			     so a plain 1000 would also catch width === 1000 itself). -->
			<p
				role="status"
				class="flex w-fit max-w-md shrink-0 items-center justify-center gap-1.5 rounded border border-edge bg-panel/90 px-3 py-1.5 text-center text-xs text-fg-muted max-[999.98px]:h-8 max-[999.98px]:w-8 max-[999.98px]:p-0"
			>
				<svg
					width="15"
					height="15"
					viewBox="0 0 24 24"
					fill="none"
					stroke="currentColor"
					stroke-width="1.75"
					stroke-linecap="round"
					class="hidden shrink-0 animate-spin max-[999.98px]:block"
					aria-hidden="true"
				>
					<path d="M12 2a10 10 0 0 1 10 10" />
				</svg>
				<span class="max-[999.98px]:sr-only"
					>Loading {loadingServices.length === 1 ? loadingServices[0] : 'map'} data - this can take a
					few seconds.</span
				>
			</p>
		{/if}
	</div>

	<!--
		Bottom-right, stacked directly above MapLibre's own NavigationControl
		(zoom/compass, also bottom-right) - one control cluster instead of two
		spatially separate ones, since these are all "controls that change how
		you look at the map" and grouping them reads as more organized than
		scattering them across corners. Locate me and Measure stack above map
		style, in the same column, for the same reason.
	-->
	<!-- max-md:z-[46]: a consuming app's own EdgeSheet-based sheet
	     (ward detail, fleet) puts a full-viewport `fixed` outside-tap
	     catcher at z-[45] while expanded - without this, that catcher
	     painted above these buttons (a `position: fixed` element with
	     any z-index always wins over plain in-flow/auto-z content
	     regardless of DOM order) and silently ate every tap meant for
	     them, confirmed live as a real bug: trying to open the layers
	     menu while the ward-detail sheet was expanded did nothing at
	     all. Same reasoning the rails in a consuming app's own
	     left-rail.svelte/right-panel.svelte already needed this for. -->
	<!-- bottom-[159px], not 155px: the native zoom/compass group right
	     below this column grew 3px taller per button (29px -> 32px, see
	     that group's own CSS comment on why) once it was resized to match
	     these buttons, which shrank the gap between them from 10px (the
	     gap used everywhere else in this column) down to 6px - confirmed
	     live. +4px here restores a measured, exact 10px gap above the
	     native group without touching its own position (which already
	     measured a correct ~10px gap to the attribution button below it -
	     moving the native group itself would have fixed one gap at the
	     other's expense). -->
	<div class="absolute right-[10px] bottom-[159px] flex flex-col items-end gap-2.5 max-md:z-[46]">
		<button
			type="button"
			onclick={locateMe}
			disabled={locating}
			aria-label="Center on my location"
			class="flex h-8 w-8 items-center justify-center rounded border border-edge bg-panel/90 text-fg-muted hover:border-fg-muted hover:text-fg disabled:opacity-50"
		>
			<svg
				width="15"
				height="15"
				viewBox="0 0 24 24"
				fill="none"
				stroke="currentColor"
				stroke-width="1.75"
				stroke-linecap="round"
				class={locating ? 'animate-spin' : ''}
			>
				<circle cx="12" cy="12" r="7" />
				<path d="M12 2v3M12 19v3M2 12h3M19 12h3" />
			</svg>
		</button>
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
				onclick={toggleLayersMenu}
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
				<!-- Desktop: opens to the LEFT of the trigger (right-full,
				     not right-0/bottom-full) at the same bottom edge
				     (bottom-0) - requested directly, matching the
				     attribution button's own popup right below. Its height
				     is capped with its own scrollbar instead of being free
				     to grow past the map's top/bottom edges (that used to
				     clip both the map style list and the Zones row at the
				     bottom silently). max-md:hidden: below that, a
				     corner-anchored popup this tall/wide ran out of room
				     and clipped against the viewport edges, confirmed live
				     - the bottom sheet below replaces it there instead. -->
				<div
					class="absolute right-full bottom-0 z-30 mr-1 flex max-h-[min(90vh,44rem)] w-72 flex-col gap-3 overflow-y-auto rounded border border-edge bg-panel/95 p-2.5 max-md:hidden"
				>
					{@render layersMenuContent()}
				</div>
				<!-- Mobile: a bottom sheet instead, same non-dimming/
				     rounded-top/safe-area treatment as the app shell's own
				     ward-detail sheet (EdgeSheet isn't reused directly - this
				     is the shared library component fleet-map.svelte itself,
				     which has no JS isMobile check available to gate it with;
				     see weatherExpandedMobile's own comment on why every
				     mobile-only toggle here is CSS-breakpoint-gated instead).
				     Outside-tap-to-close already works for this exactly like
				     the desktop popup does, via the same layersMenuEl-scoped
				     effect above - no separate catcher needed. -->
				<div
					bind:clientHeight={layersMenuMobileHeight}
					class="fixed bottom-0 z-[45] hidden flex-col overflow-y-auto rounded-t-lg border-t border-edge bg-panel max-md:flex"
					style="left: var(--mobile-left-rail-inset, 0px); right: var(--mobile-right-rail-inset, 0px); max-height: var(--mobile-sheet-max-height, 80vh)"
				>
					<SheetHeader title="Map style & layers" onclose={() => (layersMenuOpen = false)} />
					<div class="flex flex-col gap-3 p-3 pb-[max(0.75rem,env(safe-area-inset-bottom,0px))]">
						{@render layersMenuContent()}
					</div>
				</div>
			{/if}
		</div>
	</div>

	<!-- Replaces MapLibre's own native compact AttributionControl - see
	     attributionControl: false's own comment on why, and the
	     :global(.maplibregl-ctrl-bottom-right) rule below for why this
	     needs its own margin now. Back in its original corner position,
	     not grouped with locate/measure/layers: that read as a real
	     button-order change (confirmed directly - the attribution button
	     used to sit below NavigationControl, closest to the true corner,
	     from MapLibre's own native bottom-right stacking order, not above
	     it alongside this file's own custom controls). Reuses the exact
	     same "i" glyph left-rail.svelte's own About button uses, for a
	     consistent meaning across the app (that icon already reads as
	     "more info" there). -->
	<!-- max-md:z-[46]: same reasoning as the button column's own comment
	     above - without it, a consuming app's own expanded EdgeSheet
	     catcher silently ate every tap meant for this button too. -->
	<div bind:this={attributionMenuEl} class="absolute right-[10px] bottom-[10px] max-md:z-[46]">
		<button
			type="button"
			onclick={toggleAttributionMenu}
			aria-expanded={attributionMenuOpen}
			aria-label="Map data and license"
			class="flex h-8 w-8 items-center justify-center rounded border border-edge bg-panel/90 text-fg-muted hover:border-fg-muted hover:text-fg"
		>
			<svg
				width="15"
				height="15"
				viewBox="0 0 24 24"
				fill="none"
				stroke="currentColor"
				stroke-width="1.75"
				stroke-linecap="round"
				stroke-linejoin="round"
				aria-hidden="true"
			>
				<circle cx="12" cy="12" r="9" />
				<line x1="12" y1="11" x2="12" y2="16.5" />
				<circle cx="12" cy="7.5" r="1" fill="currentColor" stroke="none" />
			</svg>
		</button>
		{#if attributionMenuOpen}
			<!-- Desktop: opens to the LEFT of the button (right-full, not
			     right-0/bottom-full) - requested directly, sitting beside
			     the button at the same bottom edge (bottom-0) rather than
			     stacked above it. w-fit with no max-w: each line is
			     whitespace-nowrap (see BASEMAP_ATTRIBUTION_LINES's own
			     comment on why - a fixed, predictable height instead of
			     wrapped prose), so the box sizes itself to the one
			     genuinely longest credit rather than clipping/wrapping it
			     under an arbitrary max-w-xs cap - confirmed live, that cap
			     was exactly what forced the longest lines to wrap in the
			     first place. -->
			<div
				class="absolute right-full bottom-0 z-30 mr-1 w-fit rounded border border-edge bg-panel/95 p-2.5 text-[10px] text-fg-muted max-md:hidden"
			>
				{#each BASEMAP_ATTRIBUTION_LINES as line (line)}
					<p class="whitespace-nowrap">{line}</p>
				{/each}
			</div>
			<!-- Mobile: bottom sheet, same shape as the layers menu's own -
			     see that one's comment on why this isn't EdgeSheet directly. -->
			<div
				bind:clientHeight={attributionMenuMobileHeight}
				class="fixed bottom-0 z-[45] hidden flex-col overflow-y-auto rounded-t-lg border-t border-edge bg-panel max-md:flex"
				style="left: var(--mobile-left-rail-inset, 0px); right: var(--mobile-right-rail-inset, 0px); max-height: var(--mobile-sheet-max-height, 80vh)"
			>
				<SheetHeader title="Map data & license" onclose={() => (attributionMenuOpen = false)} />
				<div
					class="flex flex-col gap-0.5 p-3 pb-[max(0.75rem,env(safe-area-inset-bottom,0px))] text-[11px] text-fg-muted"
				>
					{#each BASEMAP_ATTRIBUTION_LINES as line (line)}
						<p>{line}</p>
					{/each}
				</div>
			</div>
		{/if}
	</div>

	{#if selectedAircraftIcao24}
		{@const selectedAircraft = aircraftStore.aircraft.find(
			(candidate) => candidate.icao24 === selectedAircraftIcao24
		)}
		{#if selectedAircraft}
			<!-- Mobile only (max-md:flex): tap-to-select's own info sheet (see
			     selectAircraft's own comment on why this exists) - same
			     hand-rolled shape as the layers/attribution sheets above, not
			     EdgeSheet directly, for the same reason neither of those is
			     either (no JS isMobile check available in this shared
			     component). Desktop keeps its existing hover popup unchanged;
			     this never renders there. Deliberately lighter than the
			     desktop popup: the synchronous fields only, not the adsbdb
			     enrichment fetch (route/manufacturer/country) that popup also
			     shows - that returns a pre-built HTML string, awkward to
			     reuse as real Svelte markup, and good enough as a smaller v1
			     rather than holding this up for it. -->
			<div
				bind:clientHeight={aircraftSheetMobileHeight}
				class="fixed bottom-0 z-[45] hidden flex-col overflow-y-auto rounded-t-lg border-t border-edge bg-panel max-md:flex"
				style="left: var(--mobile-left-rail-inset, 0px); right: var(--mobile-right-rail-inset, 0px); max-height: var(--mobile-sheet-max-height, 80vh)"
			>
				<SheetHeader
					title={selectedAircraft.callsign ?? 'Aircraft'}
					onclose={() => (selectedAircraftIcao24 = undefined)}
				>
					{#snippet extra()}
						{#if isAircraftEmergency(selectedAircraft)}
							<span
								class="rounded bg-critical/15 px-1.5 py-0.5 text-[10px] font-medium text-critical"
							>
								EMERGENCY
							</span>
						{/if}
						{#if selectedAircraft.isMilitary}
							<span class="rounded bg-white/10 px-1.5 py-0.5 text-[10px] font-medium text-fg-muted">
								MILITARY
							</span>
						{/if}
					{/snippet}
				</SheetHeader>
				<dl
					class="flex flex-col gap-1.5 p-3 pb-[max(0.75rem,env(safe-area-inset-bottom,0px))] text-[11px] text-fg-muted"
				>
					{#if isAircraftEmergency(selectedAircraft) && selectedAircraft.emergency}
						<div class="flex justify-between gap-2">
							<dt>Emergency</dt>
							<dd class="text-critical">
								{AIRCRAFT_EMERGENCY_LABELS[selectedAircraft.emergency] ??
									selectedAircraft.emergency}
							</dd>
						</div>
					{/if}
					{#if selectedAircraft.typeDescription}
						<div class="flex justify-between gap-2">
							<dt>Type</dt>
							<dd>{selectedAircraft.typeDescription}</dd>
						</div>
					{/if}
					{#if selectedAircraft.operator}
						<div class="flex justify-between gap-2">
							<dt>Operator</dt>
							<dd>{selectedAircraft.operator}</dd>
						</div>
					{/if}
					{#if selectedAircraft.registration}
						<div class="flex justify-between gap-2">
							<dt>Registration</dt>
							<dd>{selectedAircraft.registration}</dd>
						</div>
					{/if}
					<div class="flex justify-between gap-2">
						<dt>ICAO24</dt>
						<dd>{selectedAircraft.icao24}</dd>
					</div>
					{#if selectedAircraft.yearBuilt !== undefined}
						<div class="flex justify-between gap-2">
							<dt>Built</dt>
							<dd>{selectedAircraft.yearBuilt}</dd>
						</div>
					{/if}
					{#if selectedAircraft.altitudeM !== undefined}
						<div class="flex justify-between gap-2">
							<dt>Altitude</dt>
							<dd>{formatAltitude(selectedAircraft.altitudeM, unitsStore.current)}</dd>
						</div>
					{/if}
					{#if selectedAircraft.velocityMS !== undefined}
						<div class="flex justify-between gap-2">
							<dt>Speed</dt>
							<dd>{formatVehicleSpeed(selectedAircraft.velocityMS, unitsStore.current)}</dd>
						</div>
					{/if}
					{#if selectedAircraft.verticalRateMS !== undefined && Math.abs(selectedAircraft.verticalRateMS) >= 1.5}
						<div class="flex justify-between gap-2">
							<dt>{selectedAircraft.verticalRateMS > 0 ? 'Climbing' : 'Descending'}</dt>
							<dd>{formatSpeed(Math.abs(selectedAircraft.verticalRateMS), unitsStore.current)}</dd>
						</div>
					{/if}
					{#if selectedAircraft.onGround}
						<div class="flex justify-between gap-2">
							<dt>Status</dt>
							<dd>On ground</dd>
						</div>
					{/if}
				</dl>
			</div>
		{/if}
	{/if}

	{#snippet layersMenuContent()}
		<div>
			<p class="mb-1.5 text-[9px] font-medium tracking-widest text-fg-muted">MAP STYLE</p>
			<!-- A choice, not an on/off switch - the selected card gets an
					     accent border and a check badge, not a checkbox, since
					     exactly one is always active. Plain light/dark isn't offered
					     here as its own pair: "Map" follows the app-wide theme
					     toggle (themeStore) rather than exposing a second,
					     independent choice - see activeVectorStyle's own comment.
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
							class="relative aspect-[8/5] overflow-hidden rounded border {mapStyle === option.id
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
			<!-- Always rendered, unlike No-fly zones below: Aircraft/Obstacles/
						     Airports are all proxied through a consuming app's own backend
						     now (see obstacle-store.svelte.ts's own comment), so none of
						     them need an OpenAIP key client-side any more - only geozones
						     still does. Aircraft/Obstacles/Airports live here, not in Data
						     layers below - they're live traffic and structures occupying
						     the airspace, not generic geographic reference data the way
						     Cities/Weather/Earthquakes/Wildfires are. -->
			<p class="mb-1 text-[9px] font-medium tracking-widest text-fg-muted">AIRSPACE</p>
			<div class="mb-2 grid grid-cols-2 gap-x-1 gap-y-0.5">
				<label
					class="flex cursor-pointer items-center gap-1.5 rounded px-1 py-1 text-[11px] hover:bg-white/5"
				>
					<input type="checkbox" bind:checked={showAirports} class="accent-accent" />
					Airports
				</label>
				<label
					class="flex cursor-pointer items-center gap-1.5 rounded px-1 py-1 text-[11px] hover:bg-white/5"
				>
					<input type="checkbox" bind:checked={showAircraft} class="accent-accent" />
					Aircraft
				</label>
				<label
					class="flex cursor-pointer items-center gap-1.5 rounded px-1 py-1 text-[11px] hover:bg-white/5"
				>
					<input type="checkbox" bind:checked={showObstacles} class="accent-accent" />
					Obstacles
				</label>
				{#if geozoneStore.active}
					<label
						class="flex cursor-pointer items-center gap-1.5 rounded px-1 py-1 text-[11px] hover:bg-white/5"
					>
						<input type="checkbox" bind:checked={showGeozones} class="accent-accent" />
						No-fly zones
					</label>
				{/if}
			</div>
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
							     widget itself, rendered separately below. -->
				<label
					class="flex cursor-pointer items-center gap-1.5 rounded px-1 py-1 text-[11px] hover:bg-white/5"
				>
					<input type="checkbox" bind:checked={showWeather} class="accent-accent" />
					Weather
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
	{/snippet}

	<!-- One panel, always rendered (the scale bar - its last child, see
	     scaleBarSlot - is meaningful on its own even with no layer rows).
	     Dimmed until hovered as one unit: a static reference key doesn't
	     need to compete with the map until the operator actually looks.
	     max-md: with up to 3 rows + the scale bar, this can grow tall
	     enough on a short/narrow viewport to run off-screen or reach
	     content above it - capped with internal scroll. top-3, same on
	     every breakpoint, no extra --mobile-ward-card-height offset: the
	     map's own wrapper (the ancestor just above this one) already
	     shifts itself down by that much via its own `top`, so this panel
	     sticks to the wrapper's real top edge, right under the card, for
	     free - adding the card height again here double-counted it and
	     pushed the button noticeably below the card instead of right
	     under it, confirmed live as a real bug. Below max-md, this panel
	     starts collapsed into a button (same h-8 w-8/border/bg-panel
	     treatment as the bottom-right control column, and the same
	     CSS-breakpoint-gated collapse the weather widget above uses)
	     rather than permanently eating this corner. -->
	{#if !legendExpandedMobile}
		<button
			type="button"
			onclick={() => (legendExpandedMobile = true)}
			aria-label="Show map legend"
			aria-expanded="false"
			class="absolute top-3 left-[10px] hidden h-8 w-8 items-center justify-center rounded border border-edge bg-panel/90 text-fg-muted hover:border-fg-muted hover:text-fg max-md:z-[46] max-md:flex"
		>
			<svg
				width="17"
				height="17"
				viewBox="0 0 24 24"
				fill="none"
				stroke="currentColor"
				stroke-width="1.75"
				stroke-linecap="round"
				stroke-linejoin="round"
				aria-hidden="true"
			>
				<path d="M4 2.3 6.2 6 1.8 6Z" />
				<line x1="10" y1="4.3" x2="21" y2="4.3" />
				<rect x="2" y="10" width="4" height="4" rx="0.8" />
				<line x1="10" y1="12" x2="21" y2="12" />
				<circle cx="4" cy="20" r="2" />
				<line x1="10" y1="20" x2="21" y2="20" />
			</svg>
		</button>
	{/if}
	<div
		bind:this={legendMobileEl}
		class="absolute top-3 left-[10px] flex flex-col gap-1.5 rounded border border-edge bg-panel/90 px-2 py-1.5 opacity-50 transition-opacity duration-300 ease-out hover:opacity-100 max-md:z-[46] max-md:max-h-[40vh] max-md:max-w-[80vw] max-md:overflow-y-auto {legendExpandedMobile
			? 'max-md:opacity-100'
			: 'max-md:hidden'}"
	>
		<!-- Leaf slot for the scale control's DOM node - see
		     scaleBarSlot's own comment on why this can't just be the panel
		     div above it. -->
		<div use:scaleBarSlot={mapLoaded}></div>
		{#if showZoneRow}
			<ul class="flex gap-3" aria-label="Zone legend">
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
		{#if showWardsRow || showAircraftRow}
			<!-- Wards and Aircraft share a row, last: both are moving,
			     trackable entities on the map (hover/select for details),
			     unlike every static reference/hazard layer above. Confirmed
			     live a demo (synthetic) ward and a real gateway-connected one
			     needed calling out explicitly - they only ever differed by
			     marker fill color (see the ward marker sync effect below),
			     with nothing else on screen explaining what that color meant. -->
			<ul
				class="flex flex-wrap gap-3 {showZoneRow ||
				showGeozoneRow ||
				showCityRow ||
				showAirportRow ||
				showObstacleRow ||
				showWildfireRow ||
				showEarthquakeRow
					? 'border-t border-edge pt-1.5'
					: ''}"
				aria-label="Ward and aircraft legend"
			>
				{#if showWardsRow}
					<li class="flex items-center gap-1 text-[10px]">
						<span class="h-2 w-2 rounded-full" style="background-color: #f5a623"></span>
						Live ward
					</li>
					<li class="flex items-center gap-1 text-[10px]">
						<span class="h-2 w-2 rounded-full" style="background-color: #a78bfa"></span>
						Demo ward
					</li>
				{/if}
				{#if showAircraftRow}
					<li class="flex items-center gap-1 text-[10px]">
						<span class="h-2 w-2 rounded-full" style="background-color: {AIRCRAFT_COLOR}"></span>
						Aircraft
					</li>
					<li class="flex items-center gap-1 text-[10px]">
						<span class="h-2 w-2 rounded-full" style="background-color: {AIRCRAFT_MILITARY_COLOR}"
						></span>
						Military
					</li>
				{/if}
			</ul>
		{/if}
		{#if showCityRow || showAirportRow || showObstacleRow || showWildfireRow || showEarthquakeRow}
			<ul
				class="flex flex-wrap gap-3 {showZoneRow || showGeozoneRow
					? 'border-t border-edge pt-1.5'
					: ''}"
				aria-label="Point layer legend"
			>
				{#if showCityRow}
					<li class="flex items-center gap-1 text-[10px]">
						<span class="h-2 w-2 rounded-full" style="background-color: {CITY_COLOR}"></span>
						Cities
					</li>
				{/if}
				{#if showAirportRow}
					<li class="flex items-center gap-1 text-[10px]">
						<span class="h-2 w-2 rounded-full" style="background-color: {AIRPORT_COLOR}"></span>
						Airports
					</li>
				{/if}
				{#if showObstacleRow}
					<li class="flex items-center gap-1 text-[10px]">
						<span class="h-2 w-2 rounded-full" style="background-color: {OBSTACLE_COLOR}"></span>
						Obstacles
					</li>
				{/if}
				{#if showWildfireRow}
					<li class="flex items-center gap-1 text-[10px]">
						<span class="h-2 w-2 rounded-full" style="background-color: {WILDFIRE_COLOR}"></span>
						Wildfires
					</li>
				{/if}
				{#if showEarthquakeRow}
					<li class="flex items-center gap-1 text-[10px]">
						<span class="h-2 w-2 rounded-full" style="background-color: {EARTHQUAKE_COLOR}"></span>
						Earthquakes
					</li>
				{/if}
			</ul>
		{/if}
	</div>
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
	/* !important: MapLibre's own stylesheet sets background/border/box-shadow
	 * on this exact same single-class selector, so without it, load order
	 * (not specificity - both rules are equally specific) decides the
	 * winner, and the vendor's white background + drop-shadow can leak
	 * through - the zoom/compass group then visibly doesn't match the
	 * locate/measure/layers buttons right above it, which use this file's
	 * own plain border-edge/bg-panel styling with no shadow at all. */
	:global(.maplibregl-ctrl-group) {
		background: var(--color-panel) !important;
		border: 1px solid var(--color-edge) !important;
		box-shadow: none !important;
	}
	:global(.maplibregl-ctrl-group button + button) {
		border-top: 1px solid var(--color-edge);
	}
	/* 32px, MapLibre's own default is 29px: this file's own custom buttons
	   right above this group (locate/measure/layers) and the attribution
	   button right below it are all h-8 (32px) - confirmed live as a real,
	   visible size mismatch otherwise, this native group read as
	   noticeably smaller than every button surrounding it. */
	:global(.maplibregl-ctrl-group button) {
		width: 32px !important;
		height: 32px !important;
		background: transparent;
	}
	:global(.maplibregl-ctrl-group button:hover) {
		background: rgb(255 255 255 / 0.05);
	}
	/* Reserves the space the old native AttributionControl used to occupy
	   at the true bottom-right corner (10px margin + its own ~32px
	   height), now that attributionControl: false leaves only
	   NavigationControl in this native stack - without this,
	   NavigationControl dropped straight down to reclaim that space once
	   the control above it in the stack was gone, confirmed live:
	   NavigationControl ended up directly behind the custom button this
	   file now positions in that exact spot (see attributionMenuEl's own
	   comment). 42px = this file's own h-8 (32px) button + its usual
	   10px gap from everything else in this corner. */
	:global(.maplibregl-ctrl-bottom-right) {
		margin-bottom: 42px !important;
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
	/* Lives inside the legend panel now (see scaleBarSlot), as a plain row
	   matching the others there - not MapLibre's own default floating box
	   (2px #333 border, white 75%-opacity fill). Bottom border, not the
	   other rows' own border-top: this row is always last, so its divider
	   closes off the panel's own bottom edge instead of separating from
	   a row above it. */
	:global(.maplibregl-ctrl-scale) {
		background: transparent !important;
		border: none !important;
		border-bottom: 1px solid var(--color-edge) !important;
		padding: 0 0 6px !important;
		margin: 0 !important;
		font-size: 10px !important;
		line-height: 1 !important;
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
