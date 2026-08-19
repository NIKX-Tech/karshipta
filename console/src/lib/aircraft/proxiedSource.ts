import type { Aircraft, AircraftSource, ViewportBounds } from './types';

const MAX_RADIUS_NM = 250;
const MIN_RADIUS_NM = 5;
const KM_PER_NM = 1.852;
const EARTH_RADIUS_KM = 6371;

function boundsToPointRadius(bounds: ViewportBounds): {
	latitudeDeg: number;
	longitudeDeg: number;
	radiusNm: number;
} {
	const [west, south, east, north] = bounds;
	const latitudeDeg = (south + north) / 2;
	const longitudeDeg = (west + east) / 2;
	// Half-diagonal (center to a corner) via haversine, clamped to the
	// API's min/max - close enough for "aircraft roughly in view", not a
	// precision requirement the way a bbox query would be.
	const toRad = (deg: number) => (deg * Math.PI) / 180;
	const dLat = toRad(north - latitudeDeg);
	const dLon = toRad(east - longitudeDeg);
	const a =
		Math.sin(dLat / 2) ** 2 +
		Math.cos(toRad(latitudeDeg)) * Math.cos(toRad(north)) * Math.sin(dLon / 2) ** 2;
	const halfDiagonalKm = EARTH_RADIUS_KM * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
	const radiusNm = Math.min(MAX_RADIUS_NM, Math.max(MIN_RADIUS_NM, halfDiagonalKm / KM_PER_NM));
	return { latitudeDeg, longitudeDeg, radiusNm };
}

/** Thrown for a 4xx other than 429: the proxy route rejecting every
 * request outright (a misconfigured or missing route, not a transient
 * rate limit) - retrying that on a timer every few seconds forever is
 * pointless. aircraft-store.svelte.ts checks for this specifically to
 * stop its own retry loop. */
export class AircraftProxyAccessError extends Error {}

/** Calls this app's own same-origin aircraft proxy route rather than a
 * third-party URL directly - the actual source behind it (adsb.lol, see
 * adsbLolProxy.ts) sends no CORS headers at all, so a direct browser fetch
 * can never work regardless of which specific third-party ends up behind
 * the proxy. A consuming app that never wires up
 * /api/aircraft/point/[lat]/[lon]/[radius] just gets a 404 here, which
 * fails the same clean way any other network error does - the aircraft
 * layer degrades gracefully, it doesn't crash the console. Point+radius
 * only (no native bbox route), so the viewport gets reduced to its center
 * and half-diagonal above, same shape the proxy's own upstream expects. */
export class ProxiedAircraftSource implements AircraftSource {
	async fetchViewport(bounds: ViewportBounds): Promise<Aircraft[]> {
		const { latitudeDeg, longitudeDeg, radiusNm } = boundsToPointRadius(bounds);
		const url = `/api/aircraft/point/${latitudeDeg}/${longitudeDeg}/${radiusNm.toFixed(0)}`;
		const response = await fetch(url);
		if (!response.ok) {
			const message = `aircraft proxy request failed: ${response.status} ${response.statusText}`;
			if (response.status !== 429 && response.status >= 400 && response.status < 500) {
				throw new AircraftProxyAccessError(message);
			}
			throw new Error(message);
		}
		return response.json();
	}
}
