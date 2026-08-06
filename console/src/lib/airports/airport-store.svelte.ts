import type { Airport, AirportSource, ViewportBounds } from './types';
import { OpenAipAirportSource } from './openaip';
import { openAipRequestGate } from '../openaip/request-gate';

// Same tuning as geozones/geozone-store.svelte.ts and
// obstacles/obstacle-store.svelte.ts - all three share the same
// rate-limited OpenAIP key via openAipRequestGate.
const FETCH_DEBOUNCE_MS = 1500;
const FAILURE_COOLDOWN_MS = 10_000;
const CACHE_TTL_MS = 5 * 60_000;
const CACHE_GRID_DEG = 1;

interface CacheEntry {
	airports: Airport[];
	fetchedAtMs: number;
}

function cacheKeyFor(bounds: ViewportBounds): string {
	return bounds.map((value) => Math.round(value / CACHE_GRID_DEG) * CACHE_GRID_DEG).join(',');
}

/**
 * Owns the currently loaded airports for whatever the map viewport last
 * was. Structurally the same as obstacle-store.svelte.ts - see that
 * file's own comment on why this stays a separate store rather than a
 * shared generic base.
 */
class AirportStore {
	airports = $state<Airport[]>([]);
	loadError = $state<string | undefined>(undefined);

	private source = $state<AirportSource | undefined>(undefined);
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
		this.source = apiKey ? new OpenAipAirportSource(apiKey) : undefined;
		if (!this.source) {
			this.airports = [];
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
			this.airports = cached.airports;
			this.loadError = undefined;
			return;
		}

		const requestId = ++this.lastRequestId;
		await openAipRequestGate.enqueue(async () => {
			try {
				const airports = await source.fetchViewport(bounds);
				if (requestId !== this.lastRequestId) return;
				this.airports = airports;
				this.loadError = undefined;
				this.cache.set(cacheKey, { airports, fetchedAtMs: Date.now() });
			} catch (error) {
				if (requestId !== this.lastRequestId) return;
				this.loadError = error instanceof Error ? error.message : String(error);
				openAipRequestGate.noteFailure();
				console.error('airports: failed to load viewport', error);
				if (this.retryTimer) clearTimeout(this.retryTimer);
				this.retryTimer = setTimeout(() => {
					if (this.lastBounds) void this.fetchViewport(this.lastBounds);
				}, FAILURE_COOLDOWN_MS);
			}
		});
	}
}

export const airportStore = new AirportStore();
