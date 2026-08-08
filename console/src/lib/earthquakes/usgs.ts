import type { Earthquake, EarthquakeSource, ViewportBounds } from './types';

const USGS_API_URL = 'https://earthquake.usgs.gov/fdsnws/event/1/query';
// Recent activity only, not USGS's full historical archive - a fleet
// operator cares about what's happening now, not a quake from six months
// ago. 30 days is generous enough that a quiet region still shows
// something meaningful rather than reading as broken.
const LOOKBACK_DAYS = 30;
// Below this, "earthquake" starts meaning "imperceptible tremor a seismograph
// noticed" - not useful map clutter for a fleet operator.
const MIN_MAGNITUDE = 2.5;

/** GeoJSON already - confirmed live against a real bbox query, no custom
 * parsing needed the way OpenSky's positional arrays or FIRMS' CSV do. */
function parseFeature(raw: unknown): Earthquake | undefined {
	if (typeof raw !== 'object' || raw === null) return undefined;
	const feature = raw as Record<string, unknown>;
	const id = feature.id;
	const properties = feature.properties;
	const geometry = feature.geometry;
	if (
		typeof id !== 'string' ||
		typeof properties !== 'object' ||
		properties === null ||
		typeof geometry !== 'object' ||
		geometry === null
	) {
		return undefined;
	}
	const props = properties as Record<string, unknown>;
	const coords = (geometry as Record<string, unknown>).coordinates;
	if (!Array.isArray(coords) || coords.length < 3) return undefined;
	const [longitudeDeg, latitudeDeg, depthKm] = coords as number[];
	const magnitude = props.mag;
	const place = props.place;
	const time = props.time;
	if (
		typeof magnitude !== 'number' ||
		typeof latitudeDeg !== 'number' ||
		typeof longitudeDeg !== 'number' ||
		typeof depthKm !== 'number' ||
		typeof time !== 'number'
	) {
		return undefined;
	}
	return {
		id,
		magnitude,
		place: typeof place === 'string' ? place : 'Unknown location',
		timeMs: time,
		latitudeDeg,
		longitudeDeg,
		depthKm
	};
}

export class UsgsEarthquakeSource implements EarthquakeSource {
	async fetchViewport(bounds: ViewportBounds): Promise<Earthquake[]> {
		const [west, south, east, north] = bounds;
		const startTime = new Date(Date.now() - LOOKBACK_DAYS * 86_400_000).toISOString();
		const url =
			`${USGS_API_URL}?format=geojson&starttime=${startTime}&minmagnitude=${MIN_MAGNITUDE}` +
			`&minlatitude=${south}&maxlatitude=${north}&minlongitude=${west}&maxlongitude=${east}`;
		const response = await fetch(url);
		if (!response.ok) {
			throw new Error(`USGS request failed: ${response.status} ${response.statusText}`);
		}
		const body: unknown = await response.json();
		const features =
			typeof body === 'object' &&
			body !== null &&
			Array.isArray((body as { features?: unknown }).features)
				? (body as { features: unknown[] }).features
				: [];
		const earthquakes: Earthquake[] = [];
		for (const raw of features) {
			const earthquake = parseFeature(raw);
			if (earthquake) earthquakes.push(earthquake);
		}
		return earthquakes;
	}
}
