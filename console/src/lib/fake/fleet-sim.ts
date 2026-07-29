import {
	FlightMode,
	GpsFixType,
	Severity,
	WardOrigin,
	WardClass
} from '$lib/gen/karshipta/v1/common';
import {
	CommandStatus,
	MissionAction,
	type Command,
	type Mission
} from '$lib/gen/karshipta/v1/command';
import {
	FleetAckStatus,
	FleetMissionAckStatus,
	FleetMissionStatus,
	FleetMissionStopAction,
	WardMissionStatus,
	ZoneAckStatus,
	type AddWardToFleet,
	type CreateFleet,
	type CreateFleetMission,
	type CreateZone,
	type DeleteFleet,
	type DeleteZone,
	type Fleet,
	type FleetMission,
	type RemoveFleetMission,
	type RemoveWardFromFleet,
	type RenameFleet,
	type StopFleetMission,
	type UpdateFleetMissionRoutes,
	type UpdateZone,
	type WardMissionPlan,
	type WardMissionState,
	type Zone
} from '$lib/gen/karshipta/v1/fleet';
import type { Envelope } from '$lib/gen/karshipta/v1/envelope';
import type { FleetTransport } from '$lib/transport';
import type { LatLon } from '$lib/geolocation';

/**
 * Dev stand-in for the real gateway until its milestones land. Implements
 * FleetTransport: publishes telemetry Envelopes, accepts Command envelopes,
 * answers every one with CommandAcks, and emits Events, through the exact
 * same seam the WebSocket transport uses. Three simulated multirotors start
 * on patrol around the PX4 SITL default home.
 */

// Vondelpark, Amsterdam - matches docker-compose.yml's PX4_HOME_LAT/LON and
// +page.svelte's DEFAULT_MAP_CENTER, so the fake fleet and the real
// docker-compose demo appear in the same place on the map.
const HOME_LAT_DEG = 52.3579;
const HOME_LON_DEG = 4.8686;
const HOME_ALT_MSL_M = 488;

const TICK_HZ = 5;
const TICK_S = 1 / TICK_HZ;
const BATTERY_DRAIN_AIR_PCT_PER_S = 0.05;
const BATTERY_DRAIN_GROUND_PCT_PER_S = 0.005;
const LOW_BATTERY_PCT = 20;
const METERS_PER_DEG_LAT = 111_320;
const CRUISE_SPEED_M_S = 8;
const CLIMB_RATE_M_S = 2.5;
const ARRIVAL_RADIUS_M = 2;
const DEFAULT_TAKEOFF_ALT_M = 20;

export const FAKE_FLEET_CENTER = { lat: HOME_LAT_DEG, lon: HOME_LON_DEG };

/**
 * Demo-mode Fleet grouping, persisted so it survives a reload the same way
 * demo ward homes do (fleet-store.svelte.ts's DEMO_WARD_HOMES_STORAGE_KEY).
 * Membership references ward ids like "demo-1"; those stay valid across a
 * reload only because bindDemoEngine() always respawns the same persisted
 * homes in the same order, which is exactly what already keeps their ids
 * stable today.
 */
const DEMO_FLEETS_STORAGE_KEY = 'karshipta.demoFleets';

function readStoredDemoFleets(): Fleet[] {
	if (typeof localStorage === 'undefined') return [];
	const raw = localStorage.getItem(DEMO_FLEETS_STORAGE_KEY);
	if (!raw) return [];
	try {
		const parsed: unknown = JSON.parse(raw);
		if (!Array.isArray(parsed)) return [];
		return parsed.filter(
			(value): value is Fleet =>
				typeof value === 'object' &&
				value !== null &&
				typeof (value as Fleet).fleetId === 'string' &&
				typeof (value as Fleet).name === 'string' &&
				Array.isArray((value as Fleet).wardIds)
		);
	} catch {
		// stale/corrupt data - start empty, not crash
		return [];
	}
}

/** Demo-mode fleet missions, same persistence precedent as demo Fleets - a
 * ward_id like "demo-1" stays valid across a reload for the same reason. */
const DEMO_FLEET_MISSIONS_STORAGE_KEY = 'karshipta.demoFleetMissions';

function readStoredDemoFleetMissions(): FleetMission[] {
	if (typeof localStorage === 'undefined') return [];
	const raw = localStorage.getItem(DEMO_FLEET_MISSIONS_STORAGE_KEY);
	if (!raw) return [];
	try {
		const parsed: unknown = JSON.parse(raw);
		if (!Array.isArray(parsed)) return [];
		return parsed.filter(
			(value): value is FleetMission =>
				typeof value === 'object' &&
				value !== null &&
				typeof (value as FleetMission).fleetMissionId === 'string' &&
				Array.isArray((value as FleetMission).wardPlans) &&
				Array.isArray((value as FleetMission).wardStates)
		);
	} catch {
		// stale/corrupt data - start empty, not crash
		return [];
	}
}

/** Demo-mode drawn zones, same persistence precedent as demo Fleets. */
const DEMO_ZONES_STORAGE_KEY = 'karshipta.demoZones';

function readStoredDemoZones(): Zone[] {
	if (typeof localStorage === 'undefined') return [];
	const raw = localStorage.getItem(DEMO_ZONES_STORAGE_KEY);
	if (!raw) return [];
	try {
		const parsed: unknown = JSON.parse(raw);
		if (!Array.isArray(parsed)) return [];
		return parsed.filter(
			(value): value is Zone =>
				typeof value === 'object' &&
				value !== null &&
				typeof (value as Zone).zoneId === 'string' &&
				typeof (value as Zone).name === 'string' &&
				Array.isArray((value as Zone).vertices)
		);
	} catch {
		// stale/corrupt data - start empty, not crash
		return [];
	}
}

/** owner-facing contract for spawning/removing demo wards from the UI */
export interface DemoEngine extends FleetTransport {
	/** spawns one demo ward with a procedurally varied patrol centered at
	 * `location` (defaults to FAKE_FLEET_CENTER); returns its id */
	spawnWard(location?: LatLon): string;
	despawnWard(wardId: string): void;
	/** re-pushes every persisted Fleet/Zone/FleetMission; see the
	 * implementation's own doc comment for why fleet-store.svelte.ts needs
	 * this as a standalone call, not just start()'s one-time sync. */
	resync(): void;
}

interface WardSpec {
	wardId: string;
	radiusM: number;
	periodS: number;
	cruiseAltM: number;
	phaseRad: number;
	homeLatDeg: number;
	homeLonDeg: number;
}

const SPAWN_RADIUS_MIN_M = 50;
const SPAWN_RADIUS_STEP_M = 40;
const SPAWN_PERIOD_MIN_S = 40;
const SPAWN_PERIOD_STEP_S = 25;
const SPAWN_ALT_MIN_M = 20;
const SPAWN_ALT_STEP_M = 15;
/** golden-angle-ish spread so consecutively spawned wards don't overlap */
const SPAWN_PHASE_STEP_RAD = 2.4;

