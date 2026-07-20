/**
 * Shared by both the initial map center (see routes/+page.svelte) and
 * demo-vehicle placement: both just need "a point, with a fallback" and
 * don't share any state, so this is a plain function, not a store.
 */
export interface LatLon {
	lat: number;
	lon: number;
}

const GEOLOCATION_TIMEOUT_MS = 8000;

/**
 * Resolves the browser's current position, falling back to `fallback` on
 * denial, timeout, error, or when geolocation isn't available at all (SSR,
 * unsupported browser, permissions policy). Never rejects - callers never
 * need a catch, just an await/then.
 */
export function locateOrFallback(fallback: LatLon): Promise<LatLon> {
	return new Promise((resolve) => {
		if (typeof navigator === 'undefined' || !('geolocation' in navigator)) {
			resolve(fallback);
			return;
		}
		navigator.geolocation.getCurrentPosition(
			(position) => resolve({ lat: position.coords.latitude, lon: position.coords.longitude }),
			() => resolve(fallback),
			{ timeout: GEOLOCATION_TIMEOUT_MS }
		);
	});
}
