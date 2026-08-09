/** Plain [west, south, east, north] tuple - structurally identical to (and
 * interchangeable with) each OpenAIP-backed store's own ViewportBounds
 * type, deliberately not imported from any of them so this stays a
 * dependency-free shared utility, same reasoning as those types not
 * importing from each other either. */
type Bounds = [west: number, south: number, east: number, north: number];

// OpenAIP rejects a bbox wider or taller than 5 degrees (confirmed live -
// see geozones/openaip.ts's own comment); each source module still clamps
// to that window itself regardless (defense in depth, and correct even if
// this splitting were skipped for some reason). This only decides whether
// a *store* should split one moveend into two side-by-side queries instead
// of silently relying on a single 5-degree window centered on the
// viewport, which drops everything outside it - confirmed live to read as
// a dense, misleading cluster rather than a sensible "zoomed out" view,
// the same problem aircraft's own point+radius query had. Kept smaller
// than aircraft's response to that (a 2x2/4-tile grid) on purpose:
// OpenAIP's key is shared across three layers and far more tightly
// rate-limited than airplanes.live (see request-gate.ts), so this only
// ever splits into 2, along whichever axis is wider - not a full grid.
const MAX_BBOX_SPAN_DEG = 5;
// Only bother splitting once meaningfully bigger than one window - a
// small overhang past 5 degrees isn't worth doubling the request count
// for, given how tight the shared budget already is.
const SPLIT_THRESHOLD_DEG = MAX_BBOX_SPAN_DEG * 1.5;

export function tileOpenAipBounds(bounds: Bounds): Bounds[] {
	const [west, south, east, north] = bounds;
	const widthDeg = east - west;
	const heightDeg = north - south;
	if (widthDeg <= SPLIT_THRESHOLD_DEG && heightDeg <= SPLIT_THRESHOLD_DEG) {
		return [bounds];
	}
	if (widthDeg >= heightDeg) {
		const midLon = (west + east) / 2;
		return [
			[west, south, midLon, north],
			[midLon, south, east, north]
		];
	}
	const midLat = (south + north) / 2;
	return [
		[west, south, east, midLat],
		[west, midLat, east, north]
	];
}
