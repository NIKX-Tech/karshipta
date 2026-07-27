import type { Envelope } from '$lib/gen/karshipta/v1/envelope';
import type { Event, WardInfo, WardState } from '$lib/gen/karshipta/v1/telemetry';
import {
	CommandStatus,
	MissionAction,
	type Command,
	type Mission,
	type MissionProgress
} from '$lib/gen/karshipta/v1/command';
import { WardConfigStatus, type AddWard } from '$lib/gen/karshipta/v1/fleet';
import { Severity, type WardClass } from '$lib/gen/karshipta/v1/common';
import { WebSocketTransport, type FleetTransport } from '$lib/transport';
import { RelayTransport } from '$lib/transport/relay-transport';
import { FAKE_FLEET_CENTER, type DemoEngine } from '$lib/fake/fleet-sim';
import type { LatLon } from '$lib/geolocation';
import { fleetGroups } from '$lib/fleet-groups/fleet-groups-store.svelte';
import { zoneStore } from '$lib/zones/zone-store.svelte';

/** where a ward's data comes from: the local demo engine or a connected gateway */
export type WardSource = 'demo' | 'gateway';

export interface Ward {
	info: WardInfo | undefined;
	state: WardState | undefined;
	source: WardSource;
}

export interface DraftWaypoint {
	latitudeDeg: number;
	longitudeDeg: number;
	altitudeRelM: number;
}

export interface MissionDraft {
	wardId: string;
	waypoints: DraftWaypoint[];
	repeatCount: number;
}

const DEFAULT_WAYPOINT_ALT_M = 30;
const WAYPOINT_ACCEPTANCE_RADIUS_M = 2;

export interface CommandTracker {
	commandId: string;
	wardId: string;
	/** the action oneof case, e.g. "takeoff" */
	kind: string;
	status: CommandStatus;
	message: string;
	sentAtMs: number;
}

export interface WardConfigTracker {
	requestId: string;
	kind: 'add' | 'remove';
	wardId: string;
	status: WardConfigStatus;
	message: string;
	sentAtMs: number;
}

const MAX_EVENTS = 50;
const REQUEST_TIMEOUT_MS = 10_000;
/** trackers in a terminal state linger briefly so the operator sees the outcome */
const TRACKER_LINGER_MS = 6_000;
const GATEWAY_URL_STORAGE_KEY = 'karshipta.gatewayUrl';
const RELAY_URL_STORAGE_KEY = 'karshipta.relayUrl';
const RELAY_DEVICE_ID_STORAGE_KEY = 'karshipta.relayDeviceId';
// Same tier of protection as the gateway's own relay_credentials.yaml
// (gitignored, plaintext on disk, see gateway/docs/relay-transport.md) -
// stored locally, never committed anywhere, not hidden from the browser
// environment itself. There is no server-side session store this console
// could use instead (self-hosted, static app).
const RELAY_DEVICE_TOKEN_STORAGE_KEY = 'karshipta.relayDeviceToken';
/**
 * Each demo ward's home location, not just how many there were: a plain
 * count (this key's previous shape) loses exactly the thing spawnWard
 * now needs to reproduce a ward where the operator actually put it -
 * respawning N wards with no location silently drops them all back to
 * FAKE_FLEET_CENTER, which is what happened before this fix (deploy in
 * Amsterdam, refresh, ward is back in Zurich).
 */
const DEMO_WARD_HOMES_STORAGE_KEY = 'karshipta.demoWardHomes';

export function isTerminal(status: CommandStatus): boolean {
	return (
		status === CommandStatus.COMMAND_STATUS_SUCCESS ||
		status === CommandStatus.COMMAND_STATUS_REJECTED ||
		status === CommandStatus.COMMAND_STATUS_TIMEOUT
	);
}

export function isConfigTerminal(status: WardConfigStatus): boolean {
	return (
		status === WardConfigStatus.WARD_CONFIG_STATUS_ACCEPTED ||
		status === WardConfigStatus.WARD_CONFIG_STATUS_REJECTED
	);
}

