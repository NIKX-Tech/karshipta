import type { Obstacle, ObstacleCategory, ViewportBounds } from './types';

// OpenAIP: confirmed live to send no CORS headers at all (same as
// adsb.lol, see aircraft/adsbLolProxy.ts) - a direct browser fetch fails
// regardless of key validity. This module is meant to run server-side
// only, exported via this package's own "./obstacles-proxy" subpath -
// never import it from client code. See proxiedSource.ts (the client half
// of this pair) for the same-origin route a consuming app wires this into.
const OPENAIP_API_URL = 'https://api.core.openaip.net/api/obstacles';
const RESULT_LIMIT = 200;
// OpenAIP rejects a bbox wider or taller than 5 degrees on any endpoint,
// not just airspaces (see geozones/openaip.ts's own MAX_BBOX_SPAN_DEG).
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

// OpenAIP obstacle `type` codes, per their public API docs - same caveat
// as the former client-side version this replaces: not yet cross-checked
// against a live key.
const TOWER_TYPES = new Set([3, 4]); // radio/tv mast, tower
const WIND_TURBINE_TYPES = new Set([13]);

function categoryFor(type: unknown): ObstacleCategory {
	const code = typeof type === 'number' ? type : Number(type);
	if (WIND_TURBINE_TYPES.has(code)) return 'wind-turbine';
	if (TOWER_TYPES.has(code)) return 'tower';
	return 'other';
}

function isPointGeometry(geometry: unknown): geometry is GeoJSON.Point {
	if (typeof geometry !== 'object' || geometry === null) return false;
	return (geometry as Record<string, unknown>).type === 'Point';
}

// OpenAIP nests both height and elevation as { value, unit, referenceDatum }
// - confirmed live (Schiphol's control tower reports height.value=101,
// matching its real ~101m height, so unit 0 reads as meters).
function numericValueOf(field: unknown): number | undefined {
	if (typeof field !== 'object' || field === null) return undefined;
	const value = (field as Record<string, unknown>).value;
	return typeof value === 'number' ? value : undefined;
}

// osmTags.wikipedia (when present) is an OSM-style "lang:Title" reference,
// not a URL - "nl:Schipholtoren" becomes https://nl.wikipedia.org/wiki/Schipholtoren.
function wikipediaUrlFrom(osmTags: unknown): string | undefined {
	if (typeof osmTags !== 'object' || osmTags === null) return undefined;
	const wiki = (osmTags as Record<string, unknown>).wikipedia;
	if (typeof wiki !== 'string') return undefined;
	const [lang, ...titleParts] = wiki.split(':');
	const title = titleParts.join(':');
	if (!lang || !title) return undefined;
	return `https://${lang}.wikipedia.org/wiki/${encodeURIComponent(title)}`;
}

// Defensive parse: unexpected shapes are skipped, never thrown.
function parseObstacle(raw: unknown): Obstacle | undefined {
	if (typeof raw !== 'object' || raw === null) return undefined;
	const item = raw as Record<string, unknown>;
	const id = item._id;
	const geometry = item.geometry;
	if (typeof id !== 'string' || !isPointGeometry(geometry)) return undefined;
	const [longitudeDeg, latitudeDeg] = geometry.coordinates;
	if (typeof latitudeDeg !== 'number' || typeof longitudeDeg !== 'number') return undefined;
	const name = typeof item.name === 'string' && item.name ? item.name : 'Obstacle';
	const countryCode = typeof item.country === 'string' ? item.country : undefined;
	return {
		id,
		name,
		category: categoryFor(item.type),
		countryCode,
		heightM: numericValueOf(item.height),
		elevationM: numericValueOf(item.elevation),
		wikipediaUrl: wikipediaUrlFrom(item.osmTags),
		latitudeDeg,
		longitudeDeg
	};
}

/** Fetches and parses obstacles within bounds from OpenAIP. Server-side
 * only (see this file's own header comment) - a consuming app's own
 * same-origin API route calls this and hands the already-typed Obstacle[]
 * back to the browser, e.g.:
 *
 * // src/routes/api/obstacles/+server.ts
 * import { fetchObstaclesInBounds } from
 *   '@nikx-tech/karshipta-console-core/obstacles-proxy';
 * export const GET = async ({ url }) => {
 *   const bbox = url.searchParams.get('bbox')?.split(',').map(Number);
 *   return json(await fetchObstaclesInBounds(bbox, env.OPENAIP_KEY));
 * };
 */
export async function fetchObstaclesInBounds(
	bounds: ViewportBounds,
	apiKey: string
): Promise<Obstacle[]> {
	const [west, south, east, north] = clampToMaxSpan(bounds);
	const url = `${OPENAIP_API_URL}?bbox=${west},${south},${east},${north}&limit=${RESULT_LIMIT}`;
	const response = await fetch(url, { headers: { 'x-openaip-api-key': apiKey } });
	if (!response.ok) {
		throw new Error(`OpenAIP request failed: ${response.status} ${response.statusText}`);
	}
	const body: unknown = await response.json();
	const items =
		typeof body === 'object' && body !== null && Array.isArray((body as { items?: unknown }).items)
			? (body as { items: unknown[] }).items
			: [];
	const obstacles: Obstacle[] = [];
	for (const raw of items) {
		const obstacle = parseObstacle(raw);
		if (obstacle) obstacles.push(obstacle);
	}
	return obstacles;
}
