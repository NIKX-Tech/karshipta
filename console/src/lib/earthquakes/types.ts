export interface Earthquake {
	id: string;
	magnitude: number;
	place: string;
	timeMs: number;
	latitudeDeg: number;
	longitudeDeg: number;
	/** km below the surface */
	depthKm: number;
	/** USGS's own PAGER estimated-impact level, confirmed live as a plain
	 * 'green' | 'yellow' | 'orange' | 'red' string (or null, most events
	 * never get one at all - PAGER only runs for events likely to have a
	 * real human impact). Not this app inventing a severity scale. */
	alertLevel: string | undefined;
	/** USGS's own tsunami-hazard flag for this event (0 or 1 in the raw
	 * feed) - real safety data, particularly relevant to marine/submarine
	 * wards (see docs/map-layers.md's own "Useful for" note on this
	 * layer). */
	tsunamiWarning: boolean;
	/** USGS's own event page - the canonical source for anything not
	 * already surfaced here, same "external link for more" pattern as
	 * Obstacles' Wikipedia link. */
	detailsUrl: string | undefined;
}

/** west, south, east, north in degrees. */
export type ViewportBounds = [number, number, number, number];

export interface EarthquakeSource {
	fetchViewport(bounds: ViewportBounds): Promise<Earthquake[]>;
}
