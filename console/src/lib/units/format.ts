import type { UnitSystem } from './units-store.svelte';

const METERS_PER_FOOT = 0.3048;
const METERS_PER_SEC_PER_MPH = 0.44704;
const MM_PER_INCH = 25.4;
const HPA_PER_INHG = 33.8639;

/** Meters -> feet, or km -> miles for the long-distance case - the same
 * "switch to the bigger unit past 1000" behavior fleet-map.svelte's own
 * formatDistance already had for metric, mirrored for imperial (feet ->
 * miles past 5280 ft) rather than ever showing a raw multi-thousand-foot
 * number. */
export function formatDistance(meters: number, system: UnitSystem): string {
	if (system === 'metric') {
		return meters < 1000 ? `${meters.toFixed(0)} m` : `${(meters / 1000).toFixed(2)} km`;
	}
	const feet = meters / METERS_PER_FOOT;
	return feet < 5280 ? `${feet.toFixed(0)} ft` : `${(feet / 5280).toFixed(2)} mi`;
}

/** Altitude readouts stay in the "small unit" (m/ft) regardless of
 * magnitude, unlike formatDistance's km/mi switch above - an operator
 * reading a ward's altitude wants meters or feet, not "0.12 km", which is
 * how altimeters and ATC actually communicate altitude. */
export function formatAltitude(meters: number, system: UnitSystem): string {
	return system === 'metric'
		? `${meters.toFixed(1)} m`
		: `${(meters / METERS_PER_FOOT).toFixed(0)} ft`;
}

const KMH_PER_MS = 3.6;

/** Ward/drone telemetry speed - m/s is the actual unit that speed spec is
 * usually given in at this scale (a few to tens of m/s; matches how
 * DJI/drone manufacturers themselves publish max-speed figures), so it
 * reads naturally here in a way it stops doing at aircraft/wind scale -
 * see formatVehicleSpeed below for that case. */
export function formatSpeed(metersPerSecond: number, system: UnitSystem): string {
	return system === 'metric'
		? `${metersPerSecond.toFixed(1)} m/s`
		: `${(metersPerSecond / METERS_PER_SEC_PER_MPH).toFixed(1)} mph`;
}

/** Aircraft ground speed and wind - the "officially correct" aviation unit
 * for these is knots, but this app only has a Metric/Imperial toggle, not
 * a third aviation unit system, so this instead matches how vehicle-scale
 * speed is actually shown to the general public in each system (km/h or
 * mph, both familiar from road-speed intuition) rather than reusing
 * formatSpeed's raw m/s - confirmed live that "152.9 m/s" for an airliner
 * doesn't read naturally the way "550 km/h" does, unlike a drone's few
 * m/s, where m/s is the natural scale. */
export function formatVehicleSpeed(metersPerSecond: number, system: UnitSystem): string {
	return system === 'metric'
		? `${(metersPerSecond * KMH_PER_MS).toFixed(0)} km/h`
		: `${(metersPerSecond / METERS_PER_SEC_PER_MPH).toFixed(0)} mph`;
}

/** The only formatter here that isn't a straight unit-scale conversion:
 * Celsius -> Fahrenheit is affine (x9/5+32), not a ratio, so it can't share
 * formatDistance/formatSpeed's `value / factor` shape. */
export function formatTemperature(celsius: number, system: UnitSystem): string {
	return system === 'metric'
		? `${celsius.toFixed(0)}°C`
		: `${((celsius * 9) / 5 + 32).toFixed(0)}°F`;
}

/** Rain gauges report inches, not feet, at any plausible precipitation
 * total - unlike formatDistance/formatAltitude, imperial precipitation
 * never switches to a bigger unit. */
export function formatPrecipitation(millimeters: number, system: UnitSystem): string {
	return system === 'metric'
		? `${millimeters.toFixed(1)} mm`
		: `${(millimeters / MM_PER_INCH).toFixed(2)} in`;
}

/** hPa for metric (the world-standard unit almost everywhere), inHg for
 * imperial - the actual aviation-standard altimeter-setting unit in the
 * US, not just a scaled hPa the way every other imperial formatter here
 * is. */
export function formatPressure(hectopascals: number, system: UnitSystem): string {
	return system === 'metric'
		? `${hectopascals.toFixed(0)} hPa`
		: `${(hectopascals / HPA_PER_INHG).toFixed(2)} inHg`;
}
