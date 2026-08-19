import type { Aircraft, AircraftCategory } from './types';

// api.adsb.lol: community-run, keyless, real data (confirmed live) - but
// sends no CORS headers at all (confirmed live: its own OPTIONS preflight
// returns a plain 405, no Access-Control-* headers whatsoever), unlike the
// old api.airplanes.live, which sent a wildcard CORS header and could be
// called directly from a browser. This module is meant to run server-side
// only, exported via this package's own "./aircraft-proxy" subpath - never
// import it from client code, since it has nothing to offer a browser's
// CORS check regardless of origin. See proxiedSource.ts (the client half
// of this pair) for the same-origin route a consuming app wires this into.
const ADSB_LOL_API_URL = 'https://api.adsb.lol/v2/point';
// Radius is nautical miles. 250nm matches the old airplanes.live-era
// ceiling; unconfirmed whether adsb.lol enforces the identical cap, kept
// as the safe assumption.
const MAX_RADIUS_NM = 250;
const MIN_RADIUS_NM = 5;
const FEET_PER_METER = 3.28084;
const KNOTS_TO_MS = 0.514444;
const FEET_PER_MINUTE_TO_MS = 0.3048 / 60;
// bit 1 of readsb/tar1090's dbFlags convention - confirmed live against
// real military traffic on adsb.lol's own /v2/mil endpoint.
const DB_FLAG_MILITARY = 1;

// ADS-B emitter category codes (ICAO Annex 10 / DO-260B), collapsed to the
// handful of buckets this app draws differently - see AircraftCategory's
// own comment. A1 light, A2/A3/A4/A5/A6 progressively larger/faster fixed
// wing all bucket to "heavy" (the useful distinction for a map marker is
// "small GA aircraft" vs "everything bigger", not the full six-way split),
// A7 rotorcraft, B1 glider/sailplane, B6 UAV, C0-C7 surface vehicle/obstacle.
function mapCategory(raw: unknown): AircraftCategory {
	if (typeof raw !== 'string') return 'unknown';
	if (raw === 'A1') return 'light';
	if (raw === 'A2' || raw === 'A3' || raw === 'A4' || raw === 'A5' || raw === 'A6') return 'heavy';
	if (raw === 'A7') return 'rotorcraft';
	if (raw === 'B1') return 'glider';
	if (raw === 'B6') return 'uav';
	if (raw.startsWith('C')) return 'ground';
	return 'unknown';
}

// readsb/dump1090's own documented JSON shape (confirmed live) - aircraft
// are objects keyed by hex/flight/r/t/desc/alt_baro/gs/track/lat/lon, not
// OpenSky's positional arrays. No nationality field here (unlike OpenSky's
// origin_country) - registration and the type description fill that role
// instead, and are more identifying in practice. alt_baro is feet, or the
// literal string "ground" when landed; gs is knots - both converted to
// this app's internal SI units here, same as every other source module.
// heading prefers track (in-flight ADS-B track angle) but falls back to
// true_heading - confirmed live that adsb.lol only sends true_heading for
// grounded aircraft (no meaningful track angle while stationary), not
// track for every record the way the popup/marker code otherwise assumes.
function parseAircraft(raw: unknown): Aircraft | undefined {
	if (typeof raw !== 'object' || raw === null) return undefined;
	const data = raw as Record<string, unknown>;
	const icao24 = data.hex;
	const latitudeDeg = data.lat;
	const longitudeDeg = data.lon;
	if (
		typeof icao24 !== 'string' ||
		typeof latitudeDeg !== 'number' ||
		typeof longitudeDeg !== 'number'
	) {
		return undefined;
	}
	const callsign =
		typeof data.flight === 'string' && data.flight.trim() ? data.flight.trim() : undefined;
	const registration = typeof data.r === 'string' && data.r.trim() ? data.r.trim() : undefined;
	const typeDescription =
		typeof data.desc === 'string' && data.desc.trim() ? data.desc.trim() : undefined;
	const operator =
		typeof data.ownOp === 'string' && data.ownOp.trim() ? data.ownOp.trim() : undefined;
	const yearBuilt =
		typeof data.year === 'string' && /^\d{4}$/.test(data.year) ? Number(data.year) : undefined;
	const onGround = data.alt_baro === 'ground';
	const altitudeM = typeof data.alt_baro === 'number' ? data.alt_baro / FEET_PER_METER : undefined;
	const velocityMS = typeof data.gs === 'number' ? data.gs * KNOTS_TO_MS : undefined;
	const verticalRateMS =
		typeof data.baro_rate === 'number' ? data.baro_rate * FEET_PER_MINUTE_TO_MS : undefined;
	const headingDeg =
		typeof data.track === 'number'
			? data.track
			: typeof data.true_heading === 'number'
				? data.true_heading
				: undefined;
	const category = mapCategory(data.category);
	const isMilitary =
		typeof data.dbFlags === 'number' && (data.dbFlags & DB_FLAG_MILITARY) === DB_FLAG_MILITARY;
	const emergency = typeof data.emergency === 'string' ? data.emergency : undefined;
	return {
		icao24,
		callsign,
		registration,
		typeDescription,
		operator,
		yearBuilt,
		latitudeDeg,
		longitudeDeg,
		altitudeM,
		velocityMS,
		verticalRateMS,
		headingDeg,
		onGround,
		category,
		isMilitary,
		emergency
	};
}

/** Fetches and parses aircraft within radiusNm of a point from adsb.lol.
 * Server-side only (see this file's own header comment) - a consuming
 * app's own same-origin API route calls this and hands the already-typed
 * Aircraft[] back to the browser, e.g.:
 *
 * // src/routes/api/aircraft/point/[lat]/[lon]/[radius]/+server.ts
 * import { fetchAircraftNearPoint } from
 *   '@nikx-tech/karshipta-console-core/aircraft-proxy';
 * export const GET = async ({ params }) =>
 *   json(await fetchAircraftNearPoint(+params.lat, +params.lon, +params.radius));
 */
export async function fetchAircraftNearPoint(
	latitudeDeg: number,
	longitudeDeg: number,
	radiusNm: number
): Promise<Aircraft[]> {
	const clampedRadiusNm = Math.min(MAX_RADIUS_NM, Math.max(MIN_RADIUS_NM, radiusNm));
	const url = `${ADSB_LOL_API_URL}/${latitudeDeg}/${longitudeDeg}/${clampedRadiusNm.toFixed(0)}`;
	const response = await fetch(url);
	if (!response.ok) {
		throw new Error(`adsb.lol request failed: ${response.status} ${response.statusText}`);
	}
	const body: unknown = await response.json();
	const list =
		typeof body === 'object' && body !== null && Array.isArray((body as { ac?: unknown }).ac)
			? (body as { ac: unknown[] }).ac
			: [];
	const aircraft: Aircraft[] = [];
	for (const raw of list) {
		const parsed = parseAircraft(raw);
		if (parsed) aircraft.push(parsed);
	}
	return aircraft;
}
