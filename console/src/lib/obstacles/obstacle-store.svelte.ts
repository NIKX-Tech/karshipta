import type { Obstacle, ObstacleCategory, ViewportBounds } from './types';
import { ObstacleProxyAccessError, ProxiedObstacleSource } from './proxiedSource';
import { openAipRequestGate } from '../openaip/request-gate';
import { tileOpenAipBounds } from '../openaip/tile-bounds';

// Same tuning as geozones/geozone-store.svelte.ts - both ultimately draw
// against the same rate-limited OpenAIP account, coordinated via
// openAipRequestGate; see that module's own header comment for why a
// per-layer debounce alone isn't enough.
const FETCH_DEBOUNCE_MS = 1500;
const FAILURE_COOLDOWN_MS = 10_000;
const CACHE_TTL_MS = 5 * 60_000;
const CACHE_GRID_DEG = 1;

// This layer defaults on (see fleet-map.svelte), so unlike the OpenAIP
// layers that stay opt-in, it needs its own zoom thinning - a zoomed-out
// city view could otherwise return the full RESULT_LIMIT (200, see
// openaipProxy.ts) with nothing filtering it. Same pattern as
// city-store.svelte.ts's population thinning: taller, more-hazardous types
// (towers, wind turbines) show first, the catch-all 'other' bucket only
// once zoomed in. 'mast' isn't currently produced by openaipProxy.ts's own
// categoryFor, included here for type completeness only.
const OBSTACLE_MIN_ZOOM: Record<ObstacleCategory, number> = {
	'wind-turbine': 0,
	tower: 0,
	mast: 0,
	other: 6
};

function filterByZoom(obstacles: Obstacle[], zoom: number): Obstacle[] {
	return obstacles.filter((obstacle) => zoom >= OBSTACLE_MIN_ZOOM[obstacle.category]);
}

interface CacheEntry {
	obstacles: Obstacle[];
	fetchedAtMs: number;
}

function cacheKeyFor(bounds: ViewportBounds): string {
	return bounds.map((value) => Math.round(value / CACHE_GRID_DEG) * CACHE_GRID_DEG).join(',');
}

/**
 * Owns the currently loaded obstacles for whatever the map viewport last
 * was. No API key/configure() the way geozone-store etc. have - the proxy
 * route behind this needs no key client-side (see proxiedSource.ts).
 * Still routed through the shared openAipRequestGate: the key moved
 * server-side, but bursts of tile requests still benefit from client-side
 * pacing ahead of whatever the server enforces.
 */
class ObstacleStore {
	obstacles = $state<Obstacle[]>([]);
	loadError = $state<string | undefined>(undefined);

	private source = new ProxiedObstacleSource();
	private visible = false;
	private debounceTimer: ReturnType<typeof setTimeout> | undefined;
	private lastRequestId = 0;
	private lastBounds: ViewportBounds | undefined;
	private lastZoom = 0;
	private retryTimer: ReturnType<typeof setTimeout> | undefined;
	private cache = new Map<string, CacheEntry>();
	/** Set once the proxy route has outright rejected a request (see
	 * ObstacleProxyAccessError) - short-circuits further retries, since a
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
		if (this.lastBounds) {
			void this.fetchViewport(this.lastBounds, this.lastZoom);
		}
	}

	requestViewport(bounds: ViewportBounds, zoom: number): void {
		this.lastBounds = bounds;
		this.lastZoom = zoom;
		if (!this.visible || this.permanentlyFailed) return;
		if (this.debounceTimer) clearTimeout(this.debounceTimer);
		this.debounceTimer = setTimeout(() => void this.fetchViewport(bounds, zoom), FETCH_DEBOUNCE_MS);
	}

	private stopTimers(): void {
		if (this.debounceTimer) clearTimeout(this.debounceTimer);
		if (this.retryTimer) clearTimeout(this.retryTimer);
		this.debounceTimer = undefined;
		this.retryTimer = undefined;
	}

	private async fetchViewport(bounds: ViewportBounds, zoom: number): Promise<void> {
		const source = this.source;
		if (!this.visible || this.permanentlyFailed) return;

		const cacheKey = cacheKeyFor(bounds);
		const cached = this.cache.get(cacheKey);
		if (cached && Date.now() - cached.fetchedAtMs < CACHE_TTL_MS) {
			this.obstacles = filterByZoom(cached.obstacles, zoom);
			this.loadError = undefined;
			return;
		}

		const requestId = ++this.lastRequestId;
		// Split into up to 2 side-by-side queries when the viewport is wide
		// enough that one 5-degree-clamped window would only cover a small,
		// misleadingly dense fraction of what's visible - see
		// openaip/tile-bounds.ts and geozone-store.svelte.ts's own identical
		// comment on this.
		const tiles = tileOpenAipBounds(bounds);
		const tileResults: Obstacle[][] = [];
		let tileError: unknown;
		await Promise.all(
			tiles.map((tile, index) =>
				openAipRequestGate.enqueue(async () => {
					try {
						tileResults[index] = await source.fetchViewport(tile);
					} catch (error) {
						tileError = error;
					}
				})
			)
		);
		if (requestId !== this.lastRequestId) return;
		if (tileError !== undefined) {
			this.loadError = tileError instanceof Error ? tileError.message : String(tileError);
			openAipRequestGate.noteFailure();
			console.error('obstacles: failed to load viewport', tileError);
			// A permanent rejection (e.g. the server has no OPENAIP_KEY
			// configured), not a transient rate limit - retrying it on a
			// timer forever would just spam the console for no chance of
			// success. Toggling the layer off and back on (setVisible) is
			// still a valid, deliberate way to try again.
			if (tileError instanceof ObstacleProxyAccessError) {
				this.permanentlyFailed = true;
				return;
			}
			if (this.retryTimer) clearTimeout(this.retryTimer);
			this.retryTimer = setTimeout(() => {
				if (this.lastBounds) void this.fetchViewport(this.lastBounds, this.lastZoom);
			}, FAILURE_COOLDOWN_MS);
			return;
		}
		// Deduped by id: adjacent tiles can both legitimately return an
		// obstacle that straddles the split line. Cached unfiltered (the full
		// fetched set) so re-zooming the same area doesn't need a fresh
		// request just to reveal more of what's already been fetched - only
		// which of it gets displayed depends on zoom, same as
		// aircraft-store.svelte.ts's identical cache/filter split.
		const obstacles = [...new Map(tileResults.flat().map((o) => [o.id, o])).values()];
		this.obstacles = filterByZoom(obstacles, zoom);
		this.loadError = undefined;
		this.cache.set(cacheKey, { obstacles, fetchedAtMs: Date.now() });
	}
}

export const obstacleStore = new ObstacleStore();
