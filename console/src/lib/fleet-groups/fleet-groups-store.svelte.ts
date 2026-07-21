import { fleet, type DraftWaypoint } from '$lib/fleet-store.svelte';
import { MissionAction } from '$lib/gen/karshipta/v1/command';
import {
	FleetAckStatus,
	type Fleet,
	type FleetAck,
	type FleetMissionAssignment
} from '$lib/gen/karshipta/v1/fleet';

const REQUEST_TIMEOUT_MS = 10_000;
/** trackers in a terminal state linger briefly so the operator sees the outcome */
const TRACKER_LINGER_MS = 6_000;
const DEFAULT_WAYPOINT_ALT_M = 30;
const WAYPOINT_ACCEPTANCE_RADIUS_M = 2;

export function isFleetConfigTerminal(status: FleetAckStatus): boolean {
	return (
		status === FleetAckStatus.FLEET_ACK_STATUS_ACCEPTED ||
		status === FleetAckStatus.FLEET_ACK_STATUS_REJECTED
	);
}

export interface FleetConfigTracker {
	requestId: string;
	kind: 'create' | 'rename' | 'delete' | 'addWard' | 'removeWard';
	/** undefined only for a still-in-flight 'create': the id doesn't exist until the ack assigns one */
	fleetId: string | undefined;
	status: FleetAckStatus;
	message: string;
	sentAtMs: number;
}

export interface MissionAssignmentDraft {
	/** undefined = an ad-hoc ward selection, not tied to a saved Fleet */
	fleetId: string | undefined;
	/** the chosen recipients, fixed for the life of this draft */
	wardIds: string[];
	waypoints: DraftWaypoint[];
	repeatCount: number;
}

/**
 * Named, persistent groupings of wards (fleet-mission-model.md) - a
 * different concept from the singular `fleet` store (FleetStore), which
 * means "everything this gateway/console instance manages". Deliberately
 * plural to avoid that identifier collision. Requests route through
 * `fleet.sendUpstream()` (whichever channel - gateway or demo engine - is
 * currently active) rather than holding its own transport reference;
 * `fleet-store.svelte.ts`'s applyEnvelope() routes the matching downstream
 * payload kinds here (see applyFleet/applyFleetAck below), so this store
 * never touches the wire directly either.
 */
class FleetGroupsStore {
	fleets = $state<Record<string, Fleet>>({});
	fleetConfigRequests = $state<Record<string, FleetConfigTracker>>({});
	/** local-only until "Assign" sends it as one FleetMissionAssignment; the
	 * gateway fans it out to each ward independently (fleet.proto's own
	 * comment), so this store never loops per-ward uploads itself. */
	missionAssignmentDraft = $state<MissionAssignmentDraft | undefined>(undefined);

	readonly fleetIds = $derived(
		Object.keys(this.fleets).sort((a, b) =>
			(this.fleets[a]?.name ?? '').localeCompare(this.fleets[b]?.name ?? '')
		)
	);

	private timeoutTimers = new Map<string, ReturnType<typeof setTimeout>>();

	requestCreateFleet(name: string, description: string): string {
		const requestId = crypto.randomUUID();
		this.track(requestId, 'create', undefined, () => ({
			payload: { $case: 'createFleet', createFleet: { requestId, name, description } }
		}));
		return requestId;
	}

	/** Updates both name and description together (see RenameFleet's own comment). */
	requestRenameFleet(fleetId: string, name: string, description: string): string {
		const requestId = crypto.randomUUID();
		this.track(requestId, 'rename', fleetId, () => ({
			payload: { $case: 'renameFleet', renameFleet: { requestId, fleetId, name, description } }
		}));
		return requestId;
	}

	requestDeleteFleet(fleetId: string): string {
		const requestId = crypto.randomUUID();
		this.track(requestId, 'delete', fleetId, () => ({
			payload: { $case: 'deleteFleet', deleteFleet: { requestId, fleetId } }
		}));
		return requestId;
	}

	requestAddWardToFleet(fleetId: string, wardId: string): string {
		const requestId = crypto.randomUUID();
		this.track(requestId, 'addWard', fleetId, () => ({
			payload: { $case: 'addWardToFleet', addWardToFleet: { requestId, fleetId, wardId } }
		}));
		return requestId;
	}

	requestRemoveWardFromFleet(fleetId: string, wardId: string): string {
		const requestId = crypto.randomUUID();
		this.track(requestId, 'removeWard', fleetId, () => ({
			payload: {
				$case: 'removeWardFromFleet',
				removeWardFromFleet: { requestId, fleetId, wardId }
			}
		}));
		return requestId;
	}

	configRequestsFor(fleetId: string): FleetConfigTracker[] {
		return Object.values(this.fleetConfigRequests)
			.filter((tracker) => tracker.fleetId === fleetId)
			.sort((a, b) => b.sentAtMs - a.sentAtMs);
	}

	/** Fleets a given ward belongs to (many-to-many), by name. */
	fleetsForWard(wardId: string): Fleet[] {
		return Object.values(this.fleets)
			.filter((candidate) => candidate.wardIds.includes(wardId))
			.sort((a, b) => a.name.localeCompare(b.name));
	}

