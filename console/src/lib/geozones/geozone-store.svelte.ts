import booleanPointInPolygon from '@turf/boolean-point-in-polygon';
import { point } from '@turf/helpers';
import type { Geozone, GeozoneSource, ViewportBounds } from './types';
import { OpenAipGeozoneSource } from './openaip';

const FETCH_DEBOUNCE_MS = 600;

/**
 * Owns the currently loaded airspace zones for whatever the map viewport
 * last was. Inactive (no fetches, empty zones) until configure() is given an
 * API key; the map and the confirm dialogs both read this store rather than
 * each other, so neither needs to know a geozone source exists at all.
 */
class GeozoneStore {
	zones = $state<Geozone[]>([]);
	loadError = $state<string | undefined>(undefined);

	// $state so the `active` getter (read from a template) reacts to configure()
	private source = $state<GeozoneSource | undefined>(undefined);
	private debounceTimer: ReturnType<typeof setTimeout> | undefined;
	private lastRequestId = 0;

	get active(): boolean {
		return this.source !== undefined;
	}

	configure(apiKey: string | undefined): void {
		this.source = apiKey ? new OpenAipGeozoneSource(apiKey) : undefined;
		if (!this.source) {
			this.zones = [];
			this.loadError = undefined;
		}
	}

	/** debounced viewport fetch; safe to call on every map moveend */
	requestViewport(bounds: ViewportBounds): void {
		if (!this.source) return;
		if (this.debounceTimer) clearTimeout(this.debounceTimer);
		this.debounceTimer = setTimeout(() => void this.fetchViewport(bounds), FETCH_DEBOUNCE_MS);
	}

	/** zones (if any) containing a point; always empty while inactive */
	zonesContaining(latitudeDeg: number, longitudeDeg: number): Geozone[] {
		if (this.zones.length === 0) return [];
		const target = point([longitudeDeg, latitudeDeg]);
		return this.zones.filter((zone) => booleanPointInPolygon(target, zone.polygon));
	}

	private async fetchViewport(bounds: ViewportBounds): Promise<void> {
		const source = this.source;
		if (!source) return;
		const requestId = ++this.lastRequestId;
		try {
			const zones = await source.fetchViewport(bounds);
			if (requestId !== this.lastRequestId) return; // superseded by a newer viewport
			this.zones = zones;
			this.loadError = undefined;
		} catch (error) {
			if (requestId !== this.lastRequestId) return;
			this.loadError = error instanceof Error ? error.message : String(error);
			console.error('geozones: failed to load viewport', error);
		}
	}
}

export const geozoneStore = new GeozoneStore();
