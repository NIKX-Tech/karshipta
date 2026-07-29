import { flightModeToJSON, gpsFixTypeToJSON } from '$lib/gen/karshipta/v1/common';
import type { Battery, FlightState, Gps, WardState } from '$lib/gen/karshipta/v1/telemetry';

const FLIGHT_MODE_PREFIX = 'FLIGHT_MODE_';
const GPS_FIX_PREFIX = 'GPS_FIX_TYPE_';

// Placeholders pending confirmation against the autopilot's own LOW_BATTERY
// failsafe threshold (telemetry.proto's Event.code documents that value
// exists; the console doesn't currently receive the number itself, so these
// are a reasonable default, not a value read from the vehicle).
export const BATTERY_CAUTION_PCT = 30;
export const BATTERY_CRITICAL_PCT = 15;

export type BatteryTier = 'nominal' | 'caution' | 'critical';

export function batteryTier(battery: Battery | undefined): BatteryTier {
	if (!battery || battery.remainingPct < 0) return 'nominal';
	if (battery.remainingPct < BATTERY_CRITICAL_PCT) return 'critical';
	if (battery.remainingPct < BATTERY_CAUTION_PCT) return 'caution';
	return 'nominal';
}

export function batteryHintLabel(tier: BatteryTier): string | undefined {
	if (tier === 'caution') return 'WATCH';
	if (tier === 'critical') return 'LAND NOW';
	return undefined;
}

export type LinkDotState = 'ok' | 'fault' | 'lost';

/**
 * Disconnected isn't itself a fault reading - it's an absence of data, so it
 * takes priority as its own quiet state rather than inheriting whatever
 * health_ok last reported before the link dropped.
 */
export function linkDotState(state: WardState | undefined): LinkDotState {
	if (!state?.connected) return 'lost';
	return state.healthOk ? 'ok' : 'fault';
}

/**
 * Mode is a flight-autopilot concept - undefined for a ward with no flight
 * field, e.g. a livestock tag with no autopilot state machine. Shared by
 * ward-status-strip.svelte and ward-tab.svelte so the two don't each
 * reimplement the same enum-to-label conversion.
 */
export function formatModeLabel(flight: FlightState | undefined): string | undefined {
	return flight ? flightModeToJSON(flight.flightMode).replace(FLIGHT_MODE_PREFIX, '') : undefined;
}

export function formatBatteryLabel(battery: Battery | undefined): string {
	return battery && battery.remainingPct >= 0
		? `${battery.remainingPct.toFixed(0)}% (${battery.voltageV.toFixed(1)} V)`
		: 'unknown';
}

export function formatBatteryPct(battery: Battery | undefined): string {
	return battery && battery.remainingPct >= 0 ? `${battery.remainingPct.toFixed(0)}%` : '?';
}

export function formatGpsLabel(gps: Gps | undefined): string {
	return gps ? gpsFixTypeToJSON(gps.fixType).replace(GPS_FIX_PREFIX, '') : 'NO DATA';
}

const COMPASS_POINTS = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];

/**
 * headingDeg is compass bearing (0 = north, clockwise); each point covers a
 * 45-degree wedge centered on it. Padded to a fixed 2-character width (with
 * a non-breaking space, so it survives HTML whitespace collapsing) since N/
 * E/S/W are one character and the intercardinal points are two - otherwise
 * the altitude/speed text after it visibly shifts as the heading changes.
 */
export function formatHeadingCompass(headingDeg: number | undefined): string {
	if (headingDeg === undefined || Number.isNaN(headingDeg)) return '? ';
	const normalized = ((headingDeg % 360) + 360) % 360;
	return COMPASS_POINTS[Math.round(normalized / 45) % 8].padEnd(2, ' ');
}
