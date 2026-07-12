export type GeozoneCategory = 'prohibited' | 'restricted' | 'other';

export interface Geozone {
	id: string;
	name: string;
	category: GeozoneCategory;
	polygon: GeoJSON.Feature<GeoJSON.Polygon | GeoJSON.MultiPolygon>;
}

/** west, south, east, north in degrees */
export type ViewportBounds = [number, number, number, number];

/**
 * Fetches airspace zones for a map viewport. OpenAIP is the default
 * implementation; official per-country ED-269 feeds can implement this same
 * interface later without touching the map or the confirm-dialog warnings
 * that consume it.
 */
export interface GeozoneSource {
	fetchViewport(bounds: ViewportBounds): Promise<Geozone[]>;
}
