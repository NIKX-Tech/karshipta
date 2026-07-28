import type { Geozone, GeozoneCategory, GeozoneSource, ViewportBounds } from './types';

const OPENAIP_API_URL = 'https://api.core.openaip.net/api/airspaces';
const RESULT_LIMIT = 200;
// Confirmed directly against a live key: OpenAIP rejects a bbox wider or
// taller than 5 degrees with a 400 ("exceeds the maximum allowed bounding
// box width/height of 5 degrees"), independently on each axis. Passing the
// raw map viewport through unclamped means the layer starts failing the
// moment an operator zooms out past a fairly tight area - clamp to a
// MAX_BBOX_SPAN_DEG window centered on the viewport instead of just
// erroring, so panning/zooming out still shows whatever's near the current
// center rather than nothing.
const MAX_BBOX_SPAN_DEG = 5;

/** Centered sub-window of bounds, capped to MAX_BBOX_SPAN_DEG on each axis. */
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
 * OpenAIP airspace `type` codes that read as flight-restricted, per their
 * public API docs (Prohibited, Restricted, Danger, TMZ). The fetch/parse
 * path itself is now confirmed against a live key (EHAA FIR type=10,
 * LONDON TMA 1 type=7, both correctly falling through to 'other'); this
 * specific code-to-category table is still only cross-checked against those
 * few samples, not every code OpenAIP defines. If a zone renders in the
 * wrong color, check its real `type` value against this mapping first.
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
		const zones: Geozone[] = [];
		for (const raw of items) {
			const zone = parseAirspace(raw);
			if (zone) zones.push(zone);
		}
		return zones;
	}
}
