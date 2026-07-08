import { FlightMode, GpsFixType, VehicleType } from '$lib/gen/karshipta/v1/common';
import type { Envelope } from '$lib/gen/karshipta/v1/envelope';

/**
 * Emits Envelopes for three simulated multirotors circling the PX4 SITL
 * default home position, through the same sink the real transport feeds.
 * Dev stand-in until the gateway telemetry milestone lands.
 */

// PX4 SITL default home (Zurich Irchel), so the fake fleet and the real
// docker-compose demo appear in the same place on the map.
const HOME_LAT_DEG = 47.397742;
const HOME_LON_DEG = 8.545594;
const HOME_ALT_MSL_M = 488;

const TICK_HZ = 5;
const BATTERY_DRAIN_PCT_PER_S = 0.05;
const METERS_PER_DEG_LAT = 111_320;

interface FakeVehicle {
	vehicleId: string;
	radiusM: number;
	periodS: number;
	altitudeRelM: number;
	phaseRad: number;
}

const FAKE_FLEET: FakeVehicle[] = [
	{ vehicleId: 'sitl-1', radiusM: 60, periodS: 45, altitudeRelM: 20, phaseRad: 0 },
	{ vehicleId: 'sitl-2', radiusM: 100, periodS: 70, altitudeRelM: 35, phaseRad: (2 * Math.PI) / 3 },
	{ vehicleId: 'sitl-3', radiusM: 140, periodS: 95, altitudeRelM: 50, phaseRad: (4 * Math.PI) / 3 }
];

export const FAKE_FLEET_CENTER = { lat: HOME_LAT_DEG, lon: HOME_LON_DEG };

function infoEnvelope(vehicle: FakeVehicle): Envelope {
	return {
		payload: {
			$case: 'vehicleInfo',
			vehicleInfo: {
				vehicleId: vehicle.vehicleId,
				type: VehicleType.VEHICLE_TYPE_MULTIROTOR,
				autopilot: 'PX4',
				firmwareVersion: 'sim-fake',
				mavlinkSystemId: 0
			}
		}
	};
}

function stateEnvelope(vehicle: FakeVehicle, elapsedS: number, nowMs: number): Envelope {
	const angularVel = (2 * Math.PI) / vehicle.periodS;
	const theta = vehicle.phaseRad + angularVel * elapsedS;
	const northM = vehicle.radiusM * Math.cos(theta);
	const eastM = vehicle.radiusM * Math.sin(theta);
	const northVel = -vehicle.radiusM * angularVel * Math.sin(theta);
	const eastVel = vehicle.radiusM * angularVel * Math.cos(theta);
	const headingDeg = ((Math.atan2(eastVel, northVel) * 180) / Math.PI + 360) % 360;
	const metersPerDegLon = METERS_PER_DEG_LAT * Math.cos((HOME_LAT_DEG * Math.PI) / 180);

	return {
		payload: {
			$case: 'vehicleState',
			vehicleState: {
				vehicleId: vehicle.vehicleId,
				timestampMs: nowMs,
				position: {
					latitudeDeg: HOME_LAT_DEG + northM / METERS_PER_DEG_LAT,
					longitudeDeg: HOME_LON_DEG + eastM / metersPerDegLon,
					altitudeMslM: HOME_ALT_MSL_M + vehicle.altitudeRelM,
					altitudeRelM: vehicle.altitudeRelM
				},
				velocity: { northMS: northVel, eastMS: eastVel, downMS: 0 },
				headingDeg,
				battery: {
					voltageV: 15.8,
					remainingPct: Math.max(0, 100 - elapsedS * BATTERY_DRAIN_PCT_PER_S)
				},
				gps: { fixType: GpsFixType.GPS_FIX_TYPE_FIX_3D, numSatellites: 14, hdop: 0.8 },
				flightMode: FlightMode.FLIGHT_MODE_MISSION,
				armed: true,
				inAir: true,
				healthOk: true,
				connected: true
			}
		}
	};
}

export function startFakeFleet(sink: (envelope: Envelope) => void): () => void {
	const startedMs = Date.now();
	for (const vehicle of FAKE_FLEET) sink(infoEnvelope(vehicle));

	const timer = setInterval(() => {
		const nowMs = Date.now();
		const elapsedS = (nowMs - startedMs) / 1000;
		for (const vehicle of FAKE_FLEET) sink(stateEnvelope(vehicle, elapsedS, nowMs));
	}, 1000 / TICK_HZ);

	return () => clearInterval(timer);
}
