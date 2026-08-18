import { SvelteMap } from 'svelte/reactivity';
import type { Aircraft, AircraftCategory, ViewportBounds } from './types';
import { AdsbOneAccessError, AdsbOneAircraftSource } from './adsbOne';

// adsb.one documents a 1 request/second limit (see adsbOne.ts's own README
// reference) - comfortably generous compared to OpenSky's few-hundred-per-day
// anonymous tier this replaced, so this can debounce far more snappily
// while staying well under it.
const FETCH_DEBOUNCE_MS = 3_000;
const FAILURE_COOLDOWN_MS = 10_000;
// 15s+ read as sluggish - 7s keeps a single-tile refresh (the common case)
// comfortably under the 1 req/s limit while roughly halving the wait.
// Paired with fleet-map.svelte's own marker animation (see
// aircraftMarkerElement), which eases between fixes instead of snapping.
const CACHE_TTL_MS = 7_000;
const CACHE_GRID_DEG = 0.25;
// Short breadcrumb, not a real flight-path replay - just enough to read
// "which way did this one come from" at a glance, same spirit as the
// ward-trail dots this mirrors in fleet-map.svelte.
const TRAIL_MAX_POINTS = 12;
// Every other layer here only ever refetches on moveend - fine for data
// that doesn't move on its own (obstacles, cities) or changes slowly
// (weather), but aircraft genuinely move every second, and an operator
// watching a static view (the normal way to watch a map, not constantly
// panning) would otherwise only ever see one position per plane and the
// trail feature above would never accumulate a second point. A beat past
// CACHE_TTL_MS, not equal to it - equal values would race against
// Date.now() precision and could occasionally land exactly on a cache
// entry's expiry instead of unambiguously after it.
const REFRESH_INTERVAL_MS = CACHE_TTL_MS + 1_000;

// The /point endpoint's radius is capped at 250nm regardless of how far
// zoomed out the map is (see adsbOne.ts) - there's no way to make
// the query itself cover more area. Zoomed way out over a busy region
// (most of Europe, say), that fixed ~460km-diameter circle can still
// return hundreds of aircraft including every light GA/glider in range,
// which reads as a dense, arbitrary-looking blob rather than a sensible
// "zoomed out = see the big picture" view (confirmed live: a 300km-scale
// view over the Balkans returned 200+ aircraft in one tight cluster).
// Since the radius itself can't grow, the fix is the same one
// city-store.svelte.ts already uses for population thinning: show only
// the more "important" (larger) categories at low zoom, opening up to
// every category as the operator actually zooms in - same breakpoints
// (6, 8) as that store, for consistency.
const CATEGORY_MIN_ZOOM: Record<AircraftCategory, number> = {
	heavy: 0,
	rotorcraft: 6,
	glider: 6,
	uav: 6,
	light: 8,
	ground: 8,
	unknown: 8
};

function filterByZoom(aircraft: Aircraft[], zoom: number): Aircraft[] {
	return aircraft.filter((plane) => zoom >= CATEGORY_MIN_ZOOM[plane.category]);
}

// Conservative vs adsb.one's actual ~463km (250nm) diameter cap -
// deliberately smaller so adjacent tiles overlap a bit rather than
// leaving a gap between them.
const SINGLE_TILE_COVERAGE_KM = 400;
const KM_PER_DEG_LAT = 111;
// Comfortably over 1 request/second between tiles of the same batch (see
// adsbOne.ts's own comment on the documented limit).
const TILE_GAP_MS = 1_200;

function sleep(ms: number): Promise<void> {
	return new Promise((resolve) => setTimeout(resolve, ms));
}

// Hard cap per axis, not just an overall one - without this, a whole-world
// view would compute dozens of tiles and take minutes to sequence through
// the gate. 3x3 = 9 tiles worst case, ~9 x TILE_GAP_MS (~11s) to fully
// resolve - acceptable for an infrequent zoomed-out refresh, especially
// now that the loading banner gives real feedback during the wait.
const MAX_TILES_PER_AXIS = 3;

/** Splits a viewport into a grid of tiles sized to how much bigger it
 * actually is than one tile's coverage - not a fixed 2x2 regardless of
 * size, which was confirmed live to be wrong: a whole-Europe view is still
 * several times wider than 400km even after quartering, so each "tile" was
 * itself still oversized and collapsed to a small circle at its own
 * center, not spread-out coverage. Each grid cell is its own independent
 * point+radius query (the radius itself still can't exceed the API's own
 * 250nm cap). Left as a single untiled query whenever the viewport already
 * fits within roughly one tile - the common case at city/regional zoom. */
