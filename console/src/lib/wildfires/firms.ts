import type { FireHotspot, FireHotspotSource, ViewportBounds } from './types';

const FIRMS_API_URL = 'https://firms.modaps.eosdis.nasa.gov/api/area/csv';
// VIIRS SNPP: the same satellite/instrument confirmed live against a real
// key and bbox. NRT (near-real-time) is what an operational fleet cares
// about, not FIRMS' own multi-year standard-processing archive.
const DATASET = 'VIIRS_SNPP_NRT';
const LOOKBACK_DAYS = 1;

// FIRMS returns plain CSV (confirmed live: a header row, then one row per
// detection), not JSON like every other layer here - `latitude,longitude,
// bright_ti4,scan,track,acq_date,acq_time,satellite,instrument,confidence,
// version,bright_ti5,frp,daynight`. No quoted/escaped fields in this
// format, so a plain split is safe.
function parseCsv(text: string): FireHotspot[] {
	const lines = text.trim().split('\n');
	if (lines.length < 2) return [];
	const header = lines[0].split(',');
	const latIdx = header.indexOf('latitude');
	const lonIdx = header.indexOf('longitude');
	const brightIdx = header.indexOf('bright_ti4');
	const confidenceIdx = header.indexOf('confidence');
	const dateIdx = header.indexOf('acq_date');
	const timeIdx = header.indexOf('acq_time');
	const frpIdx = header.indexOf('frp');
	if (latIdx === -1 || lonIdx === -1) return [];

	const hotspots: FireHotspot[] = [];
	for (let i = 1; i < lines.length; i++) {
		const cells = lines[i].split(',');
		const latitudeDeg = Number(cells[latIdx]);
		const longitudeDeg = Number(cells[lonIdx]);
		const brightnessK = Number(cells[brightIdx]);
		if (!Number.isFinite(latitudeDeg) || !Number.isFinite(longitudeDeg)) continue;
		const date = cells[dateIdx] ?? '';
		const time = (cells[timeIdx] ?? '').padStart(4, '0');
		const frp = frpIdx !== -1 ? Number(cells[frpIdx]) : NaN;
		hotspots.push({
			id: `${latitudeDeg},${longitudeDeg},${date}${time}`,
			latitudeDeg,
			longitudeDeg,
			brightnessK: Number.isFinite(brightnessK) ? brightnessK : 0,
			confidence: cells[confidenceIdx] ?? 'unknown',
			acquiredAtIso: date && time ? `${date}T${time.slice(0, 2)}:${time.slice(2)}:00Z` : date,
			frpMw: Number.isFinite(frp) ? frp : undefined
		});
	}
	return hotspots;
}

export class FirmsFireHotspotSource implements FireHotspotSource {
	constructor(private readonly apiKey: string) {}

	async fetchViewport(bounds: ViewportBounds): Promise<FireHotspot[]> {
		const [west, south, east, north] = bounds;
		const url = `${FIRMS_API_URL}/${this.apiKey}/${DATASET}/${west},${south},${east},${north}/${LOOKBACK_DAYS}`;
		const response = await fetch(url);
		if (!response.ok) {
			throw new Error(`FIRMS request failed: ${response.status} ${response.statusText}`);
		}
		const text = await response.text();
		// FIRMS answers a bad key/area with 200 + a plain-text error line, not
		// a real 4xx - the header check in parseCsv (latIdx === -1) already
		// catches that shape (an error string has no "latitude" column) and
		// returns an empty list rather than throwing on it as if it were data.
		if (text.toLowerCase().includes('invalid')) {
			throw new Error(`FIRMS: ${text.trim()}`);
		}
		return parseCsv(text);
	}
}
