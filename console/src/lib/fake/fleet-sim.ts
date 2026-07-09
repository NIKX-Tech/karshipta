import { FlightMode, GpsFixType, Severity, VehicleType } from '$lib/gen/karshipta/v1/common';
import {
	CommandStatus,
	MissionAction,
	type Command,
	type Mission
} from '$lib/gen/karshipta/v1/command';
import type { Envelope } from '$lib/gen/karshipta/v1/envelope';
import type { FleetTransport } from '$lib/transport';

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

interface VehicleSpec {
	vehicleId: string;
	radiusM: number;
	periodS: number;
	cruiseAltM: number;
	phaseRad: number;
}

const FLEET_SPECS: VehicleSpec[] = [
	{ vehicleId: 'sitl-1', radiusM: 60, periodS: 45, cruiseAltM: 20, phaseRad: 0 },
	{ vehicleId: 'sitl-2', radiusM: 100, periodS: 70, cruiseAltM: 35, phaseRad: (2 * Math.PI) / 3 },
	{ vehicleId: 'sitl-3', radiusM: 140, periodS: 95, cruiseAltM: 50, phaseRad: (4 * Math.PI) / 3 }
];

/** what the vehicle does when it reaches its current target */
type Arrival = 'hold' | 'land' | 'mission-item';

interface Target {
	northM: number;
	eastM: number;
	altM: number;
	speedMS: number;
	onArrival: Arrival;
	commandId: string;
	commandVehicleId: string;
}

