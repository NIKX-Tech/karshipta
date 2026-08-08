/**
 * App-wide Metric/Imperial display preference - affects read-only value
 * formatting only (altitude, speed, distance readouts; see format.ts),
 * never the wire protocol or command inputs (takeoff altitude, waypoint
 * altitude): those stay in meters/m-per-second as the schema defines them,
 * so a display preference can never silently change what actually gets
 * sent to a ward. Same persisted-singleton shape as theme.svelte.ts.
 */
export type UnitSystem = 'metric' | 'imperial';

const STORAGE_KEY = 'karshipta:units';

class UnitsStore {
	current = $state<UnitSystem>('metric');

	init(): void {
		if (typeof window === 'undefined') return;
		const stored = window.localStorage.getItem(STORAGE_KEY);
		if (stored === 'metric' || stored === 'imperial') this.current = stored;
	}

	set(system: UnitSystem): void {
		this.current = system;
		if (typeof window === 'undefined') return;
		window.localStorage.setItem(STORAGE_KEY, system);
	}
}

export const unitsStore = new UnitsStore();