function tileBounds(bounds: ViewportBounds): ViewportBounds[] {
	const [west, south, east, north] = bounds;
	const midLat = (south + north) / 2;
	const kmPerDegLon = KM_PER_DEG_LAT * Math.cos((midLat * Math.PI) / 180);
	const widthKm = (east - west) * kmPerDegLon;
	const heightKm = (north - south) * KM_PER_DEG_LAT;
	const tilesX = Math.min(
		MAX_TILES_PER_AXIS,
		Math.max(1, Math.ceil(widthKm / SINGLE_TILE_COVERAGE_KM))
	);
	const tilesY = Math.min(
		MAX_TILES_PER_AXIS,
		Math.max(1, Math.ceil(heightKm / SINGLE_TILE_COVERAGE_KM))
	);
	if (tilesX === 1 && tilesY === 1) return [bounds];

	const tileWidthDeg = (east - west) / tilesX;
	const tileHeightDeg = (north - south) / tilesY;
	const tiles: ViewportBounds[] = [];
	for (let row = 0; row < tilesY; row++) {
		for (let col = 0; col < tilesX; col++) {
			const tileWest = west + col * tileWidthDeg;
			const tileSouth = south + row * tileHeightDeg;
			tiles.push([tileWest, tileSouth, tileWest + tileWidthDeg, tileSouth + tileHeightDeg]);
		}
	}
	return tiles;
}

interface CacheEntry {
	aircraft: Aircraft[];
	fetchedAtMs: number;
}

function cacheKeyFor(bounds: ViewportBounds): string {
	return bounds.map((value) => Math.round(value / CACHE_GRID_DEG) * CACHE_GRID_DEG).join(',');
}

/**
 * Owns the currently loaded aircraft for whatever the map viewport last
 * was. No API key/configure() the way geozone-store etc. have - adsb.one
 * needs no signup at all (see adsbOne.ts). Not routed through
 * openaip/request-gate.ts: that gate is specifically for OpenAIP's own
 * shared rate-limited key, a completely different service with its own
 * independent limit.
 */
class AircraftStore {
	aircraft = $state<Aircraft[]>([]);
	/** icao24 -> recent [lon, lat] positions, oldest first, capped at
	 * TRAIL_MAX_POINTS. Rebuilt from scratch each fresh fetch (see
	 * buildTrails) - an aircraft that drops out of the current response
	 * loses its trail immediately, same as it disappearing from `aircraft`
	 * itself, rather than lingering as a stale breadcrumb. */
	trails = $state<SvelteMap<string, [number, number][]>>(new SvelteMap());
	loadError = $state<string | undefined>(undefined);
	/** True only for a genuine multi-tile batch (see tileBounds), which can
	 * take several real seconds. NOT set for the common single-tile
	 * refresh: confirmed live that flipping this every background tick -
	 * even ones resolving in under a second - made the loading banner
	 * blink on and off every REFRESH_INTERVAL_MS. */
	loading = $state(false);

	private source = new AdsbOneAircraftSource();
	private visible = false;
	private debounceTimer: ReturnType<typeof setTimeout> | undefined;
	private refreshTimer: ReturnType<typeof setInterval> | undefined;
	private lastRequestId = 0;
	private cooldownUntilMs = 0;
	private lastBounds: ViewportBounds | undefined;
	private lastZoom = 0;
	private retryTimer: ReturnType<typeof setTimeout> | undefined;
	private cache = new Map<string, CacheEntry>();
	/** Set once adsb.one has outright rejected a request (see
	 * AdsbOneAccessError) - short-circuits both this.refreshTimer's
	 * periodic poll and any further debounced viewport request, since a
	 * source that just said "no" isn't going to start working again on its
	 * own between now and the next tick. Cleared by toggling the layer off
	 * and back on (see setVisible), the deliberate way to try again. */
	private permanentlyFailed = false;

	setVisible(visible: boolean): void {
		this.visible = visible;
		if (!visible) {
			this.stopTimers();
			this.loadError = undefined;
			return;
		}
		this.permanentlyFailed = false;
		if (this.lastBounds) void this.fetchViewport(this.lastBounds, this.lastZoom);
		if (this.refreshTimer) clearInterval(this.refreshTimer);
		this.refreshTimer = setInterval(() => {
			if (this.permanentlyFailed) return;
			if (this.lastBounds) void this.fetchViewport(this.lastBounds, this.lastZoom);
		}, REFRESH_INTERVAL_MS);
	}

