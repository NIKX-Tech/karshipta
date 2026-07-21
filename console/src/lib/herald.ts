import { GpsFixType, WardClass } from '$lib/gen/karshipta/v1/common';
import type { GeoPoint, VelocityNed } from '$lib/gen/karshipta/v1/common';
import type { Battery, Gps, WardState } from '$lib/gen/karshipta/v1/telemetry';

/**
 * Hand-written mirror of the standalone NIKX-Tech/herald schema
 * (herald.proto, package herald.v0). Herald lives outside this repo and
 * is not generated here; keep this in sync by hand against
 * https://github.com/NIKX-Tech/herald if the standard changes.
 */
export enum HeraldEntityClass {
	ENTITY_CLASS_UNSPECIFIED = 0,
	ENTITY_CLASS_MULTIROTOR = 1,
	ENTITY_CLASS_FIXED_WING = 2,
	ENTITY_CLASS_VTOL = 3,
	ENTITY_CLASS_HELICOPTER = 4,
	ENTITY_CLASS_GROUND_VEHICLE = 5,
	ENTITY_CLASS_LIVESTOCK_TAG = 6,
	ENTITY_CLASS_GENERIC_TRACKER = 7
}

export interface HeraldGeoPoint {
	latitudeDeg: number;
	longitudeDeg: number;
	altitudeMslM: number;
	altitudeRelM: number;
}

export interface HeraldVelocity {
	northMS: number;
	eastMS: number;
	downMS: number;
}

export interface HeraldBattery {
	voltageV: number;
	remainingPct: number;
}

export interface HeraldGps {
	numSatellites: number;
	hdop: number;
}

export interface HeraldMessage {
	entityId: string;
	timestampMs: number;
	entityClass: HeraldEntityClass;
	position: HeraldGeoPoint;
	velocity?: HeraldVelocity;
	battery?: HeraldBattery;
	gps?: HeraldGps;
	healthOk: boolean;
	tags: string[];
	// Tenant/organization scope. WardState carries no org scoping today, so
	// this has no destination field on the Karshipta side yet. Carry it
	// through here once Fleet/Zone-style org scoping is extended to
	// WardState; do not invent a WardState field for it before then.
	orgId: string;
}

const ENTITY_CLASS_TO_WARD_CLASS: Record<HeraldEntityClass, WardClass> = {
	[HeraldEntityClass.ENTITY_CLASS_UNSPECIFIED]: WardClass.WARD_CLASS_UNSPECIFIED,
	[HeraldEntityClass.ENTITY_CLASS_MULTIROTOR]: WardClass.WARD_CLASS_MULTIROTOR,
	[HeraldEntityClass.ENTITY_CLASS_FIXED_WING]: WardClass.WARD_CLASS_FIXED_WING,
	[HeraldEntityClass.ENTITY_CLASS_VTOL]: WardClass.WARD_CLASS_VTOL,
	[HeraldEntityClass.ENTITY_CLASS_HELICOPTER]: WardClass.WARD_CLASS_HELICOPTER,
	[HeraldEntityClass.ENTITY_CLASS_GROUND_VEHICLE]: WardClass.WARD_CLASS_GROUND,
	[HeraldEntityClass.ENTITY_CLASS_LIVESTOCK_TAG]: WardClass.WARD_CLASS_LIVESTOCK_TAG,
	[HeraldEntityClass.ENTITY_CLASS_GENERIC_TRACKER]: WardClass.WARD_CLASS_GENERIC_TRACKER
};

function toGeoPoint(position: HeraldGeoPoint): GeoPoint {
	return {
		latitudeDeg: position.latitudeDeg,
		longitudeDeg: position.longitudeDeg,
		altitudeMslM: position.altitudeMslM,
		altitudeRelM: position.altitudeRelM
	};
}

function toVelocity(velocity: HeraldVelocity | undefined): VelocityNed | undefined {
	return velocity
		? { northMS: velocity.northMS, eastMS: velocity.eastMS, downMS: velocity.downMS }
		: undefined;
}

function toBattery(battery: HeraldBattery | undefined): Battery | undefined {
	return battery ? { voltageV: battery.voltageV, remainingPct: battery.remainingPct } : undefined;
}

function toGps(gps: HeraldGps | undefined): Gps | undefined {
	// Herald carries no fix-type equivalent; WARD_CLASS-style "unspecified"
	// is the honest default rather than guessing a fix quality.
	return gps
		? { fixType: GpsFixType.GPS_FIX_TYPE_UNSPECIFIED, numSatellites: gps.numSatellites, hdop: gps.hdop }
		: undefined;
}

/**
 * Maps an incoming Herald message onto Karshipta's own WardState.
 * WardState is a superset produced from Herald, not Herald's own
 * canonical shape: fields Herald never carries (heading, flight state)
 * are left at their zero value or unset, and WardState.flight is never
 * populated here, only by the MAVSDK gateway for actual flight wards.
 *
 * entity_class has no counterpart on WardState itself: WardClass lives
 * on the separate WardInfo identity message (sent on connect and on
 * change), not on the per-tick WardState. Use
 * heraldEntityClassToWardClass separately when constructing a ward's
 * WardInfo.
 */
export function heraldToWardState(herald: HeraldMessage): WardState {
	return {
		wardId: herald.entityId,
		timestampMs: herald.timestampMs,
		position: toGeoPoint(herald.position),
		velocity: toVelocity(herald.velocity),
		headingDeg: 0,
		battery: toBattery(herald.battery),
		gps: toGps(herald.gps),
		healthOk: herald.healthOk,
		connected: true,
		flight: undefined,
		tags: herald.tags
	};
}

/** Maps Herald's entity_class onto a ward's WardInfo.wardClass. */
export function heraldEntityClassToWardClass(entityClass: HeraldEntityClass): WardClass {
	return ENTITY_CLASS_TO_WARD_CLASS[entityClass];
}
