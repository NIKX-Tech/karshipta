import type { Envelope } from '$lib/gen/karshipta/v1/envelope';
import type { Event, VehicleInfo, VehicleState } from '$lib/gen/karshipta/v1/telemetry';

export interface Vehicle {
	info: VehicleInfo | undefined;
	state: VehicleState | undefined;
}

const MAX_EVENTS = 50;

/**
 * Single owner of all live fleet state, keyed by vehicle_id. Everything that
 * arrives from the gateway (or the fake fleet in dev) enters through
 * applyEnvelope; components only read.
 */
class FleetStore {
	vehicles = $state<Record<string, Vehicle>>({});
	events = $state<Event[]>([]);

	readonly vehicleIds = $derived(Object.keys(this.vehicles).sort());

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
			case 'commandAck':
			case 'missionProgress':
				// handled from the commands milestone onward
				console.warn(`fleet: ignoring not yet supported payload kind ${payload.$case}`);
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
		this.vehicles = {};
		this.events = [];
	}

	private upsert(vehicleId: string): Vehicle {
		if (!(vehicleId in this.vehicles)) {
			this.vehicles[vehicleId] = { info: undefined, state: undefined };
		}
		return this.vehicles[vehicleId];
	}
}

export const fleet = new FleetStore();
