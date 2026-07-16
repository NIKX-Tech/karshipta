/**
 * Runtime protobuf codec for every generated message (MessageFns: .encode()/
 * .decode()), as a separate subpath from the package root. index.ts keeps
 * message types type-only on purpose (ts-proto redeclares `MessageFns` per
 * generated file, so re-exporting the value from more than one place there
 * would collide); most consumers only need the shapes anyway. A consumer
 * that needs to construct or decode real Envelopes off the browser (a
 * server building its own gateway-shaped WebSocket endpoint, for one)
 * imports from here instead: `@nikx-tech/karshipta-console-core/wire`.
 */
export { GeoPoint, VelocityNed } from './gen/karshipta/v1/common';
export { Envelope } from './gen/karshipta/v1/envelope';
export { AddVehicle, RemoveVehicle, VehicleConfigAck } from './gen/karshipta/v1/fleet';
export {
	ArmCommand,
	DisarmCommand,
	TakeoffCommand,
	LandCommand,
	ReturnToLaunchCommand,
	GotoCommand,
	StartMissionCommand,
	PauseMissionCommand,
	Command,
	CommandAck,
	MissionItem,
	Mission,
	MissionProgress
} from './gen/karshipta/v1/command';
export { VehicleInfo, Battery, Gps, VehicleState, Event } from './gen/karshipta/v1/telemetry';
