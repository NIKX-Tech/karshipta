import type { AircraftCategory } from './types';

/** Shapes actually drawn on the map - fewer than AircraftCategory's own
 * buckets, since 'light' and 'heavy' share one silhouette (differentiated
 * by icon-size in fleet-map.svelte instead of a second shape). */
export type AircraftIconShape = 'plane' | 'rotorcraft' | 'glider' | 'uav' | 'ground' | 'unknown';

const ICON_SIZE = 32;

export function iconIdFor(shape: AircraftIconShape): string {
	return `aircraft-icon-${shape}`;
}

export function iconShapeForCategory(category: AircraftCategory): AircraftIconShape {
	if (category === 'light' || category === 'heavy') return 'plane';
	if (
		category === 'rotorcraft' ||
		category === 'glider' ||
		category === 'uav' ||
		category === 'ground'
	) {
		return category;
	}
	return 'unknown';
}

const CENTER_X = ICON_SIZE / 2;

/** Mirrors an outline's interior points across the vertical centerline and
 * appends them, so the left half of a shape is generated from the right
 * half instead of hand-typed separately - the previous three attempts at
 * this plane icon went crooked exactly because two independently-typed
 * "mirror" halves silently drifted out of sync. `profile` must start and
 * end exactly on the centerline (x === CENTER_X), e.g. [nose, ...right
 * side points..., tail]; those two endpoints aren't mirrored again. */
function symmetricPolygon(profile: [number, number][]): [number, number][] {
	const interior = profile.slice(1, -1);
	const mirroredInterior: [number, number][] = interior
		.slice()
		.reverse()
		.map(([x, y]) => [2 * CENTER_X - x, y]);
	return [...profile, ...mirroredInterior];
}

function fillPolygon(ctx: CanvasRenderingContext2D, points: [number, number][]): void {
	ctx.beginPath();
	points.forEach(([x, y], i) => {
		if (i === 0) ctx.moveTo(x, y);
		else ctx.lineTo(x, y);
	});
	ctx.closePath();
	ctx.fill();
}

// All shapes are drawn pointing "up" (north) in a 32x32 box centered on
// (16,16), solid white on transparent - MapLibre's sdf:true image mode
// reads the alpha coverage as a template mask and recolors it per-feature
// via icon-color, so fill color here is arbitrary as long as it's opaque.
//
// Fifth pass at this shape. Every previous attempt used one continuous
// outline that tapered smoothly from nose through wing through tail -
// confirmed live to read as an organic blob (a manta ray, in the exact
// words used to describe it) rather than a mechanical object, because
// real aircraft glyphs (the Unicode/Material "flight" icon included) draw
// the wings and tail as distinct sharp shapes crossing a separate thin
// fuselage, not one flowing silhouette. This draws three plain
// diamonds/kites - fuselage, wing, tail - each still guaranteed symmetric
// via symmetricPolygon, whose union reads as a hard-edged mechanical
// shape instead of a soft one.
//
// A later attempt at rounding these corners with arcTo for a softer,
// "higher quality" look made things worse instead of better (confirmed
// live) - the fuselage kite's nose/tail points are already a very acute
// angle, and rounding an acute corner with a fixed radius bulges it
// outward into a visible artifact rather than softening it cleanly. Kept
// sharp on purpose; the only safe, unambiguous quality lever here is
// resolution (see ICON_PIXEL_RATIO below), not the geometry itself.
function drawPlane(ctx: CanvasRenderingContext2D): void {
	fillPolygon(
		ctx,
		symmetricPolygon([
			[16, 4],
			[17.2, 11],
			[16, 27]
		])
	); // fuselage
	fillPolygon(
		ctx,
		symmetricPolygon([
			[16, 13.5],
			[27, 19.5],
			[16, 18.5]
		])
	); // wings
	fillPolygon(
		ctx,
		symmetricPolygon([
			[16, 23.5],
			[20.5, 28.5],
			[16, 27]
		])
	); // tail
}

// Real path data (not hand-derived like the shapes above) from a top-down
// helicopter silhouette by SVG Repo (svgrepo.com), a freely-licensed icon
// source - see the project's own credit at
// https://www.svgrepo.com/svg/424847/helicopter-bottom-view-silhouette.
// Native viewBox is 0 0 478.874 478.873, nose-up (small y) same as every
// hand-drawn shape here, so it's scaled rather than redrawn.
const ROTORCRAFT_VIEWBOX_SIZE = 478.874;
const ROTORCRAFT_BODY_PATH =
	'M463.096,252.605l-133.38-52.861V78.503V47.101c0-4.338-3.519-7.851-7.851-7.851s-7.851,3.513-7.851,7.851v31.402h-11.569C293.433,32.987,266.884,0,235.512,0c-31.37,0-57.919,32.987-66.938,78.503h-19.416V47.101c0-4.338-3.519-7.851-7.851-7.851s-7.85,3.513-7.85,7.851v31.402v43.46l-109-43.2c-6.987-2.771-14.597-0.112-16.99,5.933c-2.395,6.045,1.327,13.187,8.312,15.961l117.678,46.639v80.363v23.551c0,4.341,3.518,7.851,7.85,7.851s7.851-3.51,7.851-7.851V227.66h48.1c7.64,25.239,14.703,58.196,14.703,94.207v78.502h7.851v39.528c0,8.079,7.027,14.644,15.701,14.644c8.674,0,15.699-6.564,15.699-14.644v-39.528h7.851v-78.502c0-35.618,6.984-68.655,14.606-94.207h40.347v23.551c0,4.341,3.519,7.851,7.851,7.851s7.851-3.51,7.851-7.851V227.66v-2.583l124.703,49.425c6.981,2.773,14.596,0.121,16.987-5.935C473.799,262.512,470.081,255.383,463.096,252.605z M314.015,94.204v99.322l-24.132-9.567c9.91-19.424,15.877-44.248,15.877-71.307c0-6.297-0.409-12.435-1.03-18.448H314.015z M149.158,94.204h17.132c-0.621,6.014-1.023,12.151-1.023,18.448c0,7.694,0.486,15.207,1.406,22.468l-17.515-6.939V94.204z M149.158,211.958v-58.436l23.536,9.327c1.775,5.688,3.829,11.093,6.155,16.186l-0.433-0.148c0,0,6.476,12.457,13.74,33.071H149.158z M278.714,211.958c0.749-2.18,1.479-4.208,2.22-6.215l15.682,6.215H278.714z';