/** procedurally varied patrol so each newly spawned demo ward looks distinct */
function nextSpec(wardId: string, spawnIndex: number, home: LatLon): WardSpec {
	return {
		wardId,
		radiusM: SPAWN_RADIUS_MIN_M + (spawnIndex % 4) * SPAWN_RADIUS_STEP_M,
		periodS: SPAWN_PERIOD_MIN_S + (spawnIndex % 3) * SPAWN_PERIOD_STEP_S,
		cruiseAltM: SPAWN_ALT_MIN_M + (spawnIndex % 5) * SPAWN_ALT_STEP_M,
		phaseRad: (spawnIndex * SPAWN_PHASE_STEP_RAD) % (2 * Math.PI),
		homeLatDeg: home.lat,
		homeLonDeg: home.lon
	};
}

/** what the ward does when it reaches its current target */
type Arrival = 'hold' | 'land' | 'mission-item';

interface Target {
	northM: number;
	eastM: number;
	altM: number;
	speedMS: number;
	onArrival: Arrival;
	commandId: string;
	commandWardId: string;
}

interface SimWard {
	spec: WardSpec;
	armed: boolean;
	inAir: boolean;
	mode: FlightMode;
	northM: number;
	eastM: number;
	altM: number;
	headingDeg: number;
	velocityNorth: number;
	velocityEast: number;
	velocityDown: number;
	batteryPct: number;
	lowBatteryReported: boolean;
	thetaRad: number;
	patrolling: boolean;
	target: Target | undefined;
	missionRun: MissionRun | undefined;
}

interface MissionRun {
	mission: Mission;
	itemIndex: number;
	/** extra passes still owed after the current one */
	passesLeft: number;
	/** command_id of the start_mission command, acked SUCCESS at the very end */
	startCommandId: string;
	holdUntilMs: number;
	paused: boolean;
}

function initialWard(spec: WardSpec): SimWard {
	return {
		spec,
		armed: true,
		inAir: true,
		mode: FlightMode.FLIGHT_MODE_MISSION,
		northM: spec.radiusM * Math.cos(spec.phaseRad),
		eastM: spec.radiusM * Math.sin(spec.phaseRad),
		altM: spec.cruiseAltM,
		headingDeg: 0,
		velocityNorth: 0,
		velocityEast: 0,
		velocityDown: 0,
		batteryPct: 100,
		lowBatteryReported: false,
		thetaRad: spec.phaseRad,
		patrolling: true,
		target: undefined,
		missionRun: undefined
	};
}

/**
 * The console's local demo engine: spawns/despawns purely client-side,
 * never touches a network. Wards are added one at a time from the UI
 * (never automatically), each getting a procedurally varied patrol so a
 * freshly spawned fleet still looks distinct. Every demo ward is a
 * flight-capable multirotor (a MAVLink SITL stand-in), so its WardState
 * always carries a populated `flight` field; a non-flight ward has no
 * simulator counterpart yet.
 */
export class FakeGateway implements DemoEngine {
	private wards = new Map<string, SimWard>();
	private missions = new Map<string, Mission>();
	private timer: ReturnType<typeof setInterval> | undefined;
	private spawnCount = 0;
	private fleets = new Map<string, Fleet>();
	private fleetSpawnCount = 0;
	private zones = new Map<string, Zone>();
	private zoneSpawnCount = 0;
	private fleetMissions = new Map<string, FleetMission>();
	private fleetMissionSpawnCount = 0;
	/** Correlates a synthesized startMission/stop Command's command_id back to
	 * the (fleetMissionId, wardId) it was issued for, resolved from ack()'s
	 * one choke point - mirrors the real gateway's pending_stops_ (see
	 * FleetManager::handle_command_outcome's own comment for why this needs
	 * to exist at all: WardMissionState tracks a dispatch's real outcome,
	 * which for rtl/land only lands asynchronously once the ward actually
	 * arrives, not when the command is merely accepted). */
	private pendingCommands = new Map<
		string,
		{ fleetMissionId: string; wardId: string; kind: 'start' | 'stop' }
	>();

	constructor(private readonly onEnvelope: (envelope: Envelope) => void) {
		for (const fleet of readStoredDemoFleets()) this.fleets.set(fleet.fleetId, fleet);
		for (const zone of readStoredDemoZones()) this.zones.set(zone.zoneId, zone);
		for (const mission of readStoredDemoFleetMissions()) {
			this.fleetMissions.set(mission.fleetMissionId, mission);
		}
	}

	start(): void {
		if (this.timer !== undefined) return;
		this.resync();
		this.timer = setInterval(() => this.tick(), 1000 / TICK_HZ);
	}

	/** Re-pushes every persisted Fleet/Zone/FleetMission, same as the
	 * gateway's own send_fleet_zone_snapshot/send_fleet_mission_snapshot on
	 * connect. start() calls this once; fleet-store.svelte.ts's
	 * disconnectGateway() also calls it directly (independent of the timer
	 * guard above) to restore this engine's own data after
	 * connectGateway()/disconnectGateway() clear it from the client-side
	 * stores - those three resource types have no per-source tag the way
	 * wards do (fleet.proto's Fleet/Zone/FleetMission are gateway-owned
	 * config, not something meant to show both a demo and a real gateway's
	 * copies merged together), so a channel switch must fully swap them,
	 * not merge. */
	resync(): void {
		for (const fleet of this.fleets.values()) {
			this.onEnvelope({ payload: { $case: 'fleet', fleet } });
		}
		for (const zone of this.zones.values()) {
			this.onEnvelope({ payload: { $case: 'zone', zone } });
		}
		for (const mission of this.fleetMissions.values()) {
			this.onEnvelope({ payload: { $case: 'fleetMission', fleetMission: mission } });
		}
	}

	stop(): void {
		if (this.timer !== undefined) {
			clearInterval(this.timer);
			this.timer = undefined;
		}
		this.wards.clear();
		this.missions.clear();
		this.spawnCount = 0;
	}

	spawnWard(location?: LatLon): string {
		const wardId = `demo-${this.spawnCount + 1}`;
		const spec = nextSpec(wardId, this.spawnCount, location ?? FAKE_FLEET_CENTER);
		this.spawnCount += 1;
		this.wards.set(wardId, initialWard(spec));
		this.onEnvelope({
			payload: {
				$case: 'wardInfo',
				wardInfo: {
					wardId,
					wardClass: WardClass.WARD_CLASS_MULTIROTOR,
					autopilot: 'PX4',
					firmwareVersion: 'demo-fake',
					mavlinkSystemId: 0,
					origin: WardOrigin.WARD_ORIGIN_SYNTHETIC
				}
			}
		});
		return wardId;
	}