	/** Ward ids from allWardIds that belong to no fleet at all. */
	unassignedWardIds(allWardIds: string[]): string[] {
		return allWardIds.filter(
			(wardId) =>
				!Object.values(this.fleets).some((candidate) => candidate.wardIds.includes(wardId))
		);
	}

	startMissionAssignment(fleetId: string | undefined, wardIds: string[]): void {
		this.missionAssignmentDraft = { fleetId, wardIds, waypoints: [], repeatCount: 0 };
	}

	cancelMissionAssignment(): void {
		this.missionAssignmentDraft = undefined;
	}

	/** called by the map on click while a mission assignment is being planned */
	addAssignmentWaypoint(latitudeDeg: number, longitudeDeg: number): void {
		const draft = this.missionAssignmentDraft;
		if (!draft) return;
		const previous = draft.waypoints.at(-1);
		draft.waypoints.push({
			latitudeDeg,
			longitudeDeg,
			altitudeRelM: previous?.altitudeRelM ?? DEFAULT_WAYPOINT_ALT_M
		});
	}

	removeAssignmentWaypoint(index: number): void {
		this.missionAssignmentDraft?.waypoints.splice(index, 1);
	}

	/** Sends the draft as one FleetMissionAssignment; no correlated ack
	 * exists for this request (mirrors solo Envelope.mission_upload, which
	 * has none either) - per-ward outcomes surface through the normal
	 * CommandAck/Event channels fleet-store.svelte.ts already displays. */
	assignMission(missionName: string): boolean {
		const draft = this.missionAssignmentDraft;
		if (!draft || draft.waypoints.length === 0 || draft.wardIds.length === 0) return false;
		const assignment: FleetMissionAssignment = {
			requestId: crypto.randomUUID(),
			fleetId: draft.fleetId ?? '',
			wardIds: draft.wardIds,
			missionName,
			repeatCount: draft.repeatCount,
			items: draft.waypoints.map((waypoint, index) => ({
				seq: index,
				action: MissionAction.MISSION_ACTION_WAYPOINT,
				position: {
					latitudeDeg: waypoint.latitudeDeg,
					longitudeDeg: waypoint.longitudeDeg,
					altitudeMslM: 0,
					altitudeRelM: waypoint.altitudeRelM
				},
				speedMS: 0,
				holdTimeS: 0,
				acceptanceRadiusM: WAYPOINT_ACCEPTANCE_RADIUS_M
			}))
		};
		fleet.sendUpstream({
			payload: { $case: 'fleetMissionAssignment', fleetMissionAssignment: assignment }
		});
		this.missionAssignmentDraft = undefined;
		return true;
	}

	/** Upsert from a downstream Fleet sync/update envelope. */
	applyFleet(value: Fleet): void {
		this.fleets[value.fleetId] = value;
	}

	/** Resolves the matching tracker; an accepted delete also drops the
	 * local Fleet object (there's no updated object to sync for a delete,
	 * unlike create/rename/membership, which the gateway re-broadcasts). */
	applyFleetAck(ack: FleetAck): void {
		const tracker = this.fleetConfigRequests[ack.requestId];
		if (!tracker) {
			console.warn(`fleetGroups: FleetAck for unknown request_id ${ack.requestId}`);
			return;
		}
		this.settle(ack.requestId, ack.status, ack.message);
		if (tracker.kind === 'delete' && ack.status === FleetAckStatus.FLEET_ACK_STATUS_ACCEPTED) {
			delete this.fleets[ack.fleetId];
		}
	}

	teardown(): void {
		for (const timer of this.timeoutTimers.values()) clearTimeout(timer);
		this.timeoutTimers.clear();
		this.fleets = {};
		this.fleetConfigRequests = {};
		this.missionAssignmentDraft = undefined;
	}

	private track(
		requestId: string,
		kind: FleetConfigTracker['kind'],
		fleetId: string | undefined,
		buildEnvelope: () => Parameters<typeof fleet.sendUpstream>[0]
	): void {
		this.fleetConfigRequests[requestId] = {
			requestId,
			kind,
			fleetId,
			status: FleetAckStatus.FLEET_ACK_STATUS_UNSPECIFIED,
			message: '',
			sentAtMs: Date.now()
		};
		this.timeoutTimers.set(
			requestId,
			setTimeout(() => {
				this.settle(
					requestId,
					FleetAckStatus.FLEET_ACK_STATUS_REJECTED,
					'no acknowledgment from gateway'
				);
			}, REQUEST_TIMEOUT_MS)
		);
		try {
			fleet.sendUpstream(buildEnvelope());
		} catch (error) {
			const reason = error instanceof Error ? error.message : String(error);
			this.settle(requestId, FleetAckStatus.FLEET_ACK_STATUS_REJECTED, reason);
		}
	}

	private settle(requestId: string, status: FleetAckStatus, message: string): void {
		const tracker = this.fleetConfigRequests[requestId];
		if (!tracker || isFleetConfigTerminal(tracker.status)) return;
		tracker.status = status;
		tracker.message = message;
		if (isFleetConfigTerminal(status)) {
			const timer = this.timeoutTimers.get(requestId);
			if (timer) {
				clearTimeout(timer);
				this.timeoutTimers.delete(requestId);
			}
			setTimeout(() => {
				delete this.fleetConfigRequests[requestId];
			}, TRACKER_LINGER_MS);
		}
	}
}

export const fleetGroups = new FleetGroupsStore();
