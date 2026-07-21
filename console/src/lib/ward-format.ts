import { flightModeToJSON, gpsFixTypeToJSON } from '$lib/gen/karshipta/v1/common';
import type { Battery, FlightState, Gps } from '$lib/gen/karshipta/v1/telemetry';

const FLIGHT_MODE_PREFIX = 'FLIGHT_MODE_';
const GPS_FIX_PREFIX = 'GPS_FIX_TYPE_';

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

export function formatGpsLabel(gps: Gps | undefined): string {
	return gps ? gpsFixTypeToJSON(gps.fixType).replace(GPS_FIX_PREFIX, '') : 'NO DATA';
}
