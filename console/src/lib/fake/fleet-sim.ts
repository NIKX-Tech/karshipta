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
	type AddWardToFleet,
	type CreateFleet,
	type DeleteFleet,
	type Fleet,
	type RemoveWardFromFleet,
	type RenameFleet
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

// PX4 SITL default home (Zurich Irchel), so the fake fleet and the real
// docker-compose demo appear in the same place on the map.
const HOME_LAT_DEG = 47.397742;
const HOME_LON_DEG = 8.545594;
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

/** owner-facing contract for spawning/removing demo wards from the UI */
export interface DemoEngine extends FleetTransport {
	/** spawns one demo ward with a procedurally varied patrol centered at
	 * `location` (defaults to FAKE_FLEET_CENTER); returns its id */
	spawnWard(location?: LatLon): string;
	despawnWard(wardId: string): void;
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

	constructor(private readonly onEnvelope: (envelope: Envelope) => void) {
		for (const fleet of readStoredDemoFleets()) this.fleets.set(fleet.fleetId, fleet);
	}

	start(): void {
		if (this.timer !== undefined) return;
		// "connect" sync: mirrors the gateway's send_fleet_zone_snapshot, so a
		// Fleet tab reload still shows whatever was persisted last session.
		for (const fleet of this.fleets.values()) {
			this.onEnvelope({ payload: { $case: 'fleet', fleet } });
		}
		this.timer = setInterval(() => this.tick(), 1000 / TICK_HZ);
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
