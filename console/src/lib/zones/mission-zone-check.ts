import { ZoneType } from '$lib/gen/karshipta/v1/fleet';
import { zoneStore } from './zone-store.svelte';

/**
 * v0 is advisory only (fleet.proto's own comment on Zone): a waypoint
 * inside a keep-out Zone, or outside every keep-in Zone (only checked when
 * at least one keep-in Zone exists at all - with none drawn, there is no
 * "outside" to violate), produces a warning string for the operator to see
 * before upload/assignment. It never blocks the action. Mirrors the
 * existing geozoneStore-based check in mission-panel.svelte's
 * `startWarning`, extended to the operator's own drawn zones.
 *
 * Takes plain points rather than MissionItem[] so both callers - solo
 * missions (MissionItem.position) and fleet mission-assignment drafts
 * (DraftWaypoint, no MissionItem wrapper yet at that stage) - can use it
 * without an intermediate conversion.
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