	despawnWard(wardId: string): void {
		this.wards.delete(wardId);
		this.missions.delete(wardId);
	}

	send(envelope: Envelope): void {
		switch (envelope.payload?.$case) {
			case 'command':
				this.handleCommand(envelope.payload.command);
				break;
			case 'missionUpload':
				this.handleMissionUpload(envelope.payload.missionUpload);
				break;
			case 'createFleet':
				this.handleCreateFleet(envelope.payload.createFleet);
				break;
			case 'renameFleet':
				this.handleRenameFleet(envelope.payload.renameFleet);
				break;
			case 'deleteFleet':
				this.handleDeleteFleet(envelope.payload.deleteFleet);
				break;
			case 'addWardToFleet':
				this.handleAddWardToFleet(envelope.payload.addWardToFleet);
				break;
			case 'removeWardFromFleet':
				this.handleRemoveWardFromFleet(envelope.payload.removeWardFromFleet);
				break;
			case 'createFleetMission':
				this.handleCreateFleetMission(envelope.payload.createFleetMission);
				break;
			case 'stopFleetMission':
				this.handleStopFleetMission(envelope.payload.stopFleetMission);
				break;
			case 'removeFleetMission':
				this.handleRemoveFleetMission(envelope.payload.removeFleetMission);
				break;
			case 'updateFleetMissionRoutes':
				this.handleUpdateFleetMissionRoutes(envelope.payload.updateFleetMissionRoutes);
				break;
			case 'createZone':
				this.handleCreateZone(envelope.payload.createZone);
				break;
			case 'updateZone':
				this.handleUpdateZone(envelope.payload.updateZone);
				break;
			case 'deleteZone':
				this.handleDeleteZone(envelope.payload.deleteZone);
				break;
			default:
				console.warn(
					'fake gateway: ignoring unsupported upstream payload',
					envelope.payload?.$case
				);
		}
	}

	private handleCreateFleet(request: CreateFleet): void {
		const fleetId = `demo-fleet-${this.fleetSpawnCount + 1}`;
		this.fleetSpawnCount += 1;
		const fleet: Fleet = {
			fleetId,
			name: request.name,
			description: request.description,
			wardIds: []
		};
		this.fleets.set(fleetId, fleet);
		this.persistDemoFleets();
		this.fleetAck(
			request.requestId,
			FleetAckStatus.FLEET_ACK_STATUS_ACCEPTED,
			'fleet created',
			fleetId
		);
		this.onEnvelope({ payload: { $case: 'fleet', fleet } });
	}

	private handleRenameFleet(request: RenameFleet): void {
		const fleet = this.fleets.get(request.fleetId);
		if (!fleet) {
			this.fleetAck(
				request.requestId,
				FleetAckStatus.FLEET_ACK_STATUS_REJECTED,
				`unknown fleet_id: ${request.fleetId}`,
				request.fleetId
			);
			return;
		}
		fleet.name = request.name;
		fleet.description = request.description;
		this.persistDemoFleets();
		this.fleetAck(
			request.requestId,
			FleetAckStatus.FLEET_ACK_STATUS_ACCEPTED,
			'fleet renamed',
			fleet.fleetId
		);
		this.onEnvelope({ payload: { $case: 'fleet', fleet } });
	}

	private handleDeleteFleet(request: DeleteFleet): void {
		if (!this.fleets.delete(request.fleetId)) {
			this.fleetAck(
				request.requestId,
				FleetAckStatus.FLEET_ACK_STATUS_REJECTED,
				`unknown fleet_id: ${request.fleetId}`,
				request.fleetId
			);
			return;
		}
		this.persistDemoFleets();
		this.fleetAck(
			request.requestId,
			FleetAckStatus.FLEET_ACK_STATUS_ACCEPTED,
			'fleet deleted',
			request.fleetId
		);
	}

	private handleAddWardToFleet(request: AddWardToFleet): void {
		const fleet = this.fleets.get(request.fleetId);
		if (!fleet) {
			this.fleetAck(
				request.requestId,
				FleetAckStatus.FLEET_ACK_STATUS_REJECTED,
				`unknown fleet_id: ${request.fleetId}`,
				request.fleetId
			);
			return;
		}
		if (!fleet.wardIds.includes(request.wardId)) fleet.wardIds.push(request.wardId);
		this.persistDemoFleets();
		this.fleetAck(
			request.requestId,
			FleetAckStatus.FLEET_ACK_STATUS_ACCEPTED,
			'ward added to fleet',
			fleet.fleetId
		);
		this.onEnvelope({ payload: { $case: 'fleet', fleet } });
	}

	private handleRemoveWardFromFleet(request: RemoveWardFromFleet): void {
		const fleet = this.fleets.get(request.fleetId);
		if (!fleet) {
			this.fleetAck(
				request.requestId,
				FleetAckStatus.FLEET_ACK_STATUS_REJECTED,
				`unknown fleet_id: ${request.fleetId}`,
				request.fleetId
			);
			return;
		}
		fleet.wardIds = fleet.wardIds.filter((id) => id !== request.wardId);
		this.persistDemoFleets();
		this.fleetAck(
			request.requestId,
			FleetAckStatus.FLEET_ACK_STATUS_ACCEPTED,
			'ward removed from fleet',
			fleet.fleetId
		);
		this.onEnvelope({ payload: { $case: 'fleet', fleet } });
	}

	private fleetAck(
		requestId: string,
		status: FleetAckStatus,
		message: string,
		fleetId: string
	): void {
		this.onEnvelope({
			payload: { $case: 'fleetAck', fleetAck: { requestId, status, message, fleetId } }
		});
	}

	private persistDemoFleets(): void {
		if (typeof localStorage === 'undefined') return;
		const fleets = [...this.fleets.values()];
		if (fleets.length > 0) {
			localStorage.setItem(DEMO_FLEETS_STORAGE_KEY, JSON.stringify(fleets));
		} else {
			localStorage.removeItem(DEMO_FLEETS_STORAGE_KEY);
		}
	}

