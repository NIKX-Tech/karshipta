import type { Envelope } from '$lib/gen/karshipta/v1/envelope';
import type { Event, VehicleInfo, VehicleState } from '$lib/gen/karshipta/v1/telemetry';
import {
	CommandStatus,
	MissionAction,
	type Command,
	type Mission,
	type MissionProgress
} from '$lib/gen/karshipta/v1/command';

export interface Vehicle {
	info: VehicleInfo | undefined;
	state: VehicleState | undefined;
}

export interface DraftWaypoint {
	latitudeDeg: number;
	longitudeDeg: number;
	altitudeRelM: number;
}

export interface MissionDraft {
	vehicleId: string;
	waypoints: DraftWaypoint[];
	repeatCount: number;
}

const DEFAULT_WAYPOINT_ALT_M = 30;
const WAYPOINT_ACCEPTANCE_RADIUS_M = 2;

export interface CommandTracker {
	commandId: string;
	vehicleId: string;
	/** the action oneof case, e.g. "takeoff" */
	kind: string;
	status: CommandStatus;
	message: string;
	sentAtMs: number;
}

const MAX_EVENTS = 50;
const COMMAND_TIMEOUT_MS = 10_000;
/** trackers in a terminal state linger briefly so the operator sees the outcome */
const TRACKER_LINGER_MS = 6_000;

export function isTerminal(status: CommandStatus): boolean {
	return (
		status === CommandStatus.COMMAND_STATUS_SUCCESS ||
		status === CommandStatus.COMMAND_STATUS_REJECTED ||
		status === CommandStatus.COMMAND_STATUS_TIMEOUT
	);
}

/**
 * Single owner of all live fleet state, keyed by vehicle_id. Everything that
 * arrives from the gateway (or the FakeGateway in dev) enters through
 * applyEnvelope; every outgoing Command leaves through sendCommand, which
 * assigns the uuid and tracks the CommandAck lifecycle. Components only read.
 */
class FleetStore {
	vehicles = $state<Record<string, Vehicle>>({});
	events = $state<Event[]>([]);
	commands = $state<Record<string, CommandTracker>>({});
	selectedVehicleId = $state<string | undefined>(undefined);
	/** goto targeting: the panel arms it, the map's next click supplies the point */
	gotoArming = $state(false);
	pendingGoto = $state<{ latitudeDeg: number; longitudeDeg: number } | undefined>(undefined);
	/** gateway link as the operator should read it; set by the page wiring */
	link = $state<'live' | 'sim' | 'connecting' | 'down'>('down');
	/** read-only session: telemetry flows, commanding is refused observably */
	readonly = $state(false);
	/** mission being planned; while set, map clicks add waypoints for its vehicle */
	missionDraft = $state<MissionDraft | undefined>(undefined);
	/** last uploaded mission per vehicle, so Start knows what it refers to */
	uploadedMissions = $state<Record<string, Mission>>({});
	missionProgress = $state<Record<string, MissionProgress>>({});

	readonly vehicleIds = $derived(Object.keys(this.vehicles).sort());
	readonly selectedVehicle = $derived(
		this.selectedVehicleId !== undefined ? this.vehicles[this.selectedVehicleId] : undefined
	);

	private sender: ((envelope: Envelope) => void) | undefined;
	// timers are bookkeeping, not state
	private timeoutTimers = new Map<string, ReturnType<typeof setTimeout>>();

	bindSender(sender: (envelope: Envelope) => void): void {
		this.sender = sender;
	}

	select(vehicleId: string | undefined): void {
		this.selectedVehicleId = vehicleId;
		this.cancelGoto();
		if (this.missionDraft && this.missionDraft.vehicleId !== vehicleId) {
			this.cancelPlanning();
		}
	}

	armGoto(): void {
		this.gotoArming = true;
	}

	cancelGoto(): void {
		this.gotoArming = false;
		this.pendingGoto = undefined;
	}

	/** called by the map on click while goto targeting is armed */
	requestGoto(latitudeDeg: number, longitudeDeg: number): void {
		if (!this.gotoArming) return;
		this.gotoArming = false;
		this.pendingGoto = { latitudeDeg, longitudeDeg };
	}

	startPlanning(vehicleId: string): void {
		this.cancelGoto();
		this.missionDraft = { vehicleId, waypoints: [], repeatCount: 0 };
	}

	cancelPlanning(): void {
		this.missionDraft = undefined;
	}

	/** called by the map on click while a mission is being planned */
	addWaypoint(latitudeDeg: number, longitudeDeg: number): void {
		const draft = this.missionDraft;
		if (!draft) return;
		const previous = draft.waypoints.at(-1);
		draft.waypoints.push({
			latitudeDeg,
			longitudeDeg,
			altitudeRelM: previous?.altitudeRelM ?? DEFAULT_WAYPOINT_ALT_M
		});
	}

	removeWaypoint(index: number): void {
		this.missionDraft?.waypoints.splice(index, 1);
	}

