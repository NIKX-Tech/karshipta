import booleanPointInPolygon from '@turf/boolean-point-in-polygon';
import { point } from '@turf/helpers';
import type { Geozone, GeozoneSource, ViewportBounds } from './types';
import { OpenAipGeozoneSource } from './openaip';
import { openAipRequestGate } from '../openaip/request-gate';

// 1500ms, not a snappier value: OpenAIP's free-tier key is rate-limited
// (confirmed directly - 4 requests inside ~6s starts returning 429s), and
// its 429 responses carry no Access-Control-Allow-Origin header, so the
// browser can't even see the 429 - fetch() just throws a generic "Failed
// to fetch" TypeError, indistinguishable from a real network failure. A
// snappier debounce (600ms, the original value) fires a fetch on nearly
// every pan/zoom step during continuous interaction, burning through that
// budget in seconds. This is this store's own moveend debounce, separate
// from - and in addition to - the cross-layer spacing openAipRequestGate
// enforces once a fetch actually gets enqueued (see that module's own
// header comment on why one alone isn't enough with obstacles/airports
// sharing the same rate-limited key).
const FETCH_DEBOUNCE_MS = 1500;
// After a failure, this store's own self-heal retry (see fetchViewport's
// catch block) waits this long before trying the last known viewport again.
// Matches openAipRequestGate's own cooldown value so the retry lands right
// as the gate becomes willing to run it, not sooner.
const FAILURE_COOLDOWN_MS = 10_000;
// Successful responses are cached this long, keyed by a coarse rounding of
// the requested bbox (see cacheKeyFor) - panning back and forth within
// roughly the same area (or two moveend events for a viewport that barely
// changed) reuses the cached result instead of hitting the network again.
// Airspace boundaries do not change minute to minute, so a short TTL costs
// nothing in staleness while meaningfully cutting request volume.
const CACHE_TTL_MS = 5 * 60_000;
// Degrees each bbox corner is rounded to before use as a cache key - coarse
// on purpose (a cache key doesn't need to distinguish two viewports that are
// obviously "the same area" for this purpose).
const CACHE_GRID_DEG = 1;

interface CacheEntry {
	zones: Geozone[];
	fetchedAtMs: number;
}

function cacheKeyFor(bounds: ViewportBounds): string {
	return bounds.map((value) => Math.round(value / CACHE_GRID_DEG) * CACHE_GRID_DEG).join(',');
}

/**
 * Owns the currently loaded airspace zones for whatever the map viewport
 * last was. Inactive (no fetches, empty zones) until configure() is given an
 * API key; the map and the confirm dialogs both read this store rather than
 * each other, so neither needs to know a geozone source exists at all.
 *
 * `visible` is this store's own concept, not just something the map reads:
 * every fetch path (requestViewport, the post-failure auto-retry) gates on
 * it directly. A real bug this fixes - the map component used to call
 * requestViewport() on every moveend regardless of its own "No-fly zones"
 * checkbox state, so unchecking it only hid already-fetched data; the
 * network requests (and this store's own retry-on-failure loop) kept firing
 * in the background indefinitely, burning through OpenAIP's rate limit even
 * while the operator had explicitly turned the layer off. Putting the gate
 * here, once, means no future call site can bypass it by forgetting to
 * check a flag living somewhere else.
 */
class GeozoneStore {
	zones = $state<Geozone[]>([]);
	loadError = $state<string | undefined>(undefined);

	// $state so the `active` getter (read from a template) reacts to configure()
	private source = $state<GeozoneSource | undefined>(undefined);
	private visible = false;
	private debounceTimer: ReturnType<typeof setTimeout> | undefined;
	private lastRequestId = 0;
	// Most recent viewport requestViewport() was given, tracked regardless
	// of visibility (a call while hidden still updates this) - both the
	// failure auto-retry below and setVisible(true) need the freshest
	// viewport available even if no fetch actually ran for it yet.
	private lastBounds: ViewportBounds | undefined;
	private retryTimer: ReturnType<typeof setTimeout> | undefined;
	private cache = new Map<string, CacheEntry>();