	/** Persists a fleet mission's own plans/state, then for each ward_plan
	 * uploads that ward's independent route and issues startMission -
	 * mirrors the real gateway's FleetManager::handle_create_fleet_mission /
	 * handle_update_fleet_mission_routes, both of which share this same
	 * dispatch loop. Registers a 'start' pendingCommands entry per ward so
	 * ack()'s resolvePendingFleetMissionCommand() can flip UPLOADING (well,
	 * UNSPECIFIED here - this engine has no real async upload delay to model
	 * a distinct UPLOADING phase for) to ACTIVE/REJECTED once startMission's
	 * own ack lands, exactly as the real gateway's upload-result observer
	 * does for the real async upload. */
	private dispatchWardPlans(
		mission: FleetMission,
		plans: WardMissionPlan[],
		repeatCount: number,
		missionName: string
	): void {
		for (const plan of plans) {
			const state = mission.wardStates.find((candidate) => candidate.wardId === plan.wardId);
			if (!state) continue;
			if (!this.wards.has(plan.wardId)) {
				state.status = WardMissionStatus.WARD_MISSION_STATUS_REJECTED;
				state.message = `unknown ward_id: ${plan.wardId}`;
				continue;
			}
			const missionId = `${mission.fleetMissionId}-${plan.wardId}-${Date.now()}`;
			state.missionId = missionId;
			this.handleMissionUpload({
				missionId,
				wardId: plan.wardId,
				name: missionName,
				repeatCount,
				items: plan.items
			});
			const commandId = `fleet-mission-start-${plan.wardId}-${Date.now()}`;
			this.pendingCommands.set(commandId, {
				fleetMissionId: mission.fleetMissionId,
				wardId: plan.wardId,
				kind: 'start'
			});
			this.handleCommand({
				commandId,
				wardId: plan.wardId,
				timestampMs: Date.now(),
				action: { $case: 'startMission', startMission: {} }
			});
		}
	}

	private handleCreateFleetMission(request: CreateFleetMission): void {
		if (request.fleetId && !this.fleets.has(request.fleetId)) {
			this.fleetMissionAck(
				request.requestId,
				FleetMissionAckStatus.FLEET_MISSION_ACK_STATUS_REJECTED,
				`unknown fleet_id: ${request.fleetId}`,
				''
			);
			return;
		}
		if (request.wardPlans.length === 0) {
			this.fleetMissionAck(
				request.requestId,
				FleetMissionAckStatus.FLEET_MISSION_ACK_STATUS_REJECTED,
				'no ward plans in request',
				''
			);
			return;
		}
		const fleetMissionId = `demo-fleet-mission-${this.fleetMissionSpawnCount + 1}`;
		this.fleetMissionSpawnCount += 1;
		const wardStates: WardMissionState[] = request.wardPlans.map((plan) => ({
			wardId: plan.wardId,
			status: WardMissionStatus.WARD_MISSION_STATUS_UNSPECIFIED,
			message: '',
			missionId: ''
		}));
		const mission: FleetMission = {
			fleetMissionId,
			fleetId: request.fleetId,
			missionName: request.missionName,
			repeatCount: request.repeatCount,
			wardPlans: request.wardPlans,
			wardStates,
			status: FleetMissionStatus.FLEET_MISSION_STATUS_ACTIVE,
			createdAtMs: Date.now()
		};
		this.fleetMissions.set(fleetMissionId, mission);
		this.dispatchWardPlans(mission, request.wardPlans, request.repeatCount, request.missionName);
		this.persistDemoFleetMissions();
		this.fleetMissionAck(
			request.requestId,
			FleetMissionAckStatus.FLEET_MISSION_ACK_STATUS_ACCEPTED,
			'fleet mission created',
			fleetMissionId
		);
		this.onEnvelope({ payload: { $case: 'fleetMission', fleetMission: mission } });
	}

	private handleStopFleetMission(request: StopFleetMission): void {
		const mission = this.fleetMissions.get(request.fleetMissionId);
		if (!mission) {
			this.fleetMissionAck(
				request.requestId,
				FleetMissionAckStatus.FLEET_MISSION_ACK_STATUS_REJECTED,
				`unknown fleet_mission_id: ${request.fleetMissionId}`,
				request.fleetMissionId
			);
			return;
		}
		const action =
			request.action === FleetMissionStopAction.FLEET_MISSION_STOP_ACTION_UNSPECIFIED
				? FleetMissionStopAction.FLEET_MISSION_STOP_ACTION_RTL
				: request.action;
		let dispatched = 0;
		for (const state of mission.wardStates) {
			if (
				state.status === WardMissionStatus.WARD_MISSION_STATUS_STOPPED ||
				state.status === WardMissionStatus.WARD_MISSION_STATUS_REJECTED ||
				state.status === WardMissionStatus.WARD_MISSION_STATUS_STOPPING
			) {
				continue;
			}
			const commandId = `fleet-mission-stop-${mission.fleetMissionId}-${state.wardId}-${Date.now()}`;
			// Set STOPPING and register the correlation entry before dispatching
			// the command: handleCommand() can resolve synchronously (a ward
			// already landed rejects rtl/land immediately), and that must
			// overwrite this STOPPING write, not race and lose to it - same
			// ordering fix as FleetManager::handle_stop_fleet_mission.
			state.status = WardMissionStatus.WARD_MISSION_STATUS_STOPPING;
			this.pendingCommands.set(commandId, {
				fleetMissionId: mission.fleetMissionId,
				wardId: state.wardId,
				kind: 'stop'
			});
			this.handleCommand({
				commandId,
				wardId: state.wardId,
				timestampMs: Date.now(),
				action:
					action === FleetMissionStopAction.FLEET_MISSION_STOP_ACTION_HOLD
						? { $case: 'pauseMission', pauseMission: {} }
						: action === FleetMissionStopAction.FLEET_MISSION_STOP_ACTION_LAND
							? { $case: 'land', land: {} }
							: { $case: 'rtl', rtl: {} }
			});
			dispatched += 1;
		}
		if (dispatched > 0) {
			mission.status = FleetMissionStatus.FLEET_MISSION_STATUS_STOPPING;
			// Catches a ward whose handleCommand() above resolved synchronously
			// (before mission.status was actually STOPPING yet).
			this.maybeFinalizeStop(mission.fleetMissionId);
		}
		this.persistDemoFleetMissions();
		this.fleetMissionAck(
			request.requestId,
			FleetMissionAckStatus.FLEET_MISSION_ACK_STATUS_ACCEPTED,
			dispatched > 0 ? `stop dispatched to ${dispatched} ward(s)` : 'no active wards to stop',
			mission.fleetMissionId
		);
		this.onEnvelope({ payload: { $case: 'fleetMission', fleetMission: mission } });
	}

	private handleRemoveFleetMission(request: RemoveFleetMission): void {
		const mission = this.fleetMissions.get(request.fleetMissionId);
		if (!mission) {
			this.fleetMissionAck(
				request.requestId,
				FleetMissionAckStatus.FLEET_MISSION_ACK_STATUS_REJECTED,
				`unknown fleet_mission_id: ${request.fleetMissionId}`,
				request.fleetMissionId
			);
			return;
		}
		if (!mission.wardStates.every(isWardMissionSettled)) {
			this.fleetMissionAck(
				request.requestId,
				FleetMissionAckStatus.FLEET_MISSION_ACK_STATUS_REJECTED,
				'cannot remove while any ward is still active; stop it first',
				mission.fleetMissionId
			);
			return;
		}
		this.fleetMissions.delete(request.fleetMissionId);
		this.persistDemoFleetMissions();
		this.fleetMissionAck(
			request.requestId,
			FleetMissionAckStatus.FLEET_MISSION_ACK_STATUS_ACCEPTED,
			'fleet mission removed',
			request.fleetMissionId
		);
	}