interface SimVehicle {
	spec: VehicleSpec;
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

function initialVehicle(spec: VehicleSpec): SimVehicle {
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

export class FakeGateway implements FleetTransport {
	private vehicles = new Map<string, SimVehicle>();
	private missions = new Map<string, Mission>();
	private timer: ReturnType<typeof setInterval> | undefined;

	constructor(private readonly onEnvelope: (envelope: Envelope) => void) {}

	start(): void {
		if (this.timer !== undefined) return;
		for (const spec of FLEET_SPECS) {
			this.vehicles.set(spec.vehicleId, initialVehicle(spec));
			this.onEnvelope({
				payload: {
					$case: 'vehicleInfo',
					vehicleInfo: {
						vehicleId: spec.vehicleId,
						type: VehicleType.VEHICLE_TYPE_MULTIROTOR,
						autopilot: 'PX4',
						firmwareVersion: 'sim-fake',
						mavlinkSystemId: 0
					}
				}
			});
		}
		this.timer = setInterval(() => this.tick(), 1000 / TICK_HZ);
	}

	stop(): void {
		if (this.timer !== undefined) {
			clearInterval(this.timer);
			this.timer = undefined;
		}
		this.vehicles.clear();
		this.missions.clear();
	}

	send(envelope: Envelope): void {
		switch (envelope.payload?.$case) {
			case 'command':
				this.handleCommand(envelope.payload.command);
				break;
			case 'missionUpload':
				this.handleMissionUpload(envelope.payload.missionUpload);
				break;
			default:
				console.warn(
					'fake gateway: ignoring unsupported upstream payload',
					envelope.payload?.$case
				);
		}
	}

	private handleMissionUpload(mission: Mission): void {
		const vehicle = this.vehicles.get(mission.vehicleId);
		if (!vehicle) {
			console.warn(`fake gateway: mission upload for unknown vehicle ${mission.vehicleId}`);
			return;
		}
		this.missions.set(mission.vehicleId, mission);
		vehicle.missionRun = undefined;
		this.event(
			vehicle,
			Severity.SEVERITY_INFO,
			'MISSION_UPLOADED',
			`mission "${mission.name}" uploaded: ${mission.items.length} items, ${mission.repeatCount} repeats`
		);
	}

	private handleCommand(command: Command): void {
		const vehicle = this.vehicles.get(command.vehicleId);
		if (!vehicle) {
			this.ack(command, CommandStatus.COMMAND_STATUS_REJECTED, 'unknown vehicle');
			return;
		}
		const action = command.action;
		if (!action) {
			this.ack(command, CommandStatus.COMMAND_STATUS_REJECTED, 'command has no action');
			return;
		}
		switch (action.$case) {
			case 'arm':
				if (vehicle.armed) {
					this.reject(command, vehicle, 'already armed');
				} else if (vehicle.batteryPct < LOW_BATTERY_PCT) {
					this.reject(command, vehicle, 'battery too low to arm');
				} else {
					vehicle.armed = true;
					vehicle.mode = FlightMode.FLIGHT_MODE_HOLD;
					this.ack(command, CommandStatus.COMMAND_STATUS_SUCCESS, 'armed');
				}
				break;
			case 'disarm':
				if (!vehicle.armed) {
					this.reject(command, vehicle, 'not armed');
				} else if (vehicle.inAir && !action.disarm.force) {
					this.reject(command, vehicle, 'vehicle is in air; use force to override');
				} else {
					this.interruptMission(vehicle, 'disarm command');
					if (vehicle.inAir) {
						this.event(
							vehicle,
							Severity.SEVERITY_CRITICAL,
							'FORCED_DISARM',
							'forced disarm while in air'
						);
						vehicle.inAir = false;
						vehicle.altM = 0;
					}
					this.stopMotion(vehicle);
					vehicle.armed = false;
					vehicle.mode = FlightMode.FLIGHT_MODE_MANUAL;
					this.ack(command, CommandStatus.COMMAND_STATUS_SUCCESS, 'disarmed');
				}
				break;
			case 'takeoff': {
				if (!vehicle.armed) {
					this.reject(command, vehicle, 'not armed');
					break;
				}
				if (vehicle.inAir) {
					this.reject(command, vehicle, 'already in air');
					break;
				}
				const altM =
					action.takeoff.altitudeRelM > 0 ? action.takeoff.altitudeRelM : DEFAULT_TAKEOFF_ALT_M;
				vehicle.inAir = true;
				vehicle.mode = FlightMode.FLIGHT_MODE_TAKEOFF;
				this.setTarget(vehicle, command, vehicle.northM, vehicle.eastM, altM, 0, 'hold');
				this.ack(command, CommandStatus.COMMAND_STATUS_EXECUTING, `taking off to ${altM} m`);
				break;
			}
			case 'land':
				if (!vehicle.inAir) {
					this.reject(command, vehicle, 'not in air');
				} else {
					this.interruptMission(vehicle, 'land command');
					vehicle.mode = FlightMode.FLIGHT_MODE_LAND;
					this.setTarget(vehicle, command, vehicle.northM, vehicle.eastM, 0, 0, 'land');
					this.ack(command, CommandStatus.COMMAND_STATUS_EXECUTING, 'landing');
				}
				break;
			case 'rtl':
				if (!vehicle.inAir) {
					this.reject(command, vehicle, 'not in air');
				} else {
					this.interruptMission(vehicle, 'RTL command');
					vehicle.mode = FlightMode.FLIGHT_MODE_RETURN;
					this.setTarget(vehicle, command, 0, 0, vehicle.altM, 0, 'land');
					this.ack(command, CommandStatus.COMMAND_STATUS_EXECUTING, 'returning to launch');
				}
				break;
			case 'goto': {
				if (!vehicle.inAir) {
					this.reject(command, vehicle, 'not in air');
					break;
				}
				const target = action.goto.target;
				if (!target) {
					this.reject(command, vehicle, 'goto has no target');
					break;
				}
				this.interruptMission(vehicle, 'goto command');
				const northM = (target.latitudeDeg - HOME_LAT_DEG) * METERS_PER_DEG_LAT;
				const eastM = (target.longitudeDeg - HOME_LON_DEG) * metersPerDegLon();
				const altM = target.altitudeRelM > 0 ? target.altitudeRelM : vehicle.altM;
				vehicle.mode = FlightMode.FLIGHT_MODE_OFFBOARD;
				this.setTarget(vehicle, command, northM, eastM, altM, action.goto.speedMS, 'hold');
				this.ack(command, CommandStatus.COMMAND_STATUS_EXECUTING, 'flying to target');
				break;
			}
			case 'startMission': {
				const mission = this.missions.get(command.vehicleId);
				if (!mission) {
					this.reject(command, vehicle, 'no mission uploaded');
					break;
				}
				if (!vehicle.armed) {
					this.reject(command, vehicle, 'not armed');
					break;
				}
				if (vehicle.missionRun && vehicle.missionRun.paused) {
					vehicle.missionRun.paused = false;
					vehicle.missionRun.startCommandId = command.commandId;
					this.ack(command, CommandStatus.COMMAND_STATUS_EXECUTING, 'mission resumed');
					this.flyToCurrentItem(vehicle);
					break;
				}
				vehicle.missionRun = {
					mission,
					itemIndex: 0,
					passesLeft: mission.repeatCount,
					startCommandId: command.commandId,
					holdUntilMs: 0,
					paused: false
				};
				vehicle.inAir = true;
				this.ack(
					command,
					CommandStatus.COMMAND_STATUS_EXECUTING,
					`mission "${mission.name}" started`
				);
				this.flyToCurrentItem(vehicle);
				break;
			}
			case 'pauseMission':
				if (!vehicle.missionRun || vehicle.missionRun.paused) {
					this.reject(command, vehicle, 'no mission running');
				} else {
					vehicle.missionRun.paused = true;
					this.stopMotion(vehicle);
					vehicle.mode = FlightMode.FLIGHT_MODE_HOLD;
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
		vehicle: SimVehicle,
		command: Command,
		northM: number,
		eastM: number,
		altM: number,
		speedMS: number,
		onArrival: Arrival
	): void {
		// a newer maneuver preempts the current one; its command still gets a
		// terminal ack so the console tracker never dangles
		const previous = vehicle.target;
		if (previous && previous.commandId !== command.commandId) {
			this.onEnvelope({
				payload: {
					$case: 'commandAck',
					commandAck: {
						commandId: previous.commandId,
						vehicleId: previous.commandVehicleId,
						status: CommandStatus.COMMAND_STATUS_REJECTED,
						message: 'superseded by a newer command'
					}
				}
			});
		}
		vehicle.patrolling = false;
		vehicle.target = {
			northM,
			eastM,
			altM,
			speedMS: speedMS > 0 ? speedMS : CRUISE_SPEED_M_S,
			onArrival,
			commandId: command.commandId,
			commandVehicleId: command.vehicleId
		};
	}

	private stopMotion(vehicle: SimVehicle): void {
		vehicle.patrolling = false;
		vehicle.target = undefined;
		vehicle.velocityNorth = 0;
		vehicle.velocityEast = 0;
		vehicle.velocityDown = 0;
	}

	private interruptMission(vehicle: SimVehicle, reason: string): void {
		const run = vehicle.missionRun;
		if (!run) return;
		vehicle.missionRun = undefined;
		this.stopMotion(vehicle);
		this.onEnvelope({
			payload: {
				$case: 'commandAck',
				commandAck: {
					commandId: run.startCommandId,
					vehicleId: vehicle.spec.vehicleId,
					status: CommandStatus.COMMAND_STATUS_REJECTED,
					message: `mission interrupted: ${reason}`
				}
			}
		});
		this.event(vehicle, Severity.SEVERITY_WARNING, 'MISSION_INTERRUPTED', reason);
	}

	private flyToCurrentItem(vehicle: SimVehicle): void {
		const run = vehicle.missionRun;
		if (!run || run.paused) return;
		const item = run.mission.items[run.itemIndex];
		if (!item || item.action !== MissionAction.MISSION_ACTION_WAYPOINT || !item.position) {
			// the editor only produces waypoints; skip anything else observably
			if (item) {
				this.event(
					vehicle,
					Severity.SEVERITY_WARNING,
					'MISSION_ITEM_SKIPPED',
					`item ${item.seq} not supported by the simulator`
				);
			}
			this.advanceMission(vehicle);
			return;
		}
		const northM = (item.position.latitudeDeg - HOME_LAT_DEG) * METERS_PER_DEG_LAT;
		const eastM = (item.position.longitudeDeg - HOME_LON_DEG) * metersPerDegLon();
		const altM = item.position.altitudeRelM > 0 ? item.position.altitudeRelM : vehicle.altM;
		vehicle.mode = FlightMode.FLIGHT_MODE_MISSION;
		this.setTarget(
			vehicle,
			{
				commandId: run.startCommandId,
				vehicleId: vehicle.spec.vehicleId,
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
		this.emitProgress(vehicle, run, item.seq, false);
	}

	/** step past the current item: next item, next pass, or finish */
	private advanceMission(vehicle: SimVehicle): void {
		const run = vehicle.missionRun;
		if (!run) return;
		run.itemIndex += 1;
		if (run.itemIndex < run.mission.items.length) {
			this.flyToCurrentItem(vehicle);
			return;
		}
		if (run.passesLeft > 0) {
			run.passesLeft -= 1;
			run.itemIndex = 0;
			this.flyToCurrentItem(vehicle);
			return;
		}
		vehicle.missionRun = undefined;
		vehicle.mode = FlightMode.FLIGHT_MODE_HOLD;
		this.emitProgress(vehicle, run, run.mission.items.length - 1, true);
		this.event(
			vehicle,
			Severity.SEVERITY_INFO,
			'MISSION_FINISHED',
			`mission "${run.mission.name}" finished`
		);
		this.onEnvelope({
			payload: {
				$case: 'commandAck',
				commandAck: {
					commandId: run.startCommandId,
					vehicleId: vehicle.spec.vehicleId,
					status: CommandStatus.COMMAND_STATUS_SUCCESS,
					message: 'mission finished'
				}
			}
		});
	}

	private emitProgress(
		vehicle: SimVehicle,
		run: MissionRun,
		currentSeq: number,
		finished: boolean
	): void {
		this.onEnvelope({
			payload: {
				$case: 'missionProgress',
				missionProgress: {
					vehicleId: vehicle.spec.vehicleId,
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
		for (const vehicle of this.vehicles.values()) {
			const run = vehicle.missionRun;
			if (
				run &&
				!run.paused &&
				!vehicle.target &&
				run.holdUntilMs > 0 &&
				nowMs >= run.holdUntilMs
			) {
				run.holdUntilMs = 0;
				this.advanceMission(vehicle);
			}
			this.advance(vehicle);
			this.drainBattery(vehicle);
			this.onEnvelope(stateEnvelope(vehicle, nowMs));
		}
	}

	private advance(vehicle: SimVehicle): void {
		if (vehicle.patrolling) {
			const angularVel = (2 * Math.PI) / vehicle.spec.periodS;
			vehicle.thetaRad += angularVel * TICK_S;
			vehicle.northM = vehicle.spec.radiusM * Math.cos(vehicle.thetaRad);
			vehicle.eastM = vehicle.spec.radiusM * Math.sin(vehicle.thetaRad);
			vehicle.velocityNorth = -vehicle.spec.radiusM * angularVel * Math.sin(vehicle.thetaRad);
			vehicle.velocityEast = vehicle.spec.radiusM * angularVel * Math.cos(vehicle.thetaRad);
			vehicle.velocityDown = 0;
			vehicle.headingDeg = headingFrom(vehicle.velocityNorth, vehicle.velocityEast);
			return;
		}
		const target = vehicle.target;
		if (!target) return;

		const dNorth = target.northM - vehicle.northM;
		const dEast = target.eastM - vehicle.eastM;
		const dAlt = target.altM - vehicle.altM;
		const horizontal = Math.hypot(dNorth, dEast);

		if (horizontal < ARRIVAL_RADIUS_M && Math.abs(dAlt) < 0.5) {
			vehicle.northM = target.northM;
			vehicle.eastM = target.eastM;
			vehicle.altM = target.altM;
			this.stopMotion(vehicle);
			this.arrive(vehicle, target);
			return;
		}

		const stepH = Math.min(target.speedMS * TICK_S, horizontal);
		if (horizontal > 0) {
			vehicle.northM += (dNorth / horizontal) * stepH;
			vehicle.eastM += (dEast / horizontal) * stepH;
			vehicle.velocityNorth = (dNorth / horizontal) * target.speedMS;
			vehicle.velocityEast = (dEast / horizontal) * target.speedMS;
			vehicle.headingDeg = headingFrom(dNorth, dEast);
		} else {
			vehicle.velocityNorth = 0;
			vehicle.velocityEast = 0;
		}
		const stepV = Math.min(CLIMB_RATE_M_S * TICK_S, Math.abs(dAlt));
		vehicle.altM += Math.sign(dAlt) * stepV;
		vehicle.velocityDown = -Math.sign(dAlt) * (stepV > 0 ? CLIMB_RATE_M_S : 0);
	}

	private arrive(vehicle: SimVehicle, target: Target): void {
		const pseudoCommand: Command = {
			commandId: target.commandId,
			vehicleId: target.commandVehicleId,
			timestampMs: Date.now(),
			action: undefined
		};
		switch (target.onArrival) {
			case 'mission-item': {
				const run = vehicle.missionRun;
				if (!run) break;
				const item = run.mission.items[run.itemIndex];
				if (item && item.holdTimeS > 0) {
					// stay here; the tick loop advances when the hold elapses
					run.holdUntilMs = Date.now() + item.holdTimeS * 1000;
				} else {
					this.advanceMission(vehicle);
				}
				break;
			}
			case 'hold':
				vehicle.mode = FlightMode.FLIGHT_MODE_HOLD;
				this.ack(pseudoCommand, CommandStatus.COMMAND_STATUS_SUCCESS, 'holding position');
				break;
			case 'land':
				if (vehicle.altM <= 0.01) {
					vehicle.altM = 0;
					vehicle.inAir = false;
					vehicle.armed = false;
					vehicle.mode = FlightMode.FLIGHT_MODE_MANUAL;
					this.event(vehicle, Severity.SEVERITY_INFO, 'LANDED', 'landed and disarmed');
					this.ack(pseudoCommand, CommandStatus.COMMAND_STATUS_SUCCESS, 'landed');
				} else {
					// reached the horizontal point; now descend in place
					vehicle.mode = FlightMode.FLIGHT_MODE_LAND;
					this.setTarget(vehicle, pseudoCommand, vehicle.northM, vehicle.eastM, 0, 0, 'land');
				}
				break;
		}
	}

	private drainBattery(vehicle: SimVehicle): void {
		const drain = vehicle.inAir ? BATTERY_DRAIN_AIR_PCT_PER_S : BATTERY_DRAIN_GROUND_PCT_PER_S;
		vehicle.batteryPct = Math.max(0, vehicle.batteryPct - drain * TICK_S);
		if (!vehicle.lowBatteryReported && vehicle.batteryPct < LOW_BATTERY_PCT) {
			vehicle.lowBatteryReported = true;
			this.event(
				vehicle,
				Severity.SEVERITY_WARNING,
				'LOW_BATTERY',
				`battery at ${vehicle.batteryPct.toFixed(0)}%`
			);
		}
	}

	private reject(command: Command, vehicle: SimVehicle, reason: string): void {
		this.ack(command, CommandStatus.COMMAND_STATUS_REJECTED, reason);
		this.event(vehicle, Severity.SEVERITY_WARNING, 'COMMAND_REJECTED', reason);
	}

	private ack(command: Command, status: CommandStatus, message: string): void {
		this.onEnvelope({
			payload: {
				$case: 'commandAck',
				commandAck: {
					commandId: command.commandId,
					vehicleId: command.vehicleId,
					status,
					message
				}
			}
		});
	}

	private event(vehicle: SimVehicle, severity: Severity, code: string, message: string): void {
		this.onEnvelope({
			payload: {
				$case: 'event',
				event: {
					vehicleId: vehicle.spec.vehicleId,
					timestampMs: Date.now(),
					severity,
					code,
					message
				}
			}
		});
	}
}

function metersPerDegLon(): number {
	return METERS_PER_DEG_LAT * Math.cos((HOME_LAT_DEG * Math.PI) / 180);
}

function headingFrom(north: number, east: number): number {
	return ((Math.atan2(east, north) * 180) / Math.PI + 360) % 360;
}

function stateEnvelope(vehicle: SimVehicle, nowMs: number): Envelope {
	return {
		payload: {
			$case: 'vehicleState',
			vehicleState: {
				vehicleId: vehicle.spec.vehicleId,
				timestampMs: nowMs,
				position: {
					latitudeDeg: HOME_LAT_DEG + vehicle.northM / METERS_PER_DEG_LAT,
					longitudeDeg: HOME_LON_DEG + vehicle.eastM / metersPerDegLon(),
					altitudeMslM: HOME_ALT_MSL_M + vehicle.altM,
					altitudeRelM: vehicle.altM
				},
				velocity: {
					northMS: vehicle.velocityNorth,
					eastMS: vehicle.velocityEast,
					downMS: vehicle.velocityDown
				},
				headingDeg: vehicle.headingDeg,
				battery: { voltageV: 15.8, remainingPct: vehicle.batteryPct },
				gps: { fixType: GpsFixType.GPS_FIX_TYPE_FIX_3D, numSatellites: 14, hdop: 0.8 },
				flightMode: vehicle.mode,
				armed: vehicle.armed,
				inAir: vehicle.inAir,
				healthOk: true,
				connected: true
			}
		}
	};
}