const ROTORCRAFT_TAIL_MARK_PATH =
	'M266.913,408.219c-4.328,0-7.851,3.518-7.851,7.85v54.954c0,4.332,3.522,7.851,7.851,7.851c4.332,0,7.85-3.519,7.85-7.851v-54.954C274.762,411.736,271.245,408.219,266.913,408.219z';

function drawRotorcraft(ctx: CanvasRenderingContext2D): void {
	const scale = ICON_SIZE / ROTORCRAFT_VIEWBOX_SIZE;
	ctx.save();
	ctx.scale(scale, scale);
	ctx.fill(new Path2D(ROTORCRAFT_BODY_PATH));
	ctx.fill(new Path2D(ROTORCRAFT_TAIL_MARK_PATH));
	ctx.restore();
}

function drawGlider(ctx: CanvasRenderingContext2D): void {
	ctx.fillRect(14.5, 5, 3, 22);
	ctx.fillRect(3, 14.5, 26, 2.2);
}

function drawUav(ctx: CanvasRenderingContext2D): void {
	ctx.save();
	ctx.lineWidth = 2;
	ctx.beginPath();
	ctx.moveTo(9, 9);
	ctx.lineTo(23, 23);
	ctx.moveTo(23, 9);
	ctx.lineTo(9, 23);
	ctx.strokeStyle = ctx.fillStyle as string;
	ctx.stroke();
	ctx.restore();
	ctx.fillRect(13, 13, 6, 6);
	for (const [cx, cy] of [
		[8, 8],
		[24, 8],
		[8, 24],
		[24, 24]
	] as const) {
		ctx.beginPath();
		ctx.arc(cx, cy, 3.4, 0, Math.PI * 2);
		ctx.fill();
	}
}

function drawGround(ctx: CanvasRenderingContext2D): void {
	const x = 9,
		y = 11,
		w = 14,
		h = 10,
		r = 3;
	ctx.beginPath();
	ctx.moveTo(x + r, y);
	ctx.arcTo(x + w, y, x + w, y + h, r);
	ctx.arcTo(x + w, y + h, x, y + h, r);
	ctx.arcTo(x, y + h, x, y, r);
	ctx.arcTo(x, y, x + w, y, r);
	ctx.closePath();
	ctx.fill();
}

function drawUnknown(ctx: CanvasRenderingContext2D): void {
	ctx.beginPath();
	ctx.moveTo(16, 8);
	ctx.lineTo(23, 16);
	ctx.lineTo(16, 24);
	ctx.lineTo(9, 16);
	ctx.closePath();
	ctx.fill();
}

const DRAWERS: Record<AircraftIconShape, (ctx: CanvasRenderingContext2D) => void> = {
	plane: drawPlane,
	rotorcraft: drawRotorcraft,
	glider: drawGlider,
	uav: drawUav,
	ground: drawGround,
	unknown: drawUnknown
};

export const AIRCRAFT_ICON_SHAPES: AircraftIconShape[] = [
	'plane',
	'rotorcraft',
	'glider',
	'uav',
	'ground',
	'unknown'
];

// All the drawing functions above work in a 32x32 logical coordinate
// space, but the actual bitmap is rasterized at 8x that (256x256) - a
// bare 32x32 source, registered at face value, looked genuinely bad
// (confirmed live: blocky, jagged edges) once MapLibre scaled it up to
// whatever the icon's real on-screen size ends up being at a given zoom,
// since a raster that small has no data left to interpolate from. Passing
// pixelRatio: ICON_PIXEL_RATIO to addImage (see fleet-map.svelte's own
// registration site) tells MapLibre this 256x256 bitmap represents a
// 32-logical-unit icon at 8x density, the same convention as a "@8x"
// retina image asset - so icon-size values already tuned against the
// 32-unit space stay correct, only the source resolution changes. Bumped
// from an initial 4x to 8x for noticeably smoother anti-aliased edges,
// particularly on 'heavy' category icons which render largest.
export const ICON_PIXEL_RATIO = 8;

/** Renders one shape to ImageData, the type maplibregl.Map#addImage
 * documents accepting directly (unlike a bare HTMLCanvasElement). Called
 * once per shape at map load, not per-feature - see fleet-map.svelte's own
 * registration site. */
export function buildAircraftIconImageData(shape: AircraftIconShape): ImageData | undefined {
	const canvas = document.createElement('canvas');
	canvas.width = ICON_SIZE * ICON_PIXEL_RATIO;
	canvas.height = ICON_SIZE * ICON_PIXEL_RATIO;
	const ctx = canvas.getContext('2d');
	if (!ctx) return undefined;
	ctx.scale(ICON_PIXEL_RATIO, ICON_PIXEL_RATIO);
	ctx.fillStyle = '#ffffff';
	DRAWERS[shape](ctx);
	return ctx.getImageData(0, 0, canvas.width, canvas.height);
}