	private handleUpdateFleetMissionRoutes(request: UpdateFleetMissionRoutes): void {
		const mission = this.fleetMissions.get(request.fleetMissionId);
		if (!mission) {
			this.fleetMissionAck(
				request.requestId,
				FleetMissionAckStatus.FLEET_MISSION_ACK_STATUS_REJECTED,
				`unknown fleet_mission_id: ${request.fleetMissionId}`,
				request.fleetMissionId
			);
			return;
		}
		if (!mission.wardStates.every(isWardMissionSettled)) {
			this.fleetMissionAck(
				request.requestId,
				FleetMissionAckStatus.FLEET_MISSION_ACK_STATUS_REJECTED,
				'cannot edit while any ward is still active; stop it first',
				mission.fleetMissionId
			);
			return;
		}
		if (request.wardPlans.length === 0) {
			this.fleetMissionAck(
				request.requestId,
				FleetMissionAckStatus.FLEET_MISSION_ACK_STATUS_REJECTED,
				'no ward plans in request',
				mission.fleetMissionId
			);
			return;
		}
		mission.wardPlans = request.wardPlans;
		mission.missionName = request.missionName;
		mission.repeatCount = request.repeatCount;
		mission.status = FleetMissionStatus.FLEET_MISSION_STATUS_ACTIVE;
		mission.wardStates = request.wardPlans.map((plan) => ({
			wardId: plan.wardId,
			status: WardMissionStatus.WARD_MISSION_STATUS_UNSPECIFIED,
			message: '',
			missionId: ''
		}));
		this.dispatchWardPlans(mission, request.wardPlans, request.repeatCount, request.missionName);
		this.persistDemoFleetMissions();
		this.fleetMissionAck(
			request.requestId,
			FleetMissionAckStatus.FLEET_MISSION_ACK_STATUS_ACCEPTED,
			'fleet mission updated',
			mission.fleetMissionId
		);
		this.onEnvelope({ payload: { $case: 'fleetMission', fleetMission: mission } });
	}

	/** Resolved from ack()'s one choke point (every CommandAck this engine
	 * emits passes through it). 'start' resolves on the FIRST ack for its
	 * commandId, including EXECUTING - "the upload landed and startMission
	 * was accepted" is what ACTIVE means, not "the whole mission finished
	 * flying" (WardMissionStatus has no such state; matches the real
	 * gateway's dispatch_mission_upload_and_start, which flips UPLOADING to
	 * ACTIVE at upload-plus-auto-start time, not at mission completion).
	 * 'stop' deliberately waits for a terminal ack (skips EXECUTING): rtl/
	 * land only really stop the ward once arrive() lands it, asynchronously,
	 * possibly seconds later. */
	private resolvePendingFleetMissionCommand(
		commandId: string,
		status: CommandStatus,
		message: string
	): void {
		const pending = this.pendingCommands.get(commandId);
		if (!pending) return;
		if (pending.kind === 'stop' && status === CommandStatus.COMMAND_STATUS_EXECUTING) return;
		this.pendingCommands.delete(commandId);
		const mission = this.fleetMissions.get(pending.fleetMissionId);
		if (!mission) return;
		const state = mission.wardStates.find((candidate) => candidate.wardId === pending.wardId);
		if (!state) return;
		const succeeded =
			status === CommandStatus.COMMAND_STATUS_SUCCESS ||
			status === CommandStatus.COMMAND_STATUS_EXECUTING;
		if (pending.kind === 'start') {
			state.status = succeeded
				? WardMissionStatus.WARD_MISSION_STATUS_ACTIVE
				: WardMissionStatus.WARD_MISSION_STATUS_REJECTED;
			state.message = succeeded ? '' : message;
		} else {
			const stopped = status === CommandStatus.COMMAND_STATUS_SUCCESS;
			state.status = stopped
				? WardMissionStatus.WARD_MISSION_STATUS_STOPPED
				: WardMissionStatus.WARD_MISSION_STATUS_ACTIVE;
			state.message = stopped ? '' : message;
			this.maybeFinalizeStop(pending.fleetMissionId);
		}
		this.persistDemoFleetMissions();
		this.onEnvelope({ payload: { $case: 'fleetMission', fleetMission: mission } });
	}

	private maybeFinalizeStop(fleetMissionId: string): void {
		const mission = this.fleetMissions.get(fleetMissionId);
		if (!mission || mission.status !== FleetMissionStatus.FLEET_MISSION_STATUS_STOPPING) return;
		if (mission.wardStates.every(isWardMissionSettled)) {
			mission.status = FleetMissionStatus.FLEET_MISSION_STATUS_STOPPED;
		}
	}

	private fleetMissionAck(
		requestId: string,
		status: FleetMissionAckStatus,
		message: string,
		fleetMissionId: string
	): void {
		this.onEnvelope({
			payload: {
				$case: 'fleetMissionAck',
				fleetMissionAck: { requestId, fleetMissionId, status, message }
			}
		});
	}

	private persistDemoFleetMissions(): void {
		if (typeof localStorage === 'undefined') return;
		const missions = [...this.fleetMissions.values()];
		if (missions.length > 0) {
			localStorage.setItem(DEMO_FLEET_MISSIONS_STORAGE_KEY, JSON.stringify(missions));
		} else {
			localStorage.removeItem(DEMO_FLEET_MISSIONS_STORAGE_KEY);
		}
	}

	private handleCreateZone(request: CreateZone): void {
		const zoneId = `demo-zone-${this.zoneSpawnCount + 1}`;
		this.zoneSpawnCount += 1;
		const zone: Zone = {
			zoneId,
			name: request.name,
			type: request.type,
			vertices: request.vertices,
			altitudeMinM: request.altitudeMinM,
			altitudeMaxM: request.altitudeMaxM
		};
		this.zones.set(zoneId, zone);
		this.persistDemoZones();
		this.zoneAck(request.requestId, ZoneAckStatus.ZONE_ACK_STATUS_ACCEPTED, 'zone created', zoneId);
		this.onEnvelope({ payload: { $case: 'zone', zone } });
	}

