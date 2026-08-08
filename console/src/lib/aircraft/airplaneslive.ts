import type { Aircraft, AircraftCategory, AircraftSource, ViewportBounds } from './types';

const AIRPLANES_LIVE_API_URL = 'https://api.airplanes.live/v2/point';
// The API's own documented ceiling for its /point/[lat]/[lon]/[radius]
// endpoint (see https://airplanes.live/api-guide/) - radius is nautical
// miles, not the bbox this store's caller works in, so a viewport gets
// reduced to a center point and radius below.
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

/** Community-run, unfiltered ADS-B/MLAT aggregator - no key or signup, and
 * critically (unlike OpenSky's anonymous REST API, which sends a fixed
 * Access-Control-Allow-Origin locked to opensky-network.org itself, so it
 * can never be called from any other browser origin - confirmed live via a
 * direct CORS error, not an assumption) this one sends a wildcard CORS
 * header and actually works from a browser. See
 * https://airplanes.live/api-guide/: no key currently required, documented
 * limit is 1 request/second, "non-commercial use", no SLA/uptime
 * guarantee - aircraft-store.svelte.ts's own debounce stays well under
 * that limit. Point+radius only (no native bbox endpoint), so the
 * viewport gets reduced to its center and half-diagonal above. */
export class AirplanesLiveAircraftSource implements AircraftSource {
	async fetchViewport(bounds: ViewportBounds): Promise<Aircraft[]> {
		const { latitudeDeg, longitudeDeg, radiusNm } = boundsToPointRadius(bounds);
		const url = `${AIRPLANES_LIVE_API_URL}/${latitudeDeg}/${longitudeDeg}/${radiusNm.toFixed(0)}`;
		const response = await fetch(url);
		if (!response.ok) {
			throw new Error(`airplanes.live request failed: ${response.status} ${response.statusText}`);
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
