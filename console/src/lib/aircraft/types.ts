/** Collapsed from the ADS-B emitter category codes (A0-A7/B0-B7/C0-C7) into
 * the handful of buckets this app actually draws differently - see
 * airplaneslive.ts's own mapping comment for the full code-to-bucket table. */
export type AircraftCategory =
	'light' | 'heavy' | 'rotorcraft' | 'glider' | 'uav' | 'ground' | 'unknown';

export interface Aircraft {
	icao24: string;
	callsign: string | undefined;
	/** Tail number (e.g. "PH-BHA") - airplanes.live has no nationality
	 * field the way OpenSky did, so this and typeDescription are the
	 * identifying details shown instead of a country. */
	registration: string | undefined;
	/** Human-readable type (e.g. "BOEING 737-800"), not the raw ICAO type
	 * code. */
	typeDescription: string | undefined;
	/** Owner/operator name (e.g. "KLM Royal Dutch Airlines") - airplanes.live's
	 * own field, not derived from callsign guessing. */
	operator: string | undefined;
	yearBuilt: number | undefined;
	latitudeDeg: number;
	longitudeDeg: number;
	/** meters, barometric altitude - undefined when there's no fix yet */
	altitudeM: number | undefined;
	velocityMS: number | undefined;
	/** positive = climbing, negative = descending, undefined when unknown */
	verticalRateMS: number | undefined;
	headingDeg: number | undefined;
	onGround: boolean;
	category: AircraftCategory;
	/** From the feed's dbFlags bitmask (bit 1) - a real classification from
	 * airplanes.live's own database, not a guess from callsign/registration
	 * patterns. */
	isMilitary: boolean;
	/** Raw ADS-B emergency status string ('none' when not declared,
	 * undefined when the aircraft doesn't report the field at all - both
	 * mean "no emergency", kept distinct only because that's what the feed
	 * sends). Real values include 'general', 'medical', 'minfuel', 'nordo',
	 * 'unlawful', 'downed'. */
	emergency: string | undefined;
}

export function isAircraftEmergency(aircraft: Pick<Aircraft, 'emergency'>): boolean {
	return aircraft.emergency !== undefined && aircraft.emergency !== 'none';
}

/** west, south, east, north in degrees - same shape as geozones/types.ts's
 * own ViewportBounds; not imported from there to keep this module free of
 * any dependency on the OpenAIP-specific one. */
export type ViewportBounds = [number, number, number, number];

export interface AircraftSource {
	fetchViewport(bounds: ViewportBounds): Promise<Aircraft[]>;
}
