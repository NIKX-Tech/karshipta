/** OpenAIP obstacle categories relevant to a low-altitude ward: towers,
 * masts, and wind turbines are the ones with real collision risk; anything
 * else falls through to 'other' rather than being dropped, since an
 * uncategorized obstacle marker is still more useful than a missing one. */
export type ObstacleCategory = 'tower' | 'mast' | 'wind-turbine' | 'other';

export interface Obstacle {
	id: string;
	name: string;
	category: ObstacleCategory;
	/** ISO country code, when OpenAIP reports one */
	countryCode: string | undefined;
	/** meters - the structure's own height, confirmed live against a real
	 * key (Schiphol's control tower reports 101, matching its real ~101m
	 * height) - not `heightAgl` as originally guessed; that field doesn't
	 * exist in the real response. */
	heightM: number | undefined;
	/** meters above sea level - the ground/base elevation, distinct from
	 * the structure's own height above. */
	elevationM: number | undefined;
	/** OpenAIP's OSM import carries a wiki reference on some obstacles
	 * (osmTags.wikipedia, e.g. "nl:Schipholtoren") - converted to a real
	 * URL when present, undefined otherwise. The closest thing to "more
	 * info" this endpoint offers; OpenAIP itself has no image field. */
	wikipediaUrl: string | undefined;
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
