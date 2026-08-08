import type { Earthquake, ViewportBounds } from './types';
import { UsgsEarthquakeSource } from './usgs';

// USGS explicitly designs this feed for public/programmatic use with no
// documented tight limit the way OpenSky's anonymous tier or OpenAIP's key
// have - a normal debounce is fine. Cache TTL is longer than the OpenAIP
// layers' 5 minutes since earthquake activity in a 30-day lookback window
// (see usgs.ts) doesn't meaningfully change minute to minute.
const FETCH_DEBOUNCE_MS = 1500;
const FAILURE_COOLDOWN_MS = 10_000;
const CACHE_TTL_MS = 10 * 60_000;
const CACHE_GRID_DEG = 1;

interface CacheEntry {
	earthquakes: Earthquake[];
	fetchedAtMs: number;
}

function cacheKeyFor(bounds: ViewportBounds): string {
	return bounds.map((value) => Math.round(value / CACHE_GRID_DEG) * CACHE_GRID_DEG).join(',');
}

/**
 * Owns the currently loaded earthquakes for whatever the map viewport last
 * was. No API key/configure() - USGS's feed is fully open - but still off
 * by default (see fleet-map.svelte's own showEarthquakes), same trust-
 * level reasoning as every other third-party layer here.
 */
class EarthquakeStore {
	earthquakes = $state<Earthquake[]>([]);
	loadError = $state<string | undefined>(undefined);

	private source = new UsgsEarthquakeSource();
	private visible = false;
	private debounceTimer: ReturnType<typeof setTimeout> | undefined;
	private lastRequestId = 0;
	private cooldownUntilMs = 0;
	private lastBounds: ViewportBounds | undefined;
	private retryTimer: ReturnType<typeof setTimeout> | undefined;
	private cache = new Map<string, CacheEntry>();

	setVisible(visible: boolean): void {
		this.visible = visible;
		if (!visible) {
			this.stopTimers();
			this.loadError = undefined;
			return;
		}
		if (this.lastBounds) void this.fetchViewport(this.lastBounds);
	}

	requestViewport(bounds: ViewportBounds): void {
		this.lastBounds = bounds;
		if (!this.visible) return;
		if (Date.now() < this.cooldownUntilMs) return;
		if (this.debounceTimer) clearTimeout(this.debounceTimer);
		this.debounceTimer = setTimeout(() => void this.fetchViewport(bounds), FETCH_DEBOUNCE_MS);
	}

	private stopTimers(): void {
		if (this.debounceTimer) clearTimeout(this.debounceTimer);
		if (this.retryTimer) clearTimeout(this.retryTimer);
		this.debounceTimer = undefined;
		this.retryTimer = undefined;
		this.cooldownUntilMs = 0;
	}

	private async fetchViewport(bounds: ViewportBounds): Promise<void> {
		if (!this.visible) return;

		const cacheKey = cacheKeyFor(bounds);
		const cached = this.cache.get(cacheKey);
		if (cached && Date.now() - cached.fetchedAtMs < CACHE_TTL_MS) {
			this.earthquakes = cached.earthquakes;
			this.loadError = undefined;
			return;
		}

		const requestId = ++this.lastRequestId;
		try {
			const earthquakes = await this.source.fetchViewport(bounds);
			if (requestId !== this.lastRequestId) return;
			this.earthquakes = earthquakes;
			this.loadError = undefined;
			this.cache.set(cacheKey, { earthquakes, fetchedAtMs: Date.now() });
		} catch (error) {
			if (requestId !== this.lastRequestId) return;
			this.loadError = error instanceof Error ? error.message : String(error);
			this.cooldownUntilMs = Date.now() + FAILURE_COOLDOWN_MS;
			console.error('earthquakes: failed to load viewport', error);
			if (this.retryTimer) clearTimeout(this.retryTimer);
			this.retryTimer = setTimeout(() => {
				if (this.lastBounds) void this.fetchViewport(this.lastBounds);
			}, FAILURE_COOLDOWN_MS);
		}
	}
}

export const earthquakeStore = new EarthquakeStore();
