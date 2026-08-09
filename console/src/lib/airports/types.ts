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
	/** ISO country code - confirmed live directly against this endpoint
	 * (a real Schiphol response), not just assumed from obstacles' own
	 * identical field. */
	countryCode: string | undefined;
	/** meters above sea level - same `{ value, unit }` shape confirmed live
	 * on obstacles' own elevation field, and now directly on this endpoint
	 * too. */
	elevationM: number | undefined;
	/** Not publicly accessible without the owner/operator's own permission -
	 * a real operational constraint, confirmed live as a plain boolean on
	 * this endpoint (not every airport sets it; false/absent both mean
	 * "no restriction reported"). */
	isPrivate: boolean;
	/** Prior Permission Required - distinct from `isPrivate`: a PPR field
	 * can be publicly listed but still require contacting the operator
	 * ahead of time, confirmed live as its own separate boolean. */
	requiresPpr: boolean;
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
