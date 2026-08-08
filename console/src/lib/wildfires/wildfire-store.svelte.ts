import type { FireHotspot, FireHotspotSource, ViewportBounds } from './types';
import { FirmsFireHotspotSource } from './firms';

// FIRMS' own documented limit (5000 transactions/10 minutes) is generous -
// similar debounce/cache to the OpenAIP layers is fine. Not routed through
// openaip/request-gate.ts: a completely different service and key with its
// own independent budget, not sharing OpenAIP's.
const FETCH_DEBOUNCE_MS = 1500;
const FAILURE_COOLDOWN_MS = 10_000;
const CACHE_TTL_MS = 5 * 60_000;
const CACHE_GRID_DEG = 1;

interface CacheEntry {
	hotspots: FireHotspot[];
	fetchedAtMs: number;
}

function cacheKeyFor(bounds: ViewportBounds): string {
	return bounds.map((value) => Math.round(value / CACHE_GRID_DEG) * CACHE_GRID_DEG).join(',');
}

/**
 * Owns the currently loaded wildfire hotspots for whatever the map
 * viewport last was. Same configure(apiKey)/active shape as
 * geozone-store.svelte.ts - inactive until a FIRMS MAP_KEY is supplied.
 */
class WildfireStore {
	hotspots = $state<FireHotspot[]>([]);
	loadError = $state<string | undefined>(undefined);

	private source = $state<FireHotspotSource | undefined>(undefined);
	private visible = false;
	private debounceTimer: ReturnType<typeof setTimeout> | undefined;
	private lastRequestId = 0;
	private cooldownUntilMs = 0;
	private lastBounds: ViewportBounds | undefined;
	private retryTimer: ReturnType<typeof setTimeout> | undefined;
	private cache = new Map<string, CacheEntry>();

	get active(): boolean {
		return this.source !== undefined;
	}

	configure(apiKey: string | undefined): void {
		this.source = apiKey ? new FirmsFireHotspotSource(apiKey) : undefined;
		if (!this.source) {
			this.hotspots = [];
			this.loadError = undefined;
			this.cache.clear();
			this.stopTimers();
		}
	}

	setVisible(visible: boolean): void {
		this.visible = visible;
		if (!visible) {
			this.stopTimers();
			this.loadError = undefined;
			return;
		}
		if (this.source && this.lastBounds) void this.fetchViewport(this.lastBounds);
	}

	requestViewport(bounds: ViewportBounds): void {
		if (!this.source) return;
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
		const source = this.source;
		if (!source || !this.visible) return;

		const cacheKey = cacheKeyFor(bounds);
		const cached = this.cache.get(cacheKey);
		if (cached && Date.now() - cached.fetchedAtMs < CACHE_TTL_MS) {
			this.hotspots = cached.hotspots;
			this.loadError = undefined;
			return;
		}

		const requestId = ++this.lastRequestId;
		try {
			const hotspots = await source.fetchViewport(bounds);
			if (requestId !== this.lastRequestId) return;
			this.hotspots = hotspots;
			this.loadError = undefined;
			this.cache.set(cacheKey, { hotspots, fetchedAtMs: Date.now() });
		} catch (error) {
			if (requestId !== this.lastRequestId) return;
			this.loadError = error instanceof Error ? error.message : String(error);
			this.cooldownUntilMs = Date.now() + FAILURE_COOLDOWN_MS;
			console.error('wildfires: failed to load viewport', error);
			if (this.retryTimer) clearTimeout(this.retryTimer);
			this.retryTimer = setTimeout(() => {
				if (this.lastBounds) void this.fetchViewport(this.lastBounds);
			}, FAILURE_COOLDOWN_MS);
		}
	}
}

export const wildfireStore = new WildfireStore();
