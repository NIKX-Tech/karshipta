import { SvelteMap } from 'svelte/reactivity';
import type { Aircraft, AircraftCategory, ViewportBounds } from './types';
import { AirplanesLiveAircraftSource } from './airplaneslive';

// airplanes.live documents a 1 request/second limit (see
// airplaneslive.ts's own comment) - comfortably generous compared to
// OpenSky's few-hundred-per-day anonymous tier this replaced, so this can
// debounce far more snappily while staying well under it.
const FETCH_DEBOUNCE_MS = 3_000;
const FAILURE_COOLDOWN_MS = 10_000;
const CACHE_TTL_MS = 15_000;
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
// zoomed out the map is (see airplaneslive.ts) - there's no way to make
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

// Conservative vs airplanes.live's actual ~463km (250nm) diameter cap -
// deliberately smaller so adjacent tiles overlap a bit rather than
// leaving a gap between them.
const SINGLE_TILE_COVERAGE_KM = 400;
const KM_PER_DEG_LAT = 111;
// Comfortably over 1 request/second between tiles of the same batch (see
// airplaneslive.ts's own comment on the documented limit).
const TILE_GAP_MS = 1_200;

function sleep(ms: number): Promise<void> {
	return new Promise((resolve) => setTimeout(resolve, ms));
}

/** Splits a viewport into a 2x2 grid of quadrants once it's wide enough
 * that a single capped-radius query centered on it would only ever
 * reach a small fraction of what's visible - confirmed live: zoomed out
 * to see most of Europe, one query returned a single dense cluster of
 * ~200 aircraft in the middle and nothing else, because one circle can
 * only ever cover a ~460km slice of a view spanning thousands of km.
 * Each quadrant becomes its own independent point+radius query (see
 * fetchViewport below and airplaneslive.ts), so coverage actually
 * spreads across the visible map instead of stacking on the center -
 * the radius itself still can't exceed the API's own cap, but the
 * *view* effectively can. Left as a single untiled query (today's
 * behavior, unchanged) whenever the viewport already fits within
 * roughly one tile - the overwhelmingly common case at city/regional
 * zoom. */
function tileBounds(bounds: ViewportBounds): ViewportBounds[] {
	const [west, south, east, north] = bounds;
	const midLat = (south + north) / 2;
	const kmPerDegLon = KM_PER_DEG_LAT * Math.cos((midLat * Math.PI) / 180);
	const widthKm = (east - west) * kmPerDegLon;
	const heightKm = (north - south) * KM_PER_DEG_LAT;
	if (widthKm <= SINGLE_TILE_COVERAGE_KM && heightKm <= SINGLE_TILE_COVERAGE_KM) {
		return [bounds];
	}
	const midLon = (west + east) / 2;
	return [
		[west, midLat, midLon, north], // NW
		[midLon, midLat, east, north], // NE
		[west, south, midLon, midLat], // SW
		[midLon, south, east, midLat] // SE
	];
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
 * was. No API key/configure() the way geozone-store etc. have -
 * airplanes.live needs no signup at all (see airplaneslive.ts) - but
 * still off by default (see fleet-map.svelte's own showAircraft) since
 * it's live third-party traffic data, the same trust level reasoning as
 * the OpenAIP layers. Not routed through openaip/request-gate.ts: that
 * gate is specifically for OpenAIP's own shared rate-limited key, a
 * completely different service with its own independent limit.
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
	/** True for the whole span of an in-flight fetchViewport call,
	 * including every tile in a multi-tile batch (see tileBounds) - unlike
	 * loadError, which is error-only. A wide zoomed-out view can now take
	 * several real seconds (sequential tiles, ~1.2s apart to respect the
	 * rate limit), and that wait had no visible feedback at all before
	 * this existed: loadError never fires on a normal successful fetch,
	 * only a failed one, so "still working on it" and "nothing's
	 * happening" looked identical. */
	loading = $state(false);

	private source = new AirplanesLiveAircraftSource();
	private visible = false;
	private debounceTimer: ReturnType<typeof setTimeout> | undefined;
	private refreshTimer: ReturnType<typeof setInterval> | undefined;
	private lastRequestId = 0;
	private cooldownUntilMs = 0;
	private lastBounds: ViewportBounds | undefined;
	private lastZoom = 0;
	private retryTimer: ReturnType<typeof setTimeout> | undefined;
	private cache = new Map<string, CacheEntry>();

	setVisible(visible: boolean): void {
		this.visible = visible;
		if (!visible) {
			this.stopTimers();
			this.loadError = undefined;
			return;
		}
		if (this.lastBounds) void this.fetchViewport(this.lastBounds, this.lastZoom);
		if (this.refreshTimer) clearInterval(this.refreshTimer);
		this.refreshTimer = setInterval(() => {
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
		if (!this.visible) return;

		const cacheKey = cacheKeyFor(bounds);
		const cached = this.cache.get(cacheKey);
		if (cached && Date.now() - cached.fetchedAtMs < CACHE_TTL_MS) {
			this.aircraft = filterByZoom(cached.aircraft, zoom);
			this.loadError = undefined;
			return;
		}

		const requestId = ++this.lastRequestId;
		this.loading = true;
		try {
			// Cached and trail-tracked unfiltered - the raw response doesn't
			// depend on zoom, only which of it we choose to show does (see
			// CATEGORY_MIN_ZOOM above), so re-zooming the same area should
			// reveal more of what's already been fetched, not force a refetch.
			const tiles = tileBounds(bounds);
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
