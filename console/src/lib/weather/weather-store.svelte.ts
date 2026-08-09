import type { WeatherConditions } from './types';
import { OpenMeteoWeatherSource } from './open-meteo';

// Open-Meteo has no documented tight rate limit for reasonable/non-commercial
// use (confirmed live, no key needed at all) - a normal debounce is fine,
// same as the OpenAIP layers.
const FETCH_DEBOUNCE_MS = 1500;
const FAILURE_COOLDOWN_MS = 10_000;
// Weather doesn't meaningfully change minute to minute, so a longer cache
// than the point-feature layers is fine, and coarser rounding is fine too -
// nobody needs a different forecast for a 200m pan.
const CACHE_TTL_MS = 10 * 60_000;
const CACHE_GRID_DEG = 0.5;

interface CacheEntry {
	conditions: WeatherConditions;
	fetchedAtMs: number;
}

function cacheKeyFor(lat: number, lon: number): string {
	const round = (value: number) => Math.round(value / CACHE_GRID_DEG) * CACHE_GRID_DEG;
	return `${round(lat)},${round(lon)}`;
}

/**
 * Owns current conditions for wherever the map center last was. Not a
 * viewport/bbox layer like geozones/obstacles/aircraft/etc. - weather is a
 * continuous field, not discrete features, so this tracks one point (the
 * map center) rather than a list. No API key/configure(): Open-Meteo needs
 * none.
 */
class WeatherStore {
	conditions = $state<WeatherConditions | undefined>(undefined);
	loadError = $state<string | undefined>(undefined);

	private source = new OpenMeteoWeatherSource();
	private visible = false;
	private debounceTimer: ReturnType<typeof setTimeout> | undefined;
	private lastRequestId = 0;
	private cooldownUntilMs = 0;
	private lastLat: number | undefined;
	private lastLon: number | undefined;
	private retryTimer: ReturnType<typeof setTimeout> | undefined;
	private cache = new Map<string, CacheEntry>();

	setVisible(visible: boolean): void {
		this.visible = visible;
		if (!visible) {
			this.stopTimers();
			this.loadError = undefined;
			return;
		}
		if (this.lastLat !== undefined && this.lastLon !== undefined) {
			void this.fetchConditions(this.lastLat, this.lastLon);
		}
	}

	requestLocation(latitudeDeg: number, longitudeDeg: number): void {
		this.lastLat = latitudeDeg;
		this.lastLon = longitudeDeg;
		if (!this.visible) return;
		if (Date.now() < this.cooldownUntilMs) return;
		if (this.debounceTimer) clearTimeout(this.debounceTimer);
		this.debounceTimer = setTimeout(
			() => void this.fetchConditions(latitudeDeg, longitudeDeg),
			FETCH_DEBOUNCE_MS
		);
	}

	private stopTimers(): void {
		if (this.debounceTimer) clearTimeout(this.debounceTimer);
		if (this.retryTimer) clearTimeout(this.retryTimer);
		this.debounceTimer = undefined;
		this.retryTimer = undefined;
		this.cooldownUntilMs = 0;
	}

	private async fetchConditions(latitudeDeg: number, longitudeDeg: number): Promise<void> {
		if (!this.visible) return;

		const cacheKey = cacheKeyFor(latitudeDeg, longitudeDeg);
		const cached = this.cache.get(cacheKey);
		if (cached && Date.now() - cached.fetchedAtMs < CACHE_TTL_MS) {
			this.conditions = cached.conditions;
			this.loadError = undefined;
			return;
		}

		const requestId = ++this.lastRequestId;
		try {
			const conditions = await this.source.fetchConditions(latitudeDeg, longitudeDeg);
			if (requestId !== this.lastRequestId) return;
			this.conditions = conditions;
			this.loadError = undefined;
			this.cache.set(cacheKey, { conditions, fetchedAtMs: Date.now() });
		} catch (error) {
			if (requestId !== this.lastRequestId) return;
			this.loadError = error instanceof Error ? error.message : String(error);
			this.cooldownUntilMs = Date.now() + FAILURE_COOLDOWN_MS;
			console.error('weather: failed to load conditions', error);
			if (this.retryTimer) clearTimeout(this.retryTimer);
			this.retryTimer = setTimeout(() => {
				if (this.lastLat !== undefined && this.lastLon !== undefined) {
					void this.fetchConditions(this.lastLat, this.lastLon);
				}
			}, FAILURE_COOLDOWN_MS);
		}
	}
}

export const weatherStore = new WeatherStore();
