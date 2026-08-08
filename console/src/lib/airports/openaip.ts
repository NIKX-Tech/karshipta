import type { Airport, AirportCategory, AirportSource, ViewportBounds } from './types';

const OPENAIP_API_URL = 'https://api.core.openaip.net/api/airports';
const RESULT_LIMIT = 200;
// Same constraint as geozones/openaip.ts's own MAX_BBOX_SPAN_DEG - see
// obstacles/openaip.ts's own comment on why this is assumed API-wide.
const MAX_BBOX_SPAN_DEG = 5;

function clampToMaxSpan([west, south, east, north]: ViewportBounds): ViewportBounds {
	const centerLon = (west + east) / 2;
	const centerLat = (south + north) / 2;
	const halfWidth = Math.min((east - west) / 2, MAX_BBOX_SPAN_DEG / 2);
	const halfHeight = Math.min((north - south) / 2, MAX_BBOX_SPAN_DEG / 2);
	return [
		centerLon - halfWidth,
		centerLat - halfHeight,
		centerLon + halfWidth,
		centerLat + halfHeight
	];
}

/**
 * OpenAIP airport `type` codes, per their public API docs. Not yet
 * cross-checked against a live key - see obstacles/openaip.ts's own
 * comment on the same caveat for its type mapping.
 */
const HELIPORT_TYPES = new Set([9]);

function categoryFor(type: unknown): AirportCategory {
	const code = typeof type === 'number' ? type : Number(type);
	return HELIPORT_TYPES.has(code) ? 'heliport' : 'airfield';
}

function isPointGeometry(geometry: unknown): geometry is GeoJSON.Point {
	if (typeof geometry !== 'object' || geometry === null) return false;
	return (geometry as Record<string, unknown>).type === 'Point';
}

/** Same { value, unit } shape confirmed live on obstacles' own elevation
 * field - see airports/types.ts's own comment on this being inferred, not
 * independently confirmed for this endpoint. */
function numericValueOf(field: unknown): number | undefined {
	if (typeof field !== 'object' || field === null) return undefined;
	const value = (field as Record<string, unknown>).value;
	return typeof value === 'number' ? value : undefined;
}

/** Defensive parse: unexpected shapes are skipped, never thrown - same
 * philosophy as geozones/openaip.ts's own parseAirspace. */
function parseAirport(raw: unknown): Airport | undefined {
	if (typeof raw !== 'object' || raw === null) return undefined;
	const item = raw as Record<string, unknown>;
	const id = item._id;
	const name = item.name;
	const geometry = item.geometry;
	if (typeof id !== 'string' || typeof name !== 'string' || !isPointGeometry(geometry)) {
		return undefined;
	}
	const [longitudeDeg, latitudeDeg] = geometry.coordinates;
	if (typeof latitudeDeg !== 'number' || typeof longitudeDeg !== 'number') return undefined;
	const icaoCode = item.icaoCode;
	return {
		id,
		name,
		category: categoryFor(item.type),
		icaoCode: typeof icaoCode === 'string' && icaoCode ? icaoCode : undefined,
		countryCode: typeof item.country === 'string' ? item.country : undefined,
		elevationM: numericValueOf(item.elevation),
		latitudeDeg,
		longitudeDeg
	};
}

export class OpenAipAirportSource implements AirportSource {
	constructor(private readonly apiKey: string) {}

	async fetchViewport(bounds: ViewportBounds): Promise<Airport[]> {
		const [west, south, east, north] = clampToMaxSpan(bounds);
		const url = `${OPENAIP_API_URL}?bbox=${west},${south},${east},${north}&limit=${RESULT_LIMIT}`;
		const response = await fetch(url, { headers: { 'x-openaip-api-key': this.apiKey } });
		if (!response.ok) {
			throw new Error(`OpenAIP request failed: ${response.status} ${response.statusText}`);
		}
		const body: unknown = await response.json();
		const items =
			typeof body === 'object' &&
			body !== null &&
			Array.isArray((body as { items?: unknown }).items)
				? (body as { items: unknown[] }).items
				: [];
		const airports: Airport[] = [];
		for (const raw of items) {
			const airport = parseAirport(raw);
			if (airport) airports.push(airport);
		}
		return airports;
	}
}
