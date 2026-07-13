import type { Envelope } from '$lib/gen/karshipta/v1/envelope';
import type { Event, VehicleInfo, VehicleState } from '$lib/gen/karshipta/v1/telemetry';
import {
	CommandStatus,
	MissionAction,
	type Command,
	type Mission,
	type MissionProgress
} from '$lib/gen/karshipta/v1/command';
import { VehicleConfigStatus, type AddVehicle } from '$lib/gen/karshipta/v1/fleet';
import type { VehicleType } from '$lib/gen/karshipta/v1/common';
import { WebSocketTransport, type FleetTransport } from '$lib/transport';
import type { DemoEngine } from '$lib/fake/fleet-sim';

/** where a vehicle's data comes from: the local demo engine or a connected gateway */
export type VehicleSource = 'demo' | 'gateway';

export interface Vehicle {
	info: VehicleInfo | undefined;
	state: VehicleState | undefined;
	source: VehicleSource;
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

export interface VehicleConfigTracker {
	requestId: string;
	kind: 'add' | 'remove';
	vehicleId: string;
	status: VehicleConfigStatus;
	message: string;
	sentAtMs: number;
}

const MAX_EVENTS = 50;
const REQUEST_TIMEOUT_MS = 10_000;
/** trackers in a terminal state linger briefly so the operator sees the outcome */
const TRACKER_LINGER_MS = 6_000;
const GATEWAY_URL_STORAGE_KEY = 'karshipta.gatewayUrl';

export function isTerminal(status: CommandStatus): boolean {
	return (
		status === CommandStatus.COMMAND_STATUS_SUCCESS ||
		status === CommandStatus.COMMAND_STATUS_REJECTED ||
		status === CommandStatus.COMMAND_STATUS_TIMEOUT
	);
}

export function isConfigTerminal(status: VehicleConfigStatus): boolean {
	return (
		status === VehicleConfigStatus.VEHICLE_CONFIG_STATUS_ACCEPTED ||
		status === VehicleConfigStatus.VEHICLE_CONFIG_STATUS_REJECTED
	);
}

function readStoredGatewayUrl(): string | undefined {
	if (typeof localStorage === 'undefined') return undefined;
	return localStorage.getItem(GATEWAY_URL_STORAGE_KEY) ?? undefined;
}

/**
 * Single owner of all live fleet state, keyed by vehicle_id. Two independent
 * channels feed it: the local demo engine (always available, instant, never
 * touches a network) and an optionally connected gateway (real hardware or
 * simulated vehicles behind a real gateway process). Every vehicle carries
 * which channel it came from, so commands and mission uploads route to the
 * right one and disconnecting the gateway never touches demo vehicles.
 * Components only read; connection and vehicle lifecycle are explicit
 * actions, never automatic.
 */
class FleetStore {
	vehicles = $state<Record<string, Vehicle>>({});
	events = $state<Event[]>([]);
	commands = $state<Record<string, CommandTracker>>({});
	vehicleConfigRequests = $state<Record<string, VehicleConfigTracker>>({});
	selectedVehicleId = $state<string | undefined>(undefined);
	/** goto targeting: the panel arms it, the map's next click supplies the point */
	gotoArming = $state(false);
	pendingGoto = $state<{ latitudeDeg: number; longitudeDeg: number } | undefined>(undefined);
	/** gateway connection as the operator should read it */
	link = $state<'live' | 'connecting' | 'down'>('down');
	gatewayUrl = $state(readStoredGatewayUrl() ?? '');
	/** read-only session: telemetry flows, commanding is refused observably */
	readonly = $state(false);
	/** how many simulated-vehicle adds this session has requested; drives the resource warning at C7's UI layer */
	simulatedVehicleAddCount = $state(0);
	/** mission being planned; while set, map clicks add waypoints for its vehicle */
	missionDraft = $state<MissionDraft | undefined>(undefined);
	/** last uploaded mission per vehicle, so Start knows what it refers to */
	uploadedMissions = $state<Record<string, Mission>>({});
	missionProgress = $state<Record<string, MissionProgress>>({});

	readonly vehicleIds = $derived(Object.keys(this.vehicles).sort());
	readonly selectedVehicle = $derived(
		this.selectedVehicleId !== undefined ? this.vehicles[this.selectedVehicleId] : undefined
	);
	readonly gatewayConnected = $derived(this.link === 'live');