	private handleUpdateZone(request: UpdateZone): void {
		const zone = this.zones.get(request.zoneId);
		if (!zone) {
			this.zoneAck(
				request.requestId,
				ZoneAckStatus.ZONE_ACK_STATUS_REJECTED,
				`unknown zone_id: ${request.zoneId}`,
				request.zoneId
			);
			return;
		}
		zone.name = request.name;
		zone.type = request.type;
		zone.altitudeMinM = request.altitudeMinM;
		zone.altitudeMaxM = request.altitudeMaxM;
		this.persistDemoZones();
		this.zoneAck(
			request.requestId,
			ZoneAckStatus.ZONE_ACK_STATUS_ACCEPTED,
			'zone updated',
			zone.zoneId
		);
		this.onEnvelope({ payload: { $case: 'zone', zone } });
	}

	private handleDeleteZone(request: DeleteZone): void {
		if (!this.zones.delete(request.zoneId)) {
			this.zoneAck(
				request.requestId,
				ZoneAckStatus.ZONE_ACK_STATUS_REJECTED,
				`unknown zone_id: ${request.zoneId}`,
				request.zoneId
			);
			return;
		}
		this.persistDemoZones();
		this.zoneAck(
			request.requestId,
			ZoneAckStatus.ZONE_ACK_STATUS_ACCEPTED,
			'zone deleted',
			request.zoneId
		);
	}

	private zoneAck(requestId: string, status: ZoneAckStatus, message: string, zoneId: string): void {
		this.onEnvelope({
			payload: { $case: 'zoneAck', zoneAck: { requestId, status, message, zoneId } }
		});
	}

	private persistDemoZones(): void {
		if (typeof localStorage === 'undefined') return;
		const zones = [...this.zones.values()];
		if (zones.length > 0) {
			localStorage.setItem(DEMO_ZONES_STORAGE_KEY, JSON.stringify(zones));
		} else {
			localStorage.removeItem(DEMO_ZONES_STORAGE_KEY);
		}
	}

	private handleMissionUpload(mission: Mission): void {
		const ward = this.wards.get(mission.wardId);
		if (!ward) {
			console.warn(`fake gateway: mission upload for unknown ward ${mission.wardId}`);
			return;
		}
		this.missions.set(mission.wardId, mission);
		ward.missionRun = undefined;
		this.event(
			ward,
			Severity.SEVERITY_INFO,
			'MISSION_UPLOADED',
			`mission "${mission.name}" uploaded: ${mission.items.length} items, ${mission.repeatCount} repeats`
		);
	}

	private handleCommand(command: Command): void {
		const ward = this.wards.get(command.wardId);
		if (!ward) {
			this.ack(command, CommandStatus.COMMAND_STATUS_REJECTED, 'unknown ward');
			return;
		}
		const action = command.action;
		if (!action) {
			this.ack(command, CommandStatus.COMMAND_STATUS_REJECTED, 'command has no action');
			return;
		}
		switch (action.$case) {
			case 'arm':
				if (ward.armed) {
					this.reject(command, ward, 'already armed');
				} else if (ward.batteryPct < LOW_BATTERY_PCT) {
					this.reject(command, ward, 'battery too low to arm');
				} else {
					ward.armed = true;
					ward.mode = FlightMode.FLIGHT_MODE_HOLD;
					this.ack(command, CommandStatus.COMMAND_STATUS_SUCCESS, 'armed');
				}
				break;
			case 'disarm':
				if (!ward.armed) {
					this.reject(command, ward, 'not armed');
				} else if (ward.inAir && !action.disarm.force) {
					this.reject(command, ward, 'ward is in air; use force to override');
				} else {
					this.interruptMission(ward, 'disarm command');
					if (ward.inAir) {
						this.event(
							ward,
							Severity.SEVERITY_CRITICAL,
							'FORCED_DISARM',
							'forced disarm while in air'
						);
						ward.inAir = false;
						ward.altM = 0;
					}
					this.stopMotion(ward);
					ward.armed = false;
					ward.mode = FlightMode.FLIGHT_MODE_MANUAL;
					this.ack(command, CommandStatus.COMMAND_STATUS_SUCCESS, 'disarmed');
				}
				break;
			case 'takeoff': {
				if (!ward.armed) {
					this.reject(command, ward, 'not armed');
					break;
				}
				if (ward.inAir) {
					this.reject(command, ward, 'already in air');
					break;
				}
				const altM =
					action.takeoff.altitudeRelM > 0 ? action.takeoff.altitudeRelM : DEFAULT_TAKEOFF_ALT_M;
				ward.inAir = true;
				ward.mode = FlightMode.FLIGHT_MODE_TAKEOFF;
				this.setTarget(ward, command, ward.northM, ward.eastM, altM, 0, 'hold');
				this.ack(command, CommandStatus.COMMAND_STATUS_EXECUTING, `taking off to ${altM} m`);
				break;
			}
			case 'land':
				if (!ward.inAir) {
					this.reject(command, ward, 'not in air');
				} else {
					this.interruptMission(ward, 'land command');
					ward.mode = FlightMode.FLIGHT_MODE_LAND;
					this.setTarget(ward, command, ward.northM, ward.eastM, 0, 0, 'land');
					this.ack(command, CommandStatus.COMMAND_STATUS_EXECUTING, 'landing');
				}
				break;
			case 'rtl':
				if (!ward.inAir) {
					this.reject(command, ward, 'not in air');
				} else {
					this.interruptMission(ward, 'RTL command');
					ward.mode = FlightMode.FLIGHT_MODE_RETURN;
					this.setTarget(ward, command, 0, 0, ward.altM, 0, 'land');
					this.ack(command, CommandStatus.COMMAND_STATUS_EXECUTING, 'returning to launch');
				}
				break;
			case 'goto': {
				if (!ward.inAir) {
					this.reject(command, ward, 'not in air');
					break;
				}
				const target = action.goto.target;
				if (!target) {
					this.reject(command, ward, 'goto has no target');
					break;
				}
				this.interruptMission(ward, 'goto command');
				const northM = (target.latitudeDeg - ward.spec.homeLatDeg) * METERS_PER_DEG_LAT;
				const eastM =
					(target.longitudeDeg - ward.spec.homeLonDeg) * metersPerDegLon(ward.spec.homeLatDeg);
				const altM = target.altitudeRelM > 0 ? target.altitudeRelM : ward.altM;
				ward.mode = FlightMode.FLIGHT_MODE_OFFBOARD;
				this.setTarget(ward, command, northM, eastM, altM, action.goto.speedMS, 'hold');
				this.ack(command, CommandStatus.COMMAND_STATUS_EXECUTING, 'flying to target');
				break;
			}
			case 'startMission': {
				const mission = this.missions.get(command.wardId);
				if (!mission) {
					this.reject(command, ward, 'no mission uploaded');
					break;
				}
				if (!ward.armed) {
					this.reject(command, ward, 'not armed');
					break;
				}
				if (ward.missionRun && ward.missionRun.paused) {
					ward.missionRun.paused = false;
					ward.missionRun.startCommandId = command.commandId;
					this.ack(command, CommandStatus.COMMAND_STATUS_EXECUTING, 'mission resumed');
					this.flyToCurrentItem(ward);
					break;
				}
				ward.missionRun = {
					mission,
					itemIndex: 0,
					passesLeft: mission.repeatCount,
					startCommandId: command.commandId,
					holdUntilMs: 0,
					paused: false
				};
				ward.inAir = true;
				this.ack(
					command,
					CommandStatus.COMMAND_STATUS_EXECUTING,
					`mission "${mission.name}" started`
				);
				this.flyToCurrentItem(ward);
				break;
			}
			case 'pauseMission':
				if (!ward.missionRun || ward.missionRun.paused) {
					this.reject(command, ward, 'no mission running');
				} else {
					ward.missionRun.paused = true;
					this.stopMotion(ward);
					ward.mode = FlightMode.FLIGHT_MODE_HOLD;
					this.ack(command, CommandStatus.COMMAND_STATUS_SUCCESS, 'mission paused');
				}
				break;
			default: {
				const unhandled: never = action;
				this.ack(command, CommandStatus.COMMAND_STATUS_REJECTED, `unsupported action ${unhandled}`);
			}
		}
	}