	/** sends the draft as a Mission envelope and remembers it for Start */
	uploadMission(): Mission | undefined {
		const draft = this.missionDraft;
		if (!draft || draft.waypoints.length === 0) return undefined;
		const mission: Mission = {
			missionId: crypto.randomUUID(),
			vehicleId: draft.vehicleId,
			name: `mission-${new Date().toISOString().slice(11, 19)}`,
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
		if (!this.sender) {
			console.error('fleet: cannot upload mission, no transport bound');
			return undefined;
		}
		this.sender({ payload: { $case: 'missionUpload', missionUpload: mission } });
		this.uploadedMissions[draft.vehicleId] = mission;
		delete this.missionProgress[draft.vehicleId];
		return mission;
	}

	sendCommand(vehicleId: string, action: NonNullable<Command['action']>): string {
		if (this.readonly) {
			// belt and braces: viewer mode hides the command UI, but any path
			// that still sends must settle observably, never silently
			const refusedId = crypto.randomUUID();
			this.commands[refusedId] = {
				commandId: refusedId,
				vehicleId,
				kind: action.$case,
				status: CommandStatus.COMMAND_STATUS_REJECTED,
				message: 'read-only session',
				sentAtMs: Date.now()
			};
			return refusedId;
		}
		const command: Command = {
			commandId: crypto.randomUUID(),
			vehicleId,
			timestampMs: Date.now(),
			action
		};
		this.commands[command.commandId] = {
			commandId: command.commandId,
			vehicleId,
			kind: action.$case,
			status: CommandStatus.COMMAND_STATUS_UNSPECIFIED,
			message: '',
			sentAtMs: command.timestampMs
		};
		if (!this.sender) {
			this.settle(command.commandId, CommandStatus.COMMAND_STATUS_REJECTED, 'no transport bound');
			return command.commandId;
		}
		this.timeoutTimers.set(
			command.commandId,
			setTimeout(() => {
				this.settle(
					command.commandId,
					CommandStatus.COMMAND_STATUS_TIMEOUT,
					'no acknowledgment from gateway'
				);
			}, COMMAND_TIMEOUT_MS)
		);
		try {
			this.sender({ payload: { $case: 'command', command } });
		} catch (error) {
			const reason = error instanceof Error ? error.message : String(error);
			this.settle(command.commandId, CommandStatus.COMMAND_STATUS_REJECTED, reason);
		}
		return command.commandId;
	}

	/** in-flight and recently settled commands for one vehicle, newest first */
	commandsFor(vehicleId: string): CommandTracker[] {
		return Object.values(this.commands)
			.filter((tracker) => tracker.vehicleId === vehicleId)
			.sort((a, b) => b.sentAtMs - a.sentAtMs);
	}

	applyEnvelope(envelope: Envelope): void {
		const payload = envelope.payload;
		if (payload === undefined) {
			console.warn('fleet: dropping Envelope with empty payload');
			return;
		}
		switch (payload.$case) {
			case 'vehicleInfo':
				this.upsert(payload.vehicleInfo.vehicleId).info = payload.vehicleInfo;
				break;
			case 'vehicleState':
				this.upsert(payload.vehicleState.vehicleId).state = payload.vehicleState;
				break;
			case 'event':
				this.events = [payload.event, ...this.events].slice(0, MAX_EVENTS);
				break;
			case 'commandAck': {
				const ack = payload.commandAck;
				const tracker = this.commands[ack.commandId];
				if (!tracker) {
					console.warn(`fleet: CommandAck for unknown command_id ${ack.commandId}`);
					break;
				}
				this.settle(ack.commandId, ack.status, ack.message);
				break;
			}
			case 'missionProgress':
				this.missionProgress[payload.missionProgress.vehicleId] = payload.missionProgress;
				break;
			case 'command':
			case 'missionUpload':
				console.warn(`fleet: ignoring upstream payload kind ${payload.$case} sent downstream`);
				break;
			default: {
				const unhandled: never = payload;
				console.warn('fleet: ignoring unknown payload kind', unhandled);
			}
		}
	}

	clear(): void {
		for (const timer of this.timeoutTimers.values()) clearTimeout(timer);
		this.timeoutTimers.clear();
		this.vehicles = {};
		this.events = [];
		this.commands = {};
		this.selectedVehicleId = undefined;
		this.missionDraft = undefined;
		this.uploadedMissions = {};
		this.missionProgress = {};
		this.cancelGoto();
	}

	private settle(commandId: string, status: CommandStatus, message: string): void {
		const tracker = this.commands[commandId];
		if (!tracker || isTerminal(tracker.status)) return;
		tracker.status = status;
		tracker.message = message;
		if (isTerminal(status)) {
			const timer = this.timeoutTimers.get(commandId);
			if (timer) {
				clearTimeout(timer);
				this.timeoutTimers.delete(commandId);
			}
			setTimeout(() => {
				delete this.commands[commandId];
			}, TRACKER_LINGER_MS);
		}
	}

	private upsert(vehicleId: string): Vehicle {
		if (!(vehicleId in this.vehicles)) {
			this.vehicles[vehicleId] = { info: undefined, state: undefined };
		}
		return this.vehicles[vehicleId];
	}
}

export const fleet = new FleetStore();
