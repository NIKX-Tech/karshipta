import type { Airport, ViewportBounds } from './types';
import { AirportProxyAccessError, ProxiedAirportSource } from './proxiedSource';
import { openAipRequestGate } from '../openaip/request-gate';
import { tileOpenAipBounds } from '../openaip/tile-bounds';

// Same tuning as geozones/geozone-store.svelte.ts and
// obstacles/obstacle-store.svelte.ts - all three ultimately draw against
// the same rate-limited OpenAIP account, coordinated via openAipRequestGate.
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
 * was. No API key/configure() the way geozone-store has - the proxy route
 * behind this needs no key client-side (see proxiedSource.ts). Structurally
 * the same as obstacle-store.svelte.ts otherwise - see that file's own
 * comment on why this stays a separate store rather than a shared generic
 * base.
 */
class AirportStore {
	airports = $state<Airport[]>([]);
	loadError = $state<string | undefined>(undefined);

	private source = new ProxiedAirportSource();
	private visible = false;
	private debounceTimer: ReturnType<typeof setTimeout> | undefined;
	private lastRequestId = 0;
	private lastBounds: ViewportBounds | undefined;
	private retryTimer: ReturnType<typeof setTimeout> | undefined;
	private cache = new Map<string, CacheEntry>();
	/** Set once the proxy route has outright rejected a request (see
	 * AirportProxyAccessError) - short-circuits further retries, since a
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
			void this.fetchViewport(this.lastBounds);
		}
	}

	requestViewport(bounds: ViewportBounds): void {
		this.lastBounds = bounds;
		if (!this.visible || this.permanentlyFailed) return;
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
		if (!this.visible || this.permanentlyFailed) return;

		const cacheKey = cacheKeyFor(bounds);
		const cached = this.cache.get(cacheKey);
		if (cached && Date.now() - cached.fetchedAtMs < CACHE_TTL_MS) {
			this.airports = cached.airports;
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
		const tileResults: Airport[][] = [];
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
			console.error('airports: failed to load viewport', tileError);
			// A permanent rejection (e.g. the server has no OPENAIP_KEY
			// configured), not a transient rate limit - retrying it on a
			// timer forever would just spam the console for no chance of
			// success. Toggling the layer off and back on (setVisible) is
			// still a valid, deliberate way to try again.
			if (tileError instanceof AirportProxyAccessError) {
				this.permanentlyFailed = true;
				return;
			}
			if (this.retryTimer) clearTimeout(this.retryTimer);
			this.retryTimer = setTimeout(() => {
				if (this.lastBounds) void this.fetchViewport(this.lastBounds);
			}, FAILURE_COOLDOWN_MS);
			return;
		}
		// Deduped by id: adjacent tiles can both legitimately return an
		// airport that straddles the split line.
		const airports = [...new Map(tileResults.flat().map((a) => [a.id, a])).values()];
		this.airports = airports;
		this.loadError = undefined;
		this.cache.set(cacheKey, { airports, fetchedAtMs: Date.now() });
	}
}

export const airportStore = new AirportStore();