	private setTarget(
		ward: SimWard,
		command: Command,
		northM: number,
		eastM: number,
		altM: number,
		speedMS: number,
		onArrival: Arrival
	): void {
		// a newer maneuver preempts the current one; its command still gets a
		// terminal ack so the console tracker never dangles
		const previous = ward.target;
		if (previous && previous.commandId !== command.commandId) {
			this.onEnvelope({
				payload: {
					$case: 'commandAck',
					commandAck: {
						commandId: previous.commandId,
						wardId: previous.commandWardId,
						status: CommandStatus.COMMAND_STATUS_REJECTED,
						message: 'superseded by a newer command'
					}
				}
			});
		}
		ward.patrolling = false;
		ward.target = {
			northM,
			eastM,
			altM,
			speedMS: speedMS > 0 ? speedMS : CRUISE_SPEED_M_S,
			onArrival,
			commandId: command.commandId,
			commandWardId: command.wardId
		};
	}

	private stopMotion(ward: SimWard): void {
		ward.patrolling = false;
		ward.target = undefined;
		ward.velocityNorth = 0;
		ward.velocityEast = 0;
		ward.velocityDown = 0;
	}

	private interruptMission(ward: SimWard, reason: string): void {
		const run = ward.missionRun;
		if (!run) return;
		ward.missionRun = undefined;
		this.stopMotion(ward);
		this.onEnvelope({
			payload: {
				$case: 'commandAck',
				commandAck: {
					commandId: run.startCommandId,
					wardId: ward.spec.wardId,
					status: CommandStatus.COMMAND_STATUS_REJECTED,
					message: `mission interrupted: ${reason}`
				}
			}
		});
		this.event(ward, Severity.SEVERITY_WARNING, 'MISSION_INTERRUPTED', reason);
	}

	private flyToCurrentItem(ward: SimWard): void {
		const run = ward.missionRun;
		if (!run || run.paused) return;
		const item = run.mission.items[run.itemIndex];
		if (!item || item.action !== MissionAction.MISSION_ACTION_WAYPOINT || !item.position) {
			// the editor only produces waypoints; skip anything else observably
			if (item) {
				this.event(
					ward,
					Severity.SEVERITY_WARNING,
					'MISSION_ITEM_SKIPPED',
					`item ${item.seq} not supported by the simulator`
				);
			}
			this.advanceMission(ward);
			return;
		}
		const northM = (item.position.latitudeDeg - ward.spec.homeLatDeg) * METERS_PER_DEG_LAT;
		const eastM =
			(item.position.longitudeDeg - ward.spec.homeLonDeg) * metersPerDegLon(ward.spec.homeLatDeg);
		const altM = item.position.altitudeRelM > 0 ? item.position.altitudeRelM : ward.altM;
		ward.mode = FlightMode.FLIGHT_MODE_MISSION;
		this.setTarget(
			ward,
			{
				commandId: run.startCommandId,
				wardId: ward.spec.wardId,
				timestampMs: Date.now(),
				action: undefined
			},
			northM,
			eastM,
			altM,
			item.speedMS,
			'mission-item'
		);
		// current_seq means the item being executed right now
		this.emitProgress(ward, run, item.seq, false);
	}

	/** step past the current item: next item, next pass, or finish */
	private advanceMission(ward: SimWard): void {
		const run = ward.missionRun;
		if (!run) return;
		run.itemIndex += 1;
		if (run.itemIndex < run.mission.items.length) {
			this.flyToCurrentItem(ward);
			return;
		}
		if (run.passesLeft > 0) {
			run.passesLeft -= 1;
			run.itemIndex = 0;
			this.flyToCurrentItem(ward);
			return;
		}
		ward.missionRun = undefined;
		ward.mode = FlightMode.FLIGHT_MODE_HOLD;
		this.emitProgress(ward, run, run.mission.items.length - 1, true);
		this.event(
			ward,
			Severity.SEVERITY_INFO,
			'MISSION_FINISHED',
			`mission "${run.mission.name}" finished`
		);
		this.onEnvelope({
			payload: {
				$case: 'commandAck',
				commandAck: {
					commandId: run.startCommandId,
					wardId: ward.spec.wardId,
					status: CommandStatus.COMMAND_STATUS_SUCCESS,
					message: 'mission finished'
				}
			}
		});
	}

	private emitProgress(
		ward: SimWard,
		run: MissionRun,
		currentSeq: number,
		finished: boolean
	): void {
		this.onEnvelope({
			payload: {
				$case: 'missionProgress',
				missionProgress: {
					wardId: ward.spec.wardId,
					missionId: run.mission.missionId,
					currentSeq,
					totalItems: run.mission.items.length,
					finished
				}
			}
		});
	}

	private tick(): void {
		const nowMs = Date.now();
		for (const ward of this.wards.values()) {
			const run = ward.missionRun;
			if (run && !run.paused && !ward.target && run.holdUntilMs > 0 && nowMs >= run.holdUntilMs) {
				run.holdUntilMs = 0;
				this.advanceMission(ward);
			}
			this.advance(ward);
			this.drainBattery(ward);
			this.onEnvelope(stateEnvelope(ward, nowMs));
		}
	}