	requestViewport(bounds: ViewportBounds, zoom: number): void {
		this.lastBounds = bounds;
		this.lastZoom = zoom;
		if (!this.visible) return;
		if (Date.now() < this.cooldownUntilMs) return;
		if (this.debounceTimer) clearTimeout(this.debounceTimer);
		this.debounceTimer = setTimeout(() => void this.fetchViewport(bounds, zoom), FETCH_DEBOUNCE_MS);
	}

	private stopTimers(): void {
		if (this.debounceTimer) clearTimeout(this.debounceTimer);
		if (this.retryTimer) clearTimeout(this.retryTimer);
		if (this.refreshTimer) clearInterval(this.refreshTimer);
		this.debounceTimer = undefined;
		this.retryTimer = undefined;
		this.refreshTimer = undefined;
		this.cooldownUntilMs = 0;
	}

	private async fetchViewport(bounds: ViewportBounds, zoom: number): Promise<void> {
		if (!this.visible || this.permanentlyFailed) return;

		const cacheKey = cacheKeyFor(bounds);
		const cached = this.cache.get(cacheKey);
		if (cached && Date.now() - cached.fetchedAtMs < CACHE_TTL_MS) {
			this.aircraft = filterByZoom(cached.aircraft, zoom);
			this.loadError = undefined;
			return;
		}

		const requestId = ++this.lastRequestId;
		// Cached and trail-tracked unfiltered - the raw response doesn't
		// depend on zoom, only which of it we choose to show does (see
		// CATEGORY_MIN_ZOOM above), so re-zooming the same area should
		// reveal more of what's already been fetched, not force a refetch.
		const tiles = tileBounds(bounds);
		// See `loading`'s own comment: only a real multi-tile wait earns a
		// visible loading state.
		if (tiles.length > 1) this.loading = true;
		try {
			const byIcao24 = new SvelteMap<string, Aircraft>();
			for (let i = 0; i < tiles.length; i++) {
				if (i > 0) await sleep(TILE_GAP_MS);
				// Bail mid-sequence if a newer request has already started
				// (the operator panned again) rather than finishing a batch
				// of tiles nobody wants anymore.
				if (requestId !== this.lastRequestId) return;
				const tileAircraft = await this.source.fetchViewport(tiles[i]);
				for (const plane of tileAircraft) byIcao24.set(plane.icao24, plane);
			}
			if (requestId !== this.lastRequestId) return;
			const rawAircraft = [...byIcao24.values()];
			const visibleAircraft = filterByZoom(rawAircraft, zoom);
			this.aircraft = visibleAircraft;
			this.trails = this.buildTrails(visibleAircraft);
			this.loadError = undefined;
			this.loading = false;
			this.cache.set(cacheKey, { aircraft: rawAircraft, fetchedAtMs: Date.now() });
		} catch (error) {
			if (requestId !== this.lastRequestId) return;
			this.loadError = error instanceof Error ? error.message : String(error);
			this.loading = false;
			this.cooldownUntilMs = Date.now() + FAILURE_COOLDOWN_MS;
			console.error('aircraft: failed to load viewport', error);
			// A permanent rejection (e.g. "contact us for access"), not a
			// transient rate limit - retrying it on a timer forever would just
			// spam the console and their server for no chance of success.
			// Toggling the layer off and back on (setVisible) is still a
			// valid, deliberate way to try again.
			if (error instanceof AdsbOneAccessError) {
				this.permanentlyFailed = true;
				return;
			}
			if (this.retryTimer) clearTimeout(this.retryTimer);
			this.retryTimer = setTimeout(() => {
				if (this.lastBounds) void this.fetchViewport(this.lastBounds, this.lastZoom);
			}, FAILURE_COOLDOWN_MS);
		}
	}

	private buildTrails(aircraft: Aircraft[]): SvelteMap<string, [number, number][]> {
		const next = new SvelteMap<string, [number, number][]>();
		for (const plane of aircraft) {
			const point: [number, number] = [plane.longitudeDeg, plane.latitudeDeg];
			const existing = this.trails.get(plane.icao24) ?? [];
			const last = existing.at(-1);
			const updated =
				last && last[0] === point[0] && last[1] === point[1]
					? existing
					: [...existing, point].slice(-TRAIL_MAX_POINTS);
			next.set(plane.icao24, updated);
		}
		return next;
	}
}

export const aircraftStore = new AircraftStore();
