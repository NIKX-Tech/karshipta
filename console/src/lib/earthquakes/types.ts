export interface Earthquake {
	id: string;
	magnitude: number;
	place: string;
	timeMs: number;
	latitudeDeg: number;
	longitudeDeg: number;
	/** km below the surface */
	depthKm: number;
}

/** west, south, east, north in degrees. */
export type ViewportBounds = [number, number, number, number];

export interface EarthquakeSource {
	fetchViewport(bounds: ViewportBounds): Promise<Earthquake[]>;
}