	get active(): boolean {
		return this.source !== undefined;
	}

	configure(apiKey: string | undefined): void {
		this.source = apiKey ? new OpenAipGeozoneSource(apiKey) : undefined;
		if (!this.source) {
			this.zones = [];
			this.loadError = undefined;
			this.cache.clear();
			this.stopTimers();
		}
	}

	/** Turns fetching on/off - the single gate every fetch path respects (see
	 * class doc comment). Turning it off cancels any pending debounce/retry
	 * and clears loadError (nothing to show an error for while hidden);
	 * turning it on immediately fetches the last known viewport instead of
	 * waiting for the next moveend, so the operator sees something right
	 * away rather than an empty layer until they next touch the map. */
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

	/** debounced viewport fetch; safe to call on every map moveend regardless
	 * of visibility - a no-op while hidden or inactive. Post-failure backoff
	 * is openAipRequestGate's job now (shared across every OpenAIP layer,
	 * not just this one) - this always arms the debounce and lets the gate
	 * decide when the resulting fetch is actually allowed to run. Caller is
	 * expected to have already decided the viewport is worth fetching at
	 * all (e.g. not zoomed out past whatever floor makes airspace data
	 * meaningful) - this store has no opinion on zoom level, only on bbox
	 * size (see openaip.ts's own 5-degree clamp). */
	requestViewport(bounds: ViewportBounds): void {
		if (!this.source) return;
		// Tracked even while hidden, deliberately before the visibility
		// check below: setVisible(true) needs the freshest viewport to fetch
		// immediately, not whatever was last seen while the layer happened
		// to be visible.
		this.lastBounds = bounds;
		if (!this.visible) return;
		if (this.debounceTimer) clearTimeout(this.debounceTimer);
		this.debounceTimer = setTimeout(() => void this.fetchViewport(bounds), FETCH_DEBOUNCE_MS);
	}

	/** zones (if any) containing a point; always empty while inactive */
	zonesContaining(latitudeDeg: number, longitudeDeg: number): Geozone[] {
		if (this.zones.length === 0) return [];
		const target = point([longitudeDeg, latitudeDeg]);
		return this.zones.filter((zone) => booleanPointInPolygon(target, zone.polygon));
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
			this.zones = cached.zones;
			this.loadError = undefined;
			return;
		}

		const requestId = ++this.lastRequestId;
		// Enqueued, not called directly: openAipRequestGate is the one place
		// that decides when this is actually safe to run relative to every
		// other OpenAIP layer's own requests (see that module's own header
		// comment).
		await openAipRequestGate.enqueue(async () => {
			try {
				const zones = await source.fetchViewport(bounds);
				if (requestId !== this.lastRequestId) return; // superseded by a newer viewport
				this.zones = zones;
				this.loadError = undefined;
				this.cache.set(cacheKey, { zones, fetchedAtMs: Date.now() });
			} catch (error) {
				if (requestId !== this.lastRequestId) return;
				this.loadError = error instanceof Error ? error.message : String(error);
				openAipRequestGate.noteFailure();
				console.error('geozones: failed to load viewport', error);
				// Self-heal: retry the latest known viewport once the cooldown
				// lifts, instead of only ever retrying on the next moveend - an
				// operator who stops touching the map right after a failure would
				// otherwise see loadError sit there indefinitely. Still gated on
				// `visible` inside fetchViewport() itself, so turning the layer
				// off cancels this via stopTimers() and it will not silently
				// fire again in the background.
				if (this.retryTimer) clearTimeout(this.retryTimer);
				this.retryTimer = setTimeout(() => {
					if (this.lastBounds) void this.fetchViewport(this.lastBounds);
				}, FAILURE_COOLDOWN_MS);
			}
		});
	}
}

export const geozoneStore = new GeozoneStore();
