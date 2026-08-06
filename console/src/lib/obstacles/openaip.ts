import type { Obstacle, ObstacleCategory, ObstacleSource, ViewportBounds } from './types';

const OPENAIP_API_URL = 'https://api.core.openaip.net/api/obstacles';
const RESULT_LIMIT = 200;
// Same constraint as geozones/openaip.ts's own MAX_BBOX_SPAN_DEG - OpenAIP
// rejects a bbox wider or taller than 5 degrees on any endpoint, not just
// airspaces (confirmed for airspaces directly; assumed to hold here too
// since it reads as a request-shape limit on the API itself, not a
// per-endpoint one - if a real key shows otherwise, this needs revisiting).
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
 * OpenAIP obstacle `type` codes, per their public API docs. Not yet
 * cross-checked against a live key the way geozones/openaip.ts's own
 * PROHIBITED_TYPES/RESTRICTED_TYPES were (see that file's own comment) -
 * these are best-effort from documented codes. If an obstacle renders with
 * the wrong icon/category, check its real `type` value against this
 * mapping first.
 */
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

/** Defensive parse: unexpected shapes are skipped, never thrown - same
 * philosophy as geozones/openaip.ts's own parseAirspace. */
function parseObstacle(raw: unknown): Obstacle | undefined {
	if (typeof raw !== 'object' || raw === null) return undefined;
	const item = raw as Record<string, unknown>;
	const id = item._id;
	const geometry = item.geometry;
	if (typeof id !== 'string' || !isPointGeometry(geometry)) return undefined;
	const [longitudeDeg, latitudeDeg] = geometry.coordinates;
	if (typeof latitudeDeg !== 'number' || typeof longitudeDeg !== 'number') return undefined;
	const heightAgl = item.heightAgl;
	const name = typeof item.name === 'string' && item.name ? item.name : 'Obstacle';
	return {
		id,
		name,
		category: categoryFor(item.type),
		heightAglM: typeof heightAgl === 'number' ? heightAgl : undefined,
		latitudeDeg,
		longitudeDeg
	};
}

export class OpenAipObstacleSource implements ObstacleSource {
	constructor(private readonly apiKey: string) {}

	async fetchViewport(bounds: ViewportBounds): Promise<Obstacle[]> {
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
		const obstacles: Obstacle[] = [];
		for (const raw of items) {
			const obstacle = parseObstacle(raw);
			if (obstacle) obstacles.push(obstacle);
		}
		return obstacles;
	}
}