function readStoredGatewayUrl(): string | undefined {
	if (typeof localStorage === 'undefined') return undefined;
	return localStorage.getItem(GATEWAY_URL_STORAGE_KEY) ?? undefined;
}

function readStoredRelayUrl(): string | undefined {
	if (typeof localStorage === 'undefined') return undefined;
	return localStorage.getItem(RELAY_URL_STORAGE_KEY) ?? undefined;
}

/**
 * The relay device id only needs to stay stable across sessions once
 * chosen - generated once, then persisted, the same way the device keypair
 * itself is persisted inside relay-transport.ts. The matching device_token
 * (see readStoredRelayDeviceToken below) is provisioned out of band against
 * a real relayly server (POST /api/v1/devices or its CLI, same as the
 * gateway side - gateway/docs/relay-transport.md), not minted here.
 */
function readOrCreateRelayDeviceId(): string {
	if (typeof localStorage === 'undefined') return crypto.randomUUID();
	const saved = localStorage.getItem(RELAY_DEVICE_ID_STORAGE_KEY);
	if (saved) return saved;
	const id = crypto.randomUUID();
	localStorage.setItem(RELAY_DEVICE_ID_STORAGE_KEY, id);
	return id;
}

function readStoredRelayDeviceToken(): string {
	if (typeof localStorage === 'undefined') return '';
	return localStorage.getItem(RELAY_DEVICE_TOKEN_STORAGE_KEY) ?? '';
}

function readStoredDemoWardHomes(): LatLon[] {
	if (typeof localStorage === 'undefined') return [];
	const raw = localStorage.getItem(DEMO_WARD_HOMES_STORAGE_KEY);
	if (!raw) return [];
	try {
		const parsed: unknown = JSON.parse(raw);
		if (!Array.isArray(parsed)) return [];
		return parsed.filter(
			(point): point is LatLon =>
				typeof point === 'object' &&
				point !== null &&
				typeof (point as LatLon).lat === 'number' &&
				typeof (point as LatLon).lon === 'number'
		);
	} catch {
		// stale/corrupt data (e.g. the old plain-count shape this key replaced) - start empty, not crash
		return [];
	}
}

/**
 * Single owner of all live fleet state, keyed by ward_id. Two independent
 * channels feed it: the local demo engine (always available, instant, never
 * touches a network) and an optionally connected gateway (real hardware or
 * simulated wards behind a real gateway process). Every ward carries
 * which channel it came from, so commands and mission uploads route to the
 * right one and disconnecting the gateway never touches demo wards.
 * Components only read; connection and ward lifecycle are explicit
 * actions, never automatic.
 */
class FleetStore {
	wards = $state<Record<string, Ward>>({});
	events = $state<Event[]>([]);
	commands = $state<Record<string, CommandTracker>>({});
	wardConfigRequests = $state<Record<string, WardConfigTracker>>({});
	selectedWardId = $state<string | undefined>(undefined);
	/** goto targeting: the panel arms it, the map's next click supplies the point */
	gotoArming = $state(false);
	pendingGoto = $state<{ latitudeDeg: number; longitudeDeg: number } | undefined>(undefined);
	/** gateway connection as the operator should read it */
	link = $state<'live' | 'connecting' | 'down'>('down');
	gatewayUrl = $state(readStoredGatewayUrl() ?? '');
	relayUrl = $state(readStoredRelayUrl() ?? '');
	relayDeviceId = $state(readOrCreateRelayDeviceId());
	relayDeviceToken = $state(readStoredRelayDeviceToken());
	/** connected to the relay but pairWithGateway() hasn't completed yet */
	relayAwaitingPair = $state(false);
	/** read-only session: telemetry flows, commanding is refused observably */
	readonly = $state(false);
	/** how many simulated-ward adds this session has requested; drives the resource warning at C7's UI layer */
	simulatedWardAddCount = $state(0);
	/** mission being planned; while set, map clicks add waypoints for its ward */
	missionDraft = $state<MissionDraft | undefined>(undefined);
	/** last uploaded mission per ward, so Start knows what it refers to */
	uploadedMissions = $state<Record<string, Mission>>({});
	missionProgress = $state<Record<string, MissionProgress>>({});

