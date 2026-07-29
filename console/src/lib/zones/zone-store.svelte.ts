import booleanPointInPolygon from '@turf/boolean-point-in-polygon';
import { point, polygon } from '@turf/helpers';
import { fleet } from '$lib/fleet-store.svelte';
import { ZoneAckStatus, ZoneType, type Zone, type ZoneAck } from '$lib/gen/karshipta/v1/fleet';

const REQUEST_TIMEOUT_MS = 10_000;
/** trackers in a terminal state linger briefly so the operator sees the outcome */
const TRACKER_LINGER_MS = 6_000;
const MIN_ZONE_VERTICES = 3;

export function isZoneConfigTerminal(status: ZoneAckStatus): boolean {
	return (
		status === ZoneAckStatus.ZONE_ACK_STATUS_ACCEPTED ||
		status === ZoneAckStatus.ZONE_ACK_STATUS_REJECTED
	);
}

export interface ZoneConfigTracker {
	requestId: string;
	kind: 'create' | 'update' | 'delete';
	/** undefined only for a still-in-flight 'create': the id doesn't exist until the ack assigns one */
	zoneId: string | undefined;
	status: ZoneAckStatus;
	message: string;
	sentAtMs: number;
}

export interface LatLonPoint {
	latitudeDeg: number;
	longitudeDeg: number;
}

export interface ZoneDraft {
	vertices: LatLonPoint[];
}

/**
 * Operator-drawn safety zones (keep-in/keep-out polygons), a different
 * concept from geozoneStore's read-only, viewport-fetched airspace data
 * (geozones/geozone-store.svelte.ts) - distinct at every level (store,
 * map source/layer, legend), not merged into it. Geometry is fixed at
 * creation (fleet.proto's own comment): only name/type/altitude bounds can
 * change afterward, so there is no "update vertices" path here.
 */
class ZoneStore {
	zones = $state<Record<string, Zone>>({});
	zoneConfigRequests = $state<Record<string, ZoneConfigTracker>>({});
	/** local-only until "Finish" sends it as CreateZone; nothing on the
	 * gateway knows about a zone until that request is accepted. */
	draft = $state<ZoneDraft | undefined>(undefined);

	readonly zoneIds = $derived(
		Object.keys(this.zones).sort((a, b) =>
			(this.zones[a]?.name ?? '').localeCompare(this.zones[b]?.name ?? '')
		)
	);

	private timeoutTimers = new Map<string, ReturnType<typeof setTimeout>>();

	startDrawing(): void {
		this.draft = { vertices: [] };
	}

	cancelDrawing(): void {
		this.draft = undefined;
	}

	/** called by the map on click while a zone is being drawn */
	addVertex(latitudeDeg: number, longitudeDeg: number): void {
		this.draft?.vertices.push({ latitudeDeg, longitudeDeg });
	}

	removeLastVertex(): void {
		this.draft?.vertices.pop();
	}

	requestCreateZone(
		name: string,
		type: ZoneType,
		altitudeMinM: number | undefined,
		altitudeMaxM: number | undefined
	): string | undefined {
		const draft = this.draft;
		if (!draft || draft.vertices.length < MIN_ZONE_VERTICES) return undefined;
		const requestId = crypto.randomUUID();
		const vertices = draft.vertices.map((vertex) => ({
			latitudeDeg: vertex.latitudeDeg,
			longitudeDeg: vertex.longitudeDeg,
			altitudeMslM: 0,
			altitudeRelM: 0
		}));
		this.track(requestId, 'create', undefined, () => ({
			payload: {
				$case: 'createZone',
				createZone: { requestId, name, type, vertices, altitudeMinM, altitudeMaxM }
			}
		}));
		this.draft = undefined;
		return requestId;
	}

	requestUpdateZone(
		zoneId: string,
		name: string,
		type: ZoneType,
		altitudeMinM: number | undefined,
		altitudeMaxM: number | undefined
	): string {
		const requestId = crypto.randomUUID();
		this.track(requestId, 'update', zoneId, () => ({
			payload: {
				$case: 'updateZone',
				updateZone: { requestId, zoneId, name, type, altitudeMinM, altitudeMaxM }
			}
		}));
		return requestId;
	}