	private demoEngine: DemoEngine | undefined;
	private gatewayTransport: FleetTransport | undefined;
	// timers are bookkeeping, not state
	private timeoutTimers = new Map<string, ReturnType<typeof setTimeout>>();

	/** wires the always-available local demo engine; call once at app start */
	bindDemoEngine(engine: DemoEngine): void {
		this.demoEngine = engine;
		engine.start();
	}

	connectGateway(url: string): void {
		this.disconnectGateway();
		this.gatewayUrl = url;
		if (typeof localStorage !== 'undefined') {
			localStorage.setItem(GATEWAY_URL_STORAGE_KEY, url);
		}
		this.link = 'connecting';
		const transport = new WebSocketTransport(url, {
			onEnvelope: (envelope) => this.applyEnvelope(envelope, 'gateway'),
			onStatus: (status) => this.setGatewayStatus(status)
		});
		this.gatewayTransport = transport;
		transport.start();
	}

	disconnectGateway(): void {
		this.gatewayTransport?.stop();
		this.gatewayTransport = undefined;
		this.link = 'down';
		for (const [id, vehicle] of Object.entries(this.vehicles)) {
			if (vehicle.source === 'gateway') delete this.vehicles[id];
		}
		if (this.selectedVehicleId && !(this.selectedVehicleId in this.vehicles)) {
			this.select(undefined);
		}
	}

