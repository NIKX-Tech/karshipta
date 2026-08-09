export interface FireHotspot {
	id: string;
	latitudeDeg: number;
	longitudeDeg: number;
	/** kelvin - VIIRS brightness temperature, FIRMS' own proxy for fire intensity */
	brightnessK: number;
	/** FIRMS reports this as 'low' | 'nominal' | 'high' for VIIRS specifically */
	confidence: string;
	acquiredAtIso: string;
	/** fire radiative power, megawatts - a second intensity signal alongside brightness */
	frpMw: number | undefined;
	/** FIRMS' own day/night detection flag ('D'/'N' in the raw CSV) - a
	 * daytime detection carries a real caveat VIIRS itself documents (solar
	 * reflection can inflate the thermal signal), so this is genuine
	 * context, not decoration. undefined when the column is missing rather
	 * than guessed. */
	isNightDetection: boolean | undefined;
}

/** west, south, east, north in degrees. */
export type ViewportBounds = [number, number, number, number];

export interface FireHotspotSource {
	fetchViewport(bounds: ViewportBounds): Promise<FireHotspot[]>;
}
