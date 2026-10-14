import type { Airport, AirportSource, ViewportBounds } from './types';

/** Thrown for a 4xx other than 429: the proxy route rejecting every
 * request outright (a misconfigured or missing route/key, not a transient
 * rate limit) - retrying that on a timer every few seconds forever is
 * pointless. airport-store.svelte.ts checks for this specifically to stop
 * its own retry loop. */
export class AirportProxyAccessError extends Error {}

/** Calls this app's own same-origin airports proxy route rather than
 * OpenAIP's API directly - OpenAIP sends no CORS headers at all (same as
 * adsb.lol, see aircraft/proxiedSource.ts's own comment), so a direct
 * browser fetch can never work. A consuming app that never wires up
 * /api/airports just gets a 404 here, which fails the same clean way any
 * other network error does - the layer degrades gracefully, it doesn't
 * crash the console. */
export class ProxiedAirportSource implements AirportSource {
	async fetchViewport(bounds: ViewportBounds): Promise<Airport[]> {
		const url = `/api/airports?bbox=${bounds.join(',')}`;
		const response = await fetch(url);
		if (!response.ok) {
			const message = `airports proxy request failed: ${response.status} ${response.statusText}`;
			if (response.status !== 429 && response.status >= 400 && response.status < 500) {
				throw new AirportProxyAccessError(message);
			}
			throw new Error(message);
		}
		return response.json();
	}
}
