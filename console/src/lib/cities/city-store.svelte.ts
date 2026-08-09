import type { City } from './types';
import { loadCities } from './city-data';

const EARTH_RADIUS_KM = 6371;
// Beyond this, "near <city>" would be misleading (the middle of an ocean or
// a sparse region can be several hundred km from the nearest population-100k+
// city) - better to fall back to raw coordinates than name a place that
// isn't actually nearby.
const MAX_NEAREST_CITY_KM = 150;

function haversineKm(lat1: number, lon1: number, lat2: number, lon2: number): number {
	const toRad = (deg: number) => (deg * Math.PI) / 180;
	const dLat = toRad(lat2 - lat1);
	const dLon = toRad(lon2 - lon1);
	const a =
		Math.sin(dLat / 2) ** 2 +
		Math.cos(toRad(lat1)) * Math.cos(toRad(lat2)) * Math.sin(dLon / 2) ** 2;
	return EARTH_RADIUS_KM * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
}

/**
 * Owns the bundled cities dataset once loaded, and whatever subset is
 * currently within the map viewport. Unlike geozone-store/obstacle-store/
 * airport-store, this never touches the network after its one-time dynamic
 * import - no debounce, no cache TTL, no openAipRequestGate coordination,
 * since the full dataset simply lives in memory once loaded (see
 * cities/city-data.ts's own comment). There is also no configure()/active
 * gate the way the OpenAIP layers have: nothing to fail, no key to be
 * missing, so the Cities checkbox in the layers menu is unconditional.
 */
class CityStore {
	cities = $state<City[]>([]);
	visibleCities = $state<City[]>([]);

	private loaded = false;
	private loading: Promise<void> | undefined;

	/** Idempotent; safe to call every time the Cities checkbox is turned on,
	 * not just the first time - only the first call actually imports the
	 * dataset. */
	async ensureLoaded(): Promise<void> {
		if (this.loaded) return;
		if (!this.loading) {
			this.loading = loadCities().then((cities) => {
				this.cities = cities;
				this.loaded = true;
			});
		}
		await this.loading;
	}

	/** Filters to the current viewport, thinning by population as zoom
	 * decreases - showing all 6,000+ cities at a whole-continent zoom would
	 * be visual noise the operator didn't ask for; the deeper they zoom,
	 * the more of the (already major-cities-only) list becomes relevant
	 * reference context rather than clutter. */
	setViewport(
		bounds: [west: number, south: number, east: number, north: number],
		zoom: number
	): void {
		const [west, south, east, north] = bounds;
		const minPopulation = zoom >= 8 ? 0 : zoom >= 6 ? 300_000 : zoom >= 4 ? 1_000_000 : 3_000_000;
		this.visibleCities = this.cities.filter(
			(city) =>
				city.population >= minPopulation &&
				city.longitudeDeg >= west &&
				city.longitudeDeg <= east &&
				city.latitudeDeg >= south &&
				city.latitudeDeg <= north
		);
	}

	/** Nearest bundled city to a point, for labeling purposes (e.g. "weather
	 * near X") rather than the viewport-filtered display above - searches the
	 * full loaded dataset regardless of what's currently on screen. Returns
	 * undefined if nothing is within MAX_NEAREST_CITY_KM, or if the dataset
	 * hasn't been loaded yet (see ensureLoaded). */
	nearestTo(latitudeDeg: number, longitudeDeg: number): City | undefined {
		let nearest: City | undefined;
		let nearestKm = MAX_NEAREST_CITY_KM;
		for (const city of this.cities) {
			const distanceKm = haversineKm(
				latitudeDeg,
				longitudeDeg,
				city.latitudeDeg,
				city.longitudeDeg
			);
			if (distanceKm < nearestKm) {
				nearest = city;
				nearestKm = distanceKm;
			}
		}
		return nearest;
	}
}

export const cityStore = new CityStore();
