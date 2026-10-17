import { ZoneType } from '$lib/gen/karshipta/v1/fleet';
import { zoneStore } from './zone-store.svelte';

/**
 * This plan-time check is still advisory only: a waypoint inside a
 * keep-out Zone, or outside every keep-in Zone (only checked when at least
 * one keep-in Zone exists at all - with none drawn, there is no "outside"
 * to violate), produces a warning string for the operator to see before
 * upload/assignment, but never blocks the action. Called from
 * fleet-mission-wizard.svelte against the active draft's own routes
 * (DraftWaypoint[], no MissionItem wrapper at that stage - the per-ward
 * Mission panel this check originally also served was removed when solo
 * missions were folded into the fleet-wide wizard's own single-ward
 * support).
 *
 * This is no longer the only line of defense, though (karshipta#247): the
 * gateway now also pushes every zone's horizontal boundary to each ward's
 * autopilot as a real MAVSDK geofence, independent of any mission plan, so
 * the autopilot itself rejects a boundary violation even if this warning
 * is ignored. Altitude bounds have no such backstop yet (karshipta#251) -
 * this warning is genuinely the only check for those.
 *
 * Takes plain points rather than DraftWaypoint[] directly so a future
 * caller with a differently-shaped route doesn't need an intermediate
 * conversion just to reuse this check.
 */
export function checkMissionAgainstZones(
	points: { latitudeDeg: number; longitudeDeg: number }[]
): string | undefined {
	const zones = Object.values(zoneStore.zones);
	const keepInZones = zones.filter((zone) => zone.type === ZoneType.ZONE_TYPE_KEEP_IN);

	const keepOutNames = new Set<string>();
	let outsideKeepInCount = 0;

	for (const point of points) {
		const containing = zoneStore.containsPoint(point.latitudeDeg, point.longitudeDeg);
		for (const zone of containing) {
			if (zone.type === ZoneType.ZONE_TYPE_KEEP_OUT) keepOutNames.add(zone.name);
		}
		if (
			keepInZones.length > 0 &&
			!containing.some((zone) => zone.type === ZoneType.ZONE_TYPE_KEEP_IN)
		) {
			outsideKeepInCount += 1;
		}
	}

	const messages: string[] = [];
	if (keepOutNames.size > 0) {
		messages.push(
			`Route crosses keep-out zone${keepOutNames.size === 1 ? '' : 's'} ${[...keepOutNames].join(', ')}.`
		);
	}
	if (outsideKeepInCount > 0) {
		messages.push(
			`${outsideKeepInCount} waypoint${outsideKeepInCount === 1 ? '' : 's'} outside every keep-in zone.`
		);
	}
	return messages.length > 0 ? messages.join(' ') : undefined;
}
