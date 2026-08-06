/** Coarse enough to pick a marker style, not a full OpenAIP type taxonomy:
 * 'heliport' matters most for a low-altitude ward sharing airspace with
 * rotorcraft; everything else that isn't a heliport falls under
 * 'airfield' rather than being dropped. */
export type AirportCategory = 'airfield' | 'heliport';

export interface Airport {
	id: string;
	name: string;
	category: AirportCategory;
	/** ICAO code if OpenAIP reports one (most do); undefined otherwise */
	icaoCode: string | undefined;
	latitudeDeg: number;
	longitudeDeg: number;
}

/** west, south, east, north in degrees - same shape as geozones/types.ts's
 * own ViewportBounds; not imported from there to keep airports free of any
 * dependency on the geozone-specific module. */
export type ViewportBounds = [number, number, number, number];

export interface AirportSource {
	fetchViewport(bounds: ViewportBounds): Promise<Airport[]>;
}
