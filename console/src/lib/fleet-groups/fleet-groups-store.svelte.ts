import { fleet, type DraftWaypoint } from '$lib/fleet-store.svelte';
import { MissionAction } from '$lib/gen/karshipta/v1/command';
import {
	FleetAckStatus,
	FleetMissionAckStatus,
	FleetMissionStopAction,
	type Fleet,
	type FleetAck,
	type FleetMission,
	type FleetMissionAck,
	type WardMissionPlan
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

export function isFleetMissionAckTerminal(status: FleetMissionAckStatus): boolean {
	return (
		status === FleetMissionAckStatus.FLEET_MISSION_ACK_STATUS_ACCEPTED ||
		status === FleetMissionAckStatus.FLEET_MISSION_ACK_STATUS_REJECTED
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

export interface FleetMissionRequestTracker {
	requestId: string;
	kind: 'create' | 'stop' | 'remove' | 'update';
	fleetMissionId: string | undefined;
	status: FleetMissionAckStatus;
	message: string;
	sentAtMs: number;
}

/**
 * A fleet mission's Add/Edit wizard state: step 1 (pick a Fleet or ad-hoc
 * wards, fixed for the life of the draft) then step 2 (plan a SEPARATE
 * route per selected ward - the actual fix over the old design, which sent
 * one shared route to every ward at once, a real collision hazard).
 * activeWardId says which ward's route the map's click handler is currently
 * appending to; setActiveWard() is the "switch which ward you're planning
 * for" control the wizard's step 2 ward switcher calls.
 */
export interface MissionAssignmentDraft {
	/** undefined = an ad-hoc ward selection, not tied to a saved Fleet */
	fleetId: string | undefined;
	/** the chosen recipients, fixed for the life of this draft */
	wardIds: string[];
	routes: Record<string, DraftWaypoint[]>;
	activeWardId: string;
	repeatCount: number;
	/** set only when this draft is re-planning an existing FleetMission (Edit) */
	editingFleetMissionId: string | undefined;
}

/**
 * Named, persistent groupings of wards (fleet-mission-model.md) - a
 * different concept from the singular `fleet` store (FleetStore), which
 * means "everything this gateway/console instance manages". Deliberately
 * plural to avoid that identifier collision. Requests route through
 * `fleet.sendUpstream()` (whichever channel - gateway or demo engine - is
 * currently active) rather than holding its own transport reference;
 * `fleet-store.svelte.ts`'s applyEnvelope() routes the matching downstream
 * payload kinds here (see applyFleet/applyFleetAck/applyFleetMission/
 * applyFleetMissionAck below), so this store never touches the wire
 * directly either.
 */
class FleetGroupsStore {
	fleets = $state<Record<string, Fleet>>({});
	fleetConfigRequests = $state<Record<string, FleetConfigTracker>>({});
	fleetMissions = $state<Record<string, FleetMission>>({});
	fleetMissionRequests = $state<Record<string, FleetMissionRequestTracker>>({});
	missionAssignmentDraft = $state<MissionAssignmentDraft | undefined>(undefined);

	readonly fleetIds = $derived(
		Object.keys(this.fleets).sort((a, b) =>
			(this.fleets[a]?.name ?? '').localeCompare(this.fleets[b]?.name ?? '')
		)
	);

	readonly fleetMissionIds = $derived(
		Object.keys(this.fleetMissions).sort(
			(a, b) =>
				(this.fleetMissions[b]?.createdAtMs ?? 0) - (this.fleetMissions[a]?.createdAtMs ?? 0)
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

	missionRequestsFor(fleetMissionId: string): FleetMissionRequestTracker[] {
		return Object.values(this.fleetMissionRequests)
			.filter((tracker) => tracker.fleetMissionId === fleetMissionId)
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

	/** Step 1 -> step 2: one empty route per selected ward, planning starts on
	 * the first one. Call setActiveWard() to switch which ward's route the
	 * map's click handler appends to. */
	startMissionAssignment(fleetId: string | undefined, wardIds: string[]): void {
		if (wardIds.length === 0) return;
		const routes: Record<string, DraftWaypoint[]> = {};
		for (const wardId of wardIds) routes[wardId] = [];
		this.missionAssignmentDraft = {
			fleetId,
			wardIds,
			routes,
			activeWardId: wardIds[0],
			repeatCount: 0,
			editingFleetMissionId: undefined
		};
	}

	/** Edit: reopens the wizard directly at step 2, pre-filled from the
	 * mission's existing per-ward routes (wards are already fixed for an
	 * existing FleetMission - only routes can change). No-op if the id is
	 * unknown (e.g. it was removed by another client just as Edit was clicked). */
	startEditFleetMission(fleetMissionId: string): void {
		const mission = this.fleetMissions[fleetMissionId];
		if (!mission || mission.wardPlans.length === 0) return;
		const routes: Record<string, DraftWaypoint[]> = {};
		for (const plan of mission.wardPlans) {
			routes[plan.wardId] = plan.items.map((item) => ({
				latitudeDeg: item.position?.latitudeDeg ?? 0,
				longitudeDeg: item.position?.longitudeDeg ?? 0,
				altitudeRelM: item.position?.altitudeRelM || DEFAULT_WAYPOINT_ALT_M
			}));
		}
		this.missionAssignmentDraft = {
			fleetId: mission.fleetId || undefined,
			wardIds: mission.wardPlans.map((plan) => plan.wardId),
			routes,
			activeWardId: mission.wardPlans[0].wardId,
			repeatCount: mission.repeatCount,
			editingFleetMissionId: fleetMissionId
		};
	}

	cancelMissionAssignment(): void {
		this.missionAssignmentDraft = undefined;
	}

	/** Which ward's route step 2's waypoint list/map clicks currently target. */
	setActiveWard(wardId: string): void {
		const draft = this.missionAssignmentDraft;
		if (!draft || !(wardId in draft.routes)) return;
		draft.activeWardId = wardId;
	}

	/** called by the map on click while a mission assignment is being planned */
	addAssignmentWaypoint(latitudeDeg: number, longitudeDeg: number): void {
		const draft = this.missionAssignmentDraft;
		if (!draft) return;
		const route = draft.routes[draft.activeWardId];
		if (!route) return;
		const previous = route.at(-1);
		route.push({
			latitudeDeg,
			longitudeDeg,
			altitudeRelM: previous?.altitudeRelM ?? DEFAULT_WAYPOINT_ALT_M
		});
	}

	removeAssignmentWaypoint(index: number): void {
		const draft = this.missionAssignmentDraft;
		if (!draft) return;
		draft.routes[draft.activeWardId]?.splice(index, 1);
	}

	/** Sends the draft as CreateFleetMission (or UpdateFleetMissionRoutes if
	 * editingFleetMissionId is set), one WardMissionPlan per ward built from
	 * that ward's own routes[wardId] - never a flat shared list. Requires
	 * every selected ward to have at least one waypoint, so a ward can't be
	 * silently left with an empty route. */
	assignMission(missionName: string): boolean {
		const draft = this.missionAssignmentDraft;
		if (!draft || draft.wardIds.length === 0) return false;
		if (draft.wardIds.some((wardId) => (draft.routes[wardId]?.length ?? 0) === 0)) return false;

		const wardPlans: WardMissionPlan[] = draft.wardIds.map((wardId) => ({
			wardId,
			items: draft.routes[wardId].map((waypoint, index) => ({
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
		}));

		const requestId = crypto.randomUUID();
		if (draft.editingFleetMissionId) {
			const fleetMissionId = draft.editingFleetMissionId;
			this.trackMission(requestId, 'update', fleetMissionId, () => ({
				payload: {
					$case: 'updateFleetMissionRoutes',
					updateFleetMissionRoutes: {
						requestId,
						fleetMissionId,
						missionName,
						wardPlans,
						repeatCount: draft.repeatCount
					}
				}
			}));
		} else {
			this.trackMission(requestId, 'create', undefined, () => ({
				payload: {
					$case: 'createFleetMission',
					createFleetMission: {
						requestId,
						fleetId: draft.fleetId ?? '',
						missionName,
						wardPlans,
						repeatCount: draft.repeatCount
					}
				}
			}));
		}
		this.missionAssignmentDraft = undefined;
		return true;
	}

	/** action left undefined defaults to RTL (StopFleetMission's own comment). */
	requestStopFleetMission(fleetMissionId: string, action?: FleetMissionStopAction): string {
		const requestId = crypto.randomUUID();
		this.trackMission(requestId, 'stop', fleetMissionId, () => ({
			payload: {
				$case: 'stopFleetMission',
				stopFleetMission: {
					requestId,
					fleetMissionId,
					action: action ?? FleetMissionStopAction.FLEET_MISSION_STOP_ACTION_RTL
				}
			}
		}));
		return requestId;
	}

	requestRemoveFleetMission(fleetMissionId: string): string {
		const requestId = crypto.randomUUID();
		this.trackMission(requestId, 'remove', fleetMissionId, () => ({
			payload: { $case: 'removeFleetMission', removeFleetMission: { requestId, fleetMissionId } }
		}));
		return requestId;
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

	/** Upsert from a downstream FleetMission sync/update envelope. */
	applyFleetMission(value: FleetMission): void {
		this.fleetMissions[value.fleetMissionId] = value;
	}

	/** Resolves the matching tracker; an accepted remove also drops the
	 * local FleetMission object (there's no updated object to sync for a
	 * remove, unlike create/stop/update, which the gateway re-broadcasts). */
	applyFleetMissionAck(ack: FleetMissionAck): void {
		const tracker = this.fleetMissionRequests[ack.requestId];
		if (!tracker) {
			console.warn(`fleetGroups: FleetMissionAck for unknown request_id ${ack.requestId}`);
			return;
		}
		this.settleMission(ack.requestId, ack.status, ack.message);
		if (
			tracker.kind === 'remove' &&
			ack.status === FleetMissionAckStatus.FLEET_MISSION_ACK_STATUS_ACCEPTED
		) {
			delete this.fleetMissions[ack.fleetMissionId];
		}
	}

	teardown(): void {
		for (const timer of this.timeoutTimers.values()) clearTimeout(timer);
		this.timeoutTimers.clear();
		this.fleets = {};
		this.fleetConfigRequests = {};
		this.fleetMissions = {};
		this.fleetMissionRequests = {};
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

	/** Same shape as track()/settle() above, parallel rather than shared:
	 * FleetAckStatus and FleetMissionAckStatus are distinct generated enum
	 * types, and this codebase's own convention (see fleet_manager.cpp's
	 * duplicated helpers) is to duplicate a small piece like this per call
	 * site rather than force a generic across two otherwise-unrelated types. */
	private trackMission(
		requestId: string,
		kind: FleetMissionRequestTracker['kind'],
		fleetMissionId: string | undefined,
		buildEnvelope: () => Parameters<typeof fleet.sendUpstream>[0]
	): void {
		this.fleetMissionRequests[requestId] = {
			requestId,
			kind,
			fleetMissionId,
			status: FleetMissionAckStatus.FLEET_MISSION_ACK_STATUS_UNSPECIFIED,
			message: '',
			sentAtMs: Date.now()
		};
		this.timeoutTimers.set(
			requestId,
			setTimeout(() => {
				this.settleMission(
					requestId,
					FleetMissionAckStatus.FLEET_MISSION_ACK_STATUS_REJECTED,
					'no acknowledgment from gateway'
				);
			}, REQUEST_TIMEOUT_MS)
		);
		try {
			fleet.sendUpstream(buildEnvelope());
		} catch (error) {
			const reason = error instanceof Error ? error.message : String(error);
			this.settleMission(
				requestId,
				FleetMissionAckStatus.FLEET_MISSION_ACK_STATUS_REJECTED,
				reason
			);
		}
	}

	private settleMission(requestId: string, status: FleetMissionAckStatus, message: string): void {
		const tracker = this.fleetMissionRequests[requestId];
		if (!tracker || isFleetMissionAckTerminal(tracker.status)) return;
		tracker.status = status;
		tracker.message = message;
		if (isFleetMissionAckTerminal(status)) {
			const timer = this.timeoutTimers.get(requestId);
			if (timer) {
				clearTimeout(timer);
				this.timeoutTimers.delete(requestId);
			}
			setTimeout(() => {
				delete this.fleetMissionRequests[requestId];
			}, TRACKER_LINGER_MS);
		}
	}
}

export const fleetGroups = new FleetGroupsStore();
