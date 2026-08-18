import type { Aircraft, AircraftCategory, AircraftSource, ViewportBounds } from './types';

// Formerly api.airplanes.live directly - that API repo is now archived
// (github.com/airplanes-live/api-archive) and its README points here
// instead: same org, same /v2/point/[lat]/[lon]/[radius] shape, response
// format documented as "conforms to the ADSBExchange v2 API" (i.e. not
// airplanes.live-specific naming at all, hence this file's own name).
const ADSB_ONE_API_URL = 'https://api.adsb.one/v2/point';
// Radius is nautical miles, not the bbox this store's caller works in, so
// a viewport gets reduced to a center point and radius below. 250nm is the
// same ceiling the old airplanes.live docs documented; unconfirmed whether
// adsb.one enforces the identical cap, kept as the safe assumption.
const MAX_RADIUS_NM = 250;
const MIN_RADIUS_NM = 5;
const KM_PER_NM = 1.852;
const FEET_PER_METER = 3.28084;
const KNOTS_TO_MS = 0.514444;
const FEET_PER_MINUTE_TO_MS = 0.3048 / 60;
const EARTH_RADIUS_KM = 6371;
// bit 1 of readsb/tar1090's dbFlags convention - confirmed live against a
// real Royal Netherlands Air Force Apache (category A7, dbFlags 1).
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

// readsb/dump1090's own documented JSON shape (confirmed live) - aircraft
// are objects keyed by hex/flight/r/t/desc/alt_baro/gs/track/lat/lon, not
// OpenSky's positional arrays. No nationality field here (unlike OpenSky's
// origin_country) - registration and the type description fill that role
// instead, and are more identifying in practice. alt_baro is feet, or the
// literal string "ground" when landed; gs is knots - both converted to
// this app's internal SI units here, same as every other source module.
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
	const headingDeg = typeof data.track === 'number' ? data.track : undefined;
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

/** Thrown for a 4xx other than 429: the source rejecting every request
 * outright, not a transient rate limit - retrying that on a timer every
 * few seconds forever is pointless and just spams both the console and
 * their server. aircraft-store.svelte.ts checks for this specifically to
 * stop its own retry loop. */
export class AdsbOneAccessError extends Error {}

/** Community-run, unfiltered ADS-B/MLAT aggregator, no key or signup - see
 * this file's own header comment on the api.airplanes.live -> api.adsb.one
 * migration. Point+radius only (no native bbox endpoint), so the viewport
 * gets reduced to its center and half-diagonal above. */
export class AdsbOneAircraftSource implements AircraftSource {
	async fetchViewport(bounds: ViewportBounds): Promise<Aircraft[]> {
		const { latitudeDeg, longitudeDeg, radiusNm } = boundsToPointRadius(bounds);
		const url = `${ADSB_ONE_API_URL}/${latitudeDeg}/${longitudeDeg}/${radiusNm.toFixed(0)}`;
		const response = await fetch(url);
		if (!response.ok) {
			const message = `adsb.one request failed: ${response.status} ${response.statusText}`;
			if (response.status !== 429 && response.status >= 400 && response.status < 500) {
				throw new AdsbOneAccessError(message);
			}
			throw new Error(message);
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
}