	requestDeleteZone(zoneId: string): string {
		const requestId = crypto.randomUUID();
		this.track(requestId, 'delete', zoneId, () => ({
			payload: { $case: 'deleteZone', deleteZone: { requestId, zoneId } }
		}));
		return requestId;
	}

	configRequestsFor(zoneId: string): ZoneConfigTracker[] {
		return Object.values(this.zoneConfigRequests)
			.filter((tracker) => tracker.zoneId === zoneId)
			.sort((a, b) => b.sentAtMs - a.sentAtMs);
	}

	/** Zones (of any type) whose polygon contains this point. */
	containsPoint(latitudeDeg: number, longitudeDeg: number): Zone[] {
		const target = point([longitudeDeg, latitudeDeg]);
		return Object.values(this.zones).filter((zone) => {
			if (zone.vertices.length < MIN_ZONE_VERTICES) return false;
			const ring = zone.vertices.map((vertex): [number, number] => [
				vertex.longitudeDeg,
				vertex.latitudeDeg
			]);
			ring.push(ring[0]);
			return booleanPointInPolygon(target, polygon([ring]));
		});
	}

	/** Upsert from a downstream Zone sync/update envelope. */
	applyZone(value: Zone): void {
		this.zones[value.zoneId] = value;
	}

	/** Resolves the matching tracker; an accepted delete also drops the
	 * local Zone object (there's no updated object to sync for a delete,
	 * unlike create/update, which the gateway re-broadcasts). */
	applyZoneAck(ack: ZoneAck): void {
		const tracker = this.zoneConfigRequests[ack.requestId];
		if (!tracker) {
			console.warn(`zoneStore: ZoneAck for unknown request_id ${ack.requestId}`);
			return;
		}
		this.settle(ack.requestId, ack.status, ack.message);
		if (tracker.kind === 'delete' && ack.status === ZoneAckStatus.ZONE_ACK_STATUS_ACCEPTED) {
			delete this.zones[ack.zoneId];
		}
	}

	teardown(): void {
		for (const timer of this.timeoutTimers.values()) clearTimeout(timer);
		this.timeoutTimers.clear();
		this.zones = {};
		this.zoneConfigRequests = {};
		this.draft = undefined;
	}

	private track(
		requestId: string,
		kind: ZoneConfigTracker['kind'],
		zoneId: string | undefined,
		buildEnvelope: () => Parameters<typeof fleet.sendUpstream>[0]
	): void {
		this.zoneConfigRequests[requestId] = {
			requestId,
			kind,
			zoneId,
			status: ZoneAckStatus.ZONE_ACK_STATUS_UNSPECIFIED,
			message: '',
			sentAtMs: Date.now()
		};
		this.timeoutTimers.set(
			requestId,
			setTimeout(() => {
				this.settle(
					requestId,
					ZoneAckStatus.ZONE_ACK_STATUS_REJECTED,
					'no acknowledgment from gateway'
				);
			}, REQUEST_TIMEOUT_MS)
		);
		try {
			fleet.sendUpstream(buildEnvelope());
		} catch (error) {
			const reason = error instanceof Error ? error.message : String(error);
			this.settle(requestId, ZoneAckStatus.ZONE_ACK_STATUS_REJECTED, reason);
		}
	}

	private settle(requestId: string, status: ZoneAckStatus, message: string): void {
		const tracker = this.zoneConfigRequests[requestId];
		if (!tracker || isZoneConfigTerminal(tracker.status)) return;
		tracker.status = status;
		tracker.message = message;
		if (isZoneConfigTerminal(status)) {
			const timer = this.timeoutTimers.get(requestId);
			if (timer) {
				clearTimeout(timer);
				this.timeoutTimers.delete(requestId);
			}
			setTimeout(() => {
				delete this.zoneConfigRequests[requestId];
			}, TRACKER_LINGER_MS);
		}
	}
}

export const zoneStore = new ZoneStore();