	readonly wardIds = $derived(Object.keys(this.wards).sort());
	readonly selectedWard = $derived(
		this.selectedWardId !== undefined ? this.wards[this.selectedWardId] : undefined
	);
	readonly gatewayConnected = $derived(this.link === 'live');

	private demoEngine: DemoEngine | undefined;
	private gatewayTransport: FleetTransport | undefined;
	private relayTransport: RelayTransport | undefined;
	// timers are bookkeeping, not state
	private timeoutTimers = new Map<string, ReturnType<typeof setTimeout>>();
	// each live demo ward's home location, keyed by id - bookkeeping for
	// persistDemoWardHomes(), not state components read directly
	private demoWardHomes: Record<string, LatLon> = {};

	/**
	 * Wires the always-available local demo engine; call once at app start.
	 * Respawns whatever demo fleet was persisted from a previous session
	 * (see addDemoWard/removeDemoWard), each at its original home
	 * location, so a browser refresh doesn't wipe a locally-run operator's
	 * fleet back to empty or move it back to FAKE_FLEET_CENTER - the
	 * wards come back on a fresh patrol (armed, not mid-mission), not
	 * with their exact prior flight state restored.
	 */
	bindDemoEngine(engine: DemoEngine): void {
		this.demoEngine = engine;
		engine.start();
		for (const home of readStoredDemoWardHomes()) {
			const wardId = engine.spawnWard(home);
			this.demoWardHomes[wardId] = home;
		}
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

	/**
	 * Connects through relayly instead of a direct WebSocket - the console
	 * half of gateway/docs/relay-transport.md, for operators whose gateway
	 * sits behind NAT/CGNAT with no reachable inbound port. Reuses the same
	 * gatewayTransport slot and wards/link plumbing as connectGateway();
	 * only the wire underneath differs, per the Transport abstraction rule
	 * in CLAUDE.md ("nothing outside the transport layer may know which is
	 * active"). relayTransport is kept separately only because pairing
	 * (pairRelay below) is relay-specific and not part of FleetTransport.
	 */
	connectRelay(relayUrl: string, deviceId: string, deviceToken: string): void {
		this.disconnectGateway();
		this.relayUrl = relayUrl;
		this.relayDeviceId = deviceId;
		this.relayDeviceToken = deviceToken;
		if (typeof localStorage !== 'undefined') {
			localStorage.setItem(RELAY_URL_STORAGE_KEY, relayUrl);
			localStorage.setItem(RELAY_DEVICE_ID_STORAGE_KEY, deviceId);
			localStorage.setItem(RELAY_DEVICE_TOKEN_STORAGE_KEY, deviceToken);
		}
		this.link = 'connecting';
		const transport = new RelayTransport(relayUrl, deviceId, deviceToken, {
			onEnvelope: (envelope) => this.applyEnvelope(envelope, 'gateway'),
			onStatus: (status) => this.setGatewayStatus(status)
		});
		this.gatewayTransport = transport;
		this.relayTransport = transport;
		this.relayAwaitingPair = true;
		transport.start();
	}

	/**
	 * One-time pairing against the code the gateway's pairing tool displayed
	 * (see gateway/docs/relay-transport.md). Only meaningful right after
	 * connectRelay(); once paired, the pairing persists server-side and
	 * future connectRelay() calls with the same deviceId reconnect straight
	 * to 'open' without this step.
	 */
	async pairRelay(code: string): Promise<void> {
		if (!this.relayTransport) throw new Error('relay transport not connected');
		await this.relayTransport.pairWithGateway(code);
		this.relayAwaitingPair = false;
	}

	disconnectGateway(): void {
		this.gatewayTransport?.stop();
		this.gatewayTransport = undefined;
		this.relayTransport = undefined;
		this.relayAwaitingPair = false;
		this.link = 'down';
		for (const [id, ward] of Object.entries(this.wards)) {
			if (ward.source === 'gateway') delete this.wards[id];
		}
		if (this.selectedWardId && !(this.selectedWardId in this.wards)) {
			this.select(undefined);
		}
	}

	setGatewayStatus(status: 'connecting' | 'open' | 'closed'): void {
		this.link = status === 'open' ? 'live' : status === 'connecting' ? 'connecting' : 'down';
		// covers relay reconnects that resume an already-established pairing
		// (RelayTransport finds it via getPeers() and reports 'open' directly,
		// without pairRelay() ever being called) - the pairing prompt should
		// not linger once telemetry is actually flowing
		if (status === 'open') this.relayAwaitingPair = false;
	}

	select(wardId: string | undefined): void {
		this.selectedWardId = wardId;
		this.cancelGoto();
		if (this.missionDraft && this.missionDraft.wardId !== wardId) {
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

	startPlanning(wardId: string): void {
		this.cancelGoto();
		this.missionDraft = { wardId, waypoints: [], repeatCount: 0 };
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
			wardId: draft.wardId,
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
		const channel = this.channelFor(draft.wardId);
		if (!channel) {
			console.error(`fleet: cannot upload mission, ${draft.wardId} has no active channel`);
			return undefined;
		}
		channel.send({ payload: { $case: 'missionUpload', missionUpload: mission } });
		this.uploadedMissions[draft.wardId] = mission;
		delete this.missionProgress[draft.wardId];
		return mission;
	}

	sendCommand(wardId: string, action: NonNullable<Command['action']>): string {
		if (this.readonly) {
			// belt and braces: viewer mode hides the command UI, but any path
			// that still sends must settle observably, never silently
			const refusedId = crypto.randomUUID();
			this.commands[refusedId] = {
				commandId: refusedId,
				wardId,
				kind: action.$case,
				status: CommandStatus.COMMAND_STATUS_REJECTED,
				message: 'read-only session',
				sentAtMs: Date.now()
			};
			return refusedId;
		}
		const command: Command = {
			commandId: crypto.randomUUID(),
			wardId,
			timestampMs: Date.now(),
			action
		};
		this.commands[command.commandId] = {
			commandId: command.commandId,
			wardId,
			kind: action.$case,
			status: CommandStatus.COMMAND_STATUS_UNSPECIFIED,
			message: '',
			sentAtMs: command.timestampMs
		};
		const channel = this.channelFor(wardId);
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

	/** in-flight and recently settled commands for one ward, newest first */
	commandsFor(wardId: string): CommandTracker[] {
		return Object.values(this.commands)
			.filter((tracker) => tracker.wardId === wardId)
			.sort((a, b) => b.sentAtMs - a.sentAtMs);
	}

	/**
	 * Adds a ward through the connected gateway (covers both a simulated
	 * SITL endpoint and real hardware; the gateway sees no difference). No
	 * gateway connection means nothing to send to, reported immediately.
	 */
	noteSimulatedWardRequested(): void {
		this.simulatedWardAddCount += 1;
	}

	requestAddWard(spec: {
		wardId: string;
		name: string;
		wardClass: WardClass;
		connectionUrl: string;
		mavlinkSystemId: number;
	}): string {
		const requestId = crypto.randomUUID();
		const addWard: AddWard = { requestId, ...spec };
		this.trackConfigRequest(requestId, 'add', spec.wardId, () => ({
			payload: { $case: 'addWard', addWard }
		}));
		return requestId;
	}

	requestRemoveWard(wardId: string): string {
		const requestId = crypto.randomUUID();
		this.trackConfigRequest(requestId, 'remove', wardId, () => ({
			payload: { $case: 'removeWard', removeWard: { requestId, wardId } }
		}));
		return requestId;
	}

	configRequestsFor(wardId: string): WardConfigTracker[] {
		return Object.values(this.wardConfigRequests)
			.filter((tracker) => tracker.wardId === wardId)
			.sort((a, b) => b.sentAtMs - a.sentAtMs);
	}

	/** spawns a new demo ward purely client-side; never touches a network */
	addDemoWard(location?: LatLon): void {
		const wardId = this.demoEngine?.spawnWard(location);
		if (wardId) this.demoWardHomes[wardId] = location ?? FAKE_FLEET_CENTER;
		this.persistDemoWardHomes();
	}

	removeDemoWard(wardId: string): void {
		this.demoEngine?.despawnWard(wardId);
		delete this.wards[wardId];
		delete this.demoWardHomes[wardId];
		if (this.selectedWardId === wardId) this.select(undefined);
		this.persistDemoWardHomes();
	}

	applyEnvelope(envelope: Envelope, source: WardSource): void {
		const payload = envelope.payload;
		if (payload === undefined) {
			console.warn('fleet: dropping Envelope with empty payload');
			return;
		}
		switch (payload.$case) {
			case 'wardInfo':
				this.upsert(payload.wardInfo.wardId, source).info = payload.wardInfo;
				break;
			case 'wardState':
				this.upsert(payload.wardState.wardId, source).state = payload.wardState;
				break;
			case 'event':
				this.events = [payload.event, ...this.events].slice(0, MAX_EVENTS);
				break;
			case 'commandAck': {
				const ack = payload.commandAck;
				const tracker = this.commands[ack.commandId];
				if (!tracker) {
					// Not necessarily a bug: the gateway itself synthesizes and
					// enqueues a StartMissionCommand once a fleet mission's per-ward
					// upload lands (ward_manager.cpp's pending_start), and also
					// synthesizes a Stop dispatch's rtl/pause_mission/land command -
					// both commands this client never sent and so never tracked. A
					// rejection would otherwise vanish with nothing but this
					// console.warn - surfaced as an Event instead, the same channel
					// a fleet mission's upload-phase rejection already uses
					// (MISSION_UPLOAD_REJECTED), so a failure on one ward is never
					// silently invisible. A success needs no separate surfacing:
					// missionProgress ticks (start) or the FleetMission's own
					// ward_states (stop) already show the outcome.
					if (
						ack.status === CommandStatus.COMMAND_STATUS_REJECTED ||
						ack.status === CommandStatus.COMMAND_STATUS_TIMEOUT
					) {
						this.events = [
							{
								wardId: ack.wardId,
								timestampMs: Date.now(),
								severity: Severity.SEVERITY_WARNING,
								code: 'COMMAND_REJECTED',
								message: ack.message
							},
							...this.events
						].slice(0, MAX_EVENTS);
					} else {
						console.warn(`fleet: CommandAck for unknown command_id ${ack.commandId}`);
					}
					break;
				}
				this.settle(ack.commandId, ack.status, ack.message);
				break;
			}
			case 'missionProgress':
				this.missionProgress[payload.missionProgress.wardId] = payload.missionProgress;
				break;
			case 'missionDownload':
				// Response to mission_download_request: the mission currently on the
				// ward. Same slot as an upload, since both represent "what's on the
				// ward now". No console UI sends mission_download_request yet.
				this.uploadedMissions[payload.missionDownload.wardId] = payload.missionDownload;
				break;
			case 'wardConfigAck': {
				const ack = payload.wardConfigAck;
				const tracker = this.wardConfigRequests[ack.requestId];
				if (!tracker) {
					console.warn(`fleet: WardConfigAck for unknown request_id ${ack.requestId}`);
					break;
				}
				this.settleConfig(ack.requestId, ack.status, ack.message);
				if (
					tracker.kind === 'remove' &&
					ack.status === WardConfigStatus.WARD_CONFIG_STATUS_ACCEPTED
				) {
					delete this.wards[ack.wardId];
					if (this.selectedWardId === ack.wardId) this.select(undefined);
				}
				break;
			}
			case 'command':
			case 'missionUpload':
			case 'addWard':
			case 'removeWard':
			case 'missionFileUpload':
			case 'missionDownloadRequest':
			case 'createFleet':
			case 'renameFleet':
			case 'deleteFleet':
			case 'addWardToFleet':
			case 'removeWardFromFleet':
			case 'createZone':
			case 'updateZone':
			case 'deleteZone':
			case 'createFleetMission':
			case 'stopFleetMission':
			case 'removeFleetMission':
			case 'updateFleetMissionRoutes':
				console.warn(`fleet: ignoring upstream payload kind ${payload.$case} sent downstream`);
				break;
			case 'fleet':
				fleetGroups.applyFleet(payload.fleet);
				break;
			case 'fleetAck':
				fleetGroups.applyFleetAck(payload.fleetAck);
				break;
			case 'fleetMission':
				fleetGroups.applyFleetMission(payload.fleetMission);
				break;
			case 'fleetMissionAck':
				fleetGroups.applyFleetMissionAck(payload.fleetMissionAck);
				break;
			case 'zone':
				zoneStore.applyZone(payload.zone);
				break;
			case 'zoneAck':
				zoneStore.applyZoneAck(payload.zoneAck);
				break;
			default: {
				const unhandled: never = payload;
				console.warn('fleet: ignoring unknown payload kind', unhandled);
			}
		}
	}

	/**
	 * Sends an envelope through whichever channel is currently authoritative
	 * for gateway-owned resources that have no per-ward channel of their own
	 * (Fleet/Zone CRUD, fleet-wide mission assignment): the connected
	 * gateway if there is one, otherwise the always-available demo engine.
	 * Lets fleetGroups/zoneStore reach the wire without either holding its
	 * own transport reference.
	 */
	sendUpstream(envelope: Envelope): void {
		const channel = this.gatewayTransport ?? this.demoEngine;
		if (!channel) throw new Error('no active channel');
		channel.send(envelope);
	}

	/** full app teardown: stops both channels and wipes everything */
	teardown(): void {
		this.demoEngine?.stop();
		this.demoEngine = undefined;
		this.disconnectGateway();
		for (const timer of this.timeoutTimers.values()) clearTimeout(timer);
		this.timeoutTimers.clear();
		this.wards = {};
		this.events = [];
		this.commands = {};
		this.wardConfigRequests = {};
		this.selectedWardId = undefined;
		this.missionDraft = undefined;
		this.uploadedMissions = {};
		this.missionProgress = {};
		this.cancelGoto();
		fleetGroups.teardown();
		zoneStore.teardown();
	}

	private channelFor(wardId: string): FleetTransport | undefined {
		const source = this.wards[wardId]?.source;
		if (source === 'demo') return this.demoEngine;
		if (source === 'gateway') return this.gatewayTransport;
		return undefined;
	}

	private trackConfigRequest(
		requestId: string,
		kind: 'add' | 'remove',
		wardId: string,
		buildEnvelope: () => Envelope
	): void {
		this.wardConfigRequests[requestId] = {
			requestId,
			kind,
			wardId,
			status: WardConfigStatus.WARD_CONFIG_STATUS_UNSPECIFIED,
			message: '',
			sentAtMs: Date.now()
		};
		if (!this.gatewayTransport) {
			this.settleConfig(
				requestId,
				WardConfigStatus.WARD_CONFIG_STATUS_REJECTED,
				'no gateway connected'
			);
			return;
		}
		this.timeoutTimers.set(
			requestId,
			setTimeout(() => {
				this.settleConfig(
					requestId,
					WardConfigStatus.WARD_CONFIG_STATUS_REJECTED,
					'no acknowledgment from gateway'
				);
			}, REQUEST_TIMEOUT_MS)
		);
		try {
			this.gatewayTransport.send(buildEnvelope());
		} catch (error) {
			const reason = error instanceof Error ? error.message : String(error);
			this.settleConfig(requestId, WardConfigStatus.WARD_CONFIG_STATUS_REJECTED, reason);
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

	private settleConfig(requestId: string, status: WardConfigStatus, message: string): void {
		const tracker = this.wardConfigRequests[requestId];
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
				delete this.wardConfigRequests[requestId];
			}, TRACKER_LINGER_MS);
		}
	}

	private persistDemoWardHomes(): void {
		if (typeof localStorage === 'undefined') return;
		const homes = Object.values(this.demoWardHomes);
		if (homes.length > 0) {
			localStorage.setItem(DEMO_WARD_HOMES_STORAGE_KEY, JSON.stringify(homes));
		} else {
			localStorage.removeItem(DEMO_WARD_HOMES_STORAGE_KEY);
		}
	}

	private upsert(wardId: string, source: WardSource): Ward {
		if (!(wardId in this.wards)) {
			this.wards[wardId] = { info: undefined, state: undefined, source };
		}
		return this.wards[wardId];
	}
}

export const fleet = new FleetStore();
