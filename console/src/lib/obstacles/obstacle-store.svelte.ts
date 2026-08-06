import type { Obstacle, ObstacleSource, ViewportBounds } from './types';
import { OpenAipObstacleSource } from './openaip';
import { openAipRequestGate } from '../openaip/request-gate';

// Same tuning as geozones/geozone-store.svelte.ts - both share the same
// rate-limited OpenAIP key via openAipRequestGate, see that module's own
// header comment for why a per-layer debounce alone isn't enough.
const FETCH_DEBOUNCE_MS = 1500;
const FAILURE_COOLDOWN_MS = 10_000;
const CACHE_TTL_MS = 5 * 60_000;
const CACHE_GRID_DEG = 1;

interface CacheEntry {
	obstacles: Obstacle[];
	fetchedAtMs: number;
}

function cacheKeyFor(bounds: ViewportBounds): string {
	return bounds.map((value) => Math.round(value / CACHE_GRID_DEG) * CACHE_GRID_DEG).join(',');
}

/**
 * Owns the currently loaded obstacles for whatever the map viewport last
 * was. Structurally the same as geozone-store.svelte.ts (inactive until
 * configure() has a key, visible gates every fetch path, requests route
 * through the shared openAipRequestGate) - kept as its own store rather
 * than generalized into one shared "OpenAIP point layer" base, matching
 * this codebase's existing preference for small independent stores over a
 * shared abstraction (see geozones/types.ts's own comment on GeozoneSource
 * being swappable without touching the map).
 */
class ObstacleStore {
	obstacles = $state<Obstacle[]>([]);
	loadError = $state<string | undefined>(undefined);

	private source = $state<ObstacleSource | undefined>(undefined);
	private visible = false;
	private debounceTimer: ReturnType<typeof setTimeout> | undefined;
	private lastRequestId = 0;
	private lastBounds: ViewportBounds | undefined;
	private retryTimer: ReturnType<typeof setTimeout> | undefined;
	private cache = new Map<string, CacheEntry>();

	get active(): boolean {
		return this.source !== undefined;
	}

	configure(apiKey: string | undefined): void {
		this.source = apiKey ? new OpenAipObstacleSource(apiKey) : undefined;
		if (!this.source) {
			this.obstacles = [];
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
		if (this.source && this.lastBounds) {
			void this.fetchViewport(this.lastBounds);
		}
	}

	requestViewport(bounds: ViewportBounds): void {
		if (!this.source) return;
		this.lastBounds = bounds;
		if (!this.visible) return;
		if (this.debounceTimer) clearTimeout(this.debounceTimer);
		this.debounceTimer = setTimeout(() => void this.fetchViewport(bounds), FETCH_DEBOUNCE_MS);
	}

	private stopTimers(): void {
		if (this.debounceTimer) clearTimeout(this.debounceTimer);
		if (this.retryTimer) clearTimeout(this.retryTimer);
		this.debounceTimer = undefined;
		this.retryTimer = undefined;
	}

	private async fetchViewport(bounds: ViewportBounds): Promise<void> {
		const source = this.source;
		if (!source || !this.visible) return;

		const cacheKey = cacheKeyFor(bounds);
		const cached = this.cache.get(cacheKey);
		if (cached && Date.now() - cached.fetchedAtMs < CACHE_TTL_MS) {
			this.obstacles = cached.obstacles;
			this.loadError = undefined;
			return;
		}

		const requestId = ++this.lastRequestId;
		await openAipRequestGate.enqueue(async () => {
			try {
				const obstacles = await source.fetchViewport(bounds);
				if (requestId !== this.lastRequestId) return;
				this.obstacles = obstacles;
				this.loadError = undefined;
				this.cache.set(cacheKey, { obstacles, fetchedAtMs: Date.now() });
			} catch (error) {
				if (requestId !== this.lastRequestId) return;
				this.loadError = error instanceof Error ? error.message : String(error);
				openAipRequestGate.noteFailure();
				console.error('obstacles: failed to load viewport', error);
				if (this.retryTimer) clearTimeout(this.retryTimer);
				this.retryTimer = setTimeout(() => {
					if (this.lastBounds) void this.fetchViewport(this.lastBounds);
				}, FAILURE_COOLDOWN_MS);
			}
		});
	}
}

export const obstacleStore = new ObstacleStore();