	setGatewayStatus(status: 'connecting' | 'open' | 'closed'): void {
		this.link = status === 'open' ? 'live' : status === 'connecting' ? 'connecting' : 'down';
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
		const channel = this.channelFor(draft.vehicleId);
		if (!channel) {
			console.error(`fleet: cannot upload mission, ${draft.vehicleId} has no active channel`);
			return undefined;
		}
		channel.send({ payload: { $case: 'missionUpload', missionUpload: mission } });
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
		const channel = this.channelFor(vehicleId);
		if (!channel) {
			this.settle(command.commandId, CommandStatus.COMMAND_STATUS_REJECTED, 'no active channel');
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
			}, REQUEST_TIMEOUT_MS)
		);
		try {
			channel.send({ payload: { $case: 'command', command } });
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

	/**
	 * Adds a vehicle through the connected gateway (covers both a simulated
	 * SITL endpoint and real hardware; the gateway sees no difference). No
	 * gateway connection means nothing to send to, reported immediately.
	 */
	noteSimulatedVehicleRequested(): void {
		this.simulatedVehicleAddCount += 1;
	}

	requestAddVehicle(spec: {
		vehicleId: string;
		name: string;
		type: VehicleType;
		connectionUrl: string;
		mavlinkSystemId: number;
	}): string {
		const requestId = crypto.randomUUID();
		const addVehicle: AddVehicle = { requestId, ...spec };
		this.trackConfigRequest(requestId, 'add', spec.vehicleId, () => ({
			payload: { $case: 'addVehicle', addVehicle }
		}));
		return requestId;
	}

	requestRemoveVehicle(vehicleId: string): string {
		const requestId = crypto.randomUUID();
		this.trackConfigRequest(requestId, 'remove', vehicleId, () => ({
			payload: { $case: 'removeVehicle', removeVehicle: { requestId, vehicleId } }
		}));
		return requestId;
	}

	configRequestsFor(vehicleId: string): VehicleConfigTracker[] {
		return Object.values(this.vehicleConfigRequests)
			.filter((tracker) => tracker.vehicleId === vehicleId)
			.sort((a, b) => b.sentAtMs - a.sentAtMs);
	}

	/** spawns a new demo vehicle purely client-side; never touches a network */
	addDemoVehicle(): void {
		this.demoEngine?.spawnVehicle();
	}

	removeDemoVehicle(vehicleId: string): void {
		this.demoEngine?.despawnVehicle(vehicleId);
		delete this.vehicles[vehicleId];
		if (this.selectedVehicleId === vehicleId) this.select(undefined);
	}

	applyEnvelope(envelope: Envelope, source: VehicleSource): void {
		const payload = envelope.payload;
		if (payload === undefined) {
			console.warn('fleet: dropping Envelope with empty payload');
			return;
		}
		switch (payload.$case) {
			case 'vehicleInfo':
				this.upsert(payload.vehicleInfo.vehicleId, source).info = payload.vehicleInfo;
				break;
			case 'vehicleState':
				this.upsert(payload.vehicleState.vehicleId, source).state = payload.vehicleState;
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
			case 'vehicleConfigAck': {
				const ack = payload.vehicleConfigAck;
				const tracker = this.vehicleConfigRequests[ack.requestId];
				if (!tracker) {
					console.warn(`fleet: VehicleConfigAck for unknown request_id ${ack.requestId}`);
					break;
				}
				this.settleConfig(ack.requestId, ack.status, ack.message);
				if (
					tracker.kind === 'remove' &&
					ack.status === VehicleConfigStatus.VEHICLE_CONFIG_STATUS_ACCEPTED
				) {
					delete this.vehicles[ack.vehicleId];
					if (this.selectedVehicleId === ack.vehicleId) this.select(undefined);
				}
				break;
			}
			case 'command':
			case 'missionUpload':
			case 'addVehicle':
			case 'removeVehicle':
				console.warn(`fleet: ignoring upstream payload kind ${payload.$case} sent downstream`);
				break;
			default: {
				const unhandled: never = payload;
				console.warn('fleet: ignoring unknown payload kind', unhandled);
			}
		}
	}

	/** full app teardown: stops both channels and wipes everything */
	teardown(): void {
		this.demoEngine?.stop();
		this.demoEngine = undefined;
		this.disconnectGateway();
		for (const timer of this.timeoutTimers.values()) clearTimeout(timer);
		this.timeoutTimers.clear();
		this.vehicles = {};
		this.events = [];
		this.commands = {};
		this.vehicleConfigRequests = {};
		this.selectedVehicleId = undefined;
		this.missionDraft = undefined;
		this.uploadedMissions = {};
		this.missionProgress = {};
		this.cancelGoto();
	}

	private channelFor(vehicleId: string): FleetTransport | undefined {
		const source = this.vehicles[vehicleId]?.source;
		if (source === 'demo') return this.demoEngine;
		if (source === 'gateway') return this.gatewayTransport;
		return undefined;
	}

	private trackConfigRequest(
		requestId: string,
		kind: 'add' | 'remove',
		vehicleId: string,
		buildEnvelope: () => Envelope
	): void {
		this.vehicleConfigRequests[requestId] = {
			requestId,
			kind,
			vehicleId,
			status: VehicleConfigStatus.VEHICLE_CONFIG_STATUS_UNSPECIFIED,
			message: '',
			sentAtMs: Date.now()
		};
		if (!this.gatewayTransport) {
			this.settleConfig(
				requestId,
				VehicleConfigStatus.VEHICLE_CONFIG_STATUS_REJECTED,
				'no gateway connected'
			);
			return;
		}
		this.timeoutTimers.set(
			requestId,
			setTimeout(() => {
				this.settleConfig(
					requestId,
					VehicleConfigStatus.VEHICLE_CONFIG_STATUS_REJECTED,
					'no acknowledgment from gateway'
				);
			}, REQUEST_TIMEOUT_MS)
		);
		try {
			this.gatewayTransport.send(buildEnvelope());
		} catch (error) {
			const reason = error instanceof Error ? error.message : String(error);
			this.settleConfig(requestId, VehicleConfigStatus.VEHICLE_CONFIG_STATUS_REJECTED, reason);
		}
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

	private settleConfig(requestId: string, status: VehicleConfigStatus, message: string): void {
		const tracker = this.vehicleConfigRequests[requestId];
		if (!tracker || isConfigTerminal(tracker.status)) return;
		tracker.status = status;
		tracker.message = message;
		if (isConfigTerminal(status)) {
			const timer = this.timeoutTimers.get(requestId);
			if (timer) {
				clearTimeout(timer);
				this.timeoutTimers.delete(requestId);
			}
			setTimeout(() => {
				delete this.vehicleConfigRequests[requestId];
			}, TRACKER_LINGER_MS);
		}
	}

	private upsert(vehicleId: string, source: VehicleSource): Vehicle {
		if (!(vehicleId in this.vehicles)) {
			this.vehicles[vehicleId] = { info: undefined, state: undefined, source };
		}
		return this.vehicles[vehicleId];
	}
}

export const fleet = new FleetStore();
