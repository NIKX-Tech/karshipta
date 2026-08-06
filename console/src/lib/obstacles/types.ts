/** OpenAIP obstacle categories relevant to a low-altitude ward: towers,
 * masts, and wind turbines are the ones with real collision risk; anything
 * else falls through to 'other' rather than being dropped, since an
 * uncategorized obstacle marker is still more useful than a missing one. */
export type ObstacleCategory = 'tower' | 'mast' | 'wind-turbine' | 'other';

export interface Obstacle {
	id: string;
	name: string;
	category: ObstacleCategory;
	/** meters above ground level; undefined when OpenAIP doesn't report one */
	heightAglM: number | undefined;
	latitudeDeg: number;
	longitudeDeg: number;
}

/** west, south, east, north in degrees - same shape as geozones/types.ts's
 * own ViewportBounds; not imported from there to keep obstacles free of any
 * dependency on the geozone-specific module. */
export type ViewportBounds = [number, number, number, number];

export interface ObstacleSource {
	fetchViewport(bounds: ViewportBounds): Promise<Obstacle[]>;
}