	private advance(ward: SimWard): void {
		if (ward.patrolling) {
			const angularVel = (2 * Math.PI) / ward.spec.periodS;
			ward.thetaRad += angularVel * TICK_S;
			ward.northM = ward.spec.radiusM * Math.cos(ward.thetaRad);
			ward.eastM = ward.spec.radiusM * Math.sin(ward.thetaRad);
			ward.velocityNorth = -ward.spec.radiusM * angularVel * Math.sin(ward.thetaRad);
			ward.velocityEast = ward.spec.radiusM * angularVel * Math.cos(ward.thetaRad);
			ward.velocityDown = 0;
			ward.headingDeg = headingFrom(ward.velocityNorth, ward.velocityEast);
			return;
		}
		const target = ward.target;
		if (!target) return;

		const dNorth = target.northM - ward.northM;
		const dEast = target.eastM - ward.eastM;
		const dAlt = target.altM - ward.altM;
		const horizontal = Math.hypot(dNorth, dEast);

		if (horizontal < ARRIVAL_RADIUS_M && Math.abs(dAlt) < 0.5) {
			ward.northM = target.northM;
			ward.eastM = target.eastM;
			ward.altM = target.altM;
			this.stopMotion(ward);
			this.arrive(ward, target);
			return;
		}

		const stepH = Math.min(target.speedMS * TICK_S, horizontal);
		if (horizontal > 0) {
			ward.northM += (dNorth / horizontal) * stepH;
			ward.eastM += (dEast / horizontal) * stepH;
			ward.velocityNorth = (dNorth / horizontal) * target.speedMS;
			ward.velocityEast = (dEast / horizontal) * target.speedMS;
			ward.headingDeg = headingFrom(dNorth, dEast);
		} else {
			ward.velocityNorth = 0;
			ward.velocityEast = 0;
		}
		const stepV = Math.min(CLIMB_RATE_M_S * TICK_S, Math.abs(dAlt));
		ward.altM += Math.sign(dAlt) * stepV;
		ward.velocityDown = -Math.sign(dAlt) * (stepV > 0 ? CLIMB_RATE_M_S : 0);
	}

	private arrive(ward: SimWard, target: Target): void {
		const pseudoCommand: Command = {
			commandId: target.commandId,
			wardId: target.commandWardId,
			timestampMs: Date.now(),
			action: undefined
		};
		switch (target.onArrival) {
			case 'mission-item': {
				const run = ward.missionRun;
				if (!run) break;
				const item = run.mission.items[run.itemIndex];
				if (item && item.holdTimeS > 0) {
					// stay here; the tick loop advances when the hold elapses
					run.holdUntilMs = Date.now() + item.holdTimeS * 1000;
				} else {
					this.advanceMission(ward);
				}
				break;
			}
			case 'hold':
				ward.mode = FlightMode.FLIGHT_MODE_HOLD;
				this.ack(pseudoCommand, CommandStatus.COMMAND_STATUS_SUCCESS, 'holding position');
				break;
			case 'land':
				if (ward.altM <= 0.01) {
					ward.altM = 0;
					ward.inAir = false;
					ward.armed = false;
					ward.mode = FlightMode.FLIGHT_MODE_MANUAL;
					this.event(ward, Severity.SEVERITY_INFO, 'LANDED', 'landed and disarmed');
					this.ack(pseudoCommand, CommandStatus.COMMAND_STATUS_SUCCESS, 'landed');
				} else {
					// reached the horizontal point; now descend in place
					ward.mode = FlightMode.FLIGHT_MODE_LAND;
					this.setTarget(ward, pseudoCommand, ward.northM, ward.eastM, 0, 0, 'land');
				}
				break;
		}
	}

	private drainBattery(ward: SimWard): void {
		const drain = ward.inAir ? BATTERY_DRAIN_AIR_PCT_PER_S : BATTERY_DRAIN_GROUND_PCT_PER_S;
		ward.batteryPct = Math.max(0, ward.batteryPct - drain * TICK_S);
		if (!ward.lowBatteryReported && ward.batteryPct < LOW_BATTERY_PCT) {
			ward.lowBatteryReported = true;
			this.event(
				ward,
				Severity.SEVERITY_WARNING,
				'LOW_BATTERY',
				`battery at ${ward.batteryPct.toFixed(0)}%`
			);
		}
	}

	private reject(command: Command, ward: SimWard, reason: string): void {
		this.ack(command, CommandStatus.COMMAND_STATUS_REJECTED, reason);
		this.event(ward, Severity.SEVERITY_WARNING, 'COMMAND_REJECTED', reason);
	}

	private ack(command: Command, status: CommandStatus, message: string): void {
		this.onEnvelope({
			payload: {
				$case: 'commandAck',
				commandAck: {
					commandId: command.commandId,
					wardId: command.wardId,
					status,
					message
				}
			}
		});
		this.resolvePendingFleetMissionCommand(command.commandId, status, message);
	}

	private event(ward: SimWard, severity: Severity, code: string, message: string): void {
		this.onEnvelope({
			payload: {
				$case: 'event',
				event: {
					wardId: ward.spec.wardId,
					timestampMs: Date.now(),
					severity,
					code,
					message
				}
			}
		});
	}
}

/** STOPPED or REJECTED: never started, or already stopped - the same gate
 * Remove and Edit both apply, mirrors FleetManager's identical helper. */
function isWardMissionSettled(state: WardMissionState): boolean {
	return (
		state.status === WardMissionStatus.WARD_MISSION_STATUS_STOPPED ||
		state.status === WardMissionStatus.WARD_MISSION_STATUS_REJECTED
	);
}

function metersPerDegLon(homeLatDeg: number): number {
	return METERS_PER_DEG_LAT * Math.cos((homeLatDeg * Math.PI) / 180);
}

function headingFrom(north: number, east: number): number {
	return ((Math.atan2(east, north) * 180) / Math.PI + 360) % 360;
}

function stateEnvelope(ward: SimWard, nowMs: number): Envelope {
	return {
		payload: {
			$case: 'wardState',
			wardState: {
				wardId: ward.spec.wardId,
				timestampMs: nowMs,
				position: {
					latitudeDeg: ward.spec.homeLatDeg + ward.northM / METERS_PER_DEG_LAT,
					longitudeDeg: ward.spec.homeLonDeg + ward.eastM / metersPerDegLon(ward.spec.homeLatDeg),
					altitudeMslM: HOME_ALT_MSL_M + ward.altM,
					altitudeRelM: ward.altM
				},
				velocity: {
					northMS: ward.velocityNorth,
					eastMS: ward.velocityEast,
					downMS: ward.velocityDown
				},
				headingDeg: ward.headingDeg,
				battery: { voltageV: 15.8, remainingPct: ward.batteryPct },
				gps: { fixType: GpsFixType.GPS_FIX_TYPE_FIX_3D, numSatellites: 14, hdop: 0.8 },
				healthOk: true,
				connected: true,
				flight: {
					flightMode: ward.mode,
					armed: ward.armed,
					inAir: ward.inAir
				},
				tags: []
			}
		}
	};
}
