import type { Geozone, GeozoneCategory, GeozoneSource, ViewportBounds } from './types';

const OPENAIP_API_URL = 'https://api.core.openaip.net/api/airspaces';
const RESULT_LIMIT = 200;

/**
 * OpenAIP airspace `type` codes that read as flight-restricted, per their
 * public API docs (Prohibited, Restricted, Danger, TMZ). Not verified
 * against a live key: this environment had none available to test with. If
 * zones render in the wrong color, check a real response body against this
 * mapping before assuming the layer itself is broken.
 */
const PROHIBITED_TYPES = new Set([3]);
const RESTRICTED_TYPES = new Set([1, 2, 5]);

function categoryFor(type: unknown): GeozoneCategory {
	const code = typeof type === 'number' ? type : Number(type);
	if (PROHIBITED_TYPES.has(code)) return 'prohibited';
	if (RESTRICTED_TYPES.has(code)) return 'restricted';
	return 'other';
}

function isPolygonGeometry(geometry: unknown): geometry is GeoJSON.Polygon | GeoJSON.MultiPolygon {
	if (typeof geometry !== 'object' || geometry === null) return false;
	const type = (geometry as Record<string, unknown>).type;
	return type === 'Polygon' || type === 'MultiPolygon';
}

/** Defensive parse: unexpected shapes are skipped, never thrown. */
function parseAirspace(raw: unknown): Geozone | undefined {
	if (typeof raw !== 'object' || raw === null) return undefined;
	const item = raw as Record<string, unknown>;
	const id = item._id;
	const name = item.name;
	const geometry = item.geometry;
	if (typeof id !== 'string' || typeof name !== 'string' || !isPolygonGeometry(geometry)) {
		return undefined;
	}
	return {
		id,
		name,
		category: categoryFor(item.type),
		polygon: { type: 'Feature', properties: {}, geometry }
	};
}

export class OpenAipGeozoneSource implements GeozoneSource {
	constructor(private readonly apiKey: string) {}

	async fetchViewport(bounds: ViewportBounds): Promise<Geozone[]> {
		const [west, south, east, north] = bounds;
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
		const zones: Geozone[] = [];
		for (const raw of items) {
			const zone = parseAirspace(raw);
			if (zone) zones.push(zone);
		}
		return zones;
	}
}
